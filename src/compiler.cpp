#include "ddasp/compiler.h"

#include <algorithm>
#include <cctype>
#include <chrono>
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

std::vector<std::string> CompileResult::shown_names(const std::vector<Atom>& atoms) const {
    std::vector<std::string> out(fact_names);
    for (size_t i = 0; i < atoms.size(); ++i) {
        std::map<Atom, std::string>::const_iterator it = names.find(atoms[i]);
        if (it != names.end()) out.push_back(it->second);
    }
    if (!terms.empty()) {
        std::set<Atom> in(atoms.begin(), atoms.end());
        for (size_t t = 0; t < terms.size(); ++t) {
            const std::vector<Literal>& cond = terms[t].condition;
            bool holds = true;
            for (size_t i = 0; i < cond.size() && holds; ++i) {
                holds = (in.count(atom_of(cond[i])) != 0) == is_positive(cond[i]);
            }
            if (holds) out.push_back(terms[t].name);
        }
    }
    // A name shown through several outputs is displayed once (clingo prints a set of symbols).
    std::set<std::string> seen;
    std::vector<std::string> unique;
    for (size_t i = 0; i < out.size(); ++i) {
        if (seen.insert(out[i]).second) unique.push_back(out[i]);
    }
    return unique;
}

std::map<Atom, std::string> atom_names(const Program& prog) {
    std::map<Atom, std::string> names;
    for (size_t o = 0; o < prog.outputs().size(); ++o) {
        const Output& out = prog.outputs()[o];
        if (out.condition.size() == 1 && is_positive(out.condition[0])) {
            names.insert(std::make_pair(out.condition[0], out.name));  // first one wins
        }
    }
    return names;
}

OrderHeuristic parse_order_heuristic(const std::string& name) {
    if (name == "id") return OrderHeuristic::AtomId;
    if (name == "name") return OrderHeuristic::NaturalName;
    throw std::invalid_argument("unknown order heuristic '" + name + "' (expected id or name)");
}

bool natural_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        bool da = isdigit(static_cast<unsigned char>(a[i])) != 0;
        bool db = isdigit(static_cast<unsigned char>(b[j])) != 0;
        if (da && db) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && isdigit(static_cast<unsigned char>(a[i2]))) ++i2;
            while (j2 < b.size() && isdigit(static_cast<unsigned char>(b[j2]))) ++j2;
            // Compare digit runs numerically: strip leading zeros, then by length, then lexically.
            size_t si = i, sj = j;
            while (si + 1 < i2 && a[si] == '0') ++si;
            while (sj + 1 < j2 && b[sj] == '0') ++sj;
            size_t la = i2 - si, lb = j2 - sj;
            if (la != lb) return la < lb;
            int c = a.compare(si, la, b, sj, lb);
            if (c != 0) return c < 0;
            i = i2;
            j = j2;
        } else {
            if (a[i] != b[j]) return a[i] < b[j];
            ++i;
            ++j;
        }
    }
    return (a.size() - i) < (b.size() - j);
}

