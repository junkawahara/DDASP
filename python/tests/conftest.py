"""Shared helpers for the ddasp test-suite.

The tests drive the ``ddasp`` CLI (built into ``build/``) and compare its
answer sets with clingo's enumeration.
"""

import os
import subprocess
import sys
from collections import Counter
from pathlib import Path

import clingo
import pytest

ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / "examples"
DDASP_BIN = Path(os.environ.get("DDASP_BIN", ROOT / "build" / "ddasp"))
GRINGO_CMD = os.environ.get("DDASP_GRINGO", f"{sys.executable} -m clingo --mode=gringo")


@pytest.fixture(scope="session")
def ddasp_bin():
    if not DDASP_BIN.exists():
        pytest.skip(f"ddasp binary not found at {DDASP_BIN}; build the project first")
    return DDASP_BIN


def run_ddasp(ddasp_bin, files, *extra):
    """Run ddasp on the given .lp files; return (stdout, stderr)."""
    cmd = [str(ddasp_bin), "--gringo", GRINGO_CMD, *extra, *map(str, files)]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
    return proc.stdout, proc.stderr


def ddasp_answer_sets(ddasp_bin, files, *extra):
    """Multiset of answer sets (frozensets of shown atom names) from ddasp --enumerate."""
    out, _ = run_ddasp(ddasp_bin, files, "--enumerate", "-q", *extra)
    return Counter(frozenset(line.split()) for line in out.splitlines())


def _external_atoms(ctl):
    return [a.symbol for a in ctl.symbolic_atoms if a.is_external]


def clingo_answer_sets(files, project=False, optimize=False):
    """Multiset of answer sets (frozensets of shown atom names) enumerated by clingo.

    #external atoms are free in ddasp, so every truth assignment of the
    externals is enumerated and the results are merged. With ``project`` the
    answer sets are projected onto the shown atoms (distinct projections);
    with ``optimize`` only optimal answer sets are kept.
    """
    # --eq=0: clasp's equivalence preprocessing (default --eq=5) returns an
    # unsupported model for some disjunctive programs, e.g.
    # `e | c | d :- e. 2 { f; e } 2 :- a. b | f :- not d, a. 2 { a; d; c } 2. { f; e; c } :- c.`
    # ({b, c, d}, the only rule for b having a false body); without it clingo agrees with ddasp.
    args = ["0", "--eq=0"]
    if project:
        args.append("--project=show")
    if optimize:
        args.append("--opt-mode=optN")
    else:
        args.append("--opt-mode=ignore")  # every answer set, whatever its #minimize cost
    ctl = clingo.Control(args)
    for f in files:
        ctl.load(str(f))
    ctl.ground([("base", [])])
    externals = _external_atoms(ctl)
    # ddasp optimizes over all assignments of the externals at once, whereas this
    # loop would optimize per assignment; the two are not comparable.
    assert not (optimize and externals), "optimize=True is not supported with #external atoms"
    result = Counter()
    for mask in range(1 << len(externals)):
        for i, sym in enumerate(externals):
            ctl.assign_external(sym, bool(mask >> i & 1))
        with ctl.solve(yield_=True) as handle:
            for model in handle:
                # Without a #minimize statement (e.g. one that grounds to nothing)
                # every model is optimal, but optimality_proven is never set.
                if optimize and model.cost and not model.optimality_proven:
                    continue
                result[frozenset(str(s) for s in model.symbols(shown=True))] += 1
    return result


def clingo_optimum(files):
    """Optimal cost vector reported by clingo (None if the program is inconsistent)."""
    ctl = clingo.Control(["0", "--opt-mode=optN", "--eq=0"])
    for f in files:
        ctl.load(str(f))
    ctl.ground([("base", [])])
    best = None
    with ctl.solve(yield_=True) as handle:
        for model in handle:
            if model.optimality_proven:
                best = list(model.cost)
    return best
