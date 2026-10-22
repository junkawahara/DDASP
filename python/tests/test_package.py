"""Tests of the ddasp Python package (clingo observer -> C++ core -> kyotodd)."""

from collections import Counter

import pytest

ddasp = pytest.importorskip("ddasp")
kyotodd = pytest.importorskip("kyotodd")

from conftest import clingo_answer_sets, clingo_costed_answer_sets, clingo_optimum  # noqa: E402
from test_against_clingo import (INLINE_PROGRAMS, OPT_PROGRAMS, STANDALONE_EXAMPLES,  # noqa: E402
                                 cost_bound_cases, expected_by_cost, expected_histogram, lex_within)


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


@pytest.mark.parametrize("example", STANDALONE_EXAMPLES, ids=lambda p: p.stem)
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


@pytest.mark.parametrize("name", sorted(OPT_PROGRAMS))
def test_cost_bounds(tmp_path, name):
    path = _write(tmp_path, name, OPT_PROGRAMS)
    text = OPT_PROGRAMS[name]
    models = clingo_costed_answer_sets([path])
    costs = sorted({c for _, c in models})
    for bound in cost_bound_cases(costs):
        for relation in ("le", "ge", "eq"):
            res = ddasp.compile_text(text, **{f"cost_{relation}": list(bound)})
            assert set(res.answer_sets()) == {s for s, c in models if lex_within(c, bound, relation)}, (bound, relation)
            assert res.optimum == []  # an absolute bound does not report the optimum
        res = ddasp.compile_text(text, cost_le=bound[0])  # a bare int bounds the first level
        assert set(res.answer_sets()) == {s for s, c in models if lex_within(c, bound[:1], "le")}
    optimum = clingo_optimum([path])
    if optimum is None:
        res = ddasp.compile_text(text, cost_slack=1)
        assert res.count == 0 and res.optimum == []
        return
    res = ddasp.compile_text(text, cost_slack=[1] * len(optimum))
    assert res.optimum == optimum
    bound = tuple(o + 1 for o in optimum)
    assert set(res.answer_sets()) == {s for s, c in models if lex_within(c, bound, "le")}
    best = ddasp.compile_text(text, cost_slack=[0] * len(optimum))  # optimize=True
    assert set(best.answer_sets()) == set(ddasp.compile_text(text, optimize=True).answer_sets())
    both = ddasp.compile_text(text, cost_ge=optimum[0] + 1, cost_slack=1)  # cost_ge combines with cost_slack
    assert set(both.answer_sets()) == {s for s, c in models if c[0] == optimum[0] + 1}
    assert both.optimum == optimum


@pytest.mark.parametrize("name", sorted(OPT_PROGRAMS))
def test_cost_histogram(tmp_path, name):
    path = _write(tmp_path, name, OPT_PROGRAMS)
    text = OPT_PROGRAMS[name]
    models = clingo_costed_answer_sets([path])
    assert ddasp.compile_text(text).cost_histogram is None  # only on request
    res = ddasp.compile_text(text, cost_histogram=True)
    expected = expected_histogram(models, project=True)
    assert list(res.cost_histogram.items()) == expected  # tuple keys in ascending order
    assert sum(res.cost_histogram.values()) == res.count
    assert res.cost_histogram is not res.cost_histogram  # a fresh dict each time
    full = ddasp.compile_text(text, project=False, cost_histogram=True)
    assert list(full.cost_histogram.items()) == expected_histogram(models, project=False)
    best = ddasp.compile_text(text, optimize=True, cost_histogram=True)
    if expected:
        assert best.cost_histogram == {tuple(best.optimum): expected[0][1]}
    else:
        assert best.cost_histogram == {} and res.count == 0
    assert "histogram_time" in res.stats
    with pytest.raises(ValueError):
        ddasp.compile_text("{ a }.", cost_histogram=True)  # no #minimize statement


@pytest.mark.parametrize("name", sorted(OPT_PROGRAMS))
def test_answer_sets_by_cost(tmp_path, name):
    path = _write(tmp_path, name, OPT_PROGRAMS)
    text = OPT_PROGRAMS[name]
    models = clingo_costed_answer_sets([path])
    full = ddasp.compile_text(text, project=False)
    rows = list(full.answer_sets(by_cost=True))
    assert [cost for cost, _ in rows] == sorted(cost for cost, _ in rows)
    assert Counter(rows) == expected_by_cost(models, project=False)
    res = ddasp.compile_text(text)
    if {level for cost in res.costs for level in cost["weights"]} - set(res.levels):
        with pytest.raises(ValueError):  # hidden atoms carry weight: only compile(best=K) orders the projected sets
            list(res.answer_sets(by_cost=True))
    else:
        rows = list(res.answer_sets(by_cost=True))
        assert [cost for cost, _ in rows] == sorted(cost for cost, _ in rows)
        assert Counter(rows) == expected_by_cost(models, project=True)
    expected = expected_by_cost(models, project=True)
    best = ddasp.compile_text(text, best=3).best
    assert len(best) == min(3, sum(expected.values())) == len(set(best))
    assert [cost for cost, _ in best] == sorted(cost for cost, _ in expected.elements())[:3]
    assert all(expected[row] for row in best)
    assert ddasp.compile_text(text).best is None
    for bad in (0, -1, True, "3"):
        with pytest.raises(ValueError):
            ddasp.compile_text(text, best=bad)
    with pytest.raises(ValueError):
        ddasp.compile_text("{ a }.", best=1)  # no #minimize statement
    with pytest.raises(ValueError):
        list(ddasp.compile_text("{ a }.").answer_sets(by_cost=True))