Method parse_method(const std::string& name) {
    if (name == "twocopy") return Method::TwoCopy;
    if (name == "scc") return Method::Scc;
    throw std::invalid_argument("unknown method '" + name + "' (expected twocopy or scc)");
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

/**
 * Split the #show outputs into fact names, primary atom names and terms, and
 * return the set of shown atoms (named atoms plus the atoms of term conditions).
 */
std::set<Atom> collect_names(const Program& prog, CompileResult& res) {
    res.names = atom_names(prog);
    std::set<Atom> shown;
    for (size_t o = 0; o < prog.outputs().size(); ++o) {
        const Output& out = prog.outputs()[o];
        if (out.condition.empty()) {
            res.fact_names.push_back(out.name);
            continue;
        }
        for (size_t i = 0; i < out.condition.size(); ++i) shown.insert(atom_of(out.condition[i]));
        if (out.condition.size() == 1 && is_positive(out.condition[0]) &&
            res.names[out.condition[0]] == out.name) {
            continue;  // the atom's primary name (a repeated identical output is dropped)
        }
        res.terms.push_back(ShowTerm(out.name, out.condition));
    }
    return shown;
}

struct NameLess {
    const std::map<Atom, std::string>* names;
    bool operator()(Atom a, Atom b) const {
        const std::string& na = names->find(a)->second;
        const std::string& nb = names->find(b)->second;
        if (natural_less(na, nb)) return true;
        if (natural_less(nb, na)) return false;
        return a < b;
    }
};

struct KeyLess {
    const std::vector<double>* key;
    bool operator()(Atom a, Atom b) const {
        if ((*key)[a] != (*key)[b]) return (*key)[a] < (*key)[b];
        return a < b;
    }
};

/**
 * NaturalName heuristic: shown atoms sorted by name; every unnamed atom is
 * placed at the average position of the atoms it shares a rule with
 * (two refinement passes so that unnamed atoms can also follow each other).
 */
std::vector<Atom> natural_name_order(const Program& prog, const std::vector<Atom>& atoms,
                                     const std::map<Atom, std::string>& names) {
    Atom max_atom = atoms.empty() ? 0 : atoms.back();
    std::vector<Atom> named, unnamed;
    for (size_t i = 0; i < atoms.size(); ++i) (names.count(atoms[i]) ? named : unnamed).push_back(atoms[i]);
    NameLess nl = {&names};
    std::sort(named.begin(), named.end(), nl);
    if (unnamed.empty()) return named;

    // An unnamed atom's neighbours are the other atoms of the rules it occurs in
    // (one occurrence = one neighbour, as a multiset). Rather than listing them,
    // which is quadratic in the rule length, each pass sums the placed keys per
    // rule once and every atom subtracts its own contribution.
    const size_t R = prog.rules().size();
    std::vector<std::vector<Atom> > rule_atoms(R);
    std::vector<std::vector<size_t> > rules_of(max_atom + 1);
    for (size_t r = 0; r < R; ++r) {
        const Rule& rule = prog.rules()[r];
        std::vector<Atom>& in_rule = rule_atoms[r];
        in_rule = rule.head;
        for (size_t i = 0; i < rule.body.lits.size(); ++i) in_rule.push_back(atom_of(rule.body.lits[i].lit));
        for (size_t i = 0; i < in_rule.size(); ++i) {
            if (!names.count(in_rule[i])) rules_of[in_rule[i]].push_back(r);
        }
    }

    const double unplaced = -1.0;
    std::vector<double> key(max_atom + 1, unplaced);
    for (size_t i = 0; i < named.size(); ++i) key[named[i]] = static_cast<double>(i);
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<double> rule_sum(R, 0.0);
        std::vector<size_t> rule_cnt(R, 0);
        for (size_t r = 0; r < R; ++r) {
            for (size_t i = 0; i < rule_atoms[r].size(); ++i) {
                double k = key[rule_atoms[r][i]];
                if (k != unplaced) {
                    rule_sum[r] += k;
                    ++rule_cnt[r];
                }
            }
        }
        std::vector<double> next(key);
        for (size_t u = 0; u < unnamed.size(); ++u) {
            Atom a = unnamed[u];
            bool placed = key[a] != unplaced;
            double sum = 0;
            size_t cnt = 0;
            for (size_t k = 0; k < rules_of[a].size(); ++k) {
                size_t r = rules_of[a][k];
                sum += rule_sum[r] - (placed ? key[a] : 0.0);
                cnt += rule_cnt[r] - (placed ? 1 : 0);
            }
            if (cnt > 0) next[a] = sum / static_cast<double>(cnt) + 0.5;  // just below the barycenter
        }
        key = next;
    }
    double tail = static_cast<double>(named.size()) + 1.0;
    for (size_t u = 0; u < unnamed.size(); ++u) {
        if (key[unnamed[u]] == unplaced) key[unnamed[u]] = tail;
    }
    std::vector<Atom> order(atoms);
    KeyLess kl = {&key};
    std::sort(order.begin(), order.end(), kl);
    return order;
}

/** Atom order from top to bottom: the user-given order first, then the heuristic order. */
std::vector<Atom> resolve_order(const Program& prog, const std::vector<Atom>& atoms,
                                const CompileOptions& options, const std::map<Atom, std::string>& names) {
    std::set<Atom> known(atoms.begin(), atoms.end());
    std::vector<Atom> order;
    std::set<Atom> placed;
    for (size_t i = 0; i < options.order.size(); ++i) {
        Atom a = options.order[i];
        if (!known.count(a)) throw std::invalid_argument("order mentions unknown atom " + std::to_string(a));
        if (placed.insert(a).second) order.push_back(a);
    }
    std::vector<Atom> rest = atoms;
    if (options.order_heuristic == OrderHeuristic::NaturalName) rest = natural_name_order(prog, atoms, names);
    for (size_t i = 0; i < rest.size(); ++i) {
        if (!placed.count(rest[i])) order.push_back(rest[i]);
    }
    return order;
}

