"""The constraints are conjoined definitions first: an aggregate whose value is
derived from many shown atoms must not blow up the intermediate BDDs."""

import pytest

ddasp = pytest.importorskip("ddasp")


def test_count_over_shown_atoms_stays_small():
    n = 16
    prog = f"{{ in(1..{n}) }}. size(S) :- S = #count {{ V : in(V) }}. #show size/1. #show in/1."
    res = ddasp.compile_text(prog)
    assert res.count == 2**n
    sizes = {next(a for a in s if a.startswith("size")) for s in res.answer_sets()}
    assert sizes == {f"size({k})" for k in range(n + 1)}
    # Before the dependency order this took 11M nodes (and tens of seconds).
    assert res.stats["peak_nodes"] < 300_000


def test_count_over_stored_atoms_stays_small():
    base = ddasp.compile_text("{ in(1..16) }. #show in/1.").stored()
    res = ddasp.compile_text("size(S) :- S = #count { V : in(V) }. #show size/1.", dds=[base])
    assert res.count == 2**16
    assert res.stats["peak_nodes"] < 300_000


def test_cyclic_definitions_do_not_block_the_order():
    # a and b define each other (one dependency component); c uses them.
    res = ddasp.compile_text("{ s }. a :- b. b :- a. a :- s. c :- a, not s. :- c.")
    assert set(res.answer_sets()) == {frozenset(), frozenset({"s", "a", "b"})}
