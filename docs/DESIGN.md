# DDASP design notes

DDASP converts a ground ASP program (produced by gringo) into a decision diagram
that represents the family of all answer sets. The DD is built with KyotoDD.

## Pipeline

```
prog.lp --gringo--> aspif text --AspifReader--> Program --Compiler--> BDD --> ZDD
                    (clingo Observer in Python builds the same Program)
```

## Program representation (`include/ddasp/program.h`)

- `Atom` is a positive integer ID (gringo numbering).
- `Literal` is a signed `int`: `+a` is the atom, `-a` is `not a` (same as aspif).
- A `Rule` has a head (`Disjunction` or `Choice`, possibly empty = constraint) and a
  body that is either a conjunction of literals (`Normal`) or a weight body
  `sum(w_i * [lit_i]) >= bound` (`Weight`). A normal body is stored as a weight body
  with all weights 1 and bound = number of literals, with the `kind` flag preserved
  so that output and tests can distinguish them.
- `Minimize { priority, [(literal, weight)] }`, `Output { name, condition }`,
  `externals` (atoms that have no defining rule and are left free).

## Stable model semantics: two-copy method (reference implementation, `--method twocopy`)

Models(X) ∧ "X has no non-empty unfounded set" with a single group that
contains every atom: one auxiliary variable `u_a` per atom (placed directly
above `x_a`), one quantification over all of them. It is the same code path as
the SCC method below with a single group, kept as the reference to compare
against. (The original plan, a second copy `y_i` of every atom with
`Y ⊨ P^X`, was replaced by the unfounded-set formulation before the first
release; `u_a` plays the role of `a ∈ X \ Y`.)

- `Models(X)`: every rule satisfies `body(X) → head(X)`; choice rules are always true.
- Weight bodies `Σ w·[lit] ≥ bound` are threshold BDDs built by dynamic
  programming over the literal list (`threshold_bdd`), processing the
  literals from the top level down.
- `#external a` is treated as the choice rule `{a}.`; its initial value
  (true/false/free/release) is ignored, the atom is always free.
- `#project` statements are read but ignored.

## Stable model semantics: SCC-local unfounded sets (default, `--method scc`)

Both methods are implemented by one compiler (`src/compiler.cpp`) in terms of
*unfounded sets*: with auxiliary variables `u_a` ("a ∈ U") and Y = X \ U,

```
Stable(X) = Models(X) ∧ ¬∃U (U ≠ ∅ ∧ U ⊆ X ∧ U is unfounded w.r.t. X)
```

A rule r does not support U when its body is false once the positive occurrences
of U's atoms are made false, or no head atom is in U, or a head atom outside U is
in X (Leone, Rullo, Scarcello). The two-copy method uses a single group U ⊆ all atoms.

Any non-empty unfounded set contains a non-empty unfounded set lying inside one
strongly connected component of the positive dependency graph (edges from positive
body atoms to head atoms; take the lowest SCC that meets U). The SCC method therefore

- adds `Supported(X)` for every atom (the singleton unfounded sets, no auxiliary
  variables needed), and
- quantifies one unfounded-set condition per non-trivial SCC, with `u` variables
  only for the atoms of such SCCs. Tight programs need no auxiliary variable at all.

This is exact for disjunctive, choice and weight rules alike. An iterative
"find a non-stable model, add its loop formula" scheme was considered and
rejected: deciding whether any non-stable model remains in the BDD is itself the
symbolic unfounded-set check, and checking models one by one amounts to
enumeration.

## Variable order and conjunction order

- `--order name` (default): shown atoms sorted by name with numeric-aware
  comparison (`q(1,2) < q(1,10)`), unnamed atoms placed at the barycenter of the
  atoms they share rules with (two refinement passes). `--order id` keeps
  gringo's numbering. `CompileOptions::order` lists atoms to put on top.
- All per-rule and per-atom constraints are conjoined in increasing order of
  their top level (`conjoin_sorted`). On 8-queens this plus the name order took
  the compilation from 27 s / 34M peak nodes to 0.02 s / 92k peak nodes.

## #show projection (default on)

The outputs of the ground program are split into

- *fact names*: outputs with an empty condition (how gringo emits facts),
  printed with every answer set;
- the *primary name* of an atom: the first output whose condition is exactly
  that atom (positive); it names the DD variable (`--atoms`, order files);
- *terms* (`CompileResult::terms`): every other output, e.g. a second name of
  an atom (`#show t : a.` when `a` is shown) or a negated condition
  (`#show t : not a.`). A term is printed when all literals of its condition
  hold. gringo rewrites compound conditions (`#show t : a, b.`) into an
  auxiliary atom, so conditions are single literals in practice.

The *shown atoms* are the named atoms plus the atoms of term conditions. With
projection, the hidden atoms are removed from the family with `ZDD::project`,
so answer sets that differ only in hidden atoms collapse, and the terms can
still be evaluated on the projected sets. `CompileResult::shown_names()`
produces the printed names of one answer set (deduplicated, like clingo's
symbol set). `--all-atoms` keeps every atom as a DD variable.

## #minimize

Each priority level becomes a linear cost over DD variables
(`CostFunction`: offset + Σ weight·[atom ∈ X]; `not a` with weight w gives
offset +w and weight −w on a). Costs are computed before projection. With
`--opt`, levels are processed from the most important priority down, each time
keeping the sets with `cost == min_weight` via `cost_bound_le`.

## Conversions and deep diagrams

