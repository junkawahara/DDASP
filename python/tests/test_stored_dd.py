"""Stored DDs: --save / --load on the command line, and the Python API."""

import subprocess
from collections import Counter

import pytest

from conftest import EXAMPLES, GRINGO_CMD, clingo_answer_sets, ddasp_answer_sets, run_ddasp

COLORING = EXAMPLES / "coloring.lp"

# A second step for examples/coloring.lp: forbid one colour, derive a new predicate.
FIX = """
:- col(1,r).
red(N) :- col(N,r).
#show red/1.
"""

# A third step on top of FIX.
MORE = """
:- red(3).
#show red/1.
"""


def _write(tmp_path, name, text):
    path = tmp_path / name
    path.write_text(text)
    return path


def _ddasp(ddasp_bin, *args):
    return subprocess.run([str(ddasp_bin), "--gringo", GRINGO_CMD, *map(str, args)], capture_output=True, text=True)


# --- command line ------------------------------------------------------------


def test_save_then_load_matches_the_combined_program(tmp_path, ddasp_bin):
    stored = tmp_path / "coloring.ddasp"
    run_ddasp(ddasp_bin, [COLORING], "--save", stored)
    assert stored.read_bytes().startswith(b"ddasp-dd 1\natoms 15\n")
    fix = _write(tmp_path, "fix.lp", FIX)
    got = ddasp_answer_sets(ddasp_bin, [fix], "--load", stored)
    assert got == clingo_answer_sets([COLORING, fix], project=True)
    assert len(got) == 12
    # The loaded atoms stay visible although fix.lp only shows red/1.
    assert all(any(name.startswith("col(") for name in s) for s in got)


def test_loaded_results_can_be_saved_again(tmp_path, ddasp_bin):
    stored = tmp_path / "coloring.ddasp"
    run_ddasp(ddasp_bin, [COLORING], "--save", stored)
    fix = _write(tmp_path, "fix.lp", FIX)
    stored2 = tmp_path / "fix.ddasp"
    run_ddasp(ddasp_bin, [fix], "--load", stored, "--save", stored2)
    more = _write(tmp_path, "more.lp", MORE)
    got = ddasp_answer_sets(ddasp_bin, [more], "--load", stored2)
    assert got == clingo_answer_sets([COLORING, fix, more], project=True)
    assert 0 < len(got) < 12


def test_several_stored_dds(tmp_path, ddasp_bin):
    ab = _write(tmp_path, "ab.lp", "{a;b}. :- a, b.")
    bc = _write(tmp_path, "bc.lp", "{b;c}. :- b, c.")
    ab_dd, bc_dd = tmp_path / "ab.ddasp", tmp_path / "bc.ddasp"
    run_ddasp(ddasp_bin, [ab], "--save", ab_dd)
    run_ddasp(ddasp_bin, [bc], "--save", bc_dd)
    need_b = _write(tmp_path, "need_b.lp", ":- not b.")
    got = ddasp_answer_sets(ddasp_bin, [need_b], "--load", ab_dd, "--load", bc_dd)
    assert got == Counter({frozenset({"b"}): 1})
    empty = _write(tmp_path, "empty.lp", "")
    got = ddasp_answer_sets(ddasp_bin, [empty], "--load", ab_dd, "--load", bc_dd)
    assert got == clingo_answer_sets([ab, bc], project=True)
    assert len(got) == 5


def test_load_with_aspif_input_needs_explicit_externals(tmp_path, ddasp_bin):
    stored = tmp_path / "coloring.ddasp"
    run_ddasp(ddasp_bin, [COLORING], "--save", stored)
    # Ground the second step by hand: the stored atoms it uses must be declared
    # (#external) and named (#show) in the aspif file; the others are synthesized.
    step = _write(tmp_path, "step.lp", "#external col(1,r). #external col(2,r). :- col(1,r). :- col(2,r). #show col/2.")
    aspif = tmp_path / "step.aspif"
    aspif.write_text(subprocess.run([*GRINGO_CMD.split(), "--output=intermediate", str(step)],
                                    capture_output=True, text=True, check=True).stdout)
    got = ddasp_answer_sets(ddasp_bin, [aspif], "--load", stored)
    constraint = _write(tmp_path, "constraint.lp", ":- col(1,r). :- col(2,r).")
    assert got == clingo_answer_sets([COLORING, constraint], project=True)


