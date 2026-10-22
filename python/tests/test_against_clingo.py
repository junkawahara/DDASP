"""Compare ddasp's answer sets with clingo's enumeration."""

import json
import subprocess
from collections import Counter
from pathlib import Path

import clingo
import kyotodd
import pytest

from conftest import (EXAMPLES, GRINGO_CMD, clingo_answer_sets, clingo_costed_answer_sets, clingo_optimum,
                      ddasp_answer_sets, run_ddasp)

INLINE_PROGRAMS = {
    "fact": "a. #show a/0.",
    "even_loop": "a :- not b. b :- not a. #show a/0. #show b/0.",
    "positive_loop": "a :- b. b :- a. #show a/0. #show b/0.",
    "disjunction": "a | b. #show a/0. #show b/0.",
    "non_hcf_disjunction": "a | b. a :- b. b :- a. #show a/0. #show b/0.",
    "disjunction_with_constraint": "a | b | c. :- a, b. c :- a. #show a/0. #show b/0. #show c/0.",
    "count_aggregate": "{ p(1..4) }. :- #count{ X : p(X) } != 2. #show p/1.",
    "sum_aggregate": "{ p(1..4) }. :- #sum{ X : p(X) } < 6. #show p/1.",
    "sum_negative_weights": "{ p(1..3) }. ok :- #sum{ X : p(X); -2 : not p(2) } >= 2. :- not ok. #show p/1.",
    "sum_in_body": "{ p(1..3) }. q :- 3 <= #sum{ X : p(X) }. #show p/1. #show q/0.",
    "choice_bounds": "1 { p(1..4) } 2. #show p/1.",
    "external": "#external e. a :- e. b :- not e. #show a/0. #show b/0.",
    "external_defined_by_a_rule": "#external e. { a }. e :- a. #show a/0. #show e/0.",
    "external_tautologies": "#external e. { g }. e | f :- g, e. e :- g, not g. #show e/0. #show f/0. #show g/0.",
    "external_head_in_negative_body": "#external e. { g }. e :- g, not e. #show e/0. #show g/0.",
    "choice_head_in_body": "{ a }. { a; b } :- a. #show a/0. #show b/0.",
    "external_in_rewritten_aggregate": "#external e. { d }. g :- #sum{ -2,e0 : e; -3,g1 : g; 2,d2 : d } >= 0. "
                                       "#show d/0. #show e/0. #show g/0.",
    "inconsistent": "a. :- a.",
    "hidden_atoms": "{ a; b }. c :- a. #show c/0.",
    "nested_negation": "{ a }. b :- not not a. #show a/0. #show b/0.",
    "conditional_literal": "p(1..3). { q(X) : p(X) }. r :- q(X) : p(X). #show r/0. #show q/1.",
    "second_name": "{ a }. #show a/0. #show t : a.",
    "negated_show": "{ a }. #show t : not a.",
    "shared_name": "{ a; b }. #show t : a. #show t : b.",
    "shown_fact_term": "c. #show c/0. #show w : c.",
    "disjunction_clasp_eq": "e | c | d :- e. 2 { f; e } 2 :- a. b | f :- not d, a. 2 { a; d; c } 2. { f; e; c } :- c. "
                            "#show a/0. #show b/0. #show c/0. #show d/0. #show e/0. #show f/0.",
    "graph_reachability": """
        edge(1,2). edge(2,3). edge(3,1). edge(3,4).
        { on(X,Y) : edge(X,Y) }.
        reach(1). reach(Y) :- reach(X), on(X,Y).
        :- edge(_,Y), not reach(Y).
        #show on/2.
    """,
}


@pytest.fixture(params=sorted(INLINE_PROGRAMS))
def inline_program(request, tmp_path):
    path = tmp_path / f"{request.param}.lp"
    path.write_text(INLINE_PROGRAMS[request.param])
    return path


METHODS = ["scc", "twocopy"]


@pytest.mark.parametrize("method", METHODS)
def test_inline_program_matches_clingo(ddasp_bin, inline_program, method):
    # Without projection every answer set is one line, so compare multisets.
    got = ddasp_answer_sets(ddasp_bin, [inline_program], "--all-atoms", "--method", method)
    assert got == clingo_answer_sets([inline_program])


