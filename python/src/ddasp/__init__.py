"""ddasp: compile ASP programs into ZDDs/BDDs of their answer sets.

Typical use::

    import ddasp
    result = ddasp.compile_text("{ a; b }. :- a, b. #show a/0. #show b/0.")
    result.count            # 3
    result.zdd              # kyotodd.ZDD over the shown atoms (needs the kyotodd package)
    list(result.answer_sets())   # [frozenset(), frozenset({'a'}), frozenset({'b'})]

Grounding is done by clingo; the ground program is collected through a
``clingo`` observer into a :class:`Program` and compiled by the C++ core.
"""

import os
import warnings
import weakref
from collections.abc import Mapping
from typing import Iterable, Iterator, List, Optional, Sequence, Tuple, Union

import clingo
from clingo.backend import Observer as _ClingoObserver

from ddasp._core import CoreResult, NodeLimitError, Program, read_aspif
from ddasp._core import compile as _core_compile
from ddasp._core import stored_dd_declarations as _stored_dd_declarations
from ddasp._core import stored_dd_image as _stored_dd_image
from ddasp._core import stored_dd_parts as _stored_dd_parts

__all__ = [
    "Program",
    "Observer",
    "Result",
    "StoredDD",
    "NodeLimitError",
    "read_aspif",
    "load",
    "compile",
    "compile_control",
    "compile_text",
    "compile_files",
]


class Observer(_ClingoObserver):
    """clingo ground-program observer that fills a :class:`Program`.

    Register it with ``control.register_observer(observer)`` *before* grounding.
    """

    def __init__(self, program: Optional[Program] = None):
        self.program = program if program is not None else Program()

    def rule(self, choice: bool, head: Sequence[int], body: Sequence[int]) -> None:
        self.program.add_rule(choice, list(head), list(body))

    def weight_rule(self, choice: bool, head: Sequence[int], lower_bound: int, body) -> None:
        self.program.add_weight_rule(choice, list(head), lower_bound, [(l, w) for l, w in body])

    def minimize(self, priority: int, literals) -> None:
        self.program.add_minimize(priority, [(l, w) for l, w in literals])

    def project(self, atoms: Sequence[int]) -> None:
        self.program.add_projection(list(atoms))

    def output_atom(self, symbol: clingo.Symbol, atom: int) -> None:
        # atom == 0 means a fact shown unconditionally (gringo does not name its atom).
        self.program.add_output(str(symbol), [atom] if atom != 0 else [])

    def output_term(self, symbol: clingo.Symbol, condition: Sequence[int]) -> None:
        self.program.add_output(str(symbol), list(condition))

    def external(self, atom: int, value: clingo.TruthValue) -> None:
        mapping = {
            clingo.TruthValue.Free: 0,
            clingo.TruthValue.True_: 1,
            clingo.TruthValue.False_: 2,
            clingo.TruthValue.Release: 3,
        }
        self.program.add_external(atom, mapping.get(value, 0))

    # Statements DDASP does not use are reported (once per kind), like the aspif reader does.
    def _ignored(self, what: str) -> None:
        warnings.warn(f"ddasp: {what} statement ignored", RuntimeWarning, stacklevel=3)

    def assume(self, literals: Sequence[int]) -> None:
        self.program.add_assumption(list(literals))

    def heuristic(self, atom: int, type_, bias: int, priority: int, condition: Sequence[int]) -> None:
        self._ignored("heuristic")

    def acyc_edge(self, node_u: int, node_v: int, condition: Sequence[int]) -> None:
        self._ignored("edge")

    def theory_atom(self, atom_id_or_zero: int, term_id: int, elements: Sequence[int]) -> None:
        self._ignored("theory")

    def theory_atom_with_guard(self, atom_id_or_zero: int, term_id: int, elements: Sequence[int],
                               operator_id: int, right_hand_side_id: int) -> None:
        self._ignored("theory")


