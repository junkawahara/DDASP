#include "ddasp/compiler.h"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "ddasp/dd_util.h"

namespace ddasp {

using kyotodd::BDD;
using kyotodd::ZDD;
using kyotodd::bddvar;

std::string CompileResult::atom_name(Atom a) const {
    std::map<Atom, std::string>::const_iterator it = names.find(a);
    if (it != names.end()) return it->second;
    return "__atom" + std::to_string(a);
}

namespace {

/** Collect every atom mentioned in the program, in ascending id order. */
std::vector<Atom> collect_atoms(const Program& prog) {
    std::set<Atom> atoms;
    for (size_t r = 0; r < prog.rules().size(); ++r) {
        const Rule& rule = prog.rules()[r];
        atoms.insert(rule.head.begin(), rule.head.end());
        for (size_t i = 0; i < rule.body.lits.size(); ++i) atoms.insert(atom_of(rule.body.lits[i].lit));
    }
    for (size_t m = 0; m < prog.minimizes().size(); ++m) {
        for (size_t i = 0; i < prog.minimizes()[m].lits.size(); ++i) {
            atoms.insert(atom_of(prog.minimizes()[m].lits[i].lit));
        }
    }
    for (size_t o = 0; o < prog.outputs().size(); ++o) {
        for (size_t i = 0; i < prog.outputs()[o].condition.size(); ++i) {
            atoms.insert(atom_of(prog.outputs()[o].condition[i]));
        }
    }
    for (size_t e = 0; e < prog.externals().size(); ++e) atoms.insert(prog.externals()[e].atom);
    return std::vector<Atom>(atoms.begin(), atoms.end());
}

/** Atom order from top to bottom: the user-given order first, then the rest. */
std::vector<Atom> resolve_order(const std::vector<Atom>& atoms, const std::vector<Atom>& user_order) {
    std::set<Atom> known(atoms.begin(), atoms.end());
    std::vector<Atom> order;
    std::set<Atom> placed;
    for (size_t i = 0; i < user_order.size(); ++i) {
        Atom a = user_order[i];
        if (!known.count(a)) throw std::invalid_argument("order mentions unknown atom " + std::to_string(a));
        if (placed.insert(a).second) order.push_back(a);
    }
    for (size_t i = 0; i < atoms.size(); ++i) {
        if (!placed.count(atoms[i])) order.push_back(atoms[i]);
    }
    return order;
}

/**
 * Two-copy compilation.
 *
 * x_a: "a ∈ X" (candidate answer set), y_a: "a ∈ Y" (candidate smaller model of the reduct).
 */
class TwoCopyCompiler {
public:
    TwoCopyCompiler(const Program& prog, const std::vector<Atom>& order) : prog_(prog), order_(order) {
        allocate_variables();
    }

    CompileResult run() {
        BDD models = build_models();
        BDD counter = build_counterexamples();
        BDD stable = models & ~counter;

        CompileResult res;
        res.bdd = stable;
        res.vars = x_vars_;
        res.zdd = bdd_to_zdd_over(stable, x_vars_);
        res.var_to_atom = var_to_atom_;
        res.atom_to_var = atom_to_var_;
        collect_names(res);
        return res;
    }

private:
    void allocate_variables() {
        Atom max_atom = 0;
        for (size_t i = 0; i < order_.size(); ++i) max_atom = std::max(max_atom, order_[i]);
        atom_to_var_.assign(max_atom + 1, 0);
        atom_to_yvar_.assign(max_atom + 1, 0);

        // x variables: allocate bottom-up so that order_[0] ends up at the top level.
        std::vector<bddvar> xs(order_.size());
        for (size_t i = order_.size(); i-- > 0;) {
            xs[i] = kyotodd::bddnewvar();
            atom_to_var_[order_[i]] = xs[i];
        }
        // y variables: insert each one directly above its x variable.
        for (size_t i = 0; i < order_.size(); ++i) {
            bddvar lev = kyotodd::bddlevofvar(xs[i]);
            bddvar y = kyotodd::bddnewvaroflev(lev + 1);
            atom_to_yvar_[order_[i]] = y;
            y_vars_.push_back(y);
        }
        x_vars_ = xs;

        bddvar max_var = kyotodd::bddvarused();
        var_to_atom_.assign(max_var + 1, 0);
        for (size_t i = 0; i < order_.size(); ++i) var_to_atom_[xs[i]] = order_[i];
    }

    BDD x_of(Atom a) const { return BDD::prime(atom_to_var_[a]); }
    BDD y_of(Atom a) const { return BDD::prime(atom_to_yvar_[a]); }

