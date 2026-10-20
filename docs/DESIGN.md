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

## Stable model semantics: iterative loop formulas (fast path, later milestone)

Start from the Clark completion BDD, repeatedly extract a non-stable model, compute an
unfounded set, conjoin its loop formula, until no non-stable model remains.

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