@pytest.mark.parametrize("method", METHODS)
def test_inline_program_projection_matches_clingo(ddasp_bin, inline_program, method):
    projected = ddasp_answer_sets(ddasp_bin, [inline_program], "--method", method)
    assert all(n == 1 for n in projected.values())
    assert set(projected) == set(clingo_answer_sets([inline_program], project=True))


# Second steps on a stored DD (tested in test_stored_dd.py); alone they mean nothing.
SECOND_STEPS = {"coloring_fix.lp", "independent_set_maximal.lp", "shift.lp"}
STANDALONE_EXAMPLES = sorted(p for p in EXAMPLES.glob("*.lp") if p.name not in SECOND_STEPS)


@pytest.mark.parametrize("method", METHODS)
@pytest.mark.parametrize("example", STANDALONE_EXAMPLES, ids=lambda p: p.stem)
def test_example_matches_clingo(ddasp_bin, example, method):
    got = ddasp_answer_sets(ddasp_bin, [example], "--all-atoms", "--method", method)
    assert got == clingo_answer_sets([example])
    projected = ddasp_answer_sets(ddasp_bin, [example], "--method", method)
    assert set(projected) == set(clingo_answer_sets([example], project=True))


OPT_PROGRAMS = {
    "simple": "{ a; b; c }. :- not a, not b, not c. #minimize{ 1@1 : a; 2@1 : b; 5@0 : not c }. #show a/0. #show b/0. #show c/0.",
    "vertex_cover": """
        edge(1,2). edge(2,3). edge(3,4). edge(4,1). edge(1,3).
        { in(X) : edge(X,_) ; in(Y) : edge(_,Y) }.
        :- edge(X,Y), not in(X), not in(Y).
        #minimize{ 1,X : in(X) }.
        #show in/1.
    """,
    "weighted_hidden": """
        { p(1..4) }.
        w(X,X*X) :- p(X).
        :- #count{ X : p(X) } < 2.
        #minimize{ W,X : w(X,W) }.
        #show p/1.
    """,
    "two_levels": """
        { p(1..3) }. { q(1..3) }.
        :- p(X), q(X).
        :- #count{ X : p(X) } < 1.
        #minimize{ 1@2,X : p(X) }.
        #minimize{ -1@1,X : q(X) }.
        #show p/1. #show q/1.
    """,
    "unsat": "a. :- a. #minimize{ 1 : a }.",
}


@pytest.mark.parametrize("name", sorted(OPT_PROGRAMS))
def test_optimization_matches_clingo(ddasp_bin, tmp_path, name):
    path = tmp_path / f"{name}.lp"
    path.write_text(OPT_PROGRAMS[name])
    assert ddasp_answer_sets(ddasp_bin, [path], "--opt", "--all-atoms") == clingo_answer_sets([path], optimize=True)
    assert set(ddasp_answer_sets(ddasp_bin, [path], "--opt")) == set(
        clingo_answer_sets([path], project=True, optimize=True))
    out, _ = run_ddasp(ddasp_bin, [path], "--opt")
    optimum = [int(x) for x in out.split("optimum:")[1].split()] if "optimum:" in out else None
    assert optimum == clingo_optimum([path])


def lex_within(cost, bound, relation):
    """Is the cost vector within the bound, compared lexicographically on the levels the bound lists?"""
    prefix, bound = tuple(cost[:len(bound)]), tuple(bound)
    return {"le": prefix <= bound, "ge": prefix >= bound, "eq": prefix == bound}[relation]