    /** Literal evaluated in X. */
    BDD lit_x(Literal l) const { return is_positive(l) ? x_of(atom_of(l)) : ~x_of(atom_of(l)); }
    /** Literal of the reduct P^X evaluated in Y: positive atoms in Y, negated atoms in X. */
    BDD lit_reduct(Literal l) const { return is_positive(l) ? y_of(atom_of(l)) : ~x_of(atom_of(l)); }

    template <typename LitFn>
    BDD body_bdd(const Body& body, LitFn lit) const {
        if (body.type == BodyType::Normal) {
            BDD b = BDD::True;
            for (size_t i = 0; i < body.lits.size(); ++i) b &= lit(body.lits[i].lit);
            return b;
        }
        std::vector<BDD> terms;
        std::vector<long long> weights;
        for (size_t i = 0; i < body.lits.size(); ++i) {
            terms.push_back(lit(body.lits[i].lit));
            weights.push_back(body.lits[i].weight);
        }
        return threshold_bdd(terms, weights, body.bound);
    }

    struct LitX {
        const TwoCopyCompiler* c;
        BDD operator()(Literal l) const { return c->lit_x(l); }
    };
    struct LitReduct {
        const TwoCopyCompiler* c;
        BDD operator()(Literal l) const { return c->lit_reduct(l); }
    };

    /** Models(X): X satisfies every rule classically (choice rules impose nothing). */
    BDD build_models() const {
        BDD models = BDD::True;
        LitX lx = {this};
        for (size_t r = 0; r < prog_.rules().size(); ++r) {
            const Rule& rule = prog_.rules()[r];
            if (rule.head_type == HeadType::Choice) continue;
            BDD head = BDD::False;
            for (size_t i = 0; i < rule.head.size(); ++i) head |= x_of(rule.head[i]);
            models &= ~body_bdd(rule.body, lx) | head;
        }
        return models;
    }

    /** ∃Y (Y ⊆ X ∧ Y ≠ X ∧ Y ⊨ P^X): X has a strictly smaller model of its reduct. */
    BDD build_counterexamples() const {
        LitReduct lr = {this};
        BDD f = BDD::True;
        for (size_t i = 0; i < order_.size(); ++i) {
            f &= ~y_of(order_[i]) | x_of(order_[i]);  // Y ⊆ X
        }
        for (size_t r = 0; r < prog_.rules().size(); ++r) {
            const Rule& rule = prog_.rules()[r];
            BDD body = body_bdd(rule.body, lr);
            if (rule.head_type == HeadType::Choice) {
                // {H} :- B  contributes  a :- B+, not B-, not not a  for each a in H.
                for (size_t i = 0; i < rule.head.size(); ++i) {
                    Atom a = rule.head[i];
                    f &= ~body | ~x_of(a) | y_of(a);
                }
            } else {
                BDD head = BDD::False;
                for (size_t i = 0; i < rule.head.size(); ++i) head |= y_of(rule.head[i]);
                f &= ~body | head;
            }
        }
        // #external a is treated as the choice rule {a}.
        for (size_t e = 0; e < prog_.externals().size(); ++e) {
            Atom a = prog_.externals()[e].atom;
            f &= ~x_of(a) | y_of(a);
        }
        BDD neq = BDD::False;
        for (size_t i = 0; i < order_.size(); ++i) {
            neq |= x_of(order_[i]) & ~y_of(order_[i]);  // Y ≠ X
        }
        f &= neq;
        return f.exist(y_vars_);
    }

    void collect_names(CompileResult& res) const {
        for (size_t o = 0; o < prog_.outputs().size(); ++o) {
            const Output& out = prog_.outputs()[o];
            if (out.condition.empty()) {
                res.fact_names.push_back(out.name);
            } else if (out.condition.size() == 1 && is_positive(out.condition[0])) {
                Atom a = out.condition[0];
                if (!res.names.count(a)) res.names[a] = out.name;
            }
        }
    }

    const Program& prog_;
    std::vector<Atom> order_;
    std::vector<bddvar> x_vars_;
    std::vector<bddvar> y_vars_;
    std::vector<bddvar> atom_to_var_;
    std::vector<bddvar> atom_to_yvar_;
    std::vector<Atom> var_to_atom_;
};

}  // namespace

CompileResult compile(const Program& program, const CompileOptions& options) {
    std::vector<Atom> atoms = collect_atoms(program);
    std::vector<Atom> order = resolve_order(atoms, options.order);
    switch (options.method) {
        case Method::TwoCopy: {
            TwoCopyCompiler c(program, order);
            return c.run();
        }
    }
    throw std::invalid_argument("unknown compilation method");
}

}  // namespace ddasp