KyotoDD's `BDD::to_zdd` / `ZDD::to_bdd` work on levels, and the binary export
format records levels too. Because the `u` variables are interleaved with the
`x` variables, DDASP has its own `bdd_to_zdd_over` / `zdd_to_bdd_over`
(`include/ddasp/dd_util.h`) that convert over an explicit variable set. The
`--atoms` table therefore lists the level of each atom: after importing the
binary file in a fresh process, variable numbers equal levels.

Programs with tens of thousands of atoms give diagrams with as many levels.
DDASP's own traversals (`dd_util`: conversions, threshold builder, set
enumeration) use explicit stacks. KyotoDD switches to iterative
implementations above 8192 levels for most operations; DDASP quantifies with
a variable-set BDD (`variable_set`) because `BDD::exist(std::vector)` always
takes the recursive path, and avoids `ZDD::card()` (recursive) in favour of
`exact_count()`.

## Stored DDs (`--save` / `--load`)

A *stored DD* (`include/ddasp/stored_dd.h`) is the answer-set family of a
compilation in one file: a text header with the atom names (top of the
diagram first) and the fact names, followed by the KyotoDD binary of the ZDD.
Before the export the family is renamed onto levels 1..N (`rename_zdd`, a
linear bottom-up rebuild with `getnode` when the renaming keeps the level
order), so importing it needs exactly N variables; the terminal families are
written as `zdd empty` / `zdd single` because KyotoDD's export of a terminal
records every variable of the process. Every stored variable must have a
primary name, names must be distinct and writable as atoms; terms are dropped
with a warning, costs are not stored.

Loading a stored DD D into the compilation of a program P is defined as
adding to P: `#external n.` and `#show n : n.` for every atom name n of D
(the term show keeps the atom named whatever P's `#show` statements are; it
does not change the visibility of P's other atoms), the fact `f.` for every
fact name f of D, and the constraint "X restricted to the atoms of D is a set
of D". The CLI and `compile_control` ground these declarations together with
P (a temporary file / the part `ddasp_loaded`); `compile()` on a ground
`Program` matches the stored names with P's outputs (every output whose
condition is a single positive atom counts) and adds the atoms P does not
mention as fresh named `#external` atoms, so they stay free and visible.

Correctness follows from the splitting-set theorem: D is AS(P_old) projected
onto its shown atoms, P may use those atoms only in bodies (an atom of D in a
rule head or as a fact of P is an error, as are ambiguous names, two stored
atoms on one atom of P, and a name that is a fact on one side and an atom on
the other), and an integrity constraint over x variables does not change
supportedness or unfoundedness, so `Stable(P ∪ D) = Stable(P) ∧ D`. The
stored ZDD is renamed onto P's x variables and conjoined with the models
before the unfounded-set check (it prunes the intermediate BDDs); the result
is AS(P_old ∪ P) projected onto atoms(D) ∪ shown(P). Hidden atoms of P_old are
invisible to P, and P's `#external` values are ignored as always.

Variable order: the atoms of each loaded DD keep their stored relative order
among the positions the heuristic gives them (`apply_spines`, applied before
the barycenter placement of unnamed atoms; earlier DDs win on shared atoms,
explicit `--order` atoms are placed first regardless), so the renaming is
linear; any other order is still correct, only slower.

## Limits

- KyotoDD supports at most 65536 BDD variables. Every atom of the ground
  program needs one (also with `#show` projection, which happens after the
  stable models are built) and every atom in a non-trivial SCC needs a
  second one; the compiler reports a clear error beyond that. The atoms of
  loaded DDs count too.
- KyotoDD refuses to import diagrams with more than 10^7 nodes or 10^6
  levels, so such a family can be saved but not loaded.
- There is no node limit: `bddinit` is called with an unbounded table, so a
  huge intermediate BDD (e.g. a `#count` over 60000 atoms with a bound of
  30000, O(n·k) nodes) uses all the memory of the machine.

## Decisions log

- 2026-10-20: language scope = everything gringo emits (normal/choice/constraint/
  disjunction/weight bodies/#minimize/#external). Grounding is delegated to gringo.
- 2026-10-20: two-copy method first (reference), loop formulas second (speed).
- 2026-10-20: general disjunction is supported (two-copy method does not need HCF).
- 2026-10-20: #show projection is optional (default: shown atoms; flag for all atoms).
- 2026-10-20: #minimize: both "keep only optimal answer sets" and "return costs" modes.
- 2026-10-20: #external atoms are kept as free variables.
- 2026-10-20: C++ core + Python frontend (pybind11); ZDDs cross to the `kyotodd`
  Python module as `export_binary` bytes because each extension owns its own node table.
- 2026-10-20: correctness is checked against clingo's enumeration (clingo in `.venv`).
- 2026-10-20: the planned iterative loop-formula method was replaced by the SCC-local
  unfounded-set method (see above); the user asked for a fast method, and the iterative
  scheme cannot decide symbolically when all remaining models are stable.
- 2026-10-20: default variable order = natural name order with barycenter placement of
  unnamed atoms; constraints are conjoined in order of their top level.
- 2026-10-20 (review round 1): every `#show` output is honoured (extra names, negated
  conditions) by keeping the condition atoms as shown atoms; all DDASP traversals are
  non-recursive; `--enumerate` streams from the ZDD.
- 2026-10-21: stored DDs (`--save` / `--load`, `Result.save()` / `dds=`): one file with
  the ZDD and the names; loading = declaring every stored atom `#external` (so the new
  program uses them without declarations and they always stay visible) plus the family
  as a constraint; atoms the new program defines are errors; stored order is kept in
  the variable order; costs and terms are not stored.
