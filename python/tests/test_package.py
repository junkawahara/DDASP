"""Tests of the ddasp Python package (clingo observer -> C++ core -> kyotodd)."""

import pytest

ddasp = pytest.importorskip("ddasp")
kyotodd = pytest.importorskip("kyotodd")

from conftest import EXAMPLES, clingo_answer_sets, clingo_optimum  # noqa: E402
from test_against_clingo import INLINE_PROGRAMS, OPT_PROGRAMS  # noqa: E402


def _write(tmp_path, name, programs=INLINE_PROGRAMS):
    path = tmp_path / f"{name}.lp"
    path.write_text(programs[name])
    return path


@pytest.mark.parametrize("name", sorted(INLINE_PROGRAMS))
@pytest.mark.parametrize("method", ["scc", "twocopy"])
def test_compile_text_matches_clingo(tmp_path, name, method):
    path = _write(tmp_path, name)
    res = ddasp.compile_text(INLINE_PROGRAMS[name], method=method, project=False)
    assert res.count == sum(clingo_answer_sets([path]).values())
    assert set(res.answer_sets()) == set(clingo_answer_sets([path], project=True))
    projected = ddasp.compile_text(INLINE_PROGRAMS[name], method=method)
    assert set(projected.answer_sets()) == set(clingo_answer_sets([path], project=True))
    assert projected.zdd.exact_count == projected.count


@pytest.mark.parametrize("example", sorted(EXAMPLES.glob("*.lp")), ids=lambda p: p.stem)
def test_compile_files_matches_clingo(example):
    res = ddasp.compile_files([example])
    assert set(res.answer_sets()) == set(clingo_answer_sets([example], project=True))
    # The levels are stable across compilations (the core resets KyotoDD).
    again = ddasp.compile_files([example])
    assert again.levels == res.levels
    assert again.zdd_bytes == res.zdd_bytes


@pytest.mark.parametrize("name", sorted(OPT_PROGRAMS))
def test_optimize(tmp_path, name):
    path = _write(tmp_path, name, OPT_PROGRAMS)
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


def test_program_rejects_bad_atoms_and_literals():
    prog = ddasp.Program()
    with pytest.raises(ValueError, match="atom must be positive"):
        prog.add_rule(False, [-1], [])
    with pytest.raises(ValueError, match="literal must be nonzero"):
        prog.add_rule(False, [1], [0])
    with pytest.raises(ValueError, match="literal must be nonzero"):
        prog.add_weight_rule(False, [1], 1, [(0, 1)])
    with pytest.raises(ValueError, match="literal must be nonzero"):
        prog.add_minimize(0, [(0, 1)])
    with pytest.raises(ValueError, match="literal must be nonzero"):
        prog.add_output("a", [0])
    with pytest.raises(ValueError, match="atom must be positive"):
        prog.add_external(0)
    with pytest.raises(ValueError, match="atom must be positive"):
        prog.add_projection([0])
    assert prog.num_rules == 0 and prog.max_atom == 0
    assert ddasp.compile(prog).count == 1


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


def test_variables_of_atoms_true_in_no_answer_set():
    # a sits on top of b and is never true: the exported diagram has no node at
    # its level, so its variable has to be created when the diagram is imported.
    res = ddasp.compile_text("{a;b}. :- a. #show a/0. #show b/0.")
    assert [res.level_name(lv) for lv in res.levels] == ["a", "b"]
    vars_ = res.variables()
    assert [kyotodd.to_level(v) for v in vars_] == res.levels
    assert {kyotodd.to_var(lv): res.level_name(lv) for lv in res.levels}
    assert set(res.answer_sets()) == {frozenset(), frozenset({"b"})}
    assert res.bdd.count(len(res.levels)) == 2
    stored = res.stored()
    assert kyotodd.to_var(len(stored.names)) and stored.count == 2 and stored.zdd.exact_count == 2


def test_show_terms():
    res = ddasp.compile_text("{ a }. #show a/0. #show t : a. #show u : not a.")
    assert res.names == {1: "a"}
    assert sorted(res.terms) == [("t", [1]), ("u", [-1])]
    assert set(res.answer_sets()) == {frozenset({"a", "t"}), frozenset({"u"})}
    assert res.count == 2


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


def test_observer_warns_on_ignored_statements():
    import warnings

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        res = ddasp.compile_text("{ a }. #heuristic a. [1,true] #show a/0.")
    assert res.count == 2
    assert any("heuristic" in str(w.message) for w in caught)


def test_result_maps_are_plain_dicts():
    res = ddasp.compile_text("{ a; b }. c :- a. #show a/0. #show b/0.")
    assert res.names == {1: "a", 2: "b"}
    assert set(res.level_to_atom.values()) == {1, 2}
    assert set(res.atom_to_level) == {1, 2, 3}  # hidden c has a level too
    assert res.level_name(res.levels[0]) == "a"
    assert res.atom_name(3) == "__atom3"
    assert res.stats["atoms"] == 3
    assert [c["weights"] for c in ddasp.compile_text("{ a }. c :- a. #minimize{ 1 : c }. #show a/0.").costs] == [
        {ddasp.compile_text("{ a }. c :- a. #minimize{ 1 : c }. #show a/0.").atom_to_level[2]: 1}
    ]
