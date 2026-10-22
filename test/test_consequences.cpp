#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ddasp/aspif_reader.h"
#include "ddasp/compiler.h"
#include "ddasp/consequences.h"
#include "ddasp/dd_util.h"

using namespace ddasp;
using kyotodd::ZDD;
using kyotodd::bddvar;

namespace {

struct InitKyotoDD {
    InitKyotoDD() { kyotodd::bddinit(1024); }
};
InitKyotoDD init_once;

typedef std::map<std::string, long long> Counts;

Counts as_map(const std::vector<NameCount>& freq) {
    Counts m;
    for (size_t i = 0; i < freq.size(); ++i) m[freq[i].name] = std::stoll(freq[i].count.to_string());
    return m;
}

/** Frequencies by brute force: enumerate the family and count the displayed names. */
Counts brute_force(const CompileResult& res) {
    Counts m;
    std::vector<std::vector<bddvar> > sets = res.zdd.enumerate();
    for (size_t i = 0; i < sets.size(); ++i) {
        std::vector<Atom> atoms;
        for (size_t j = 0; j < sets[i].size(); ++j) atoms.push_back(res.var_to_atom[sets[i][j]]);
        std::vector<std::string> shown = res.shown_names(atoms);
        for (size_t j = 0; j < shown.size(); ++j) ++m[shown[j]];
    }
    return m;
}

std::vector<std::string> names_of(const std::vector<NameCount>& freq) {
    std::vector<std::string> out;
    for (size_t i = 0; i < freq.size(); ++i) out.push_back(freq[i].name);
    return out;
}

std::vector<std::string> V() { return std::vector<std::string>(); }
std::vector<std::string> V(const char* a) { return std::vector<std::string>(1, a); }
std::vector<std::string> V(const char* a, const char* b) { std::vector<std::string> v = V(a); v.push_back(b); return v; }
std::vector<std::string> V(const char* a, const char* b, const char* c) { std::vector<std::string> v = V(a, b); v.push_back(c); return v; }

/** Compile with and without projection, check the frequencies against brute force, return the projected ones. */
std::vector<NameCount> checked_frequencies(const std::string& aspif) {
    Program p = read_aspif_string(aspif);
    std::vector<NameCount> projected;
    for (int proj = 0; proj < 2; ++proj) {
        for (int m = 0; m < 2; ++m) {
            CompileOptions o;
            o.project_to_shown = proj == 1;
            o.method = m == 0 ? Method::TwoCopy : Method::Scc;
            CompileResult res = compile(p, o);
            std::vector<NameCount> freq = name_frequencies(res);
            Counts expected = brute_force(res);
            Counts got = as_map(freq);
            for (Counts::const_iterator it = got.begin(); it != got.end(); ++it) {
                EXPECT_EQ(expected[it->first], it->second) << it->first << " (projection " << proj << ")";
            }
            for (Counts::const_iterator it = expected.begin(); it != expected.end(); ++it) {
                EXPECT_EQ(1u, got.count(it->first)) << it->first << " missing (projection " << proj << ")";
            }
            EXPECT_EQ(got.size(), names_of(freq).size()) << "a name is listed twice";
            if (proj == 1 && m == 1) projected = freq;
        }
    }
    return projected;
}

}  // namespace

TEST(DdUtil, ZddVariableCounts) {
    bddvar a = kyotodd::bddnewvar(), b = kyotodd::bddnewvar(), c = kyotodd::bddnewvar();
    ZDD power = ZDD(1).change(a) + ZDD(1);  // {∅, {a}}
    power = power.change(b) + power;        // × {∅, {b}}
    power = power.change(c) + power;        // 8 sets
    std::map<bddvar, bigint::BigInt> counts = zdd_variable_counts(power);
    ASSERT_EQ(3u, counts.size());
    EXPECT_EQ("4", counts[a].to_string());
    EXPECT_EQ("4", counts[b].to_string());
    EXPECT_EQ("4", counts[c].to_string());
    ZDD few = ZDD(1).change(a).change(c) + ZDD(1).change(b);  // {{a,c}, {b}}
    counts = zdd_variable_counts(few);
    EXPECT_EQ("1", counts[a].to_string());
    EXPECT_EQ("1", counts[b].to_string());
    EXPECT_EQ("1", counts[c].to_string());
    EXPECT_TRUE(zdd_variable_counts(ZDD(0)).empty());
    EXPECT_TRUE(zdd_variable_counts(ZDD(1)).empty());
    // A variable of the universe that occurs in no set is absent.
    EXPECT_EQ(0u, zdd_variable_counts(ZDD(1).change(a)).count(b));
}

