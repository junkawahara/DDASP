"""Compare ddasp's answer sets with clingo's enumeration."""

import subprocess
from pathlib import Path

import kyotodd
import pytest

from conftest import EXAMPLES, GRINGO_CMD, clingo_answer_sets, clingo_optimum, ddasp_answer_sets, run_ddasp

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
