#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <vector>

#include "ddasp/aspif_reader.h"
#include "ddasp/compiler.h"
#include "ddasp/dd_util.h"

using namespace ddasp;
using kyotodd::BDD;
using kyotodd::ZDD;
using kyotodd::bddvar;

namespace {

struct InitKyotoDD {
    InitKyotoDD() { kyotodd::bddinit(1024); }
};
InitKyotoDD init_once;

typedef std::set<std::set<Atom> > Family;

Family family_of(const CompileResult& res) {
    Family fam;
    std::vector<std::vector<bddvar> > sets = res.zdd.enumerate();
    for (size_t i = 0; i < sets.size(); ++i) {
        std::set<Atom> s;
        for (size_t j = 0; j < sets[i].size(); ++j) s.insert(res.var_to_atom[sets[i][j]]);
        fam.insert(s);
    }
    return fam;
}

Family compile_family(const std::string& aspif, const CompileOptions& opts = CompileOptions()) {
    Program p = read_aspif_string(aspif);
    return family_of(compile(p, opts));
}

std::set<Atom> S() { return std::set<Atom>(); }
std::set<Atom> S(int a) { std::set<Atom> s; s.insert(a); return s; }
std::set<Atom> S(int a, int b) { std::set<Atom> s = S(a); s.insert(b); return s; }
std::set<Atom> S(int a, int b, int c) { std::set<Atom> s = S(a, b); s.insert(c); return s; }

Family F(const std::set<Atom>& a) { Family f; f.insert(a); return f; }
Family F(const std::set<Atom>& a, const std::set<Atom>& b) { Family f = F(a); f.insert(b); return f; }
Family F(const std::set<Atom>& a, const std::set<Atom>& b, const std::set<Atom>& c) { Family f = F(a, b); f.insert(c); return f; }

}  // namespace

TEST(DdUtil, ThresholdBdd) {
    bddvar a = kyotodd::bddnewvar(), b = kyotodd::bddnewvar(), c = kyotodd::bddnewvar();
    std::vector<BDD> terms;
    terms.push_back(BDD::prime(a));
    terms.push_back(BDD::prime(b));
    terms.push_back(BDD::prime(c));
    std::vector<long long> w;
    w.push_back(2);
    w.push_back(3);
    w.push_back(-1);
    // 2a + 3b - c >= 3
    BDD t = threshold_bdd(terms, w, 3);
    for (int va = 0; va < 2; ++va)
        for (int vb = 0; vb < 2; ++vb)
            for (int vc = 0; vc < 2; ++vc) {
                BDD assign = (va ? BDD::prime(a) : ~BDD::prime(a)) & (vb ? BDD::prime(b) : ~BDD::prime(b)) &
                             (vc ? BDD::prime(c) : ~BDD::prime(c));
                bool expected = 2 * va + 3 * vb - vc >= 3;
                EXPECT_EQ(expected, !(assign & t).is_zero()) << va << vb << vc;
            }
    EXPECT_TRUE(threshold_bdd(terms, w, 0).is_one() == false);  // -c can make it negative
    EXPECT_TRUE(threshold_bdd(terms, w, -1).is_one());
    EXPECT_TRUE(threshold_bdd(terms, w, 6).is_zero());
    EXPECT_TRUE(threshold_bdd(std::vector<BDD>(), std::vector<long long>(), 0).is_one());
    EXPECT_TRUE(threshold_bdd(std::vector<BDD>(), std::vector<long long>(), 1).is_zero());
}

TEST(DdUtil, BddToZddOverRejectsForeignVariable) {
    bddvar a = kyotodd::bddnewvar(), b = kyotodd::bddnewvar();
    BDD f = BDD::prime(a) & BDD::prime(b);
    EXPECT_THROW(bdd_to_zdd_over(f, std::vector<bddvar>(1, a)), std::invalid_argument);
    ZDD z = bdd_to_zdd_over(f.exist(b), std::vector<bddvar>(1, a));
    EXPECT_EQ(1u, z.card());
}

TEST(Compiler, Fact) {
    EXPECT_EQ(F(S(1)), compile_family("asp 1 0 0\n1 0 1 1 0 0\n0\n"));
}

TEST(Compiler, EmptyProgramHasOneEmptyAnswerSet) {
    EXPECT_EQ(F(S()), compile_family("asp 1 0 0\n0\n"));
}

TEST(Compiler, ChoiceAndDerivedAtom) {
    // {a}. b :- a.
    EXPECT_EQ(F(S(), S(1, 2)), compile_family("asp 1 0 0\n1 1 1 1 0 0\n1 0 1 2 0 1 1\n0\n"));
}

TEST(Compiler, ChoiceWithConstraint) {
    // {a;b}. :- a, b.
    EXPECT_EQ(F(S(), S(1), S(2)), compile_family("asp 1 0 0\n1 1 2 1 2 0 0\n1 0 0 0 2 1 2\n0\n"));
}

TEST(Compiler, EvenLoopThroughNegation) {
    // a :- not b. b :- not a.
    EXPECT_EQ(F(S(1), S(2)), compile_family("asp 1 0 0\n1 0 1 1 0 1 -2\n1 0 1 2 0 1 -1\n0\n"));
}

TEST(Compiler, PositiveLoopIsUnfounded) {
    // a :- b. b :- a.   (completion would also allow {a,b})
    EXPECT_EQ(F(S()), compile_family("asp 1 0 0\n1 0 1 1 0 1 2\n1 0 1 2 0 1 1\n0\n"));
}

