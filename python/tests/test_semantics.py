"""Supported and classical models checked against a brute-force oracle.

The oracle evaluates the ground program (as gringo emits it, auxiliary atoms
included) over every assignment of its atoms, so it also pins down what the two
semantics mean in DDASP: supported = models of Clark's completion (a true atom
needs a rule with it in the head, a true body and the other head atoms false;
free #external atoms need nothing), classical = models of the rules read as
implications, choice rules imposing nothing.
"""

import random
from collections import Counter

import clingo
import pytest

ddasp = pytest.importorskip("ddasp")

from test_random_programs import ATOMS, SEED, random_program  # noqa: E402

N_PROGRAMS = 150
MAX_ATOMS = 13  # 2^13 assignments per program


def _quiet(code, message):
    pass


def ground_aspif(text):
    """The ground program of ``text`` as aspif text (through the clingo observer)."""
    prog = ddasp.Program()
    ctl = clingo.Control(logger=_quiet)
    ctl.register_observer(ddasp.Observer(prog))
    ctl.add("base", [], text)
    ctl.ground([("base", [])])
    return str(prog)


def parse_aspif(aspif):
    """``(rules, names, externals)``: rules as (choice, head, bound, [(lit, weight)])."""
    rules, names, externals = [], {}, set()
    for line in aspif.splitlines():
        t = line.split()
        if not t or t[0] not in ("1", "4", "5"):
            continue
        if t[0] == "1":
            n = int(t[2])
            head = [int(x) for x in t[3:3 + n]]
            rest = t[3 + n:]
            if rest[0] == "0":
                m = int(rest[1])
                lits = [(int(x), 1) for x in rest[2:2 + m]]
                bound = m
            else:
                bound, m = int(rest[1]), int(rest[2])
                vals = rest[3:3 + 2 * m]
                lits = [(int(vals[2 * i]), int(vals[2 * i + 1])) for i in range(m)]
            rules.append((t[1] == "1", head, bound, lits))
        elif t[0] == "4":
            k = int(t[3])
            lits = [int(x) for x in t[4:4 + k]]
            if k == 1 and lits[0] > 0:
                names[lits[0]] = t[2]
        else:
            externals.add(int(t[1]))
    return rules, names, externals


def body_true(X, bound, lits):
    return sum(w for lit, w in lits if (abs(lit) in X) == (lit > 0)) >= bound


def brute_force_models(rules, externals, semantics):
    atoms = sorted({a for _, head, _, _ in rules for a in head}
                   | {abs(lit) for _, _, _, lits in rules for lit, _ in lits} | externals)
    free = externals - {a for _, head, _, _ in rules for a in head}
    family = []
    for mask in range(1 << len(atoms)):
        X = frozenset(a for i, a in enumerate(atoms) if mask >> i & 1)
        ok = all(choice or not body_true(X, bound, lits) or any(a in X for a in head)
                 for choice, head, bound, lits in rules)
        if ok and semantics == "supported":
            ok = all(a in free or any(a in head and body_true(X, bound, lits) and
                                      (choice or all(b not in X for b in head if b != a))
                                      for choice, head, bound, lits in rules)
                     for a in X)
        if ok:
            family.append(X)
    return atoms, family


@pytest.mark.parametrize("semantics", ["supported", "classical"])
def test_random_programs_match_brute_force(semantics):
    checked = 0
    for i in range(N_PROGRAMS):
        text = random_program(random.Random(SEED + i))
        text = "\n".join(l for l in text.splitlines() if not l.startswith("#show")) + "\n"
        text += "".join(f"#show {a}/0.\n" for a in ATOMS)
        rules, names, externals = parse_aspif(ground_aspif(text))
        atoms, family = brute_force_models(rules, externals, semantics)
        if len(atoms) > MAX_ATOMS:
            continue
        expected = Counter(frozenset(names[a] for a in X if a in names) for X in family)
        res = ddasp.compile_text(text, semantics=semantics, project=False)
        got = Counter(res.answer_sets())
        if got != expected or res.count != len(family):
            pytest.fail(f"random program {i} differs from the brute-force {semantics} models:\n{text}"
                        f"oracle: {sorted(sorted(s) for s in expected)}\nddasp:  {sorted(sorted(s) for s in got)} "
                        f"(count {res.count}, oracle {len(family)})")
        checked += 1
    assert checked > 0


def test_semantics_argument():
    text = "{ c }. a :- b. b :- a. a :- c. #show a/0. #show b/0. #show c/0."
    assert ddasp.compile_text(text).count == 2
    assert ddasp.compile_text(text, semantics="supported").count == 3
    assert ddasp.compile_text(text, semantics="classical").count == 3
    assert ddasp.compile_text(text, semantics="supported").stats["aux_vars"] == 0
    with pytest.raises(ValueError):
        ddasp.compile_text(text, semantics="minimal")