def _ensure_levels(n: int) -> None:
    """Make sure the kyotodd instance has variables at levels ``1..n``.

    The binary export of a diagram records only the levels that carry nodes, so
    importing it creates no variable for the levels above its top node (atoms
    that are true in no answer set); ``kyotodd.to_var`` would fail for them.
    """
    import kyotodd

    while kyotodd.var_count() < n:
        kyotodd.new_var()


class Result:
    """Result of a compilation.

    Attributes mirror :class:`ddasp._core.CoreResult`. The decision diagrams are
    available as KyotoDD binary bytes (``zdd_bytes``, ``bdd_bytes``) and, when the
    ``kyotodd`` package is installed, as ``zdd`` / ``bdd`` objects. In the imported
    diagrams the atom of a variable ``v`` is ``level_to_atom[kyotodd.to_level(v)]``.
    """

    def __init__(self, core: CoreResult):
        self._core = core
        self._zdd = None
        self._bdd = None
        # The core maps are std::map objects that pybind11 converts to a new dict
        # on every access; convert them once.
        self._names = dict(core.names)
        self._level_to_atom = dict(core.level_to_atom)
        self._atom_to_level = dict(core.atom_to_level)

    # --- plain data -------------------------------------------------------
    @property
    def zdd_bytes(self) -> bytes:
        return self._core.zdd_bytes

    @property
    def bdd_bytes(self) -> bytes:
        return self._core.bdd_bytes

    @property
    def levels(self) -> List[int]:
        """Levels of the shown atoms' variables, top of the DD first."""
        return list(self._core.levels)

    @property
    def level_to_atom(self) -> dict:
        """Level -> atom id for the variables of the result (the shown atoms)."""
        return dict(self._level_to_atom)

    @property
    def atom_to_level(self) -> dict:
        """Atom id -> level for every atom of the program (hidden atoms included)."""
        return dict(self._atom_to_level)

    @property
    def names(self) -> dict:
        """Atom id -> primary name of every named atom."""
        return dict(self._names)

    @property
    def fact_names(self) -> List[str]:
        """Names shown in every answer set (facts)."""
        return list(self._core.fact_names)

    @property
    def terms(self) -> List[Tuple[str, List[int]]]:
        """The other #show outputs as ``(name, condition)`` pairs.

        ``condition`` is a list of signed atom ids; the name is shown when every
        literal holds. This covers further names of an atom (``#show t : a.``
        when ``a`` is already shown) and negated conditions (``#show t : not a.``).
        The atoms of these conditions count as shown atoms.
        """
        return [(name, list(cond)) for name, cond in self._core.terms]

    @property
    def costs(self) -> list:
        """One dict per #minimize priority (most important first): priority, offset, weights {level: w}.

        The costs refer to the answer sets *before* projection: ``weights`` may
        mention levels of hidden atoms (see ``atom_to_level``), which are not
        variables of ``zdd`` when ``project=True``.
        """
        return list(self._core.costs)

    @property
    def optimum(self) -> List[int]:
        """Optimal #minimize cost per priority level, most important first.

        Filled by ``optimize=True`` and by ``cost_slack`` (on a consistent program);
        empty otherwise.
        """
        return list(self._core.optimum)

    @property
    def count(self) -> int:
        """Number of answer sets (exact)."""
        return int(self._core.count)

    @property
    def stats(self) -> dict:
        return dict(self._core.stats)

    # --- consequences ------------------------------------------------------
    @property
    def frequencies(self) -> dict:
        """Name -> number of answer sets displaying it (insertion order = display order).

        The names are those of :meth:`answer_sets`: fact names, the names of the
        atoms of the result, then the #show terms; a name shown through several
        outputs counts the answer sets displaying it through any of them. With
        projection the count refers to the projected answer sets.
        """
        return {name: int(count) for name, count in self._core.frequencies}

    @property
    def brave(self) -> frozenset:
        """Brave consequences: the names displayed by at least one answer set."""
        return frozenset(name for name, count in self._core.frequencies if int(count) > 0)

    @property
    def cautious(self) -> frozenset:
        """Cautious consequences: the names displayed by every answer set (empty if there is none)."""
        total = self.count
        if total == 0:
            return frozenset()
        return frozenset(name for name, count in self._core.frequencies if int(count) == total)

    def atom_name(self, atom: int) -> str:
        return self._names.get(atom, f"__atom{atom}")

    def level_name(self, level: int) -> str:
        return self.atom_name(self._level_to_atom[level])

    # --- kyotodd objects --------------------------------------------------
    @property
    def zdd(self):
        """The family of answer sets as a ``kyotodd.ZDD`` (imports lazily).

        The variables of every level in ``levels`` exist afterwards, also those
        of atoms that are true in no answer set.
        """
        if self._zdd is None:
            import kyotodd

            self._zdd = kyotodd.ZDD.import_binary_str(self._core.zdd_bytes)
            _ensure_levels(max(self._core.levels, default=0))
        return self._zdd

    @property
    def bdd(self):
        """The characteristic function of the answer sets as a ``kyotodd.BDD``."""
        if self._bdd is None:
            import kyotodd

            self._bdd = kyotodd.BDD.import_binary_str(self._core.bdd_bytes)
            _ensure_levels(max(self._core.levels, default=0))
        return self._bdd

    def variables(self) -> List[int]:
        """``kyotodd`` variable numbers of the shown atoms, top of the DD first."""
        import kyotodd

        self.zdd  # make sure the variables exist in the kyotodd instance
        return [kyotodd.to_var(lev) for lev in self._core.levels]

    def answer_sets(self) -> Iterator[frozenset]:
        """Iterate over the answer sets as frozensets of shown names (via kyotodd).

        Each set holds the fact names, the names of its atoms and the terms
        whose conditions hold, like clingo's output. Unnamed (hidden) atoms are
        left out, so without projection the same frozenset can be yielded
        several times.
        """
        import kyotodd

        facts = frozenset(self._core.fact_names)
        names = self._names
        terms = self._core.terms
        level_to_atom = self._level_to_atom
        for s in self.zdd.enumerate():
            atoms = frozenset(level_to_atom[kyotodd.to_level(v)] for v in s)
            shown = set(names[a] for a in atoms if a in names)
            for name, cond in terms:
                if all((abs(l) in atoms) == (l > 0) for l in cond):
                    shown.add(name)
            yield facts | frozenset(shown)

    # --- stored DDs -------------------------------------------------------
    def stored(self) -> "StoredDD":
        """The result as a :class:`StoredDD` (the file image written by ``save``).

        Every variable of the result must carry a #show name (so not with
        ``project=False`` and hidden atoms); otherwise ``ValueError`` is raised.
        Show terms (additional names, negated conditions) are not stored; a
        ``RuntimeWarning`` reports it.
        """
        data = self._core.stored_bytes
        if data is None:
            raise ValueError(self._core.stored_error)
        for w in self._core.stored_warnings:
            warnings.warn("ddasp: " + w, RuntimeWarning, stacklevel=2)
        return StoredDD(data)

    def save(self, path) -> None:
        """Write the result as a stored DD file (see :meth:`stored`)."""
        self.stored().save(path)


