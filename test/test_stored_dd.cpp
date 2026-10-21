#include <gtest/gtest.h>

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ddasp/aspif_reader.h"
#include "ddasp/compiler.h"
#include "ddasp/stored_dd.h"

using namespace ddasp;
using kyotodd::ZDD;
using kyotodd::bddvar;

namespace {

struct InitKyotoDD {
    InitKyotoDD() { kyotodd::bddinit(1024); }
};
InitKyotoDD init_once;

typedef std::set<std::set<std::string> > NameFamily;

NameFamily family_of(const CompileResult& res) {
    NameFamily fam;
    std::vector<std::vector<bddvar> > sets = res.zdd.enumerate();
    for (size_t i = 0; i < sets.size(); ++i) {
        std::set<std::string> s;
        for (size_t j = 0; j < sets[i].size(); ++j) s.insert(res.atom_name(res.var_to_atom[sets[i][j]]));
        fam.insert(s);
    }
    return fam;
}

NameFamily family_of(const LoadedDD& dd) {
    std::map<bddvar, std::string> name_of;
    for (size_t k = 0; k < dd.vars.size(); ++k) name_of[dd.vars[k]] = dd.data.names[k];
    NameFamily fam;
    std::vector<std::vector<bddvar> > sets = dd.zdd.enumerate();
    for (size_t i = 0; i < sets.size(); ++i) {
        std::set<std::string> s;
        for (size_t j = 0; j < sets[i].size(); ++j) s.insert(name_of.at(sets[i][j]));
        fam.insert(s);
    }
    return fam;
}

CompileResult compile_aspif(const std::string& text, const CompileOptions& opts = CompileOptions()) {
    return compile(read_aspif_string(text), opts);
}

std::string store(const CompileResult& res, std::vector<std::string>* warnings = 0) {
    std::ostringstream os;
    write_stored_dd(os, res, warnings);
    return os.str();
}

StoredDDData parse(const std::string& bytes) {
    std::istringstream is(bytes);
    return read_stored_dd(is);
}

std::set<std::string> N() { return std::set<std::string>(); }
std::set<std::string> N(const char* a) { std::set<std::string> s; s.insert(a); return s; }

// {a;b;c}. :- a, b.  #show a/0. #show b/0. #show c/0.
const char* const kChoice =
    "asp 1 0 0\n"
    "1 1 3 1 2 3 0 0\n"
    "1 0 0 0 2 1 2\n"
    "4 1 a 1 1\n"
    "4 1 b 1 2\n"
    "4 1 c 1 3\n"
    "0\n";

}  // namespace

TEST(StoredDD, RoundTrip) {
    CompileResult res = compile_aspif(kChoice);
    ASSERT_EQ(3u, res.vars.size());
    std::vector<std::string> warnings;
    std::string bytes = store(res, &warnings);
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(0u, bytes.find("ddasp-dd 1\natoms 3\na\nb\nc\nfacts 0\nzdd "));

    StoredDDData dd = parse(bytes);
    EXPECT_EQ(0, dd.terminal);
    ASSERT_EQ(3u, dd.names.size());
    EXPECT_EQ("a", dd.names[0]);  // top of the diagram first (natural name order)
    EXPECT_EQ("c", dd.names[2]);
    EXPECT_TRUE(dd.fact_names.empty());
    EXPECT_FALSE(dd.payload.empty());

    LoadedDD loaded = import_stored_dd(dd);
    ASSERT_EQ(3u, loaded.vars.size());
    EXPECT_GT(kyotodd::bddlevofvar(loaded.vars[0]), kyotodd::bddlevofvar(loaded.vars[2]));
    EXPECT_EQ(family_of(res), family_of(loaded));
    EXPECT_EQ("6", loaded.zdd.exact_count().to_string());
}