def cost_bound_cases(costs):
    """Bound vectors to try on a program whose answer sets have the given (sorted, distinct) cost vectors."""
    if not costs:
        return [(0,)]
    picked = [costs[0], costs[len(costs) // 2], costs[-1]]
    cases = set(picked)
    cases.update(c[:1] for c in picked)  # bounds on the first level only
    cases.add(tuple(k + 1 for k in costs[len(costs) // 2]))  # a vector no answer set may have
    return sorted(cases)


@pytest.mark.parametrize("name", sorted(OPT_PROGRAMS))
def test_cost_bounds_match_clingo(ddasp_bin, tmp_path, name):
    path = tmp_path / f"{name}.lp"
    path.write_text(OPT_PROGRAMS[name])
    models = clingo_costed_answer_sets([path])
    costs = sorted({c for _, c in models})
    for bound in cost_bound_cases(costs):
        text = ",".join(str(k) for k in bound)
        for relation in ("le", "ge", "eq"):
            expected = Counter(s for s, c in models if lex_within(c, bound, relation))
            got = ddasp_answer_sets(ddasp_bin, [path], f"--cost-{relation}", text, "--all-atoms")
            assert got == expected, (bound, relation)
            projected = ddasp_answer_sets(ddasp_bin, [path], f"--cost-{relation}", text)
            assert set(projected) == set(expected), (bound, relation)
        # --cost-le is clingo's --opt-mode=enum,K,...
        assert ddasp_answer_sets(ddasp_bin, [path], "--cost-le", text, "--all-atoms") == clingo_answer_sets(
            [path], bound=bound)
    if costs:  # both bounds combine: the answer sets between the smallest and the median cost vector
        lo, hi = costs[0], costs[len(costs) // 2]
        expected = Counter(s for s, c in models if lex_within(c, lo, "ge") and lex_within(c, hi, "le"))
        got = ddasp_answer_sets(ddasp_bin, [path], "--cost-ge", ",".join(map(str, lo)), "--cost-le",
                                ",".join(map(str, hi)), "--all-atoms")
        assert got == expected


@pytest.mark.parametrize("name", sorted(OPT_PROGRAMS))
def test_relative_cost_bounds(ddasp_bin, tmp_path, name):
    path = tmp_path / f"{name}.lp"
    path.write_text(OPT_PROGRAMS[name])
    models = clingo_costed_answer_sets([path])
    optimum = clingo_optimum([path])
    if optimum is None:  # inconsistent: nothing to keep and no optimum to print
        assert ddasp_answer_sets(ddasp_bin, [path], "--cost-le", "+1") == Counter()
        out, _ = run_ddasp(ddasp_bin, [path], "--cost-le", "+1")
        assert "optimum:" not in out
        return
    levels = len(optimum)
    for slack in ([0], [1], [0] * levels, [2] * levels, [1, 0][:levels]):
        bound = tuple(o + d for o, d in zip(optimum, slack))
        text = ",".join(f"+{d}" for d in slack)
        for relation in ("le", "ge", "eq"):
            expected = Counter(s for s, c in models if lex_within(c, bound, relation))
            got = ddasp_answer_sets(ddasp_bin, [path], f"--cost-{relation}", text, "--all-atoms")
            assert got == expected, (text, relation)
        out, _ = run_ddasp(ddasp_bin, [path], "--cost-le", text)  # a relative bound prints the optimum
        assert [int(x) for x in out.split("optimum:")[1].split()] == optimum
    # +0 on every level is --opt; an absolute bound prints no optimum; absolute and relative entries mix.
    full = ",".join("+0" for _ in optimum)
    assert ddasp_answer_sets(ddasp_bin, [path], "--cost-eq", full, "--all-atoms") == clingo_answer_sets(
        [path], optimize=True)
    out, _ = run_ddasp(ddasp_bin, [path], "--cost-le", ",".join(map(str, optimum)))
    assert "optimum:" not in out
    if levels > 1:
        mixed = (optimum[0] + 1, optimum[1])
        expected = Counter(s for s, c in models if lex_within(c, mixed, "le"))
        assert ddasp_answer_sets(ddasp_bin, [path], "--cost-le", f"{optimum[0] + 1},+0", "--all-atoms") == expected


def expected_histogram(models, project, keep=lambda cost: True):
    """``--cost-histogram`` lines as (cost vector, count) pairs, from clingo's costed answer sets.

    With ``project`` every shown set counts once, at the smallest cost vector of the
    answer sets kept for it; ``keep`` restricts the answer sets first (a cost bound).
    """
    kept = [(shown, cost) for shown, cost in models if keep(cost)]
    if project:
        smallest = {}
        for shown, cost in kept:
            if shown not in smallest or cost < smallest[shown]:
                smallest[shown] = cost
        costs = list(smallest.values())
    else:
        costs = [cost for _, cost in kept]
    return sorted(Counter(costs).items())


def parse_histogram(out):
    pairs = [line.split() for line in out.splitlines()]
    return [(tuple(int(k) for k in cost.split(",")), int(count)) for cost, count in pairs]


@pytest.mark.parametrize("name", sorted(OPT_PROGRAMS))
def test_cost_histogram_matches_clingo(ddasp_bin, tmp_path, name):
    path = tmp_path / f"{name}.lp"
    path.write_text(OPT_PROGRAMS[name])
    models = clingo_costed_answer_sets([path])
    out, _ = run_ddasp(ddasp_bin, [path], "-q", "--cost-histogram", "--all-atoms")
    assert parse_histogram(out) == expected_histogram(models, project=False)
    out, _ = run_ddasp(ddasp_bin, [path], "-q", "--cost-histogram")
    hist = parse_histogram(out)
    assert hist == expected_histogram(models, project=True)
    # The counts add up to the number of answer sets, and cumulatively to the count of --cost-le.
    count_out, _ = run_ddasp(ddasp_bin, [path], "-q", "--count")
    assert sum(n for _, n in hist) == int(count_out)
    seen = 0
    for cost, n in hist:
        seen += n
        upto, _ = run_ddasp(ddasp_bin, [path], "-q", "--count", "--cost-le", ",".join(map(str, cost)))
        assert int(upto) == seen
    if hist:  # the histogram describes the result: one line with --opt, the sets within a bound otherwise
        out, _ = run_ddasp(ddasp_bin, [path], "-q", "--opt", "--cost-histogram")
        assert parse_histogram(out) == [(tuple(clingo_optimum([path])), hist[0][1])]
        mid = hist[len(hist) // 2][0]
        out, _ = run_ddasp(ddasp_bin, [path], "-q", "--cost-histogram", "--cost-ge", ",".join(map(str, mid)))
        assert parse_histogram(out) == expected_histogram(models, True, lambda c: lex_within(c, mid, "ge"))


def expected_by_cost(models, project):
    """``--by-cost`` lines as a multiset of (cost vector, shown set).

    With ``project`` every shown set appears once, at the smallest cost vector of
    the answer sets projecting to it; otherwise every answer set appears.
    """
    if project:
        smallest = {}
        for shown, cost in models:
            if shown not in smallest or cost < smallest[shown]:
                smallest[shown] = cost
        return Counter((cost, shown) for shown, cost in smallest.items())
    return Counter((cost, shown) for shown, cost in models)


def parse_by_cost(out):
    rows = []
    for line in out.splitlines():
        cost, sep, names = line.partition(":")
        assert sep == ":", line
        rows.append((tuple(int(k) for k in cost.split(",")), frozenset(names.split())))
    return rows


@pytest.mark.parametrize("name", sorted(OPT_PROGRAMS))
def test_by_cost_matches_clingo(ddasp_bin, tmp_path, name):
    path = tmp_path / f"{name}.lp"
    path.write_text(OPT_PROGRAMS[name])
    models = clingo_costed_answer_sets([path])
    for extra in ((), ("--all-atoms",)):
        out, _ = run_ddasp(ddasp_bin, [path], "-q", "--by-cost", *extra)
        rows = parse_by_cost(out)
        assert [cost for cost, _ in rows] == sorted(cost for cost, _ in rows)
        assert Counter(rows) == expected_by_cost(models, project=not extra)
        out3, _ = run_ddasp(ddasp_bin, [path], "-q", "--by-cost", "--limit", "3", *extra)  # the 3 best
        assert out3.splitlines() == out.splitlines()[:3]
        same, _ = run_ddasp(ddasp_bin, [path], "-q", "--enumerate", "--by-cost", *extra)
        assert same == out
    costs = sorted({cost for _, cost in models})
    if costs:  # --opt and the bounds restrict the listing; the summary lines come first
        out, _ = run_ddasp(ddasp_bin, [path], "--opt", "--by-cost")
        lines = out.splitlines()
        assert lines[0].startswith("answer sets:") and lines[1].startswith("optimum:")
        best = parse_by_cost("\n".join(lines[2:]))
        assert {cost for cost, _ in best} == {tuple(clingo_optimum([path]))}
        assert len(best) == int(lines[0].split()[-1])
        mid = costs[len(costs) // 2]
        out, _ = run_ddasp(ddasp_bin, [path], "-q", "--by-cost", "--cost-ge", ",".join(map(str, mid)))
        kept = [(shown, cost) for shown, cost in models if lex_within(cost, mid, "ge")]
        assert Counter(parse_by_cost(out)) == expected_by_cost(kept, project=True)


def test_cost_bound_errors(ddasp_bin, tmp_path):
    path = tmp_path / "simple.lp"
    path.write_text(OPT_PROGRAMS["simple"])  # two priority levels (1 and 0)

    def run(*args):
        return subprocess.run([str(ddasp_bin), "--gringo", GRINGO_CMD, *args, str(path)], capture_output=True, text=True)

    for args in (["--opt", "--cost-le", "1"], ["--cost-eq", "1", "--cost-ge", "0"], ["--cost-le", "x"],
                 ["--cost-le", "+-1"], ["--cost-le", ""], ["--cost-le", "1,"], ["--cost-ge", " 1"]):
        proc = run(*args)
        assert proc.returncode == 2 and "cost" in proc.stderr, args
    proc = run("--cost-le", "1,2,3")
    assert proc.returncode == 1 and "only 2 #minimize priority level(s)" in proc.stderr
    plain = tmp_path / "plain.lp"
    plain.write_text("{ a }.")
    for option in (["--cost-le", "1"], ["--cost-histogram"], ["--by-cost"]):
        proc = subprocess.run([str(ddasp_bin), "--gringo", GRINGO_CMD, *option, str(plain)],
                              capture_output=True, text=True)
        assert proc.returncode == 1 and "#minimize" in proc.stderr, option


OPT_EXAMPLES = [p for p in STANDALONE_EXAMPLES if "#minimize" in p.read_text()]


@pytest.mark.parametrize("example", OPT_EXAMPLES, ids=lambda p: p.stem)
def test_optimization_example_matches_clingo(ddasp_bin, example):
    assert set(ddasp_answer_sets(ddasp_bin, [example], "--opt")) == set(
        clingo_answer_sets([example], project=True, optimize=True))
    out, _ = run_ddasp(ddasp_bin, [example], "--opt")
    optimum = [int(x) for x in out.split("optimum:")[1].split()]
    assert optimum == clingo_optimum([example])


def test_exported_zdd_round_trips_through_kyotodd(ddasp_bin, tmp_path):
    example = EXAMPLES / "coloring.lp"
    zdd_file = tmp_path / "out.zdd"
    atoms_file = tmp_path / "atoms.txt"
    out, _ = run_ddasp(ddasp_bin, [example], "-q", "--count", "-o", zdd_file, "--atoms", atoms_file)
    count = int(out.strip())

    # The atoms table gives the DD level of each atom; in a fresh process
    # variable numbers equal levels, so the imported ZDD can be decoded.
    level_to_name = {}
    for line in atoms_file.read_text().splitlines():
        if line.startswith("#"):
            continue
        var, level, atom, name = line.split(maxsplit=3)
        level_to_name[int(level)] = name

    zdd = kyotodd.ZDD.import_binary_str(zdd_file.read_bytes())
    assert zdd.exact_count == count
    family = set()
    for s in zdd.enumerate():
        names = (level_to_name[kyotodd.to_level(v)] for v in s)
        family.add(frozenset(n for n in names if not n.startswith("__atom")))
    assert family == set(clingo_answer_sets([example]))


def test_enumerate_limit_and_order(ddasp_bin, tmp_path):
    path = tmp_path / "p.lp"
    path.write_text("{ p(1..10) }. #show p/1.")
    out, _ = run_ddasp(ddasp_bin, [path], "-q", "--enumerate", "--limit", "3")
    assert len(out.splitlines()) == 3
    out, _ = run_ddasp(ddasp_bin, [path], "-q", "--enumerate", "--limit", "0")
    assert len(out.splitlines()) == 1024
    # Atoms of a line come in variable order (= name order), p(2) before p(10).
    out, _ = run_ddasp(ddasp_bin, [path], "-q", "--enumerate")
    full = [line.split() for line in out.splitlines() if len(line.split()) == 10]
    assert full == [[f"p({i})" for i in range(1, 11)]]
    proc = subprocess.run([str(ddasp_bin), "--enumerate", "--limit", "abc", str(path)], capture_output=True, text=True)
    assert proc.returncode != 0 and "--limit" in proc.stderr


def test_grounder_failure_is_reported(ddasp_bin, tmp_path):
    proc = subprocess.run([str(ddasp_bin), "--gringo", GRINGO_CMD, str(tmp_path / "missing.lp")],
                          capture_output=True, text=True)
    assert proc.returncode != 0
    assert "grounder" in proc.stderr
    aspif = tmp_path / "p.aspif"
    aspif.write_text("asp 1 0 0\n0\n")
    proc = subprocess.run([str(ddasp_bin), str(aspif), str(tmp_path / "x.lp")], capture_output=True, text=True)
    assert proc.returncode != 0 and "mix" in proc.stderr


def test_atoms_file_minimize_lines(ddasp_bin, tmp_path):
    path = tmp_path / "p.lp"
    path.write_text("{ a; b }. c :- a. #minimize{ 1 : c; 2 : b }. #show a/0. #show b/0.")
    atoms_file = tmp_path / "atoms.txt"
    run_ddasp(ddasp_bin, [path], "-q", "--all-atoms", "--atoms", atoms_file)
    lines = atoms_file.read_text().splitlines()
    level_of = {name: int(level) for var, level, atom, name in
                (l.split(maxsplit=3) for l in lines if not l.startswith("#"))}
    (mini,) = [l for l in lines if l.startswith("# minimize")]
    assert "incomplete" not in mini
    weights = dict(tuple(map(int, w.split(":"))) for w in mini.split("offset 0")[1].split())
    assert weights == {level_of["__atom3"]: 1, level_of["b"]: 2}
    run_ddasp(ddasp_bin, [path], "-q", "--atoms", atoms_file)
    (mini,) = [l for l in atoms_file.read_text().splitlines() if l.startswith("# minimize")]
    assert "incomplete: 1 hidden" in mini


def test_order_file_and_bdd_export(ddasp_bin, tmp_path):
    path = tmp_path / "p.lp"
    # Atoms: f = 1, p(1..3) = 2..4, q = 5; `#show w : f.` gets the condition `not __atom6` (no rule).
    path.write_text("{ p(1..3) }. q :- p(1). f. #show p/1. #show w : f. #show t : p(2).")
    order = tmp_path / "order.txt"
    order.write_text("# comment\np(3)\n__atom5\nnope\n__atom6\nt\n")
    atoms_file = tmp_path / "atoms.txt"
    bdd_file = tmp_path / "out.bdd"
    _, err = run_ddasp(ddasp_bin, [path], "-q", "--all-atoms", "--order", order, "--atoms", atoms_file,
                       "-o", bdd_file, "--bdd")
    assert "unknown atom 'nope'" in err
    assert "unknown atom '__atom6'" in err  # an atom of no rule, below the largest atom id
    rows = [l.split(maxsplit=3) for l in atoms_file.read_text().splitlines() if not l.startswith("#")]
    names_top_down = [name for var, level, atom, name in rows]
    assert names_top_down == ["p(3)", "__atom5", "p(2)", "p(1)", "__atom1"]
    bdd = kyotodd.BDD.import_binary_str(bdd_file.read_bytes())
    assert bdd.count(5) == 8  # characteristic function of 8 answer sets over 5 variables


def test_external_values_flag(ddasp_bin, tmp_path):
    path = tmp_path / "ext.lp"
    path.write_text("#external a. [true] #external b. [false] #external c. d :- a. "
                    "#show a/0. #show b/0. #show c/0. #show d/0.")
    assert sum(ddasp_answer_sets(ddasp_bin, [path]).values()) == 8
    got = ddasp_answer_sets(ddasp_bin, [path], "--external-values")
    assert set(got) == {frozenset({"a", "d"})}  # `#external c.` is declared false by gringo


PROJECT_PROGRAM = """
    { a; b; c }. d :- a, b.
    #show a/0. #show b/0. #show t : not c.
    #project a. #project d.
"""


def clingo_projected(files):
    ctl = clingo.Control(["0", "--project"])
    for f in files:
        ctl.load(str(f))
    ctl.ground([("base", [])])
    with ctl.solve(yield_=True) as handle:
        return [frozenset(str(s) for s in m.symbols(shown=True)) for m in handle]


def test_project_statements(ddasp_bin, tmp_path):
    path = tmp_path / "project.lp"
    path.write_text(PROJECT_PROGRAM)
    assert len(ddasp_answer_sets(ddasp_bin, [path])) == 8  # #project is ignored by default
    out, err = run_ddasp(ddasp_bin, [path], "--enumerate", "-q", "--project")
    got = Counter(frozenset(line.split()) for line in out.splitlines())
    assert got == Counter({frozenset(): 1, frozenset({"a"}): 1, frozenset({"a", "__atom4"}): 1})
    assert "#project" in err and "not shown" in err
    # clingo --project gives one representative per projection class, i.e. the same number of sets.
    assert len(clingo_projected([path])) == 3
    proc = subprocess.run([str(ddasp_bin), "--gringo", GRINGO_CMD, "--project", "--all-atoms", str(path)],
                          capture_output=True, text=True)
    assert proc.returncode == 2 and "cannot be combined" in proc.stderr


def consequences_from(answer_sets):
    """(brave, cautious, frequencies) of a multiset of answer sets."""
    sets = list(answer_sets.elements())
    brave = frozenset().union(*sets) if sets else frozenset()
    cautious = frozenset.intersection(*sets) if sets else frozenset()
    freq = Counter()
    for s in sets:
        freq.update(s)
    return brave, cautious, freq


@pytest.mark.parametrize("name", sorted(INLINE_PROGRAMS))
def test_consequences_match_clingo(ddasp_bin, tmp_path, name):
    path = tmp_path / f"{name}.lp"
    path.write_text(INLINE_PROGRAMS[name])
    brave, cautious, _ = consequences_from(clingo_answer_sets([path], project=True))
    out, _ = run_ddasp(ddasp_bin, [path], "--brave", "--cautious", "-q")
    lines = out.split("\n")
    assert frozenset(lines[0].split()) == brave
    assert frozenset(lines[1].split()) == cautious
    # Frequencies over all answer sets (no projection) are the multiset counts.
    _, _, freq = consequences_from(clingo_answer_sets([path]))
    out, _ = run_ddasp(ddasp_bin, [path], "--frequencies", "--all-atoms", "-q")
    got = {line.split()[0]: int(line.split()[1]) for line in out.splitlines()}
    assert {k: v for k, v in got.items() if v} == dict(freq)
    assert all(not k.startswith("__atom") for k in got)


def test_consequences_output_format(ddasp_bin, tmp_path):
    path = tmp_path / "c.lp"
    path.write_text("f. { a; b }. c :- a. :- not a, not b. #show f/0. #show a/0. #show b/0. #show c/0.")
    out, _ = run_ddasp(ddasp_bin, [path], "--brave", "--cautious", "--frequencies")
    assert out.splitlines() == ["answer sets: 3", "f a b c", "f", "f 3", "a 2", "b 2", "c 2"]


def test_node_max(ddasp_bin, tmp_path):
    path = tmp_path / "big.lp"
    path.write_text("{ p(1..60) }. :- #count{ X : p(X) } != 30. #show p/1.")
    out, _ = run_ddasp(ddasp_bin, [path], "-q", "--count")
    assert int(out) == 118264581564861424  # C(60, 30): unlimited by default
    proc = subprocess.run([str(ddasp_bin), "--gringo", GRINGO_CMD, "--node-max", "2000", str(path)],
                          capture_output=True, text=True)
    assert proc.returncode == 1
    assert "node limit of 2000" in proc.stderr
    proc = subprocess.run([str(ddasp_bin), "--gringo", GRINGO_CMD, "--node-max", "0", str(path)],
                          capture_output=True, text=True)
    assert proc.returncode == 2 and "positive integer" in proc.stderr
    out, _ = run_ddasp(ddasp_bin, [path], "-q", "--count", "--node-max", "10000000")
    assert int(out) == 118264581564861424


def test_constants_are_passed_to_the_grounder(ddasp_bin, tmp_path):
    path = tmp_path / "p.lp"
    path.write_text("#const n = 2. #const m = 0. { p(1..n) }. :- #count{ X : p(X) } < m. #show p/1.")
    out, _ = run_ddasp(ddasp_bin, [path], "-q", "--count")
    assert out.strip() == "4"
    out, _ = run_ddasp(ddasp_bin, [path], "-q", "--count", "-c", "n=3")  # -c overrides #const
    assert out.strip() == "8"
    out, _ = run_ddasp(ddasp_bin, [path], "-q", "--count", "-c", "n=3", "--const", "m=3")
    assert out.strip() == "1"
    for bad in ("n", "=3", "n="):
        proc = subprocess.run([str(ddasp_bin), "--gringo", GRINGO_CMD, "-c", bad, str(path)],
                              capture_output=True, text=True)
        assert proc.returncode != 0 and "NAME=VALUE" in proc.stderr
    aspif = tmp_path / "p.aspif"
    aspif.write_text("asp 1 0 0\n0\n")
    proc = subprocess.run([str(ddasp_bin), "-c", "n=3", str(aspif)], capture_output=True, text=True)
    assert proc.returncode != 0 and "aspif" in proc.stderr


def test_json_output(ddasp_bin, tmp_path):
    path = tmp_path / "p.lp"
    path.write_text('{ a; b; c }. :- a, b. #minimize { 1,a : a; 2,b : b; 1,c : c }. '
                    '#show a/0. #show b/0. #show c/0. #show s("x\\"y") : c.')
    out, _ = run_ddasp(ddasp_bin, [path], "--json", "--enumerate", "--brave", "--cautious", "--frequencies",
                       "--cost-histogram", "--stats")
    data = json.loads(out)
    text, _ = run_ddasp(ddasp_bin, [path], "-q", "--enumerate")
    sets = sorted(sorted(line.split()) for line in text.splitlines())
    assert sorted(sorted(s) for s in data["answer_sets"]) == sets
    assert data["count"] == len(sets) == 6
    assert "optimum" not in data
    assert set(data["brave"]) == {"a", "b", "c", 's("x\\"y")'}  # the quotes of the name are escaped
    assert data["cautious"] == []
    freq, _ = run_ddasp(ddasp_bin, [path], "-q", "--frequencies")
    assert data["frequencies"] == {l.rsplit(maxsplit=1)[0]: int(l.rsplit(maxsplit=1)[1]) for l in freq.splitlines()}
    hist, _ = run_ddasp(ddasp_bin, [path], "-q", "--cost-histogram")
    assert data["cost_histogram"] == [{"cost": [int(k) for k in l.split()[0].split(",")], "count": int(l.split()[1])}
                                      for l in hist.splitlines()]
    assert data["stats"]["rules"] >= 1 and data["stats"]["shown_atoms"] == 3 and "compile_time" in data["stats"]
    assert list(data) == ["count", "brave", "cautious", "frequencies", "cost_histogram", "stats", "answer_sets"]
    # --by-cost gives {cost, atoms} objects, best first; --limit applies; -q and --count change nothing.
    out, _ = run_ddasp(ddasp_bin, [path], "--json", "--by-cost", "--limit", "2", "--cost-le", "+1", "-q", "--count")
    data = json.loads(out)
    assert data["count"] == 3 and data["optimum"] == [0]
    assert data["answer_sets"][0] == {"cost": [0], "atoms": []}
    assert data["answer_sets"][1]["cost"] == [1] and len(data["answer_sets"]) == 2
    # An inconsistent program and an empty enumeration are valid JSON too.
    path.write_text("a. :- a. #show a/0.")
    out, _ = run_ddasp(ddasp_bin, [path], "--json", "--enumerate", "--cautious")
    assert json.loads(out) == {"count": 0, "cautious": [], "answer_sets": []}
