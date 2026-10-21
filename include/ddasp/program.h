#ifndef DDASP_PROGRAM_H
#define DDASP_PROGRAM_H

/**
 * @file program.h
 * @brief In-memory representation of a ground ASP program.
 *
 * The representation mirrors the aspif intermediate format produced by gringo:
 * atoms are positive integers, literals are signed integers (negative = default
 * negation), rules have a disjunctive or choice head and a normal or weight body.
 */

#include <string>
#include <vector>

namespace ddasp {

/** @brief Atom identifier (positive integer, gringo numbering). */
typedef int Atom;

/** @brief Signed literal: +a means "a", -a means "not a". */
typedef int Literal;

/** @brief Return the atom of a literal. */
inline Atom atom_of(Literal lit) { return lit < 0 ? -lit : lit; }

/** @brief True if the literal is a positive literal. */
inline bool is_positive(Literal lit) { return lit > 0; }

/** @brief Head type of a rule. */
enum class HeadType {
    Disjunction,  ///< a1 | a2 | ... :- body.  (empty head = integrity constraint)
    Choice        ///< { a1 ; a2 ; ... } :- body.
};

/** @brief Body type of a rule. */
enum class BodyType {
    Normal,  ///< conjunction of literals
    Weight   ///< sum(w_i * [lit_i]) >= bound
};

/** @brief A literal with an attached weight. */
struct WeightedLiteral {
    Literal lit;
    long long weight;

    WeightedLiteral() : lit(0), weight(0) {}
    WeightedLiteral(Literal l, long long w) : lit(l), weight(w) {}

    bool operator==(const WeightedLiteral& o) const {
        return lit == o.lit && weight == o.weight;
    }
};

/**
 * @brief Rule body.
 *
 * For a Normal body, @c lits holds the conjunction (weights are all 1 and
 * @c bound equals the number of literals, so that a normal body is also a valid
 * weight body). For a Weight body, the body is satisfied iff the sum of the
 * weights of the true literals is at least @c bound.
 */
struct Body {
    BodyType type;
    long long bound;
    std::vector<WeightedLiteral> lits;

    Body() : type(BodyType::Normal), bound(0) {}

    /** @brief Build a normal body from a list of literals. */
    static Body normal(const std::vector<Literal>& literals);
    /** @brief Build a weight body. */
    static Body weight(long long bound, const std::vector<WeightedLiteral>& literals);

    bool operator==(const Body& o) const {
        return type == o.type && bound == o.bound && lits == o.lits;
    }
};

/** @brief A ground rule. */
struct Rule {
    HeadType head_type;
    std::vector<Atom> head;
    Body body;

    Rule() : head_type(HeadType::Disjunction) {}
    Rule(HeadType ht, const std::vector<Atom>& h, const Body& b)
        : head_type(ht), head(h), body(b) {}

    /** @brief True if the rule is an integrity constraint (empty disjunctive head). */
    bool is_constraint() const {
        return head_type == HeadType::Disjunction && head.empty();
    }

    bool operator==(const Rule& o) const {
        return head_type == o.head_type && head == o.head && body == o.body;
    }
};

/** @brief A #minimize statement (one priority level). */
struct Minimize {
    int priority;
    std::vector<WeightedLiteral> lits;

    Minimize() : priority(0) {}
    Minimize(int p, const std::vector<WeightedLiteral>& l) : priority(p), lits(l) {}

    bool operator==(const Minimize& o) const {
        return priority == o.priority && lits == o.lits;
    }
};

/** @brief An output (#show) statement: @c name is shown when all of @c condition hold. */
struct Output {
    std::string name;
    std::vector<Literal> condition;

    Output() {}
    Output(const std::string& n, const std::vector<Literal>& c) : name(n), condition(c) {}

    bool operator==(const Output& o) const {
        return name == o.name && condition == o.condition;
    }
};

/** @brief Initial value of an #external atom (as in aspif). */
enum class ExternalValue { Free, True, False, Release };

/** @brief An #external statement. */
struct External {
    Atom atom;
    ExternalValue value;

    External() : atom(0), value(ExternalValue::Free) {}
    External(Atom a, ExternalValue v) : atom(a), value(v) {}

    bool operator==(const External& o) const {
        return atom == o.atom && value == o.value;
    }
};

/**
 * @brief A ground ASP program.
 *
 * Built incrementally through the @c add_* methods (by the aspif reader or by
 * a Python observer) and consumed by the compiler.
 */
class Program {
public:
    Program() : max_atom_(0) {}

    /**
     * @name Building the program
     * Every atom must be positive and every literal nonzero; otherwise
     * std::invalid_argument is thrown and the statement is not added.
     */
    ///@{
    void add_rule(const Rule& rule);
    void add_minimize(const Minimize& minimize);
    void add_output(const Output& output);
    void add_external(const External& external);
    /** @brief Record a #project statement (kept for information only). */
    void add_projection(const std::vector<Atom>& atoms);
    ///@}

    const std::vector<Rule>& rules() const { return rules_; }
    const std::vector<Minimize>& minimizes() const { return minimizes_; }
    const std::vector<Output>& outputs() const { return outputs_; }
    const std::vector<External>& externals() const { return externals_; }
    const std::vector<Atom>& projections() const { return projections_; }

    /** @brief Largest atom id mentioned anywhere in the program (0 if none). */
    Atom max_atom() const { return max_atom_; }

    /** @brief Render the program as aspif-like text (for debugging and tests). */
    std::string to_string() const;

private:
    void note_atom(Atom a);
    void note_literal(Literal l) { note_atom(atom_of(l)); }

    std::vector<Rule> rules_;
    std::vector<Minimize> minimizes_;
    std::vector<Output> outputs_;
    std::vector<External> externals_;
    std::vector<Atom> projections_;
    Atom max_atom_;
};

}  // namespace ddasp

#endif  // DDASP_PROGRAM_H