TEST(StoredDD, FactsAndDeclarations) {
    // f. g.  {p(1)}.  #show f/0. #show g/0. #show p/1.   (facts are shown without an atom)
    CompileResult res = compile_aspif(
        "asp 1 0 0\n"
        "1 0 1 1 0 0\n"
        "1 0 1 2 0 0\n"
        "1 1 1 3 0 0\n"
        "4 1 f 0\n"
        "4 1 g 0\n"
        "4 1 f 0\n"
        "4 4 p(1) 1 3\n"
        "0\n");
    StoredDDData dd = parse(store(res));
    ASSERT_EQ(1u, dd.names.size());
    EXPECT_EQ("p(1)", dd.names[0]);
    ASSERT_EQ(2u, dd.fact_names.size());  // duplicates dropped
    EXPECT_EQ("f", dd.fact_names[0]);
    EXPECT_EQ("g", dd.fact_names[1]);
    EXPECT_EQ("#external p(1).\n#show p(1) : p(1).\nf.\ng.\n", stored_dd_declarations(dd));
    LoadedDD loaded = import_stored_dd(dd);
    NameFamily expected;
    expected.insert(N());
    expected.insert(N("p(1)"));
    EXPECT_EQ(expected, family_of(loaded));
}

TEST(StoredDD, TerminalFamilies) {
    // a. :- a.  #show a/0.   -> no answer set
    std::string empty = store(compile_aspif("asp 1 0 0\n1 0 1 1 0 0\n1 0 0 0 1 1\n4 1 a 1 1\n0\n"));
    EXPECT_NE(std::string::npos, empty.find("\nzdd empty\n"));
    StoredDDData dd = parse(empty);
    EXPECT_EQ(1, dd.terminal);
    ASSERT_EQ(1u, dd.names.size());
    LoadedDD loaded = import_stored_dd(dd);
    EXPECT_TRUE(loaded.zdd.is_zero());
    EXPECT_EQ(1u, loaded.vars.size());

    // {a}. :- a.  #show a/0.   -> only the empty answer set
    std::string single = store(compile_aspif("asp 1 0 0\n1 1 1 1 0 0\n1 0 0 0 1 1\n4 1 a 1 1\n0\n"));
    EXPECT_NE(std::string::npos, single.find("\nzdd single\n"));
    dd = parse(single);
    EXPECT_EQ(2, dd.terminal);
    loaded = import_stored_dd(dd);
    EXPECT_TRUE(loaded.zdd.is_one());
    EXPECT_EQ(1u, loaded.vars.size());

    // f.   (a fact only: no atoms, one empty answer set)
    std::string facts_only = store(compile_aspif("asp 1 0 0\n1 0 1 1 0 0\n4 1 f 0\n0\n"));
    dd = parse(facts_only);
    EXPECT_EQ(2, dd.terminal);
    EXPECT_TRUE(dd.names.empty());
    ASSERT_EQ(1u, dd.fact_names.size());
    loaded = import_stored_dd(dd);
    EXPECT_TRUE(loaded.zdd.is_one());
    EXPECT_TRUE(loaded.vars.empty());
}

TEST(StoredDD, ImportIntoAFreshInstanceCreatesTheMissingVariables) {
    // {a;b}. :- a.  #show a/0. #show b/0.   The top atom (a) is never true, so the
    // diagram has a node at level 1 only and the export records a single level.
    std::string bytes;
    {
        CompileResult res = compile_aspif("asp 1 0 0\n1 1 2 1 2 0 0\n1 0 0 0 1 1\n4 1 a 1 1\n4 1 b 1 2\n0\n");
        bytes = store(res);
    }
    kyotodd::bddfinal();  // a fresh instance, as in the CLI and the Python binding
    kyotodd::bddinit(1024);
    LoadedDD loaded = import_stored_dd(parse(bytes));
    EXPECT_EQ(2u, kyotodd::bddvarused());
    ASSERT_EQ(2u, loaded.vars.size());
    EXPECT_EQ(2u, loaded.vars[0]);  // variable number = level in a fresh instance
    EXPECT_EQ(1u, loaded.vars[1]);
    NameFamily expected;
    expected.insert(N());
    expected.insert(N("b"));
    EXPECT_EQ(expected, family_of(loaded));
}