TEST(Consequences, ChoiceAndDerivedAtom) {
    // {a; b}. c :- a.  #show a/0. #show b/0. #show c/0.
    std::vector<NameCount> freq = checked_frequencies(
        "asp 1 0 0\n1 1 2 1 2 0 0\n1 0 1 3 0 1 1\n4 1 a 1 1\n4 1 b 1 2\n4 1 c 1 3\n0\n");
    EXPECT_EQ(V("a", "b", "c"), names_of(freq));
    Counts m = as_map(freq);
    EXPECT_EQ(2, m["a"]);
    EXPECT_EQ(2, m["b"]);
    EXPECT_EQ(2, m["c"]);
    EXPECT_EQ(V("a", "b", "c"), brave_consequences(freq));
    EXPECT_EQ(V(), cautious_consequences(freq, bigint::BigInt(4)));
}

TEST(Consequences, FactsAndForcedAtomsAreCautious) {
    // f. {a}. :- not a.  #show f/0. #show a/0.   (gringo shows the fact f without a condition)
    std::vector<NameCount> freq = checked_frequencies(
        "asp 1 0 0\n1 0 1 1 0 0\n1 1 1 2 0 0\n1 0 0 0 1 -2\n4 1 f 0\n4 1 a 1 2\n0\n");
    EXPECT_EQ(V("f", "a"), names_of(freq));
    EXPECT_EQ(V("f", "a"), brave_consequences(freq));
    EXPECT_EQ(V("f", "a"), cautious_consequences(freq, bigint::BigInt(1)));
}

TEST(Consequences, NameSharedBySeveralOutputsCountsTheUnion) {
    // {a; b}.  #show t : a. #show t : b.   -> t is displayed by 3 of the 4 answer sets
    std::vector<NameCount> freq = checked_frequencies("asp 1 0 0\n1 1 2 1 2 0 0\n4 1 t 1 1\n4 1 t 1 2\n0\n");
    EXPECT_EQ(V("t"), names_of(freq));
    EXPECT_EQ("3", freq[0].count.to_string());
}

TEST(Consequences, NegatedAndCompoundConditions) {
    // {a}.  #show a/0. #show t : not a. #show w : a, not z. #show v : a, z.   (z has no rule)
    std::vector<NameCount> freq = checked_frequencies(
        "asp 1 0 0\n1 1 1 1 0 0\n4 1 a 1 1\n4 1 t 1 -1\n4 1 w 2 1 -5\n4 1 v 2 1 5\n0\n");
    Counts m = as_map(freq);
    EXPECT_EQ(1, m["a"]);
    EXPECT_EQ(1, m["t"]);
    EXPECT_EQ(1, m["w"]);
    EXPECT_EQ(0, m["v"]);
    EXPECT_EQ(V("a", "t", "w"), brave_consequences(freq));
    EXPECT_EQ(V(), cautious_consequences(freq, bigint::BigInt(2)));
}

TEST(Consequences, InconsistentProgramHasNoConsequences) {
    // a. :- a.  #show a/0.
    std::vector<NameCount> freq = checked_frequencies("asp 1 0 0\n1 0 1 1 0 0\n1 0 0 0 1 1\n4 1 a 1 1\n0\n");
    EXPECT_EQ(V("a"), names_of(freq));
    EXPECT_TRUE(freq[0].count.is_zero());
    EXPECT_EQ(V(), brave_consequences(freq));
    EXPECT_EQ(V(), cautious_consequences(freq, bigint::BigInt(0)));
}

TEST(Consequences, HiddenAtomsAreNotListed) {
    // {a; b}. c :- a, b.  #show c/0.
    CompileResult res = compile(read_aspif_string("asp 1 0 0\n1 1 2 1 2 0 0\n1 0 1 3 0 2 1 2\n4 1 c 1 3\n0\n"));
    std::vector<NameCount> freq = name_frequencies(res);
    ASSERT_EQ(1u, freq.size());
    EXPECT_EQ("c", freq[0].name);
    EXPECT_EQ("1", freq[0].count.to_string());  // of 2 projected sets
    CompileOptions all;
    all.project_to_shown = false;
    freq = name_frequencies(compile(read_aspif_string("asp 1 0 0\n1 1 2 1 2 0 0\n1 0 1 3 0 2 1 2\n4 1 c 1 3\n0\n"), all));
    ASSERT_EQ(1u, freq.size());
    EXPECT_EQ("1", freq[0].count.to_string());  // of 4 answer sets
}
