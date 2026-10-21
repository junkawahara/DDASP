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


# --- Python API ---------------------------------------------------------------

ddasp = pytest.importorskip("ddasp")
pytest.importorskip("kyotodd")


def test_python_result_as_stored_dd_matches_the_combined_program(tmp_path):
    fix = _write(tmp_path, "fix.lp", FIX)
    old = ddasp.compile_files([COLORING])
    d = old.stored()
    assert isinstance(d, ddasp.StoredDD)
    assert len(d.names) == 15 and d.names[0] == "col(1,b)" and d.fact_names == []
    assert d.count == old.count == 18
    assert set(d.answer_sets()) == set(old.answer_sets())
    new = ddasp.compile_files([fix], dds=[d])
    assert set(new.answer_sets()) == set(clingo_answer_sets([COLORING, fix], project=True))
    assert new.count == 12
    assert new.stats["loaded_dds"] == 1 and new.stats["synthesized_atoms"] == 0
    # A Result, a file path and the raw bytes are accepted as well.
    assert ddasp.compile_files([fix], dds=[old]).count == 12
    path = tmp_path / "coloring.ddasp"
    old.save(path)
    assert ddasp.compile_files([fix], dds=[str(path)]).count == 12
    assert ddasp.compile_files([fix], dds=[path.read_bytes()]).count == 12


def test_python_save_and_load_round_trip(tmp_path):
    res = ddasp.compile_text("f. {p(1..3)} 2. #show f/0. #show p/1.")
    path = tmp_path / "p.ddasp"
    res.save(path)
    d = ddasp.load(path)
    assert d.names == ["p(1)", "p(2)", "p(3)"]
    assert d.fact_names == ["f"]
    assert d.data == path.read_bytes()
    assert d.declarations() == (
        "#external p(1).\n#show p(1) : p(1).\n#external p(2).\n#show p(2) : p(2).\n"
        "#external p(3).\n#show p(3) : p(3).\nf.\n"
    )
    assert set(d.answer_sets()) == set(res.answer_sets())
    assert d.count == res.count == 7
    assert repr(d) == "StoredDD(3 atoms, 1 facts)"
    # Terminal families are stored without a diagram.
    none = ddasp.compile_text("a. :- a. #show a/0.").stored()
    assert none.count == 0 and list(none.answer_sets()) == []
    only_empty = ddasp.compile_text("{a}. :- a. #show a/0.").stored()
    assert only_empty.count == 1 and list(only_empty.answer_sets()) == [frozenset()]


def test_python_stored_atoms_stay_visible_and_facts_are_merged():
    base = ddasp.compile_text("node(1..3). {in(N) : node(N)} 1. #show node/1. #show in/1.").stored()
    assert base.fact_names == ["node(1)", "node(2)", "node(3)"]
    new = ddasp.compile_text("cnt(C) :- C = #count{N : in(N)}. #show cnt/1.", dds=[base])
    sets = set(new.answer_sets())
    assert len(sets) == 4
    assert all({"node(1)", "node(2)", "node(3)"} <= s for s in sets)
    assert frozenset({"node(1)", "node(2)", "node(3)", "in(2)", "cnt(1)"}) in sets
    assert new.fact_names == ["node(1)", "node(2)", "node(3)"]


def test_python_several_dds_and_raw_program():
    ab = ddasp.compile_text("{a;b}. :- a, b.").stored()
    bc = ddasp.compile_text("{b;c}. :- b, c.").stored()
    both = ddasp.compile_text(":- not b.", dds=[ab, bc])
    assert set(both.answer_sets()) == {frozenset({"b"})}
    # A raw Program is compiled as it is: the stored atoms are synthesized.
    res = ddasp.compile(ddasp.Program(), dds=[ab, bc])
    assert res.stats["synthesized_atoms"] == 3
    assert len(set(res.answer_sets())) == 5


def test_python_errors_and_warnings():
    d = ddasp.compile_text("{a;b}. #show a/0. #show b/0.").stored()
    with pytest.raises(ValueError, match="defined by a rule"):
        ddasp.compile_text("a :- b.", dds=[d])
    with pytest.raises(ValueError, match="fact"):
        ddasp.compile_text("a.", dds=[d])
    with pytest.raises(ValueError, match="not a DDASP stored DD"):
        ddasp.StoredDD(b"junk")
    with pytest.raises(ValueError, match="no name"):
        ddasp.compile_text("{a}. b :- a. #show a/0.", project=False).stored()
    with pytest.warns(RuntimeWarning, match="not stored"):
        ddasp.compile_text("{a}. #show a/0. #show t : not a.").stored()


def test_python_and_cli_files_interoperate(tmp_path, ddasp_bin):
    fix = _write(tmp_path, "fix.lp", FIX)
    cli_file = tmp_path / "cli.ddasp"
    run_ddasp(ddasp_bin, [COLORING], "--save", cli_file)
    py_file = tmp_path / "py.ddasp"
    ddasp.compile_files([COLORING]).save(py_file)
    assert cli_file.read_bytes() == py_file.read_bytes()
    from_cli = ddasp.compile_files([fix], dds=[ddasp.load(cli_file)])
    assert set(from_cli.answer_sets()) == set(ddasp_answer_sets(ddasp_bin, [fix], "--load", py_file))