class StoredDD:
    """A stored DD: the answer-set family of a compilation together with its
    atom names and fact names, in the file format of ``ddasp --save``.

    Loading one into a compilation (the ``dds`` argument of :func:`compile_text`
    and friends) adds every stored atom to the program as a named ``#external``
    atom and keeps only the answer sets whose restriction to the stored atoms is
    one of the stored sets. The program may use the stored atoms in rule bodies
    and constraints but must not define them.
    """

    def __init__(self, data: bytes):
        names, fact_names, payload, terminal = _stored_dd_parts(data)  # validates the image
        self._data = bytes(data)
        self._names = list(names)
        self._fact_names = list(fact_names)
        self._payload = payload
        self._terminal = terminal
        self._zdd = None

    @classmethod
    def load(cls, path) -> "StoredDD":
        with open(path, "rb") as f:
            return cls(f.read())

    @classmethod
    def from_zdd(cls, zdd, names, fact_names: Iterable[str] = ()) -> "StoredDD":
        """A stored DD from a ``kyotodd.ZDD`` built directly with KyotoDD (not by a
        compilation), so that an ASP program can use the family as a constraint.

        ``names`` gives the atom name of each variable of ``zdd``: a dict
        ``{variable: name}``, or a list naming the variables ``1, 2, ...`` in order.
        The named variables must occupy levels ``1..N`` of the kyotodd instance
        (as ``kyotodd.new_vars(N)`` in a fresh instance does) and every variable
        that occurs in ``zdd`` must be named. ``fact_names`` are shown in every
        answer set. Names must be distinct and writable as atoms of a program;
        ``ValueError`` otherwise.
        """
        import kyotodd

        items = list(names.items()) if isinstance(names, Mapping) else list(enumerate(names, start=1))
        n = len(items)
        by_level = {}
        for v, name in items:
            lev = kyotodd.to_level(v)
            if lev in by_level:
                raise ValueError(f"variables {by_level[lev][0]} and {v} are the same variable")
            by_level[lev] = (v, str(name))
        if sorted(by_level) != list(range(1, n + 1)):
            raise ValueError(
                f"the named variables must occupy levels 1..{n} of the kyotodd instance "
                f"(their levels are {sorted(by_level)})"
            )
        named = {v for v, _ in items}
        unnamed = [v for v in zdd.support_vars() if v not in named]
        if unnamed:
            raise ValueError(f"variables {sorted(unnamed)} occur in the ZDD but have no name")
        top_first = [by_level[lev][1] for lev in range(n, 0, -1)]
        if zdd.is_zero:
            terminal, payload = 1, b""
        elif zdd.is_one:
            terminal, payload = 2, b""
        else:
            terminal, payload = 0, zdd.export_binary_str()
        return cls(_stored_dd_image(top_first, [str(f) for f in fact_names], payload, terminal))

    def save(self, path) -> None:
        with open(path, "wb") as f:
            f.write(self._data)

    @property
    def data(self) -> bytes:
        """The file image."""
        return self._data

    @property
    def names(self) -> List[str]:
        """Atom names, top of the diagram first."""
        return list(self._names)

    @property
    def fact_names(self) -> List[str]:
        """Names shown in every answer set."""
        return list(self._fact_names)

    def declarations(self) -> str:
        """Program text (``#external``, ``#show``, facts) that declares the stored atoms."""
        return _stored_dd_declarations(self._data)

    @property
    def zdd(self):
        """The family as a ``kyotodd.ZDD``; the atom of variable ``v`` is
        ``names[len(names) - kyotodd.to_level(v)]``."""
        if self._zdd is None:
            import kyotodd

            if self._terminal:
                self._zdd = kyotodd.ZDD(1 if self._terminal == 2 else 0)
            else:
                self._zdd = kyotodd.ZDD.import_binary_str(self._payload)
            _ensure_levels(len(self._names))
        return self._zdd

    @property
    def count(self) -> int:
        """Number of stored answer sets (via kyotodd)."""
        return int(self.zdd.exact_count)

    def answer_sets(self) -> Iterator[frozenset]:
        """Iterate over the stored answer sets as frozensets of names (via kyotodd)."""
        import kyotodd

        facts = frozenset(self._fact_names)
        n = len(self._names)
        for s in self.zdd.enumerate():
            yield facts | frozenset(self._names[n - kyotodd.to_level(v)] for v in s)

    def __repr__(self) -> str:
        return f"StoredDD({len(self._names)} atoms, {len(self._fact_names)} facts)"


