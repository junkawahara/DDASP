#include <gtest/gtest.h>

#include "ddasp/aspif_reader.h"

using namespace ddasp;

namespace {

// gringo output for:  {a}. b :- a. :- not b.  #show a/0. #show b/0.
const char* kSimple =
    "asp 1 0 0\n"
    "1 1 1 1 0 0\n"
    "1 0 1 2 0 1 1\n"
    "1 0 0 0 1 -2\n"
    "4 1 a 1 1\n"
    "4 1 b 1 2\n"
    "0\n";

}  // namespace

TEST(AspifReader, ParsesRulesAndOutputs) {
    Program p = read_aspif_string(kSimple);
    ASSERT_EQ(3u, p.rules().size());

    const Rule& choice = p.rules()[0];
    EXPECT_EQ(HeadType::Choice, choice.head_type);
    ASSERT_EQ(1u, choice.head.size());
    EXPECT_EQ(1, choice.head[0]);
    EXPECT_EQ(BodyType::Normal, choice.body.type);
    EXPECT_TRUE(choice.body.lits.empty());

    const Rule& normal = p.rules()[1];
    EXPECT_EQ(HeadType::Disjunction, normal.head_type);
    ASSERT_EQ(1u, normal.head.size());
    EXPECT_EQ(2, normal.head[0]);
    ASSERT_EQ(1u, normal.body.lits.size());
    EXPECT_EQ(1, normal.body.lits[0].lit);

    const Rule& constraint = p.rules()[2];
    EXPECT_TRUE(constraint.is_constraint());
    ASSERT_EQ(1u, constraint.body.lits.size());
    EXPECT_EQ(-2, constraint.body.lits[0].lit);

    ASSERT_EQ(2u, p.outputs().size());
    EXPECT_EQ("a", p.outputs()[0].name);
    EXPECT_EQ(std::vector<Literal>(1, 1), p.outputs()[0].condition);
    EXPECT_EQ("b", p.outputs()[1].name);
    EXPECT_EQ(2, p.max_atom());
}

TEST(AspifReader, RoundTripThroughToString) {
    Program p = read_aspif_string(kSimple);
    EXPECT_EQ(std::string(kSimple), p.to_string());
}

TEST(AspifReader, WeightBodyMinimizeExternalProjection) {
    const char* text =
        "asp 1 0 0\n"
        "1 0 1 3 1 2 2 1 1 -2 1\n"
        "2 0 2 1 2 -3 4\n"
        "3 2 1 3\n"
        "5 4 0\n"
        "5 5 2\n"
        "0\n";
    Program p = read_aspif_string(text);
    ASSERT_EQ(1u, p.rules().size());
    const Body& b = p.rules()[0].body;
    EXPECT_EQ(BodyType::Weight, b.type);
    EXPECT_EQ(2, b.bound);
    ASSERT_EQ(2u, b.lits.size());
    EXPECT_EQ(WeightedLiteral(1, 1), b.lits[0]);
    EXPECT_EQ(WeightedLiteral(-2, 1), b.lits[1]);

    ASSERT_EQ(1u, p.minimizes().size());
    EXPECT_EQ(0, p.minimizes()[0].priority);
    ASSERT_EQ(2u, p.minimizes()[0].lits.size());
    EXPECT_EQ(WeightedLiteral(-3, 4), p.minimizes()[0].lits[1]);

    std::vector<Atom> proj;
    proj.push_back(1);
    proj.push_back(3);
    EXPECT_EQ(proj, p.projections());

    ASSERT_EQ(2u, p.externals().size());
    EXPECT_EQ(4, p.externals()[0].atom);
    EXPECT_EQ(ExternalValue::Free, p.externals()[0].value);
    EXPECT_EQ(5, p.externals()[1].atom);
    EXPECT_EQ(ExternalValue::False, p.externals()[1].value);
    EXPECT_EQ(5, p.max_atom());
}

TEST(AspifReader, OutputStringMayContainSpaces) {
    const char* text =
        "asp 1 0 0\n"
        "4 8 p(\"a b\") 1 1\n"
        "4 3 foo 0\n"
        "0\n";
    Program p = read_aspif_string(text);
    ASSERT_EQ(2u, p.outputs().size());
    EXPECT_EQ("p(\"a b\")", p.outputs()[0].name);
    EXPECT_EQ(std::vector<Literal>(1, 1), p.outputs()[0].condition);
    EXPECT_EQ("foo", p.outputs()[1].name);
    EXPECT_TRUE(p.outputs()[1].condition.empty());
}

TEST(AspifReader, CommentsBlankLinesAndIncrementalSteps) {
    const char* text =
        "asp 1 0 0 incremental\n"
        "10 this is a comment\n"
        "\n"
        "1 0 1 1 0 0\n"
        "0\n"
        "1 0 1 2 0 1 1\n"
        "0\n";
    Program p = read_aspif_string(text);
    EXPECT_EQ(2u, p.rules().size());
}

TEST(AspifReader, SkippedStatementsProduceWarnings) {
    const char* text =
        "asp 1 0 0\n"
        "6 1 1\n"
        "7 0 1 1 0 0\n"
        "8 0 1 0\n"
        "9 0 1 2\n"
        "0\n";
    std::vector<std::string> warnings;
    Program p = read_aspif_string(text, &warnings);
    EXPECT_TRUE(p.rules().empty());
    ASSERT_EQ(4u, warnings.size());
    EXPECT_NE(std::string::npos, warnings[0].find("assumption"));
    EXPECT_NE(std::string::npos, warnings[1].find("heuristic"));
    EXPECT_NE(std::string::npos, warnings[2].find("edge"));
    EXPECT_NE(std::string::npos, warnings[3].find("theory"));
}

TEST(AspifReader, Errors) {
    EXPECT_THROW(read_aspif_string(""), AspifError);
    EXPECT_THROW(read_aspif_string("hello\n"), AspifError);
    EXPECT_THROW(read_aspif_string("asp 2 0 0\n0\n"), AspifError);
    EXPECT_THROW(read_aspif_string("asp 1 0 0\n1 0 1 1 0\n0\n"), AspifError);      // missing body size
    EXPECT_THROW(read_aspif_string("asp 1 0 0\n1 2 0 0 0\n0\n"), AspifError);      // bad head type
    EXPECT_THROW(read_aspif_string("asp 1 0 0\n1 0 1 0 0 0\n0\n"), AspifError);    // atom 0 in head
    EXPECT_THROW(read_aspif_string("asp 1 0 0\n1 0 0 0 1 0\n0\n"), AspifError);    // literal 0
    EXPECT_THROW(read_aspif_string("asp 1 0 0\n1 0 0 0 0 7\n0\n"), AspifError);    // trailing token
    EXPECT_THROW(read_aspif_string("asp 1 0 0\n4 5 abc 0\n0\n"), AspifError);      // short output string
    EXPECT_THROW(read_aspif_string("asp 1 0 0\n5 1 9\n0\n"), AspifError);          // bad external value
    EXPECT_THROW(read_aspif_string("asp 1 0 0\n42 1\n0\n"), AspifError);           // unknown statement
    try {
        read_aspif_string("asp 1 0 0\n1 0 0 0\n0\n");
        FAIL() << "expected AspifError";
    } catch (const AspifError& e) {
        EXPECT_EQ(2, e.line());
    }
}