def test_facts_of_the_stored_dd_are_shown_and_usable(tmp_path, ddasp_bin):
    base = _write(tmp_path, "base.lp", "node(1..3). { in(N) : node(N) } 1. #show node/1. #show in/1.")
    stored = tmp_path / "base.ddasp"
    run_ddasp(ddasp_bin, [base], "--save", stored)
    assert b"facts 3\nnode(1)\nnode(2)\nnode(3)\n" in stored.read_bytes()
    step = _write(tmp_path, "step.lp", "cnt(C) :- C = #count { N : in(N) }. :- cnt(0). #show cnt/1.")
    got = ddasp_answer_sets(ddasp_bin, [step], "--load", stored)
    assert got == clingo_answer_sets([base, step], project=True)
    assert all({"node(1)", "node(2)", "node(3)", "cnt(1)"} <= s for s in got)
    assert len(got) == 3


def test_load_rejects_programs_that_define_stored_atoms(tmp_path, ddasp_bin):
    stored = tmp_path / "coloring.ddasp"
    run_ddasp(ddasp_bin, [COLORING], "--save", stored)
    bad = _write(tmp_path, "bad.lp", "col(1,r).")
    proc = _ddasp(ddasp_bin, "--load", stored, bad)
    assert proc.returncode == 1
    assert "col(1,r)" in proc.stderr and "fact" in proc.stderr
    bad2 = _write(tmp_path, "bad2.lp", "col(1,r) :- col(2,g).")
    proc = _ddasp(ddasp_bin, "--load", stored, bad2)
    assert proc.returncode == 1
    assert "defined by a rule" in proc.stderr
    proc = _ddasp(ddasp_bin, "--load", tmp_path / "missing.ddasp", bad)
    assert proc.returncode == 1
    assert "cannot open" in proc.stderr
    garbage = _write(tmp_path, "garbage.ddasp", "hello")
    proc = _ddasp(ddasp_bin, "--load", garbage, bad)
    assert proc.returncode == 1
    assert "garbage.ddasp" in proc.stderr


def test_save_rejects_unnamed_atoms(tmp_path, ddasp_bin):
    prog = _write(tmp_path, "hidden.lp", "{a}. b :- a. #show a/0.")
    proc = _ddasp(ddasp_bin, "--all-atoms", "--save", tmp_path / "out.ddasp", prog)
    assert proc.returncode == 1
    assert "no name" in proc.stderr
    assert not (tmp_path / "out.ddasp").exists() or (tmp_path / "out.ddasp").stat().st_size == 0
    # With projection every variable is named.
    run_ddasp(ddasp_bin, [prog], "--save", tmp_path / "ok.ddasp")
    assert (tmp_path / "ok.ddasp").read_bytes().startswith(b"ddasp-dd 1\natoms 1\na\n")


def test_stats_and_help_mention_stored_dds(tmp_path, ddasp_bin):
    stored = tmp_path / "coloring.ddasp"
    run_ddasp(ddasp_bin, [COLORING], "--save", stored)
    fix = _write(tmp_path, "fix.lp", FIX)
    _, err = run_ddasp(ddasp_bin, [fix], "--load", stored, "--stats")
    assert "loaded dds: 1\n" in err
    assert "synthesized atoms: 0\n" in err  # the declarations are grounded with fix.lp
    assert "loaded dd time:" in err
    out = subprocess.run([str(ddasp_bin), "--help"], capture_output=True, text=True).stdout
    assert "--save FILE" in out and "--load FILE" in out