TEST(Compiler, PositiveLoopWithExternalSupport) {
    // {c}. a :- b. b :- a. a :- c.
    EXPECT_EQ(F(S(), S(1, 2, 3)),
              compile_family("asp 1 0 0\n1 1 1 3 0 0\n1 0 1 1 0 1 2\n1 0 1 2 0 1 1\n1 0 1 1 0 1 3\n0\n"));
}

TEST(Compiler, Disjunction) {
    // a | b.
    EXPECT_EQ(F(S(1), S(2)), compile_family("asp 1 0 0\n1 0 2 1 2 0 0\n0\n"));
}

TEST(Compiler, NonHeadCycleFreeDisjunction) {
    // a | b.  a :- b.  b :- a.   -> only {a,b} (shifting would give no answer set)
    EXPECT_EQ(F(S(1, 2)), compile_family("asp 1 0 0\n1 0 2 1 2 0 0\n1 0 1 1 0 1 2\n1 0 1 2 0 1 1\n0\n"));
}

TEST(Compiler, WeightBodyCountConstraint) {
    // {a;b;c}.  aux :- 2 <= #count{a;b;c}.  :- not aux.
    Family fam = compile_family(
        "asp 1 0 0\n"
        "1 1 3 1 2 3 0 0\n"
        "1 0 1 4 1 2 3 1 1 2 1 3 1\n"
        "1 0 0 0 1 -4\n"
        "0\n");
    Family expected;
    expected.insert(S(1, 2, 4));
    expected.insert(S(1, 3, 4));
    expected.insert(S(2, 3, 4));
    std::set<Atom> all = S(1, 2, 3);
    all.insert(4);
    expected.insert(all);
    EXPECT_EQ(expected, fam);
}

TEST(Compiler, WeightBodyWithNegativeLiteral) {
    // {a;b}.  c :- 3 <= #sum{2:a; 2:not b}.   -> c iff a and not b
    Family fam = compile_family(
        "asp 1 0 0\n"
        "1 1 2 1 2 0 0\n"
        "1 0 1 3 1 3 2 1 2 -2 2\n"
        "0\n");
    Family expected;
    expected.insert(S());
    expected.insert(S(1, 3));
    expected.insert(S(2));
    expected.insert(S(1, 2));
    EXPECT_EQ(expected, fam);
}

TEST(Compiler, ExternalIsFree) {
    // #external a.  b :- a.
    EXPECT_EQ(F(S(), S(1, 2)), compile_family("asp 1 0 0\n5 1 0\n1 0 1 2 0 1 1\n0\n"));
}

TEST(Compiler, UnsupportedAtomInBodyOnly) {
    // a :- b.   (b has no rule)
    EXPECT_EQ(F(S()), compile_family("asp 1 0 0\n1 0 1 1 0 1 2\n0\n"));
}

TEST(Compiler, Inconsistent) {
    // a. :- a.
    EXPECT_TRUE(compile_family("asp 1 0 0\n1 0 1 1 0 0\n1 0 0 0 1 1\n0\n").empty());
}

TEST(Compiler, NamesAndVariableMaps) {
    Program p = read_aspif_string(
        "asp 1 0 0\n"
        "1 1 2 1 2 0 0\n"
        "4 1 a 1 1\n"
        "4 4 p(1) 1 2\n"
        "0\n");
    CompileResult res = compile(p);
    EXPECT_EQ("a", res.atom_name(1));
    EXPECT_EQ("p(1)", res.atom_name(2));
    EXPECT_EQ("__atom3", res.atom_name(3));
    ASSERT_EQ(2u, res.vars.size());
    EXPECT_EQ(1, res.var_to_atom[res.atom_to_var[1]]);
    EXPECT_EQ(2, res.var_to_atom[res.atom_to_var[2]]);
    EXPECT_EQ(4u, res.zdd.card());
    // Default order: atom 1 at the top level.
    EXPECT_GT(kyotodd::bddlevofvar(res.atom_to_var[1]), kyotodd::bddlevofvar(res.atom_to_var[2]));
    EXPECT_EQ(res.vars[0], res.atom_to_var[1]);
}

TEST(Compiler, FactNamesComeFromUnconditionalOutputs) {
    // a. {c}. #show a/0. #show c/0.   (gringo names the fact without a condition)
    Program p = read_aspif_string(
        "asp 1 0 0\n"
        "1 0 1 1 0 0\n"
        "1 1 1 2 0 0\n"
        "4 1 a 0\n"
        "4 1 c 1 2\n"
        "0\n");
    CompileResult res = compile(p);
    ASSERT_EQ(1u, res.fact_names.size());
    EXPECT_EQ("a", res.fact_names[0]);
    EXPECT_EQ(0u, res.names.count(1));
    EXPECT_EQ("c", res.atom_name(2));
    EXPECT_EQ(2u, res.zdd.card());
}

TEST(Compiler, CustomOrder) {
    Program p = read_aspif_string("asp 1 0 0\n1 1 3 1 2 3 0 0\n0\n");
    CompileOptions opts;
    opts.order.push_back(3);
    opts.order.push_back(1);
    CompileResult res = compile(p, opts);
    EXPECT_EQ(res.vars[0], res.atom_to_var[3]);
    EXPECT_EQ(res.vars[1], res.atom_to_var[1]);
    EXPECT_EQ(res.vars[2], res.atom_to_var[2]);
    EXPECT_EQ(8u, res.zdd.card());

    CompileOptions bad;
    bad.order.push_back(9);
    EXPECT_THROW(compile(p, bad), std::invalid_argument);
}
