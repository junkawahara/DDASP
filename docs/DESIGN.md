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
  with all weights 1 and bound = number of literals, with `Body::type` preserved
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
  (true/false/free/release) is ignored by default, the atom is always free.
  With `CompileOptions::external_values` (`--external-values`) a free
  external declared true or false is fixed to that value by a unit
  constraint (the last declaration of an atom wins; gringo declares a plain
  `#external a.` as false, `[true]`/`[false]` as written; free and released
  atoms stay free). When a rule has `a` in the head, the declaration is
  ignored (as in clingo, where such an atom is no longer external):
  `#external e. {a}. e :- a.` has the answer sets `{}` and `{a, e}` only.
  Tautological rules do not count: before compiling, `Program::without_tautologies`
  drops the rules whose body contains an atom and its negation and the
  disjunctive rules with a head atom in the positive body, and removes a head
  atom that occurs in the body of a choice rule or in the negative body of a
  disjunctive rule (`a | H :- B, not a` becomes `H :- B, not a`). These
  transformations preserve strong equivalence, so the answer sets are the same,
  and an external atom whose rules are all tautologies stays free, as in
  clingo, which decides after clasp's simplifications. clasp goes further:
  `:- g. e :- g.` leaves `e` external there because propagation removes the
  rule (while `:- g, k. :- g, not k. e :- g.` does not); DDASP does not follow
  that, `e` is defined by `e :- g` and false.
- Assumptions (aspif `6` statements, `Observer.assume`) are unit integrity
  constraints: every answer set satisfies each assumed literal. An assumed
  atom of no rule is false, so assuming it leaves no answer set. They are
  conjoined before every other constraint (`Part::first`), like loaded DDs.
- `#project` statements are read and used only with
  `CompileOptions::use_project_statements` (`--project`), see below.

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

## Semantics other than stable (`--semantics`)

