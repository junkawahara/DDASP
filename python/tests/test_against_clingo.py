"""Compare ddasp's answer sets with clingo's enumeration."""

from pathlib import Path

import kyotodd
import pytest

from conftest import EXAMPLES, clingo_answer_sets, ddasp_answer_sets, run_ddasp

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
    "inconsistent": "a. :- a.",
    "hidden_atoms": "{ a; b }. c :- a. #show c/0.",
    "nested_negation": "{ a }. b :- not not a. #show a/0. #show b/0.",
    "conditional_literal": "p(1..3). { q(X) : p(X) }. r :- q(X) : p(X). #show r/0. #show q/1.",
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


def test_inline_program_matches_clingo(ddasp_bin, inline_program):
    assert ddasp_answer_sets(ddasp_bin, [inline_program]) == clingo_answer_sets([inline_program])


@pytest.mark.parametrize("example", sorted(EXAMPLES.glob("*.lp")), ids=lambda p: p.stem)
def test_example_matches_clingo(ddasp_bin, example):
    assert ddasp_answer_sets(ddasp_bin, [example]) == clingo_answer_sets([example])


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
