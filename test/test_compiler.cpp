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

CompileOptions no_projection() {
    CompileOptions o;
    o.project_to_shown = false;
    return o;
}

/** Compile with both methods and both order heuristics; check they agree and return the family. */
Family compile_family(const std::string& aspif, const CompileOptions& opts = no_projection()) {
    Program p = read_aspif_string(aspif);
    Family result;
    bool first = true;
    for (int m = 0; m < 2; ++m) {
        for (int h = 0; h < 2; ++h) {
            CompileOptions o = opts;
            o.method = m == 0 ? Method::TwoCopy : Method::Scc;
            o.order_heuristic = h == 0 ? OrderHeuristic::AtomId : OrderHeuristic::NaturalName;
            Family f = family_of(compile(p, o));
            if (first) {
                result = f;
                first = false;
            } else {
                EXPECT_EQ(result, f) << "method " << m << " / heuristic " << h << " disagrees";
            }
        }
    }
    return result;
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
    EXPECT_EQ("1", z.exact_count().to_string());
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

TEST(Compiler, ExternalDefinedByARuleIsNotFree) {
    // #external e. {a}. e :- a.   (as in clingo, the rule overrides the declaration: no {e})
    EXPECT_EQ(F(S(), S(1, 2)), compile_family("asp 1 0 0\n1 1 1 1 0 0\n1 0 1 2 0 1 1\n5 2 2\n0\n"));
    // #external e. {e}.   (a choice rule defines e as well; the family is the same)
    EXPECT_EQ(F(S(), S(1)), compile_family("asp 1 0 0\n1 1 1 1 0 0\n5 1 0\n0\n"));
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
    CompileOptions by_id = no_projection();
    by_id.order_heuristic = OrderHeuristic::AtomId;
    CompileResult res = compile(p, by_id);
    EXPECT_EQ("a", res.atom_name(1));
    EXPECT_EQ("p(1)", res.atom_name(2));
    EXPECT_EQ("__atom3", res.atom_name(3));
    ASSERT_EQ(2u, res.vars.size());
    EXPECT_EQ(1, res.var_to_atom[res.atom_to_var[1]]);
    EXPECT_EQ(2, res.var_to_atom[res.atom_to_var[2]]);
    EXPECT_EQ("4", res.zdd.exact_count().to_string());
    // AtomId order: atom 1 at the top level.
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
    CompileResult res = compile(p, no_projection());
    ASSERT_EQ(1u, res.fact_names.size());
    EXPECT_EQ("a", res.fact_names[0]);
    EXPECT_EQ(0u, res.names.count(1));
    EXPECT_EQ("c", res.atom_name(2));
    EXPECT_EQ("2", res.zdd.exact_count().to_string());
}

TEST(Compiler, CustomOrder) {
    Program p = read_aspif_string("asp 1 0 0\n1 1 3 1 2 3 0 0\n0\n");
    CompileOptions opts = no_projection();
    opts.order.push_back(3);
    opts.order.push_back(1);
    CompileResult res = compile(p, opts);
    EXPECT_EQ(res.vars[0], res.atom_to_var[3]);
    EXPECT_EQ(res.vars[1], res.atom_to_var[1]);
    EXPECT_EQ(res.vars[2], res.atom_to_var[2]);
    EXPECT_EQ("8", res.zdd.exact_count().to_string());

    CompileOptions bad = no_projection();
    bad.order.push_back(9);
    EXPECT_THROW(compile(p, bad), std::invalid_argument);
}

TEST(Compiler, ProjectionOntoShownAtoms) {
    // {a;b}. c :- a.  #show c/0.
    const char* text =
        "asp 1 0 0\n"
        "1 1 2 1 2 0 0\n"
        "1 0 1 3 0 1 1\n"
        "4 1 c 1 3\n"
        "0\n";
    EXPECT_EQ(4u, compile_family(text).size());
    Program p = read_aspif_string(text);
    CompileResult res = compile(p);  // default: projected
    EXPECT_EQ(F(S(), S(3)), family_of(res));
    ASSERT_EQ(1u, res.vars.size());
    EXPECT_EQ(res.atom_to_var[3], res.vars[0]);
    // The BDD is the characteristic function over the shown variable only.
    EXPECT_TRUE(res.bdd.is_one());
}

std::vector<std::string> shown_of(const CompileResult& res, const std::set<Atom>& atoms) {
    return res.shown_names(std::vector<Atom>(atoms.begin(), atoms.end()));
}

TEST(Compiler, CompoundShowConditionKeepsItsAtomsShown) {
    // {a;b}. #show t : a, b.   (an output with a two-literal condition; a and b are kept)
    Program p = read_aspif_string("asp 1 0 0\n1 1 2 1 2 0 0\n4 1 t 2 1 2\n0\n");
    CompileResult res = compile(p);
    EXPECT_EQ(2u, res.vars.size());
    EXPECT_EQ("4", res.zdd.exact_count().to_string());
    ASSERT_EQ(1u, res.terms.size());
    EXPECT_EQ("t", res.terms[0].name);
    EXPECT_TRUE(res.names.empty());
    EXPECT_EQ(std::vector<std::string>(), shown_of(res, S(1)));
    EXPECT_EQ(std::vector<std::string>(1, "t"), shown_of(res, S(1, 2)));
}

TEST(Compiler, SecondNameOfAnAtomIsATerm) {
    // {a}. #show a/0. #show t : a.   -> clingo prints "a t"
    Program p = read_aspif_string("asp 1 0 0\n1 1 1 1 0 0\n4 1 a 1 1\n4 1 t 1 1\n0\n");
    CompileResult res = compile(p);
    EXPECT_EQ("a", res.atom_name(1));
    ASSERT_EQ(1u, res.terms.size());
    EXPECT_EQ("t", res.terms[0].name);
    std::vector<std::string> expected;
    expected.push_back("a");
    expected.push_back("t");
    EXPECT_EQ(expected, shown_of(res, S(1)));
    EXPECT_EQ(std::vector<std::string>(), shown_of(res, S()));
    EXPECT_EQ(res.names, atom_names(p));
}

TEST(Compiler, NegatedShowConditionIsEvaluatedOnTheProjectedSets) {
    // {a}. #show t : not a.   -> clingo prints "t" and "" ; a stays a DD variable
    Program p = read_aspif_string("asp 1 0 0\n1 1 1 1 0 0\n4 1 t 1 -1\n0\n");
    CompileResult res = compile(p);
    ASSERT_EQ(1u, res.vars.size());
    EXPECT_EQ("__atom1", res.atom_name(1));
    EXPECT_EQ(std::vector<std::string>(1, "t"), shown_of(res, S()));
    EXPECT_EQ(std::vector<std::string>(), shown_of(res, S(1)));
}

TEST(Compiler, SameNameThroughSeveralOutputsIsShownOnce) {
    // {a;b}. #show t : a. #show t : b.
    Program p = read_aspif_string("asp 1 0 0\n1 1 2 1 2 0 0\n4 1 t 1 1\n4 1 t 1 2\n0\n");
    CompileResult res = compile(p);
    EXPECT_EQ(2u, res.vars.size());
    EXPECT_EQ(std::vector<std::string>(1, "t"), shown_of(res, S(1, 2)));
    EXPECT_EQ(std::vector<std::string>(1, "t"), shown_of(res, S(2)));
}

TEST(Compiler, ProjectionToNothingGivesSingleEmptySet) {
    Program p = read_aspif_string("asp 1 0 0\n1 1 2 1 2 0 0\n0\n");
    CompileResult res = compile(p);
    EXPECT_TRUE(res.vars.empty());
    EXPECT_EQ(F(S()), family_of(res));
    Program bad = read_aspif_string("asp 1 0 0\n1 0 1 1 0 0\n1 0 0 0 1 1\n0\n");
    EXPECT_TRUE(family_of(compile(bad)).empty());
}

TEST(Compiler, AtomsOfNoRuleAreFalseAndGetNoVariable) {
    // f. {a}. #show a/0. #show f/0. #show w : f.   (gringo: `4 1 w 1 -3`, atom 3 occurs nowhere else)
    // #minimize{ 2 : a; 3 : not 3 }   (the weight of the absent atom's negation is a constant)
    const char* text =
        "asp 1 0 0\n"
        "1 0 1 1 0 0\n"
        "1 1 1 2 0 0\n"
        "2 0 2 2 2 -3 3\n"
        "4 1 a 1 2\n"
        "4 1 f 0\n"
        "4 1 w 1 -3\n"
        "0\n";
    Program p = read_aspif_string(text);
    CompileResult res = compile(p, no_projection());
    EXPECT_EQ(2u, res.vars.size());  // atoms 1 and 2 only
    EXPECT_EQ(3u, res.atom_to_var.size());
    EXPECT_EQ(F(S(1), S(1, 2)), family_of(res));
    std::vector<std::string> shown = res.shown_names(std::vector<Atom>(1, 1));
    ASSERT_EQ(2u, shown.size());  // f, w: the condition `not __atom3` always holds
    EXPECT_EQ("f", shown[0]);
    EXPECT_EQ("w", shown[1]);
    ASSERT_EQ(1u, res.costs.size());
    EXPECT_EQ(3, res.costs[0].offset);
    EXPECT_EQ(2, res.costs[0].var_weights[res.atom_to_var[2]]);
    CompileOptions opt = no_projection();
    opt.optimize = true;
    CompileResult best = compile(p, opt);
    ASSERT_EQ(1u, best.optimum.size());
    EXPECT_EQ(3, best.optimum[0]);
    EXPECT_EQ(F(S(1)), family_of(best));
    // program_atoms lists the atoms of rules and #external statements only.
    std::vector<Atom> atoms = program_atoms(p);
    ASSERT_EQ(2u, atoms.size());
    EXPECT_EQ(1, atoms[0]);
    EXPECT_EQ(2, atoms[1]);
}

TEST(Compiler, CostFunctionsAndOptimization) {
    // {a;b;c}. :- not a, not b, not c.
    // #minimize{ 1@1 : a; 2@1 : b; 5@0 : not c }
    const char* text =
        "asp 1 0 0\n"
        "1 1 3 1 2 3 0 0\n"
        "1 0 0 0 3 -1 -2 -3\n"
        "2 1 2 1 1 2 2\n"
        "2 0 1 -3 5\n"
        "0\n";
    Program p = read_aspif_string(text);
    CompileResult all = compile(p, no_projection());
    ASSERT_EQ(2u, all.costs.size());
    EXPECT_EQ(1, all.costs[0].priority);
    EXPECT_EQ(0, all.costs[0].offset);
    EXPECT_EQ(1, all.costs[0].var_weights[all.atom_to_var[1]]);
    EXPECT_EQ(2, all.costs[0].var_weights[all.atom_to_var[2]]);
    EXPECT_EQ(0, all.costs[1].priority);
    EXPECT_EQ(5, all.costs[1].offset);
    EXPECT_EQ(-5, all.costs[1].var_weights[all.atom_to_var[3]]);
    EXPECT_TRUE(all.optimum.empty());
    EXPECT_EQ("7", all.zdd.exact_count().to_string());

    CompileOptions opt = no_projection();
    opt.optimize = true;
    CompileResult best = compile(p, opt);
    ASSERT_EQ(2u, best.optimum.size());
    EXPECT_EQ(0, best.optimum[0]);  // neither a nor b
    EXPECT_EQ(0, best.optimum[1]);  // c true
    EXPECT_EQ(F(S(3)), family_of(best));
}

TEST(Compiler, OptimizeInconsistentProgram) {
    Program p = read_aspif_string("asp 1 0 0\n1 0 1 1 0 0\n1 0 0 0 1 1\n2 0 1 1 1\n0\n");
    CompileOptions opt = no_projection();
    opt.optimize = true;
    CompileResult res = compile(p, opt);
    EXPECT_TRUE(res.optimum.empty());
    EXPECT_TRUE(family_of(res).empty());
}

TEST(DdUtil, ZddToBddOverRoundTrip) {
    bddvar a = kyotodd::bddnewvar(), b = kyotodd::bddnewvar(), c = kyotodd::bddnewvar();
    std::vector<bddvar> vars;
    vars.push_back(a);
    vars.push_back(b);
    vars.push_back(c);
    BDD f = (BDD::prime(a) & ~BDD::prime(b)) | BDD::prime(c);
    ZDD z = bdd_to_zdd_over(f, vars);
    EXPECT_EQ(f, zdd_to_bdd_over(z, vars));
    // A variable absent from the family is forced to false in the BDD.
    ZDD single_a = ZDD::Single.change(a);
    EXPECT_EQ(BDD::prime(a) & ~BDD::prime(b) & ~BDD::prime(c), zdd_to_bdd_over(single_a, vars));
}

TEST(Compiler, ParseMethod) {
    EXPECT_EQ(Method::TwoCopy, parse_method("twocopy"));
    EXPECT_EQ(Method::Scc, parse_method("scc"));
    EXPECT_THROW(parse_method("loop"), std::invalid_argument);
    EXPECT_EQ(Method::Scc, CompileOptions().method);
}

TEST(Compiler, SccMethodAllocatesAuxVariablesOnlyInLoops) {
    // {c}. a :- b. b :- a. a :- c. d :- a.   (loop {a,b}; c, d tight)
    Program p = read_aspif_string(
        "asp 1 0 0\n1 1 1 3 0 0\n1 0 1 1 0 1 2\n1 0 1 2 0 1 1\n1 0 1 1 0 1 3\n1 0 1 4 0 1 1\n0\n");
    kyotodd::bddvar before = kyotodd::bddvarused();
    CompileOptions scc = no_projection();
    scc.method = Method::Scc;
    CompileResult res = compile(p, scc);
    EXPECT_EQ(before + 4 + 2, kyotodd::bddvarused());
    std::set<Atom> all = S(1, 2, 3);
    all.insert(4);
    EXPECT_EQ(F(S(), all), family_of(res));

    before = kyotodd::bddvarused();
    CompileOptions two = no_projection();
    two.method = Method::TwoCopy;
    compile(p, two);
    EXPECT_EQ(before + 4 + 4, kyotodd::bddvarused());
}

TEST(Compiler, SelfLoopIsANontrivialScc) {
    // a :- a.  {b}. a :- b.
    EXPECT_EQ(F(S(), S(1, 2)), compile_family("asp 1 0 0\n1 0 1 1 0 1 1\n1 1 1 2 0 0\n1 0 1 1 0 1 2\n0\n"));
}

TEST(Compiler, DisjunctiveLoopAcrossTwoRules) {
    // a | b :- c.  c :- a.  c :- b.  {d}. c :- d.
    // Loop {a,b,c}; with d: c true, then one of a/b (minimality) -> {a,c,d}, {b,c,d}.
    const char* text =
        "asp 1 0 0\n"
        "1 0 2 1 2 0 1 3\n"
        "1 0 1 3 0 1 1\n"
        "1 0 1 3 0 1 2\n"
        "1 1 1 4 0 0\n"
        "1 0 1 3 0 1 4\n"
        "0\n";
    Family expected;
    expected.insert(S());
    std::set<Atom> s1 = S(1, 3);
    s1.insert(4);
    std::set<Atom> s2 = S(2, 3);
    s2.insert(4);
    expected.insert(s1);
    expected.insert(s2);
    EXPECT_EQ(expected, compile_family(text));
}

TEST(Compiler, WeightBodyInsideLoop) {
    // {c}. a :- 1 <= #sum{1:b; 1:c}. b :- a.
    // Loop {a,b}; without c the loop is unfounded.
    const char* text =
        "asp 1 0 0\n"
        "1 1 1 3 0 0\n"
        "1 0 1 1 1 1 2 2 1 3 1\n"
        "1 0 1 2 0 1 1\n"
        "0\n";
    EXPECT_EQ(F(S(), S(1, 2, 3)), compile_family(text));
}

TEST(Compiler, NaturalLess) {
    EXPECT_TRUE(natural_less("p(2)", "p(10)"));
    EXPECT_FALSE(natural_less("p(10)", "p(2)"));
    EXPECT_TRUE(natural_less("p(1,9)", "p(1,10)"));
    EXPECT_TRUE(natural_less("a", "b"));
    EXPECT_TRUE(natural_less("p(1)", "p(1,2)"));
    EXPECT_FALSE(natural_less("p(1)", "p(1)"));
    EXPECT_TRUE(natural_less("p(007)", "p(8)"));
    EXPECT_TRUE(natural_less("q", "q(1)"));
    EXPECT_EQ(OrderHeuristic::AtomId, parse_order_heuristic("id"));
    EXPECT_EQ(OrderHeuristic::NaturalName, parse_order_heuristic("name"));
    EXPECT_THROW(parse_order_heuristic("x"), std::invalid_argument);
}

TEST(Compiler, NaturalNameOrderPlacesUnnamedAtomsNearNeighbours) {
    // Atoms: 1=p(10), 2=p(2), 3=p(1); 4 unnamed, used only with p(10); 5 unnamed, isolated.
    // {p(1);p(2);p(10)}.  aux4 :- p(10).  {aux5}.
    Program p = read_aspif_string(
        "asp 1 0 0\n"
        "1 1 3 1 2 3 0 0\n"
        "1 0 1 4 0 1 1\n"
        "1 1 1 5 0 0\n"
        "4 5 p(10) 1 1\n"
        "4 4 p(2) 1 2\n"
        "4 4 p(1) 1 3\n"
        "0\n");
    CompileResult res = compile(p, no_projection());
    ASSERT_EQ(5u, res.vars.size());
    // Top to bottom: p(1), p(2), p(10), aux4 (next to p(10)), aux5 (isolated, last).
    EXPECT_EQ(res.atom_to_var[3], res.vars[0]);
    EXPECT_EQ(res.atom_to_var[2], res.vars[1]);
    EXPECT_EQ(res.atom_to_var[1], res.vars[2]);
    EXPECT_EQ(res.atom_to_var[4], res.vars[3]);
    EXPECT_EQ(res.atom_to_var[5], res.vars[4]);
    EXPECT_EQ("16", res.zdd.exact_count().to_string());
}

TEST(DdUtil, DeepConversionsDoNotRecurse) {
    // One variable per level over tens of thousands of levels: a recursive
    // conversion would overflow the default stack. The variables are allocated
    // in a fresh KyotoDD instance, and the usual one is restored afterwards
    // (65536 variables is the cap; later tests must not inherit these).
    kyotodd::bddfinal();
    kyotodd::bddinit(1024);
    {
    const size_t n = 60000;
    std::vector<bddvar> vars;
    for (size_t i = 0; i < n; ++i) vars.push_back(kyotodd::bddnewvar());  // variable i+1 at level i+1
    BDD all = BDD::True;  // x_1 ∧ ... ∧ x_n, built from the bottom level (1) up
    for (size_t i = 0; i < n; ++i) all = BDD::prime(vars[i]) & all;
    ZDD z = bdd_to_zdd_over(all, vars);
    EXPECT_EQ("1", z.exact_count().to_string());
    EXPECT_EQ(all, zdd_to_bdd_over(z, vars));

    std::vector<BDD> terms;
    std::vector<long long> weights(n, 1);
    for (size_t i = 0; i < n; ++i) terms.push_back(BDD::prime(vars[i]));
    BDD at_least_all = threshold_bdd(terms, weights, static_cast<long long>(n));
    EXPECT_EQ(all, at_least_all);
    std::reverse(terms.begin(), terms.end());  // bottom level first: threshold_bdd sorts the terms itself
    std::reverse(weights.begin(), weights.end());
    BDD at_most_one_missing = threshold_bdd(terms, weights, static_cast<long long>(n) - 1);
    EXPECT_EQ(std::to_string(n + 1), bdd_to_zdd_over(at_most_one_missing, vars).exact_count().to_string());

    // Renaming walks the chain iteratively as well: shift the single set up by
    // `shift` levels (order preserving), then reverse a shorter chain (set operations).
    const size_t shift = 1000;
    std::vector<bddvar> from(vars.begin(), vars.end() - shift), to(vars.begin() + shift, vars.end());
    ZDD chain = ZDD::from_sets(std::vector<std::vector<bddvar> >(1, from));
    EXPECT_EQ(ZDD::from_sets(std::vector<std::vector<bddvar> >(1, to)), rename_zdd(chain, from, to));
    const size_t m = 2000;
    std::vector<bddvar> head(vars.begin(), vars.begin() + m), reversed(head.rbegin(), head.rend());
    ZDD short_chain = ZDD::from_sets(std::vector<std::vector<bddvar> >(1, head));
    EXPECT_EQ(short_chain, rename_zdd(short_chain, head, reversed));  // a permutation of the same set
    }
    kyotodd::bddfinal();
    kyotodd::bddinit(1024);
    EXPECT_EQ(0u, kyotodd::bddvarused());
}

namespace {

typedef std::set<std::set<bddvar> > VarFamily;

VarFamily var_family(const ZDD& z) {
    VarFamily fam;
    std::vector<std::vector<bddvar> > sets = z.enumerate();
    for (size_t i = 0; i < sets.size(); ++i) fam.insert(std::set<bddvar>(sets[i].begin(), sets[i].end()));
    return fam;
}

std::vector<bddvar> V(bddvar a, bddvar b, bddvar c) {
    std::vector<bddvar> v;
    v.push_back(a);
    v.push_back(b);
    v.push_back(c);
    return v;
}

/** {{a}, {b, c}, {a, c}} */
ZDD three_sets(bddvar a, bddvar b, bddvar c) {
    std::vector<std::vector<bddvar> > sets;
    sets.push_back(std::vector<bddvar>(1, a));
    sets.push_back(V(b, c, c));
    sets.push_back(V(a, c, c));
    return ZDD::from_sets(sets);
}

}  // namespace

TEST(DdUtil, RenameZddKeepingTheOrder) {
    bddvar a = kyotodd::bddnewvar(), b = kyotodd::bddnewvar(), c = kyotodd::bddnewvar();
    ZDD z = three_sets(a, b, c);
    bddvar x = kyotodd::bddnewvar(), y = kyotodd::bddnewvar(), w = kyotodd::bddnewvar();  // above a, b, c
    ZDD r = rename_zdd(z, V(a, b, c), V(x, y, w));
    EXPECT_EQ(var_family(three_sets(x, y, w)), var_family(r));
    EXPECT_EQ(z, rename_zdd(z, V(a, b, c), V(a, b, c)));  // identity
    // Variables of `from` that do not occur in the ZDD are ignored.
    EXPECT_EQ(r, rename_zdd(z, V(b, c, a), V(y, w, x)));
    // Back again (the targets lie below the sources this time).
    EXPECT_EQ(z, rename_zdd(r, V(x, y, w), V(a, b, c)));
}

TEST(DdUtil, RenameZddChangingTheOrder) {
    bddvar a = kyotodd::bddnewvar(), b = kyotodd::bddnewvar(), c = kyotodd::bddnewvar();
    ZDD z = three_sets(a, b, c);
    bddvar x = kyotodd::bddnewvar(), y = kyotodd::bddnewvar(), w = kyotodd::bddnewvar();
    // a -> w, b -> y, c -> x reverses the order: {{w}, {y, x}, {w, x}}
    ZDD r = rename_zdd(z, V(a, b, c), V(w, y, x));
    EXPECT_EQ(var_family(three_sets(w, y, x)), var_family(r));
    // Renaming onto the same variables in a different order: {{c}, {b, a}, {c, a}}
    ZDD s = rename_zdd(z, V(a, b, c), V(c, b, a));
    EXPECT_EQ(var_family(three_sets(c, b, a)), var_family(s));
    EXPECT_EQ(z, rename_zdd(s, V(c, b, a), V(a, b, c)));
}

TEST(DdUtil, RenameZddTerminalsAndErrors) {
    bddvar a = kyotodd::bddnewvar(), b = kyotodd::bddnewvar(), c = kyotodd::bddnewvar();
    EXPECT_EQ(ZDD::Empty, rename_zdd(ZDD::Empty, V(a, b, c), V(c, b, a)));
    EXPECT_EQ(ZDD::Single, rename_zdd(ZDD::Single, V(a, b, c), V(c, b, a)));
    ZDD z = three_sets(a, b, c);
    EXPECT_THROW(rename_zdd(z, V(a, b, c), std::vector<bddvar>(2, a)), std::invalid_argument);  // sizes differ
    EXPECT_THROW(rename_zdd(z, V(a, b, c), V(a, a, c)), std::invalid_argument);                 // target twice
    EXPECT_THROW(rename_zdd(z, V(a, a, c), V(a, b, c)), std::invalid_argument);                 // source twice
    EXPECT_THROW(rename_zdd(z, std::vector<bddvar>(1, a), std::vector<bddvar>(1, b)), std::invalid_argument);
}
