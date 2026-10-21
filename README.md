# DDASP — ASP programs as decision diagrams

DDASP compiles an ASP (Answer Set Programming) program into a **ZDD** (or BDD)
that represents the family of *all* its answer sets, using the
[KyotoDD](https://github.com/graphillion/kyotodd) library. Grounding is delegated to gringo/clingo.

Supported: normal, choice, constraint and disjunctive rules (including non
head-cycle-free programs), `#count`/`#sum` bodies, `#show` projection
(including `#show t : not a.` style terms), `#minimize` (optimal answer sets
only, or cost functions), `#external` atoms (kept as free variables; their
initial value is ignored). `#project`, `#heuristic`, assumptions, edges and
theory atoms are ignored with a warning.

**New to ASP or DDASP?** Read the beginner's guide (Japanese):
[docs/GUIDE_ja.md](docs/GUIDE_ja.md) — 初心者向け利用ガイド(ASP の基本文法から ZDD の活用まで)。

## Build

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build            # C++ unit tests (Google Test)
```

KyotoDD is a git submodule in `third_party/KyotoDD` (fetch it with
`git submodule update --init --recursive`); it is built as a static
library inside this project.

## Python environment (clingo and the Python packages)

```sh
python3 -m venv .venv
.venv/bin/pip install clingo pytest scikit-build-core pybind11
.venv/bin/pip install --no-build-isolation -e third_party/KyotoDD/python   # kyotodd
.venv/bin/pip install --no-build-isolation -e python           # ddasp
.venv/bin/pytest                                               # clingo comparison tests
```

The pip `clingo` package has no `gringo` executable; use
`python -m clingo --mode=gringo` as the grounder command (see `--gringo`).

## Command line

```sh
G=".venv/bin/python -m clingo --mode=gringo"
./build/ddasp --gringo "$G" examples/queens.lp                  # answer sets: 4
./build/ddasp --gringo "$G" --enumerate examples/coloring.lp
./build/ddasp --gringo "$G" --opt --stats prog.lp
./build/ddasp --gringo "$G" -o out.zdd --atoms atoms.txt prog.lp
./build/ddasp prog.aspif                                        # already ground
./build/ddasp --gringo "$G" --save coloring.ddasp examples/coloring.lp
./build/ddasp --gringo "$G" --load coloring.ddasp --enumerate examples/coloring_fix.lp
```

Options: `--enumerate [--limit N]` (streamed; `0` = all), `-o FILE` (KyotoDD
binary; `--bdd` for the BDD), `--atoms FILE` (variable / level / atom / name
table plus the `#minimize` cost functions by level), `--all-atoms` (no `#show`
projection), `--opt` (`#minimize`), `--method scc|twocopy`,
`--order name|id|FILE`, `--stats`. The summary lines `answer sets: N` and
`optimum: ...` are printed by default; `-q` suppresses them and `-q --count`
prints the bare count.

The binary format records DD *levels*; the `--atoms` table lists the level of
every atom. Importing the file in a fresh KyotoDD process gives variable
number = level.

`--save FILE` writes the answer sets as a *stored DD* (one file: the ZDD, the
atom names and the fact names). `--load FILE` (repeatable) brings it into a
later compilation: every stored atom becomes a named `#external` atom of the
new program (declared automatically, so the program just uses the atoms in
bodies and constraints) and the answer sets are restricted to the stored
family. The result equals the answer sets of the old program plus the new one,
projected onto the stored atoms and the new program's shown atoms. The new
program must not define a stored atom (rule head or fact). See
[docs/DESIGN.md](docs/DESIGN.md) for the format and the semantics.

Limits: KyotoDD allows 65536 BDD variables (one per atom of the ground
program, two for atoms in positive loops), and the node table is unbounded,
so very large intermediate diagrams use all available memory.

## Python

```python
import ddasp
res = ddasp.compile_text("{ a; b }. :- a, b. #show a/0. #show b/0.")
res.count                     # 3
list(res.answer_sets())       # frozensets of shown atom names
res.zdd                       # kyotodd.ZDD (variables: level_to_atom / names)
res = ddasp.compile_files(["prog.lp"], optimize=True); res.optimum
res.save("prog.ddasp")        # stored DD (ZDD + names), also ddasp --save / --load
more = ddasp.compile_text(":- a. q :- b. #show q/0.", dds=[ddasp.load("prog.ddasp")])
```

`ddasp.Program` can also be filled by hand or from aspif text
(`ddasp.read_aspif`), and `ddasp.Observer` is a clingo ground-program observer.

## How it works

See [docs/DESIGN.md](docs/DESIGN.md). In short, with one BDD variable `x_a`
per atom and auxiliary variables `u_a` for the atoms of non-trivial strongly
connected components,

```
Stable(X) = Models(X) ∧ Supported(X) ∧ ∧_SCC ¬∃U (U ≠ ∅ ∧ U ⊆ X ∧ U unfounded w.r.t. X)
```

is built as a BDD and converted to a ZDD over the shown atoms. The `twocopy`
method (one group with all atoms) is the reference implementation.