def load(path) -> StoredDD:
    """Read a stored DD file (written by :meth:`Result.save` or ``ddasp --save``)."""
    return StoredDD.load(path)


def _as_stored(dd) -> StoredDD:
    if isinstance(dd, StoredDD):
        return dd
    if isinstance(dd, Result):
        return dd.stored()
    if isinstance(dd, (bytes, bytearray)):
        return StoredDD(bytes(dd))
    if isinstance(dd, (str, os.PathLike)):
        return StoredDD.load(dd)
    raise TypeError(f"dds: expected a StoredDD, a Result, a file path or bytes, got {type(dd).__name__}")


def _stored_list(dds) -> List[StoredDD]:
    """The ``dds`` argument as a list of StoredDD (a single DD may be given bare)."""
    if isinstance(dds, (StoredDD, Result, bytes, bytearray, str, os.PathLike)):
        dds = [dds]
    return [_as_stored(dd) for dd in dds]


def _cost_bound(name: str, value, non_negative: bool = False) -> List[int]:
    """A cost bound argument as a list of ints, one per priority level (``None`` or ``[]``: no bound)."""
    if value is None:
        return []
    entries = list(value) if isinstance(value, (list, tuple)) else [value]
    if any(isinstance(k, bool) or not isinstance(k, int) for k in entries):
        raise ValueError(f"{name}: expected an int or a list of ints (one per #minimize priority level), got {value!r}")
    if non_negative and any(k < 0 for k in entries):
        raise ValueError(f"{name}: expected non-negative entries, got {value!r}")
    return entries


