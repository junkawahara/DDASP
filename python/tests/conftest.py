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


def ddasp_answer_sets(ddasp_bin, files):
    """Multiset of answer sets (frozensets of shown atom names) from ddasp --enumerate."""
    out, _ = run_ddasp(ddasp_bin, files, "--enumerate", "-q")
    return Counter(frozenset(line.split()) for line in out.splitlines())


def _external_atoms(ctl):
    return [a.symbol for a in ctl.symbolic_atoms if a.is_external]


def clingo_answer_sets(files):
    """Multiset of answer sets (frozensets of shown atom names) enumerated by clingo.

    #external atoms are free in ddasp, so every truth assignment of the
    externals is enumerated and the results are merged.
    """
    ctl = clingo.Control(["0"])
    for f in files:
        ctl.load(str(f))
    ctl.ground([("base", [])])
    externals = _external_atoms(ctl)
    result = Counter()
    for mask in range(1 << len(externals)):
        for i, sym in enumerate(externals):
            ctl.assign_external(sym, bool(mask >> i & 1))
        with ctl.solve(yield_=True) as handle:
            for model in handle:
                result[frozenset(str(s) for s in model.symbols(shown=True))] += 1
    return result
