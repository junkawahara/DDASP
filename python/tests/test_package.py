"""Tests of the ddasp Python package (clingo observer -> C++ core -> kyotodd)."""

import pytest

ddasp = pytest.importorskip("ddasp")
kyotodd = pytest.importorskip("kyotodd")

from conftest import EXAMPLES, clingo_answer_sets, clingo_optimum  # noqa: E402
from test_against_clingo import INLINE_PROGRAMS, OPT_PROGRAMS  # noqa: E402


@pytest.mark.parametrize("name", sorted(INLINE_PROGRAMS))
@pytest.mark.parametrize("method", ["scc", "twocopy"])
def test_compile_text_matches_clingo(name, method):
    res = ddasp.compile_text(INLINE_PROGRAMS[name], method=method, project=False)
    assert res.count == sum(clingo_answer_sets([_write(name)]).values())
    assert set(res.answer_sets()) == set(clingo_answer_sets([_write(name)], project=True))
    projected = ddasp.compile_text(INLINE_PROGRAMS[name], method=method)
    assert set(projected.answer_sets()) == set(clingo_answer_sets([_write(name)], project=True))
    assert projected.zdd.exact_count == projected.count


def _write(name, programs=INLINE_PROGRAMS):
    import tempfile
    import os

    path = os.path.join(tempfile.gettempdir(), f"ddasp_pkg_{name}.lp")
    with open(path, "w") as f:
        f.write(programs[name])
    return path


@pytest.mark.parametrize("example", sorted(EXAMPLES.glob("*.lp")), ids=lambda p: p.stem)
def test_compile_files_matches_clingo(example):
    res = ddasp.compile_files([example])
    assert set(res.answer_sets()) == set(clingo_answer_sets([example], project=True))
    # The levels are stable across compilations (the core resets KyotoDD).
    again = ddasp.compile_files([example])
    assert again.levels == res.levels
    assert again.zdd_bytes == res.zdd_bytes


@pytest.mark.parametrize("name", sorted(OPT_PROGRAMS))
def test_optimize(name):
    path = _write(name, OPT_PROGRAMS)
    res = ddasp.compile_text(OPT_PROGRAMS[name], optimize=True)
    assert set(res.answer_sets()) == set(clingo_answer_sets([path], project=True, optimize=True))
    expected = clingo_optimum([path])
    assert (res.optimum or None) == expected
    if expected is not None:
        assert len(res.costs) == len(expected)


def test_program_builder_and_aspif():
    prog = ddasp.Program()
    prog.add_rule(True, [1, 2], [])          # {a; b}.
    prog.add_rule(False, [], [1, 2])         # :- a, b.
    prog.add_rule(False, [3], [1])           # c :- a.
    prog.add_output("a", [1])
    prog.add_output("b", [2])
    prog.add_output("c", [3])
    res = ddasp.compile(prog)
    assert res.count == 3
    assert set(res.answer_sets()) == {frozenset(), frozenset({"a", "c"}), frozenset({"b"})}
    assert res.names == {1: "a", 2: "b", 3: "c"}
    assert sorted(res.level_to_atom.values()) == [1, 2, 3]

    text = str(prog)
    parsed, warnings = ddasp.read_aspif(text)
    assert warnings == []
    assert parsed.num_rules == 3
    assert str(parsed) == text
    assert ddasp.compile(parsed).count == 3


def test_order_and_variables():
    prog = ddasp.Program()
    prog.add_rule(True, [1, 2, 3], [])
    for a, n in ((1, "p(10)"), (2, "p(2)"), (3, "p(1)")):
        prog.add_output(n, [a])
    res = ddasp.compile(prog)
    assert [res.level_name(l) for l in res.levels] == ["p(1)", "p(2)", "p(10)"]
    res_id = ddasp.compile(prog, order="id")
    assert [res_id.level_name(l) for l in res_id.levels] == ["p(10)", "p(2)", "p(1)"]
    res_custom = ddasp.compile(prog, order=[2])
    assert [res_custom.level_name(l) for l in res_custom.levels] == ["p(2)", "p(1)", "p(10)"]
    vars_ = res_custom.variables()
    assert [kyotodd.to_level(v) for v in vars_] == res_custom.levels
    assert res_custom.zdd.exact_count == 8
    assert res_custom.bdd is not None


def test_external_and_hidden():
    res = ddasp.compile_text("#external e. a :- e. b :- not e. #show a/0. #show b/0.")
    assert set(res.answer_sets()) == {frozenset({"a"}), frozenset({"b"})}
    res = ddasp.compile_text("{ a; b }. c :- a. #show c/0.", project=False)
    assert res.count == 4
    assert len(res.levels) == 3
    assert sorted(set(res.answer_sets())) == [frozenset(), frozenset({"c"})]
    assert res.stats["aux_vars"] == 0
    projected = ddasp.compile_text("{ a; b }. c :- a. #show c/0.")
    assert projected.count == 2
    assert len(projected.levels) == 1
    loop = ddasp.compile_text("{c}. a :- b. b :- a. a :- c.", project=False)
    assert loop.stats["aux_vars"] == 2
    assert loop.stats["groups"] == 1
