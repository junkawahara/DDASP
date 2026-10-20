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

## Stable model semantics: two-copy method (reference implementation)

For every atom `a_i` the compiler allocates two BDD variables: `x_i` (candidate set X)
and `y_i` (subset Y). The levels interleave `x_i, y_i` so that `Y ⊆ X` stays small.

```
Stable(X) = Models(X) ∧ ¬∃Y [ Y ⊆ X ∧ Y ≠ X ∧ Y ⊨ P^X ]
```

- `Models(X)`: every rule satisfies `body(X) → head(X)`; choice rules are always true.
- `Y ⊨ P^X` (Y is a model of the reduct of P under X), per rule `H :- B+, not B-`:
  - disjunctive head: `(B- ∩ X = ∅) ∧ (B+ ⊆ Y) → (H ∩ Y ≠ ∅)`
  - choice head, for each `a ∈ H`: `(B- ∩ X = ∅) ∧ (B+ ⊆ Y) ∧ (a ∈ X) → (a ∈ Y)`
  - weight body: `Σ_pos w·[a ∈ Y] + Σ_neg w·[b ∉ X] ≥ bound`, built by dynamic
    programming over the literal list (threshold BDD).
- `#external a` is treated as the choice rule `{a}.`
- Finally the Y variables are existentially quantified and the BDD is converted to a ZDD
  over the atom variables.

This handles normal, choice, constraint, disjunctive (including non-HCF) rules and
weight bodies uniformly, at the cost of doubling the number of BDD variables.

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

Atoms named by a `#show` output statement whose condition is a single positive
atom are "shown". Output statements with an empty condition (how gringo emits
facts) are names printed with every answer set. With projection, the hidden
atoms are removed from the family with `ZDD::project`, so answer sets that
differ only in hidden atoms collapse. `--all-atoms` keeps every atom as a DD
variable. A `#show` with a compound condition is an error while projecting.

## #minimize

Each priority level becomes a linear cost over DD variables
(`CostFunction`: offset + Σ weight·[atom ∈ X]; `not a` with weight w gives
offset +w and weight −w on a). Costs are computed before projection. With
`--opt`, levels are processed from the most important priority down, each time
keeping the sets with `cost == min_weight` via `cost_bound_le`.

## Conversions

KyotoDD's `BDD::to_zdd` / `ZDD::to_bdd` work on levels, and the binary export
format records levels too. Because the y variables are interleaved with the x
variables, DDASP has its own `bdd_to_zdd_over` / `zdd_to_bdd_over`
(`include/ddasp/dd_util.h`) that convert over an explicit variable set. The
`--atoms` table therefore lists the level of each atom: after importing the
binary file in a fresh process, variable numbers equal levels.

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