TEST(StoredDD, RejectsFamiliesThatCannotBeNamed) {
    // {a}. b :- a.  #show a/0.   with every atom kept: b has no name.
    CompileOptions all;
    all.project_to_shown = false;
    EXPECT_THROW(store(compile_aspif("asp 1 0 0\n1 1 1 1 0 0\n1 0 1 2 0 1 1\n4 1 a 1 1\n0\n", all)),
                 std::invalid_argument);
    // #show t : a. #show t : b.   -> two atoms named t
    EXPECT_THROW(store(compile_aspif("asp 1 0 0\n1 1 2 1 2 0 0\n4 1 t 1 1\n4 1 t 1 2\n0\n")), std::invalid_argument);
    // #show 1 : a.   -> "1" is not an atom
    EXPECT_THROW(store(compile_aspif("asp 1 0 0\n1 1 1 1 0 0\n4 1 1 1 1\n0\n")), std::invalid_argument);
    // #show 42.   -> a fact name that is not an atom
    EXPECT_THROW(store(compile_aspif("asp 1 0 0\n1 1 1 1 0 0\n4 1 a 1 1\n4 2 42 0\n0\n")), std::invalid_argument);
    // Names with arguments, strings, negative numbers and classical negation are fine.
    CompileResult ok = compile_aspif(
        "asp 1 0 0\n"
        "1 1 3 1 2 3 0 0\n"
        "4 10 p(1,\"a b\") 1 1\n"
        "4 5 q(-1) 1 2\n"
        "4 4 -r_s 1 3\n"
        "0\n");
    EXPECT_NO_THROW(store(ok));
}

TEST(StoredDD, WarnsAboutDroppedTerms) {
    // {a}. #show a/0. #show t : not a.
    CompileResult res = compile_aspif("asp 1 0 0\n1 1 1 1 0 0\n4 1 a 1 1\n4 1 t 1 -1\n0\n");
    ASSERT_EQ(1u, res.terms.size());
    std::vector<std::string> warnings;
    std::string bytes = store(res, &warnings);
    ASSERT_EQ(1u, warnings.size());
    EXPECT_NE(std::string::npos, warnings[0].find("not stored"));
    StoredDDData dd = parse(bytes);
    ASSERT_EQ(1u, dd.names.size());
    EXPECT_EQ("a", dd.names[0]);
}

TEST(StoredDD, RejectsMalformedFiles) {
    EXPECT_THROW(parse(""), std::runtime_error);
    EXPECT_THROW(parse("BDD..."), std::runtime_error);
    EXPECT_THROW(parse("ddasp-dd 2\natoms 0\nfacts 0\nzdd single\n"), std::runtime_error);
    EXPECT_THROW(parse("ddasp-dd 1\natoms x\n"), std::runtime_error);
    EXPECT_THROW(parse("ddasp-dd 1\natoms 2\na\n"), std::runtime_error);                  // too few names
    EXPECT_THROW(parse("ddasp-dd 1\natoms 2\na\na\nfacts 0\nzdd single\n"), std::runtime_error);  // duplicate
    EXPECT_THROW(parse("ddasp-dd 1\natoms 1\na\nfacts 1\na\nzdd single\n"), std::runtime_error);  // fact = atom
    EXPECT_THROW(parse("ddasp-dd 1\natoms 1\na\nfacts 0\n"), std::runtime_error);         // no diagram
    EXPECT_THROW(parse("ddasp-dd 1\natoms 1\na\nfacts 0\nzdd 100\nBDD"), std::runtime_error);  // truncated
    StoredDDData garbage = parse("ddasp-dd 1\natoms 1\na\nfacts 0\nzdd 5\nhello");
    EXPECT_THROW(import_stored_dd(garbage), std::runtime_error);
    // A diagram with more levels than atoms.
    std::string bytes = store(compile_aspif(kChoice));
    size_t atoms = bytes.find("atoms 3\na\nb\nc\n");
    ASSERT_NE(std::string::npos, atoms);
    std::string fewer = bytes.substr(0, atoms) + "atoms 2\nb\nc\n" + bytes.substr(atoms + std::string("atoms 3\na\nb\nc\n").size());
    EXPECT_THROW(import_stored_dd(parse(fewer)), std::runtime_error);
}