def compile(
    program: Program,
    method: str = "scc",
    order: Union[str, Sequence[int]] = "name",
    project: Union[bool, str] = True,
    optimize: bool = False,
    external_values: bool = False,
    dds: Iterable = (),
    node_max: Optional[int] = None,
    cost_le: Union[int, Sequence[int], None] = None,
    cost_ge: Union[int, Sequence[int], None] = None,
    cost_eq: Union[int, Sequence[int], None] = None,
    cost_slack: Union[int, Sequence[int], None] = None,
) -> Result:
    """Compile a :class:`Program`.

    :param method: ``"scc"`` (default) or ``"twocopy"``.
    :param order: ``"name"`` (default), ``"id"``, or a list of atom ids to place on top
                  (the remaining atoms follow in name order).
    :param project: ``True`` (default): project the answer sets onto the shown atoms;
                    ``False``: every atom becomes a DD variable; ``"project"``: project
                    onto the atoms of the ``#project`` statements instead, when the
                    program has any (like clingo's ``--project``; projected atoms
                    without a ``#show`` name are named ``__atom<id>``, and ``#show``
                    terms whose condition leaves the projection are dropped with a
                    ``RuntimeWarning``).
    :param optimize: keep only the optimal answer sets (#minimize). Weights must fit
                     in 32 bits (``ValueError`` otherwise); note that clingo itself
                     truncates larger weights to 32 bits while grounding.
    :param external_values: fix every free ``#external`` atom to its declared initial
                            value (``#external a. [true]`` / ``[false]``; clingo declares
                            a plain ``#external a.`` as false); by default the initial
                            values are ignored and every external atom is free.
    :param dds: stored DDs to load (:class:`StoredDD`, :class:`Result`, file path or
                bytes; one of them or a list). The program is compiled as it is: a
                stored atom it uses must be ``#external`` and named in it
                (:func:`compile_control` and friends add the declarations before
                grounding; stored *facts* are not added here); the other stored
                atoms are added as free, named atoms.
    :param node_max: limit on the number of nodes of KyotoDD's node table during this
                     compilation; :class:`NodeLimitError` is raised when the diagrams
                     need more. ``None`` (default): unlimited (a huge intermediate
                     diagram then uses all the memory of the machine).
    :param cost_le: keep only the answer sets whose ``#minimize`` cost vector (one cost
                    per priority level, most important first, as in :attr:`Result.optimum`)
                    is lexicographically at most this bound: an int bounds the first
                    level, a list bounds the first ``len(list)`` levels (the lower levels
                    stay free), like clingo's ``--opt-mode=enum,K,...``. Like ``optimize``,
                    the bound applies before projection.
    :param cost_ge: keep only the answer sets whose cost vector is at least this bound
                    (same form; combines with ``cost_le`` or ``cost_slack``).
    :param cost_eq: keep only the answer sets whose cost vector equals this bound on the
                    levels it lists (same form; not with the other bounds).
    :param cost_slack: an upper bound relative to the optimum: ``d`` or ``[d1, d2, ...]``
                       (non-negative ints) keeps the answer sets whose cost vector is at
                       most the optimum plus the slack, level by level, and fills
                       :attr:`Result.optimum`. ``cost_slack=0`` on every level is
                       ``optimize=True``. Not with ``cost_le`` or ``optimize``.
    """
    if isinstance(order, str):
        heuristic, order_atoms = order, []
    else:
        heuristic, order_atoms = "name", list(order)
    loaded = [dd.data for dd in _stored_list(dds)]
    if project == "project":
        project, use_project = True, True
    elif isinstance(project, bool):
        use_project = False
    else:
        raise ValueError(f"project: expected True, False or 'project', got {project!r}")
    if node_max is not None and (not isinstance(node_max, int) or node_max <= 0):
        raise ValueError(f"node_max: expected a positive integer or None, got {node_max!r}")
    le, ge, eq = _cost_bound("cost_le", cost_le), _cost_bound("cost_ge", cost_ge), _cost_bound("cost_eq", cost_eq)
    slack = _cost_bound("cost_slack", cost_slack, non_negative=True)
    if optimize and (le or ge or eq or slack):
        raise ValueError("optimize=True cannot be combined with a cost bound (cost_slack=0 keeps the optimal answer sets)")
    if le and slack:
        raise ValueError("cost_le and cost_slack cannot be combined (both are upper bounds)")
    if eq and (le or ge or slack):
        raise ValueError("cost_eq cannot be combined with cost_le, cost_ge or cost_slack")
    return Result(_core_compile(program, method, heuristic, order_atoms, project, use_project, optimize,
                                external_values, loaded, node_max or 0, cost_le=le, cost_ge=ge, cost_eq=eq,
                                cost_slack=slack))


