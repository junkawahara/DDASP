#include "ddasp/program.h"

#include <sstream>
#include <stdexcept>

namespace ddasp {

namespace {

void check_atom(Atom a, const char* what) {
    if (a <= 0) throw std::invalid_argument(std::string("atom must be positive in ") + what);
}

void check_literal(Literal l, const char* what) {
    if (l == 0) throw std::invalid_argument(std::string("literal must be nonzero in ") + what);
}

}  // namespace

Body Body::normal(const std::vector<Literal>& literals) {
    Body b;
    b.type = BodyType::Normal;
    b.bound = static_cast<long long>(literals.size());
    b.lits.reserve(literals.size());
    for (size_t i = 0; i < literals.size(); ++i) {
        b.lits.push_back(WeightedLiteral(literals[i], 1));
    }
    return b;
}

Body Body::weight(long long bound, const std::vector<WeightedLiteral>& literals) {
    Body b;
    b.type = BodyType::Weight;
    b.bound = bound;
    b.lits = literals;
    return b;
}

void Program::note_atom(Atom a) {
    if (a > max_atom_) max_atom_ = a;
}

void Program::add_rule(const Rule& rule) {
    for (size_t i = 0; i < rule.head.size(); ++i) check_atom(rule.head[i], "a rule head");
    for (size_t i = 0; i < rule.body.lits.size(); ++i) check_literal(rule.body.lits[i].lit, "a rule body");
    for (size_t i = 0; i < rule.head.size(); ++i) note_atom(rule.head[i]);
    for (size_t i = 0; i < rule.body.lits.size(); ++i) note_literal(rule.body.lits[i].lit);
    rules_.push_back(rule);
}

void Program::add_minimize(const Minimize& minimize) {
    for (size_t i = 0; i < minimize.lits.size(); ++i) check_literal(minimize.lits[i].lit, "a #minimize statement");
    for (size_t i = 0; i < minimize.lits.size(); ++i) note_literal(minimize.lits[i].lit);
    minimizes_.push_back(minimize);
}

void Program::add_output(const Output& output) {
    for (size_t i = 0; i < output.condition.size(); ++i) check_literal(output.condition[i], "a #show condition");
    for (size_t i = 0; i < output.condition.size(); ++i) note_literal(output.condition[i]);
    outputs_.push_back(output);
}

void Program::add_external(const External& external) {
    check_atom(external.atom, "an #external statement");
    note_atom(external.atom);
    externals_.push_back(external);
}

void Program::add_projection(const std::vector<Atom>& atoms) {
    for (size_t i = 0; i < atoms.size(); ++i) check_atom(atoms[i], "a #project statement");
    for (size_t i = 0; i < atoms.size(); ++i) {
        note_atom(atoms[i]);
        projections_.push_back(atoms[i]);
    }
}

namespace {

void write_weighted(std::ostringstream& os, const std::vector<WeightedLiteral>& lits) {
    os << ' ' << lits.size();
    for (size_t i = 0; i < lits.size(); ++i) {
        os << ' ' << lits[i].lit << ' ' << lits[i].weight;
    }
}

}  // namespace

std::string Program::to_string() const {
    std::ostringstream os;
    os << "asp 1 0 0\n";
    for (size_t r = 0; r < rules_.size(); ++r) {
        const Rule& rule = rules_[r];
        os << "1 " << (rule.head_type == HeadType::Choice ? 1 : 0) << ' ' << rule.head.size();
        for (size_t i = 0; i < rule.head.size(); ++i) os << ' ' << rule.head[i];
        if (rule.body.type == BodyType::Normal) {
            os << " 0 " << rule.body.lits.size();
            for (size_t i = 0; i < rule.body.lits.size(); ++i) os << ' ' << rule.body.lits[i].lit;
        } else {
            os << " 1 " << rule.body.bound;
            write_weighted(os, rule.body.lits);
        }
        os << '\n';
    }
    for (size_t m = 0; m < minimizes_.size(); ++m) {
        os << "2 " << minimizes_[m].priority;
        write_weighted(os, minimizes_[m].lits);
        os << '\n';
    }
    if (!projections_.empty()) {
        os << "3 " << projections_.size();
        for (size_t i = 0; i < projections_.size(); ++i) os << ' ' << projections_[i];
        os << '\n';
    }
    for (size_t o = 0; o < outputs_.size(); ++o) {
        const Output& out = outputs_[o];
        os << "4 " << out.name.size() << ' ' << out.name << ' ' << out.condition.size();
        for (size_t i = 0; i < out.condition.size(); ++i) os << ' ' << out.condition[i];
        os << '\n';
    }
    for (size_t e = 0; e < externals_.size(); ++e) {
        os << "5 " << externals_[e].atom << ' ' << static_cast<int>(externals_[e].value) << '\n';
    }
    os << "0\n";
    return os.str();
}

}  // namespace ddasp
