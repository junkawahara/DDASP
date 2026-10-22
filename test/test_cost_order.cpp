#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "ddasp/aspif_reader.h"
#include "ddasp/compiler.h"
#include "ddasp/cost_order.h"

using namespace ddasp;
using kyotodd::bddvar;

namespace {

struct InitKyotoDD {
    InitKyotoDD() { kyotodd::bddinit(1024); }
};
InitKyotoDD init_once;

typedef std::vector<long long> Cost;
typedef std::pair<Cost, std::set<Atom> > Visit;

// {a;b;c;d} with two #minimize levels (every subset is an answer set):
//   priority 2: 1:a, 1:b, 2:not b   ->  cost1 = 2 + [a] - [b]
//   priority 1: 2:b, 2:c, 1:not d   ->  cost2 = 1 + 2[b] + 2[c] - [d]
const char* kTwoLevelText =
    "asp 1 0 0\n"
    "1 1 4 1 2 3 4 0 0\n"
    "2 2 3 1 1 2 1 -2 2\n"
    "2 1 3 2 2 3 2 -4 1\n"
    "0\n";

Cost two_level_cost(const std::set<Atom>& x) {
    Cost c(2);
    c[0] = 2 + x.count(1) - x.count(2);
    c[1] = 1 + 2 * x.count(2) + 2 * x.count(3) - x.count(4);
    return c;
}

// {a;b;c;d} with one level: 1:a, 2:b, 3:not c (d carries no weight)  ->  cost = 3 + [a] + 2[b] - 3[c]
const char* kOneLevelText =
    "asp 1 0 0\n"
    "1 1 4 1 2 3 4 0 0\n"
    "2 0 3 1 1 2 2 -3 3\n"
    "0\n";

Cost one_level_cost(const std::set<Atom>& x) { return Cost(1, 3 + x.count(1) + 2 * x.count(2) - 3 * x.count(3)); }

/** The visits of enumerate_by_cost in order (stopping after `stop_after` sets when non-zero). */
std::vector<Visit> visits_of(const CompileResult& res, size_t stop_after = 0) {
    std::vector<Visit> v;
    enumerate_by_cost(res, [&](const Cost& cost, const std::vector<bddvar>& set) -> bool {
        std::set<Atom> atoms;
        for (size_t i = 0; i < set.size(); ++i) atoms.insert(res.var_to_atom[set[i]]);
        v.push_back(Visit(cost, atoms));
        return stop_after == 0 || v.size() < stop_after;
    });
    return v;
}

/**
 * Brute force over the subsets of {1..n} projected onto `shown`: every projected
 * set once, at the smallest cost of the subsets projecting to it; sorted.
 */
std::vector<Visit> expected_visits(int n, Cost (*cost_of)(const std::set<Atom>&), const std::set<Atom>& shown) {
    std::map<std::set<Atom>, Cost> smallest;
    for (int mask = 0; mask < (1 << n); ++mask) {
        std::set<Atom> x, p;
        for (int a = 1; a <= n; ++a) {
            if (mask >> (a - 1) & 1) {
                x.insert(a);
                if (shown.count(a)) p.insert(a);
            }
        }
        Cost c = cost_of(x);
        std::map<std::set<Atom>, Cost>::iterator it = smallest.find(p);
        if (it == smallest.end() || c < it->second) smallest[p] = c;
    }
    std::vector<Visit> v;
    for (std::map<std::set<Atom>, Cost>::iterator it = smallest.begin(); it != smallest.end(); ++it) {
        v.push_back(Visit(it->second, it->first));
    }
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<Visit> sorted(std::vector<Visit> v) {
    std::sort(v.begin(), v.end());
    return v;
}

bool costs_ascending(const std::vector<Visit>& v) {
    for (size_t i = 1; i < v.size(); ++i) {
        if (v[i].first < v[i - 1].first) return false;
    }
    return true;
}

std::set<Atom> S(int a, int b) { std::set<Atom> s; s.insert(a); s.insert(b); return s; }
std::set<Atom> S(int a, int b, int c) { std::set<Atom> s = S(a, b); s.insert(c); return s; }
std::set<Atom> S(int a, int b, int c, int d) { std::set<Atom> s = S(a, b, c); s.insert(d); return s; }

CompileOptions no_projection() {
    CompileOptions o;
    o.project_to_shown = false;
    return o;
}

/** kText with `#show` outputs for the given atoms (names a, b, c, d). */
std::string showing(const char* text, const std::set<Atom>& atoms) {
    std::string s(text);
    std::string outputs;
    const char* names = "?abcd";
    for (std::set<Atom>::const_iterator it = atoms.begin(); it != atoms.end(); ++it) {
        outputs += std::string("4 1 ") + names[*it] + " 1 " + std::to_string(*it) + "\n";
    }
    s.insert(s.size() - 2, outputs);  // before the final "0\n"
    return s;
}

}  // namespace

TEST(CostOrder, PeelsSeveralPriorityLevels) {
    Program p = read_aspif_string(kTwoLevelText);
    CompileResult res = compile(p, no_projection());
    EXPECT_EQ(CostOrderMethod::Peeling, cost_order_method(res));
    std::vector<Visit> v = visits_of(res);
    EXPECT_EQ(16u, v.size());
    EXPECT_TRUE(costs_ascending(v));
    EXPECT_EQ(expected_visits(4, two_level_cost, S(1, 2, 3, 4)), sorted(v));
    EXPECT_EQ(2u, visits_of(res, 2).size());  // the visitor stops the enumeration
    EXPECT_EQ(v[0], visits_of(res, 1)[0]);
}

TEST(CostOrder, ProjectedSetsAreVisitedOnceAtTheirSmallestCost) {
    Program p = read_aspif_string(showing(kTwoLevelText, S(1, 2, 3)));  // d hidden, with weight
    CompileResult res = compile(p);
    EXPECT_EQ(CostOrderMethod::Peeling, cost_order_method(res));
    std::vector<Visit> v = visits_of(res);
    EXPECT_EQ(8u, v.size());
    EXPECT_TRUE(costs_ascending(v));
    EXPECT_EQ(expected_visits(4, two_level_cost, S(1, 2, 3)), sorted(v));
    EXPECT_EQ(3u, visits_of(res, 3).size());
}

TEST(CostOrder, KBestIteratorForOneLevelWithoutHiddenWeights) {
    Program p = read_aspif_string(kOneLevelText);
    CompileResult res = compile(p, no_projection());
    EXPECT_EQ(CostOrderMethod::KBest, cost_order_method(res));
    std::vector<Visit> v = visits_of(res);
    EXPECT_EQ(16u, v.size());
    EXPECT_TRUE(costs_ascending(v));
    EXPECT_EQ(expected_visits(4, one_level_cost, S(1, 2, 3, 4)), sorted(v));
    EXPECT_EQ(3u, visits_of(res, 3).size());
    // Sets are given from the top level down, like for_each_set.
    enumerate_by_cost(res, [&](const Cost&, const std::vector<bddvar>& set) -> bool {
        for (size_t i = 1; i < set.size(); ++i) EXPECT_GT(kyotodd::bddlevofvar(set[i - 1]), kyotodd::bddlevofvar(set[i]));
        return true;
    });
    // Hiding the weightless d keeps the iterator (on the projected family); hiding c (weight -3) peels.
    CompileResult no_d = compile(read_aspif_string(showing(kOneLevelText, S(1, 2, 3))));
    EXPECT_EQ(CostOrderMethod::KBest, cost_order_method(no_d));
    std::vector<Visit> vd = visits_of(no_d);
    EXPECT_EQ(8u, vd.size());
    EXPECT_TRUE(costs_ascending(vd));
    EXPECT_EQ(expected_visits(4, one_level_cost, S(1, 2, 3)), sorted(vd));
    CompileResult no_c = compile(read_aspif_string(showing(kOneLevelText, S(1, 2, 4))));
    EXPECT_EQ(CostOrderMethod::Peeling, cost_order_method(no_c));
    std::vector<Visit> vc = visits_of(no_c);
    EXPECT_TRUE(costs_ascending(vc));
    EXPECT_EQ(expected_visits(4, one_level_cost, S(1, 2, 4)), sorted(vc));
}

TEST(CostOrder, BestOptionCollectsTheFirstSets) {
    Program p = read_aspif_string(kTwoLevelText);
    CompileOptions o = no_projection();
    o.best = 3;
    CompileResult res = compile(p, o);
    ASSERT_EQ(3u, res.best.size());
    std::vector<Visit> v = visits_of(res);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(v[i].first, res.best[i].cost);
        EXPECT_EQ(v[i].second, std::set<Atom>(res.best[i].atoms.begin(), res.best[i].atoms.end()));
    }
    o.best = 100;
    EXPECT_EQ(16u, compile(p, o).best.size());
    EXPECT_TRUE(compile(p, no_projection()).best.empty());  // only on request
    o.best = 2;
    CompileResult one = compile(read_aspif_string(kOneLevelText), o);  // k-best path
    ASSERT_EQ(2u, one.best.size());
    EXPECT_EQ(Cost(1, 0), one.best[0].cost);  // {c}: 3 - 3
    EXPECT_EQ(std::vector<Atom>(1, 3), one.best[0].atoms);
    Program unsat = read_aspif_string(std::string(kTwoLevelText).replace(10, 0, "1 0 0 0 0\n"));
    EXPECT_TRUE(compile(unsat, o).best.empty());
    EXPECT_TRUE(visits_of(compile(unsat, no_projection())).empty());
    Program no_minimize = read_aspif_string("asp 1 0 0\n1 1 1 1 0 0\n0\n");
    EXPECT_THROW(compile(no_minimize, o), std::invalid_argument);
    CompileResult plain = compile(no_minimize, no_projection());
    EXPECT_THROW(cost_order_method(plain), std::invalid_argument);
    EXPECT_THROW(visits_of(plain), std::invalid_argument);
}

TEST(CostOrder, SingleEmptyAnswerSet) {
    // {a}. :- a. #minimize{ 1 : a }.   -> the only answer set is empty, cost 0
    Program p = read_aspif_string("asp 1 0 0\n1 1 1 1 0 0\n1 0 0 0 1 1\n2 0 1 1 1\n0\n");
    CompileResult res = compile(p, no_projection());
    std::vector<Visit> v = visits_of(res);
    ASSERT_EQ(1u, v.size());
    EXPECT_EQ(Cost(1, 0), v[0].first);
    EXPECT_TRUE(v[0].second.empty());
}
