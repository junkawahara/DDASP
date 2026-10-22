#include <gtest/gtest.h>

#include <stdexcept>

#include "ddasp/program.h"

using namespace ddasp;

TEST(Program, NormalBodyStoresUnitWeights) {
    std::vector<Literal> lits;
    lits.push_back(2);
    lits.push_back(-3);
    Body b = Body::normal(lits);
    EXPECT_EQ(BodyType::Normal, b.type);
    EXPECT_EQ(2, b.bound);
    ASSERT_EQ(2u, b.lits.size());
    EXPECT_EQ(2, b.lits[0].lit);
    EXPECT_EQ(1, b.lits[0].weight);
    EXPECT_EQ(-3, b.lits[1].lit);
    EXPECT_EQ(1, b.lits[1].weight);
}

TEST(Program, WeightBody) {
    std::vector<WeightedLiteral> wl;
    wl.push_back(WeightedLiteral(1, 3));
    wl.push_back(WeightedLiteral(-2, 5));
    Body b = Body::weight(4, wl);
    EXPECT_EQ(BodyType::Weight, b.type);
    EXPECT_EQ(4, b.bound);
    EXPECT_EQ(wl, b.lits);
}

TEST(Program, LiteralHelpers) {
    EXPECT_EQ(5, atom_of(5));
    EXPECT_EQ(5, atom_of(-5));
    EXPECT_TRUE(is_positive(5));
    EXPECT_FALSE(is_positive(-5));
}

TEST(Program, ConstraintDetection) {
    Rule constraint(HeadType::Disjunction, std::vector<Atom>(), Body::normal(std::vector<Literal>(1, 1)));
    EXPECT_TRUE(constraint.is_constraint());
    Rule fact(HeadType::Disjunction, std::vector<Atom>(1, 1), Body::normal(std::vector<Literal>()));
    EXPECT_FALSE(fact.is_constraint());
    Rule choice(HeadType::Choice, std::vector<Atom>(), Body::normal(std::vector<Literal>()));
    EXPECT_FALSE(choice.is_constraint());
}

TEST(Program, MaxAtomTracksAllStatements) {
    Program p;
    EXPECT_EQ(0, p.max_atom());
    p.add_rule(Rule(HeadType::Disjunction, std::vector<Atom>(1, 3), Body::normal(std::vector<Literal>(1, -7))));
    EXPECT_EQ(7, p.max_atom());
    p.add_minimize(Minimize(0, std::vector<WeightedLiteral>(1, WeightedLiteral(-9, 1))));
    EXPECT_EQ(9, p.max_atom());
    p.add_output(Output("q", std::vector<Literal>(1, 11)));
    EXPECT_EQ(11, p.max_atom());
    p.add_external(External(12, ExternalValue::Free));
    EXPECT_EQ(12, p.max_atom());
    p.add_projection(std::vector<Atom>(1, 13));
    EXPECT_EQ(13, p.max_atom());
    p.add_assumption(std::vector<Literal>(1, -14));
    EXPECT_EQ(14, p.max_atom());
    EXPECT_EQ(1u, p.assumptions().size());
    EXPECT_EQ(1u, p.rules().size());
    EXPECT_EQ(1u, p.minimizes().size());
    EXPECT_EQ(1u, p.outputs().size());
    EXPECT_EQ(1u, p.externals().size());
    EXPECT_EQ(1u, p.projections().size());
}

TEST(Program, ToStringRoundTripsAspifShape) {
    Program p;
    p.add_rule(Rule(HeadType::Choice, std::vector<Atom>(1, 1), Body::normal(std::vector<Literal>())));
    std::vector<Literal> body;
    body.push_back(1);
    body.push_back(-2);
    p.add_rule(Rule(HeadType::Disjunction, std::vector<Atom>(1, 2), Body::normal(body)));
    std::vector<WeightedLiteral> wl;
    wl.push_back(WeightedLiteral(1, 2));
    wl.push_back(WeightedLiteral(2, 3));
    p.add_rule(Rule(HeadType::Disjunction, std::vector<Atom>(), Body::weight(4, wl)));
    p.add_minimize(Minimize(1, std::vector<WeightedLiteral>(1, WeightedLiteral(2, 1))));
    p.add_output(Output("p(1)", std::vector<Literal>(1, 1)));
    p.add_external(External(3, ExternalValue::False));
    const std::string expected =
        "asp 1 0 0\n"
        "1 1 1 1 0 0\n"
        "1 0 1 2 0 2 1 -2\n"
        "1 0 0 1 4 2 1 2 2 3\n"
        "2 1 1 2 1\n"
        "4 4 p(1) 1 1\n"
        "5 3 2\n"
        "0\n";
    EXPECT_EQ(expected, p.to_string());
}

TEST(Program, RejectsNonPositiveAtomsAndZeroLiterals) {
    Program p;
    EXPECT_THROW(p.add_rule(Rule(HeadType::Disjunction, std::vector<Atom>(1, -1), Body::normal(std::vector<Literal>()))),
                 std::invalid_argument);
    EXPECT_THROW(p.add_rule(Rule(HeadType::Choice, std::vector<Atom>(1, 0), Body::normal(std::vector<Literal>()))),
                 std::invalid_argument);
    EXPECT_THROW(p.add_rule(Rule(HeadType::Disjunction, std::vector<Atom>(1, 1), Body::normal(std::vector<Literal>(1, 0)))),
                 std::invalid_argument);
    EXPECT_THROW(p.add_rule(Rule(HeadType::Disjunction, std::vector<Atom>(1, 1),
                                 Body::weight(1, std::vector<WeightedLiteral>(1, WeightedLiteral(0, 1))))),
                 std::invalid_argument);
    EXPECT_THROW(p.add_minimize(Minimize(0, std::vector<WeightedLiteral>(1, WeightedLiteral(0, 1)))), std::invalid_argument);
    EXPECT_THROW(p.add_output(Output("a", std::vector<Literal>(1, 0))), std::invalid_argument);
    EXPECT_THROW(p.add_external(External(0, ExternalValue::Free)), std::invalid_argument);
    EXPECT_THROW(p.add_projection(std::vector<Atom>(1, -2)), std::invalid_argument);
    EXPECT_THROW(p.add_assumption(std::vector<Literal>(1, 0)), std::invalid_argument);
    EXPECT_TRUE(p.assumptions().empty());
    // Nothing was added and the largest atom is unchanged.
    EXPECT_TRUE(p.rules().empty() && p.minimizes().empty() && p.outputs().empty() && p.externals().empty());
    EXPECT_TRUE(p.projections().empty());
    EXPECT_EQ(0, p.max_atom());
}