_used_controls = weakref.WeakSet()  # controls compile_control has grounded


def compile_control(control: clingo.Control, parts=(("base", []),), dds: Iterable = (), **kwargs) -> Result:
    """Ground ``parts`` of a ``clingo.Control`` and compile the ground program.

    The observer is registered here, so pass a control whose ``parts`` have not
    been grounded yet (rules grounded earlier are not seen). A control can be
    passed once: grounding it again does not reproduce the ground program, so a
    second call raises ``ValueError``. To compile the same program several
    times, reuse the :class:`Program` of an :class:`Observer` with
    :func:`compile` instead.

    The declarations of the stored DDs in ``dds`` are added as the program part
    ``ddasp_loaded`` and grounded together with ``parts``.
    """
    if control in _used_controls:
        raise ValueError("compile_control grounds a clingo.Control once; pass a new control "
                         "(or reuse the Program of an Observer with compile())")
    _used_controls.add(control)
    dds = _stored_list(dds)
    parts = list(parts)
    if dds:
        control.add("ddasp_loaded", [], "".join(dd.declarations() for dd in dds))
        parts.append(("ddasp_loaded", []))
    observer = Observer()
    control.register_observer(observer)
    control.ground(parts)
    return compile(observer.program, dds=dds, **kwargs)



def compile_text(text: str, clingo_args: Iterable[str] = (), **kwargs) -> Result:
    """Ground and compile an ASP program given as text."""
    control = clingo.Control(list(clingo_args))
    control.add("base", [], text)
    return compile_control(control, **kwargs)


def compile_files(files: Iterable[str], clingo_args: Iterable[str] = (), **kwargs) -> Result:
    """Ground and compile ASP program files."""
    control = clingo.Control(list(clingo_args))
    for f in files:
        control.load(str(f))
    return compile_control(control, **kwargs)