/** Conjoin BDDs in increasing order of their top level, which keeps intermediate results local. */
BDD conjoin_sorted(std::vector<BDD>& parts) {
    struct TopLess {
        bool operator()(const BDD& a, const BDD& b) const {
            bddvar la = a.is_terminal() ? 0 : kyotodd::bddlevofvar(a.top());
            bddvar lb = b.is_terminal() ? 0 : kyotodd::bddlevofvar(b.top());
            return la < lb;
        }
    };
    std::stable_sort(parts.begin(), parts.end(), TopLess());
    BDD f = BDD::True;
    for (size_t i = 0; i < parts.size(); ++i) f &= parts[i];
    return f;
}

/**
 * Non-trivial strongly connected components of the positive dependency graph
 * (edge b -> h for every rule with b a positive body atom and h a head atom).
 * A component is non-trivial if it has more than one atom or a self-loop.
 * Returned as lists of atoms; the order of components is unspecified.
 */
std::vector<std::vector<Atom> > nontrivial_sccs(const Program& prog, Atom max_atom) {
    std::vector<std::vector<Atom> > succ(max_atom + 1);
    std::vector<bool> self_loop(max_atom + 1, false);
    for (size_t r = 0; r < prog.rules().size(); ++r) {
        const Rule& rule = prog.rules()[r];
        for (size_t i = 0; i < rule.body.lits.size(); ++i) {
            Literal l = rule.body.lits[i].lit;
            if (!is_positive(l)) continue;
            for (size_t h = 0; h < rule.head.size(); ++h) {
                succ[l].push_back(rule.head[h]);
                if (rule.head[h] == l) self_loop[l] = true;
            }
        }
    }

    // Iterative Tarjan.
    std::vector<int> index(max_atom + 1, -1), lowlink(max_atom + 1, 0);
    std::vector<bool> on_stack(max_atom + 1, false);
    std::vector<Atom> stack;
    std::vector<std::vector<Atom> > sccs;
    int next_index = 0;
    struct Frame {
        Atom v;
        size_t next_child;
    };
    for (Atom root = 1; root <= max_atom; ++root) {
        if (index[root] != -1) continue;
        std::vector<Frame> call;
        Frame f0 = {root, 0};
        call.push_back(f0);
        index[root] = lowlink[root] = next_index++;
        stack.push_back(root);
        on_stack[root] = true;
        while (!call.empty()) {
            Frame& fr = call.back();
            Atom v = fr.v;
            if (fr.next_child < succ[v].size()) {
                Atom w = succ[v][fr.next_child++];
                if (index[w] == -1) {
                    index[w] = lowlink[w] = next_index++;
                    stack.push_back(w);
                    on_stack[w] = true;
                    Frame fw = {w, 0};
                    call.push_back(fw);
                } else if (on_stack[w]) {
                    lowlink[v] = std::min(lowlink[v], index[w]);
                }
                continue;
            }
            if (lowlink[v] == index[v]) {
                std::vector<Atom> comp;
                Atom w;
                do {
                    w = stack.back();
                    stack.pop_back();
                    on_stack[w] = false;
                    comp.push_back(w);
                } while (w != v);
                if (comp.size() > 1 || self_loop[v]) sccs.push_back(comp);
            }
            call.pop_back();
            if (!call.empty()) {
                Atom parent = call.back().v;
                lowlink[parent] = std::min(lowlink[parent], lowlink[v]);
            }
        }
    }
    return sccs;
}

/**
 * Compiler shared by both methods.
 *
 * x_a: "a ∈ X" (candidate answer set).
 * u_a: "a ∈ U" (candidate unfounded set), allocated only for atoms that belong
 *      to some group. Y = X \ U is the candidate smaller model of the reduct P^X.
 *
 * For every group G (a set of atoms), the compiler removes the candidates X
 * that have a non-empty unfounded set U ⊆ G:
 *   Bad_G(X) = ∃U ⊆ G [ U ∩ X ≠ ∅ ∧ U ⊆ X ∧ ∀ rules r with head(r) ∩ G ≠ ∅: Unf_r(X, U) ]
 * where Unf_r says that r does not support U: its body is false once the
 * positive occurrences of atoms in U are made false, or no head atom is in U,
 * or a head atom outside U is in X.
 *
 * TwoCopy uses the single group of all atoms. Scc uses the non-trivial SCCs of
 * the positive dependency graph as groups plus an explicit supportedness
 * constraint for every atom (the singleton unfounded sets).
 */