`CompileOptions::semantics` (`--semantics`, Python `semantics=`) selects the
family: `stable` (default) as above; `supported`, the models of Clark's
completion, built as Models(X) ∧ Supported(X) without any unfounded-set
quantification (no `u` variables, `groups: 0`); `classical`, Models(X) alone,
the rules read as implications with choice rules imposing nothing.
Supported(X) is the Clark support: a true atom needs a rule with it in the
head whose body is true and whose other head atoms are false, and free atoms
(#external without rules, loaded) need nothing; it is the constraint the SCC
method already uses. The tautological rules are removed for the stable
semantics only (`a :- a.` supports `a` under completion). Projection,
`#minimize`, consequences and stored DDs work on the chosen family. Every
semantics refers to the ground program as gringo emits it: its auxiliary
atoms (aggregates, `not not`) are ordinary atoms there, so the supported and
classical families can differ from a reading of the source program
(`a :- #count{...} >= 1` becomes `aux :- 1 {...}` and `a :- aux`, and
classically `aux` may be true without the count). The method option applies
to the stable semantics only. `python/tests/test_semantics.py` checks both
families against a brute-force oracle on random programs.

## Variable order and conjunction order

- `--order name` (default): shown atoms sorted by name with numeric-aware
  comparison (`q(1,2) < q(1,10)`), unnamed atoms placed at the barycenter of the
  atoms they share rules with (two refinement passes). `--order id` keeps
  gringo's numbering. `CompileOptions::order` lists atoms to put on top.
- All per-rule and per-atom constraints are conjoined definitions first
  (`conjoin_sorted`): a constraint that mentions an atom waits until every
  constraint defining that atom (the rules with it in the head, its support
  constraint) has been conjoined, and among the constraints that are ready the
  one with the lowest top level goes first. Dependencies are taken across the
  strongly connected components of the full (positive and negative) dependency
  graph only, so cyclic definitions never block, and choice / external atoms
  have no defining constraints. Without the dependency rule, an intermediate
  result had to carry the values of atoms whose definitions were still
  missing: `size(S) :- S = #count { V : in(V) }` over 16 shown atoms took 11M
  nodes and 20 s (the `size ↔ aux` constraints came first because their top
  levels were lowest), now 70k nodes and 0.03 s. The top-level rule alone
  already took 8-queens from 27 s / 34M peak nodes to 0.02 s / 92k, and the
  dependency rule leaves that unchanged. The constraints of loaded DDs go
  before every other ready constraint whatever their top level: they are
  strong constraints over free atoms, so everything else is built inside the
  loaded family (a `#count` over 16 loaded atoms: 858k peak nodes -> 51k).

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

The *shown atoms* are the named atoms plus the atoms of term conditions. An
atom that occurs in no rule and no `#external` statement (gringo gives
`#show w : f.` with `f` a fact the condition `not __atomN` on such an atom) is
false in every answer set: it gets no DD variable (`program_atoms`), its
literals in conditions and `#minimize` statements are constants. With
projection, the hidden atoms are removed from the family with `ZDD::project`,
so answer sets that differ only in hidden atoms collapse, and the terms can
still be evaluated on the projected sets. `CompileResult::shown_names()`
produces the printed names of one answer set (deduplicated, like clingo's
symbol set). `--all-atoms` keeps every atom as a DD variable.

With `use_project_statements` (`--project`, Python `project="project"`) and a
program that has `#project` statements, the shown atoms are replaced by the
`#project` atoms (those with a DD variable; the others are false): they are
the variables of the result, named by their primary `#show` name or
`__atom<id>` (entered into `names` so that they are displayed), the fact
names stay, and a term whose condition mentions an atom outside the
projection (other than a constant atom of no rule) is dropped with a message
in `CompileResult::warnings`. This is the family clingo's `--project`
enumerates one representative of. Without `#project` statements the option
changes nothing; it cannot be combined with `--all-atoms`.

## Consequences and frequencies (`include/ddasp/consequences.h`)

`name_frequencies` gives, for every displayed name in display order (fact
names, atom names in variable order, terms), the number of answer sets of
the result that display it. `zdd_variable_counts` (`dd_util`) computes the
count of every variable in one iterative pass over the ZDD: the nodes are
collected top-down (memoized by node id, which includes KyotoDD's complement
flag), each node gets the number of sets below it and the number of paths
from the root, and a variable's count is the sum over its nodes of
paths × sets below the 1-child. A name shown by one single-literal output
is read off these counts (`not a`: total − count of a); a name shown by
several outputs or by a compound condition is counted on the family
restricted with `onset`/`change`/`offset` and united over its outputs, so
`#show t : a. #show t : b.` counts the sets containing a or b. Fact names
count every answer set. The brave consequences are the names with a positive
count, the cautious consequences those whose count equals the number of
answer sets (none for an inconsistent program). The counts refer to the
projected family (`--all-atoms` for all answer sets). The Python binding
computes them with every compilation (`Result.frequencies/brave/cautious`).

## #minimize

Each priority level becomes a linear cost over DD variables
(`CostFunction`: offset + Σ weight·[atom ∈ X]; `not a` with weight w gives
offset +w and weight −w on a). Costs are computed before projection. With
`--opt`, levels are processed from the most important priority down, each time
keeping the sets with `cost == min_weight` via `cost_bound_le`; KyotoDD's
weights are 32-bit, so a weight outside that range is an error when
optimizing (cost functions themselves are 64-bit). clingo truncates weights
to 32 bits before DDASP sees them (its observer and gringo's aspif output), so
`#minimize { 3000000000 : a }` arrives as -1294967296 without a warning.

Cost bounds (`--cost-le` / `--cost-ge` / `--cost-eq K,...`,
`CompileOptions::cost_le/cost_ge/cost_eq`, Python `cost_le=` etc.) keep the
sets whose cost vector is lexicographically ≤ / ≥ / = the bound on the levels
the bound lists, like clingo's `--opt-mode=enum,K,...`: for ≤ the bounded
levels are processed from the last one up with
`acc = {cost < b} ∪ ({cost = b} ∩ acc)`, where `{cost < b}` and `{cost ≤ b}`
are two `cost_bound_le` calls sharing a `CostBoundMemo` (≥ is symmetric with
`cost_bound_ge`); = chains `cost_bound_eq` over the levels. A relative entry
`+d` (Python `cost_slack`) is the optimum of its level, computed as `--opt`
does, plus d, and the optimum is then reported. The bounds apply before
projection like `--opt`; `--opt` excludes them, `--cost-eq` excludes the other
two, and a bound with more entries than priority levels (or without any
`#minimize`) is rejected before compiling.

The cost histogram (`--cost-histogram`, `CompileOptions::cost_histogram`,
Python `cost_histogram=True`) lists the number of answer sets per cost vector.
Under projection each projected set counts once, at the lexicographically
smallest cost vector of the answer sets projecting to it (the reading of the
bounds), so the counts add up to the number of answer sets and the cumulative
count at a vector is the count of `--cost-le` there. It is computed before
projection by peeling the family: the lexicographic optimum of what is left
(as `--opt` finds it) is projected and united with the projected sets seen so
far, the growth of that union is the count, and the optimal sets are removed.
This costs a few `cost_bound` passes and one projection per distinct vector
but needs no per-node cost tables (a node-wise convolution would take
nodes × distinct vectors memory and could not count projected sets).

Cost-ordered enumeration (`--by-cost`, `enumerate_by_cost` in
`include/ddasp/cost_order.h`, Python `answer_sets(by_cost=True)` and
`compile(best=K)`) visits the projected answer sets best first with the same
reading: once each, at the smallest cost vector of their answer sets. With one
priority level and no weighted atom hidden by projection it runs KyotoDD's
`ZddMinWeightIterator` (Yen's k-best algorithm) on the result's ZDD; otherwise
it peels the family before projection like the histogram, enumerating the
projected sets of each cost class that were not seen before. `CompileResult`
keeps the unprojected family (`family`) for this. The Python generator works on
the imported, projected ZDD with kyotodd's `iter_min_weight` / `cost_bound_le`,
so it needs every weighted atom to be a variable of the result; `compile(best=K)`
collects the K best in C++ instead, hidden weights included.

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
implementations above 8192 levels (KyotoDD a4d60be or later is required: older
versions quantified `BDD::exist(std::vector)` recursively). DDASP avoids
`ZDD::card()` (recursive only) in favour of `exact_count()`.

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
mention as fresh named `#external` atoms, so they stay free and visible. The
fact names of D are only appended to the result there: a ground `Program`
that uses a stored fact in a body sees it as an atom of no rule (false)
unless the caller added the fact itself, as the CLI and `compile_control` do.

Correctness follows from the splitting-set theorem: D is AS(P_old) projected
onto its shown atoms, P may use those atoms only in bodies (an atom of D in a
rule head or as a fact of P is an error, as are ambiguous names, two stored
atoms on one atom of P, and a name that is a fact on one side and an atom on
the other), and an integrity constraint over x variables does not change
supportedness or unfoundedness, so `Stable(P ∪ D) = Stable(P) ∧ D`. The
loaded atoms are free in the compilation whatever rules mention them. One
head occurrence is allowed: when gringo rewrites a recursive aggregate with
`!=` or negative weights, it emits `aux | a :- B` together with `aux :- not a`
for an atom `a` of the aggregate (`aux` unnamed); given the second rule the
first can never support `a`, so it is accepted (`defined_atoms` in
`src/compiler.cpp`). The
stored ZDD is renamed onto P's x variables and conjoined with the models
before the unfounded-set check (it prunes the intermediate BDDs); the result
is AS(P_old ∪ P) projected onto atoms(D) ∪ shown(P). Hidden atoms of P_old are
invisible to P, and P's `#external` values are ignored as always.

`write_stored_dd_data` writes a stored DD from its parts (names, fact names,
ZDD bytes or a terminal marker) with the same validation, so diagrams that did
not come from a compilation can be stored. The Python `StoredDD.from_zdd(zdd,
names)` uses it for a `kyotodd.ZDD` (the kyotodd module's node table is
separate from the extension's, so the ZDD crosses over as its binary export):
the named variables must sit at levels 1..N of the user's instance and every
variable of the ZDD must be named; the names are written top of the diagram
first. Examples: `examples/shift_dd.py` + `examples/shift.lp` (a KyotoDD-built
family as an ASP constraint) and `examples/independent_set_maximal.lp` (a
second ASP step on a stored DD).

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
- The node table is unbounded by default, so a huge intermediate BDD (e.g. a
  `#count` over 60000 atoms with a bound of 30000, O(n·k) nodes) uses all the
  memory of the machine. `--node-max N` (Python `node_max=N`) passes the
  limit to `bddinit`; KyotoDD then throws "node table exhausted", which the
  CLI reports as the limit being reached and Python raises as
  `ddasp.NodeLimitError` (the instance is reset for the next compilation).

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
- 2026-10-22: `StoredDD.from_zdd` / `write_stored_dd_data`: a DD built directly with
  KyotoDD becomes a stored DD and so a constraint of an ASP program (guide chapter 12).
- 2026-10-22: constraints are conjoined definitions first (dependency order across SCCs,
  lowest top level among the ready ones); fixes the blow-up of value-rich aggregates
  (`#count` into a derived atom) over many shown or stored atoms.
- 2026-10-22: assumptions are constraints (always); `#external` initial values are used
  only with `--external-values` (gringo declares a plain `#external a.` as false);
  `#project` only with `--project`, which replaces the shown atoms by the `#project`
  atoms (unnamed ones displayed as `__atomN`, terms leaving the projection dropped with
  a warning); brave/cautious consequences and per-name frequencies are read off the ZDD
  and printed as bare lines; `--node-max` bounds the node table, unlimited by default.
- 2026-10-22: cost bounds `--cost-le` / `--cost-ge` / `--cost-eq` (Python `cost_le=` /
  `cost_ge=` / `cost_eq=`, relative upper bound `cost_slack=`): the user chose the
  lexicographic comparison of clingo's `--opt-mode=enum,K,...` for several priority levels
  (verified against clingo), relative entries `+d` on top of absolute ones, and the lower
  and equality bounds alongside the upper one; `--opt` is not combined with them.
- 2026-10-22: `--cost-histogram` / `cost_histogram=True`: the joint distribution over cost
  vectors (not per-level marginals); projected sets counted once at their smallest cost
  (consistent with the bounds and `answer sets:`); bare `K[,K..] count` lines without
  zero counts; Python dict with tuple keys in ascending order, `None` unless requested.
- 2026-10-22: `--semantics stable|supported|classical` (Python `semantics=`): the user chose Clark's
  completion for `supported` (`a :- a.` supports a, so the tautologies are kept there); summary
  line `supported models:` / `classical models:`; the method applies to stable only.
- 2026-10-22: `--json` prints one JSON object with the keys of the Python `Result` (`count`,
  `optimum`, `brave`, `cautious`, `frequencies`, `cost_histogram`, `stats`, `answer_sets`), only
  the requested items, the answer sets streamed into an array (`{cost, atoms}` objects with
  `--by-cost`) and counts as JSON integers; the user chose this over clingo's `--outf=2` layout
  and JSON Lines.
- 2026-10-22: `-c NAME=VALUE` (CLI only; Python passes `clingo_args=["-c", "n=8"]`) is forwarded
  to the grounder as gringo's `-c` and rejected with aspif input (already ground).
- 2026-10-22: `python/tests/test_random_programs.py` checks ddasp against clingo on a seeded
  stream of random propositional programs (`DDASP_RANDOM_PROGRAMS`, default 500; both methods ×
  projection × optimization). It found that clingo keeps an `#external` atom free when every
  rule with it in the head is a tautology (`a :- a, B`, a body with `l` and `not l`, `a :- B,
  not a`) or is removed by clasp's propagation (`:- g. a :- g.`). The user chose to drop the
  syntactic tautologies before compiling (`Program::without_tautologies`, answer sets unchanged)
  so that such externals stay free as in clingo, and not to follow clasp's propagation; the
  random programs therefore put their `#external` atoms in no rule head.
- 2026-10-22: cost-ordered enumeration uses both methods (KyotoDD's k-best iterator when
  it applies, peeling otherwise); lines are `K[,K..]: atoms`; `--by-cost` implies
  `--enumerate`; Python offers the lazy `answer_sets(by_cost=True)` (weighted atoms must
  be shown) and `compile(best=K)` (computed in C++, hidden weights fine).
