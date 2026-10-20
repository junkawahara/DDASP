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

import warnings
from typing import Iterable, Iterator, List, Optional, Sequence, Tuple, Union

import clingo
from clingo.backend import Observer as _ClingoObserver

from ddasp._core import CoreResult, Program, read_aspif
from ddasp._core import compile as _core_compile

__all__ = [
    "Program",
    "Observer",
    "Result",
    "read_aspif",
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
        self._ignored("assumption")

    def heuristic(self, atom: int, type_, bias: int, priority: int, condition: Sequence[int]) -> None:
        self._ignored("heuristic")

    def acyc_edge(self, node_u: int, node_v: int, condition: Sequence[int]) -> None:
        self._ignored("edge")

    def theory_atom(self, atom_id_or_zero: int, term_id: int, elements: Sequence[int]) -> None:
        self._ignored("theory")

    def theory_atom_with_guard(self, atom_id_or_zero: int, term_id: int, elements: Sequence[int],
                               operator_id: int, right_hand_side_id: int) -> None:
        self._ignored("theory")


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
        return list(self._core.optimum)

    @property
    def count(self) -> int:
        """Number of answer sets (exact)."""
        return int(self._core.count)

    @property
    def stats(self) -> dict:
        return dict(self._core.stats)

    def atom_name(self, atom: int) -> str:
        return self._names.get(atom, f"__atom{atom}")

    def level_name(self, level: int) -> str:
        return self.atom_name(self._level_to_atom[level])

    # --- kyotodd objects --------------------------------------------------
    @property
    def zdd(self):
        """The family of answer sets as a ``kyotodd.ZDD`` (imports lazily)."""
        if self._zdd is None:
            import kyotodd

            self._zdd = kyotodd.ZDD.import_binary_str(self._core.zdd_bytes)
        return self._zdd

    @property
    def bdd(self):
        """The characteristic function of the answer sets as a ``kyotodd.BDD``."""
        if self._bdd is None:
            import kyotodd

            self._bdd = kyotodd.BDD.import_binary_str(self._core.bdd_bytes)
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


def compile(
    program: Program,
    method: str = "scc",
    order: Union[str, Sequence[int]] = "name",
    project: bool = True,
    optimize: bool = False,
) -> Result:
    """Compile a :class:`Program`.

    :param method: ``"scc"`` (default) or ``"twocopy"``.
    :param order: ``"name"`` (default), ``"id"``, or a list of atom ids to place on top
                  (the remaining atoms follow in name order).
    :param project: project the answer sets onto the shown atoms.
    :param optimize: keep only the optimal answer sets (#minimize).
    """
    if isinstance(order, str):
        heuristic, order_atoms = order, []
    else:
        heuristic, order_atoms = "name", list(order)
    return Result(_core_compile(program, method, heuristic, order_atoms, project, optimize))


def compile_control(control: clingo.Control, parts=(("base", []),), **kwargs) -> Result:
    """Ground ``parts`` of a ``clingo.Control`` and compile the ground program.

    The observer is registered here, so pass a control whose ``parts`` have not
    been grounded yet (rules grounded earlier are not seen). Each call grounds
    and registers again; to compile the same program several times, reuse the
    :class:`Program` of an :class:`Observer` with :func:`compile` instead.
    """
    observer = Observer()
    control.register_observer(observer)
    control.ground(list(parts))
    return compile(observer.program, **kwargs)


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