class Compiler {
public:
    Compiler(const Program& prog, const std::vector<Atom>& order, const std::set<Atom>& shown,
             const CompileOptions& options)
        : prog_(prog), order_(order), shown_(shown), options_(options) {
        Atom max_atom = 0;
        for (size_t i = 0; i < order_.size(); ++i) max_atom = std::max(max_atom, order_[i]);
        if (options_.method == Method::TwoCopy) {
            groups_.push_back(order_);
        } else {
            groups_ = nontrivial_sccs(prog_, max_atom);
        }
        allocate_variables(max_atom);
        index_rules(max_atom);
    }

    CompileResult run(CompileResult& res) {
        res.var_to_atom = var_to_atom_;
        res.atom_to_var = atom_to_var_;
        CompileStats& st = res.stats;
        st.groups = groups_.size();
        for (size_t i = 0; i < in_group_.size(); ++i) st.aux_vars += in_group_[i] ? 1 : 0;

        Timer t;
        std::vector<BDD> parts;
        build_models(parts);
        st.models_time = t.lap();
        if (options_.method == Method::Scc) {
            build_supported(parts);
            st.supported_time = t.lap();
        }
        BDD stable = conjoin_sorted(parts);
        parts.clear();
        st.conjoin_time = t.lap();
        st.models_nodes = stable.raw_size();
        note_peak(st);
        for (size_t g = 0; g < groups_.size(); ++g) {
            stable &= ~build_unfounded(groups_[g]);
            note_peak(st);
        }
        st.unfounded_time = t.lap();
        st.stable_nodes = stable.raw_size();
        ZDD family = bdd_to_zdd_over(stable, x_vars_);
        st.convert_time = t.lap();
        note_peak(st);

        res.costs = build_costs();
        if (options_.optimize && !family.is_zero()) {
            for (size_t c = 0; c < res.costs.size(); ++c) {
                std::vector<int> w = int_weights(res.costs[c].var_weights);
                long long best = family.min_weight(w);
                family = family.cost_bound_le(w, best);
                res.optimum.push_back(best + res.costs[c].offset);
            }
        }
        st.optimize_time = t.lap();

        res.vars = x_vars_;
        if (options_.project_to_shown) {
            std::vector<bddvar> hidden, shown;
            for (size_t i = 0; i < order_.size(); ++i) {
                Atom a = order_[i];
                (shown_.count(a) ? shown : hidden).push_back(atom_to_var_[a]);
            }
            if (!hidden.empty()) family = family.project(hidden);
            res.vars = shown;
        }
        res.zdd = family;
        res.bdd = zdd_to_bdd_over(family, res.vars);
        st.project_time = t.lap();
        note_peak(st);
        return res;
    }

private:
    struct Timer {
        std::chrono::steady_clock::time_point last;
        Timer() : last(std::chrono::steady_clock::now()) {}
        double lap() {
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            double s = std::chrono::duration<double>(now - last).count();
            last = now;
            return s;
        }
    };

    static void note_peak(CompileStats& st) {
        uint64_t used = kyotodd::bddused();
        if (used > st.peak_nodes) st.peak_nodes = used;
    }

    void allocate_variables(Atom max_atom) {
        atom_to_var_.assign(max_atom + 1, 0);
        atom_to_uvar_.assign(max_atom + 1, 0);
        in_group_.assign(max_atom + 1, false);
        for (size_t g = 0; g < groups_.size(); ++g) {
            for (size_t i = 0; i < groups_[g].size(); ++i) in_group_[groups_[g][i]] = true;
        }

        // x variables: allocate bottom-up so that order_[0] ends up at the top level.
        std::vector<bddvar> xs(order_.size());
        for (size_t i = order_.size(); i-- > 0;) {
            xs[i] = kyotodd::bddnewvar();
            atom_to_var_[order_[i]] = xs[i];
        }
        // u variables: insert each one directly above its x variable.
        for (size_t i = 0; i < order_.size(); ++i) {
            Atom a = order_[i];
            if (!in_group_[a]) continue;
            bddvar lev = kyotodd::bddlevofvar(xs[i]);
            atom_to_uvar_[a] = kyotodd::bddnewvaroflev(lev + 1);
        }
        x_vars_ = xs;

        bddvar max_var = kyotodd::bddvarused();
        var_to_atom_.assign(max_var + 1, 0);
        for (size_t i = 0; i < order_.size(); ++i) var_to_atom_[xs[i]] = order_[i];
    }