def test_cost_bound_arguments():
    text = OPT_PROGRAMS["two_levels"]
    for kwargs in (dict(optimize=True, cost_le=1), dict(cost_le=1, cost_slack=0), dict(cost_eq=1, cost_ge=0),
                   dict(cost_eq=[1], cost_slack=[0]), dict(cost_le=[1, 2, 3]), dict(cost_le="1"),
                   dict(cost_le=True), dict(cost_ge=[1, None]), dict(cost_slack=-1), dict(cost_slack=[0, -1])):
        with pytest.raises(ValueError):
            ddasp.compile_text(text, **kwargs)
    with pytest.raises(ValueError):
        ddasp.compile_text("{ a }.", cost_le=1)  # no #minimize statement
    assert ddasp.compile_text(text, cost_le=[], cost_ge=None).count == ddasp.compile_text(text).count


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


def test_assumptions_from_the_backend():
    import clingo

    ctl = clingo.Control()
    ctl.add("base", [], "{ a; b }. c :- a. #show a/0. #show b/0. #show c/0.")
    observer = ddasp.Observer()
    ctl.register_observer(observer)
    ctl.ground([("base", [])])
    lit = {str(s.symbol): s.literal for s in ctl.symbolic_atoms}
    with ctl.backend() as backend:
        backend.add_assume([lit["a"], -lit["b"]])
    res = ddasp.compile(observer.program)
    assert set(res.answer_sets()) == {frozenset({"a", "c"})}
    # clingo honours the same assumptions when solving
    with ctl.solve(yield_=True) as handle:
        models = [frozenset(str(s) for s in m.symbols(shown=True)) for m in handle]
    assert set(models) == set(res.answer_sets())


def test_external_values():
    import clingo

    text = "#external a. [true] #external b. [false] #external c. d :- a. #show a/0. #show b/0. #show c/0. #show d/0."
    assert ddasp.compile_text(text).count == 8  # ignored by default
    # gringo declares an external without a value as false, so c is fixed to false too.
    res = ddasp.compile_text(text, external_values=True)
    assert set(res.answer_sets()) == {frozenset({"a", "d"})}
    # clingo's default solve uses the same initial values.
    ctl = clingo.Control(["0"])
    ctl.add("base", [], text)
    ctl.ground([("base", [])])
    with ctl.solve(yield_=True) as handle:
        models = {frozenset(str(s) for s in m.symbols(shown=True)) for m in handle}
    assert models == {frozenset({"a", "d"})}


def test_project_statements():
    import warnings

    from test_against_clingo import PROJECT_PROGRAM

    assert ddasp.compile_text(PROJECT_PROGRAM).count == 8
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        res = ddasp.compile_text(PROJECT_PROGRAM, project="project")
    assert set(res.answer_sets()) == {frozenset(), frozenset({"a"}), frozenset({"a", "__atom4"})}
    assert any("#project" in str(w.message) for w in caught)
    assert res.terms == []
    with pytest.raises(ValueError):
        ddasp.compile_text(PROJECT_PROGRAM, project="shown")
    with pytest.raises(ValueError):
        ddasp.compile_text(PROJECT_PROGRAM, project="project", method="nope")


@pytest.mark.parametrize("name", sorted(INLINE_PROGRAMS))
def test_consequences(tmp_path, name):
    from test_against_clingo import consequences_from

    path = _write(tmp_path, name)
    brave, cautious, _ = consequences_from(clingo_answer_sets([path], project=True))
    res = ddasp.compile_text(INLINE_PROGRAMS[name])
    assert res.brave == brave
    assert res.cautious == cautious
    _, _, freq = consequences_from(clingo_answer_sets([path]))
    full = ddasp.compile_text(INLINE_PROGRAMS[name], project=False)
    assert {k: v for k, v in full.frequencies.items() if v} == dict(freq)
    assert list(res.frequencies) == list(res.frequencies)  # display order is stable


def test_node_max():
    text = "{ p(1..60) }. :- #count{ X : p(X) } != 30. #show p/1."
    with pytest.raises(ddasp.NodeLimitError, match="node limit of 2000"):
        ddasp.compile_text(text, node_max=2000)
    assert issubclass(ddasp.NodeLimitError, RuntimeError)
    # The instance is usable again afterwards, with and without a limit.
    assert ddasp.compile_text(text, node_max=10_000_000).count == 118264581564861424
    assert ddasp.compile_text(text).count == 118264581564861424
    with pytest.raises(ValueError):
        ddasp.compile_text(text, node_max=0)