    /** rules_of_head_[a] = indices of the rules having a in the head. */
    void index_rules(Atom max_atom) {
        rules_of_head_.assign(max_atom + 1, std::vector<size_t>());
        for (size_t r = 0; r < prog_.rules().size(); ++r) {
            const Rule& rule = prog_.rules()[r];
            for (size_t h = 0; h < rule.head.size(); ++h) rules_of_head_[rule.head[h]].push_back(r);
        }
        is_external_.assign(max_atom + 1, false);
        for (size_t e = 0; e < prog_.externals().size(); ++e) is_external_[prog_.externals()[e].atom] = true;
    }

    BDD x_of(Atom a) const { return BDD::prime(atom_to_var_[a]); }
    BDD u_of(Atom a) const { return BDD::prime(atom_to_uvar_[a]); }

    /** Literal evaluated in X. */
    BDD lit_x(Literal l) const { return is_positive(l) ? x_of(atom_of(l)) : ~x_of(atom_of(l)); }

    /**
     * Literal evaluated in Y = X \ U, where U ranges over the atoms of the current
     * group: positive atoms of the group become x_a ∧ ¬u_a, everything else is as in X.
     */
    BDD lit_reduct(Literal l, const std::set<Atom>& group) const {
        Atom a = atom_of(l);
        if (is_positive(l) && group.count(a)) return x_of(a) & ~u_of(a);
        return lit_x(l);
    }

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
        const Compiler* c;
        BDD operator()(Literal l) const { return c->lit_x(l); }
    };
    struct LitReduct {
        const Compiler* c;
        const std::set<Atom>* group;
        BDD operator()(Literal l) const { return c->lit_reduct(l, *group); }
    };

    /** Models(X): X satisfies every rule classically (choice rules impose nothing). */
    void build_models(std::vector<BDD>& parts) const {
        LitX lx = {this};
        for (size_t r = 0; r < prog_.rules().size(); ++r) {
            const Rule& rule = prog_.rules()[r];
            if (rule.head_type == HeadType::Choice) continue;
            BDD head = BDD::False;
            for (size_t i = 0; i < rule.head.size(); ++i) head |= x_of(rule.head[i]);
            parts.push_back(~body_bdd(rule.body, lx) | head);
        }
    }

    /**
     * Supported(X): every atom a ∈ X has a rule with a in the head whose body is
     * true in X and whose other head atoms are false in X (external atoms are
     * always supported). This is the unfounded-set condition for singleton sets.
     */
    void build_supported(std::vector<BDD>& parts) const {
        LitX lx = {this};
        for (size_t i = 0; i < order_.size(); ++i) {
            Atom a = order_[i];
            if (is_external_[a]) continue;
            BDD support = BDD::False;
            const std::vector<size_t>& rs = rules_of_head_[a];
            for (size_t k = 0; k < rs.size(); ++k) {
                const Rule& rule = prog_.rules()[rs[k]];
                BDD s = body_bdd(rule.body, lx);
                if (rule.head_type == HeadType::Disjunction) {
                    for (size_t h = 0; h < rule.head.size(); ++h) {
                        if (rule.head[h] != a) s &= ~x_of(rule.head[h]);
                    }
                }
                support |= s;
            }
            parts.push_back(~x_of(a) | support);
        }
    }

    /** Bad_G(X) = ∃U ⊆ G (U ∩ X ≠ ∅ ∧ U ⊆ X ∧ U unfounded w.r.t. X). */
    BDD build_unfounded(const std::vector<Atom>& group_atoms) const {
        std::set<Atom> group(group_atoms.begin(), group_atoms.end());
        LitReduct lr = {this, &group};

        std::vector<BDD> parts;
        for (size_t i = 0; i < group_atoms.size(); ++i) {
            parts.push_back(~u_of(group_atoms[i]) | x_of(group_atoms[i]));  // U ⊆ X
        }

        std::set<size_t> rules;
        for (size_t i = 0; i < group_atoms.size(); ++i) {
            const std::vector<size_t>& rs = rules_of_head_[group_atoms[i]];
            rules.insert(rs.begin(), rs.end());
        }
        for (std::set<size_t>::const_iterator it = rules.begin(); it != rules.end(); ++it) {
            const Rule& rule = prog_.rules()[*it];
            BDD body = body_bdd(rule.body, lr);  // body with the atoms of U removed
            if (rule.head_type == HeadType::Choice) {
                // {H} :- B  behaves as  a :- B, not not a  for each a in H.
                for (size_t h = 0; h < rule.head.size(); ++h) {
                    Atom a = rule.head[h];
                    if (!group.count(a)) continue;
                    parts.push_back(~body | ~u_of(a));
                }
            } else {
                // r does not support U: body false, or head ∩ U = ∅, or (head \ U) ∩ X ≠ ∅.
                BDD head_in_u = BDD::False;
                BDD head_outside_u_in_x = BDD::False;
                for (size_t h = 0; h < rule.head.size(); ++h) {
                    Atom a = rule.head[h];
                    if (group.count(a)) {
                        head_in_u |= u_of(a);
                        head_outside_u_in_x |= x_of(a) & ~u_of(a);
                    } else {
                        head_outside_u_in_x |= x_of(a);
                    }
                }
                parts.push_back(~body | ~head_in_u | head_outside_u_in_x);
            }
        }
        // #external a is treated as the choice rule {a}: a can never be unfounded.
        for (size_t i = 0; i < group_atoms.size(); ++i) {
            if (is_external_[group_atoms[i]]) parts.push_back(~u_of(group_atoms[i]));
        }

        BDD nonempty = BDD::False;
        std::vector<bddvar> uvars;
        for (size_t i = 0; i < group_atoms.size(); ++i) {
            nonempty |= u_of(group_atoms[i]);
            uvars.push_back(atom_to_uvar_[group_atoms[i]]);
        }
        parts.push_back(nonempty);
        BDD f = conjoin_sorted(parts);
        return f.exist(variable_set(uvars));
    }

    /** Linear cost functions of the #minimize statements, most important priority first. */
    std::vector<CostFunction> build_costs() const {
        std::map<int, CostFunction> by_priority;
        for (size_t m = 0; m < prog_.minimizes().size(); ++m) {
            const Minimize& min = prog_.minimizes()[m];
            CostFunction& cf = by_priority[min.priority];
            cf.priority = min.priority;
            if (cf.var_weights.empty()) cf.var_weights.assign(kyotodd::bddvarused() + 1, 0);
            for (size_t i = 0; i < min.lits.size(); ++i) {
                Literal l = min.lits[i].lit;
                long long w = min.lits[i].weight;
                bddvar v = atom_to_var_[atom_of(l)];
                if (is_positive(l)) {
                    cf.var_weights[v] += w;
                } else {
                    cf.offset += w;
                    cf.var_weights[v] -= w;
                }
            }
        }
        std::vector<CostFunction> costs;
        for (std::map<int, CostFunction>::reverse_iterator it = by_priority.rbegin(); it != by_priority.rend(); ++it) {
            costs.push_back(it->second);
        }
        return costs;
    }

    static std::vector<int> int_weights(const std::vector<long long>& w) {
        std::vector<int> r(w.size());
        for (size_t i = 0; i < w.size(); ++i) {
            if (w[i] > 0x7fffffffLL || w[i] < -0x7fffffffLL) {
                throw std::invalid_argument("#minimize weight out of 32-bit range");
            }
            r[i] = static_cast<int>(w[i]);
        }
        return r;
    }

    const Program& prog_;
    std::vector<Atom> order_;
    std::set<Atom> shown_;
    CompileOptions options_;
    std::vector<std::vector<Atom> > groups_;
    std::vector<bool> in_group_;
    std::vector<bddvar> x_vars_;
    std::vector<bddvar> atom_to_var_;
    std::vector<bddvar> atom_to_uvar_;
    std::vector<Atom> var_to_atom_;
    std::vector<std::vector<size_t> > rules_of_head_;
    std::vector<bool> is_external_;
};

}  // namespace

CompileResult compile(const Program& program, const CompileOptions& options) {
    CompileResult res;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::set<Atom> shown = collect_names(program, res);
    std::vector<Atom> atoms = collect_atoms(program);
    std::vector<Atom> order = resolve_order(program, atoms, options, res.names);
    res.stats.order_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Compiler c(program, order, shown, options);
    return c.run(res);
}

}  // namespace ddasp
