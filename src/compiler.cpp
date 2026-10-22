#include "ddasp/compiler.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <limits>
#include <queue>
#include <set>
#include <stdexcept>

#include "ddasp/cost_order.h"
#include "ddasp/dd_util.h"
#include "ddasp/stored_dd.h"

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

std::vector<Atom> program_atoms(const Program& prog) {
    std::set<Atom> atoms;
    for (size_t r = 0; r < prog.rules().size(); ++r) {
        const Rule& rule = prog.rules()[r];
        atoms.insert(rule.head.begin(), rule.head.end());
        for (size_t i = 0; i < rule.body.lits.size(); ++i) atoms.insert(atom_of(rule.body.lits[i].lit));
    }
    for (size_t e = 0; e < prog.externals().size(); ++e) atoms.insert(prog.externals()[e].atom);
    return std::vector<Atom>(atoms.begin(), atoms.end());
}

namespace {

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

typedef std::vector<std::vector<Atom> > Spines;

/**
 * Make the atoms of every spine (the atoms of a loaded DD, top first) appear
 * in the spine's order: the positions they occupy in `order` are kept, only
 * which atom sits at which position changes, so the other atoms do not move.
 * Atoms of an earlier spine are left where they are. Keeping the stored order
 * lets the loaded diagram be renamed onto the program's variables in linear time.
 */
void apply_spines(std::vector<Atom>& order, const Spines& spines) {
    if (spines.empty()) return;
    std::map<Atom, size_t> position;
    for (size_t i = 0; i < order.size(); ++i) position[order[i]] = i;
    std::set<Atom> done;
    for (size_t s = 0; s < spines.size(); ++s) {
        std::vector<Atom> atoms;
        std::vector<size_t> positions;
        for (size_t k = 0; k < spines[s].size(); ++k) {
            Atom a = spines[s][k];
            std::map<Atom, size_t>::const_iterator it = position.find(a);
            if (it == position.end() || !done.insert(a).second) continue;
            atoms.push_back(a);
            positions.push_back(it->second);
        }
        std::sort(positions.begin(), positions.end());
        for (size_t j = 0; j < atoms.size(); ++j) {
            order[positions[j]] = atoms[j];
            position[atoms[j]] = positions[j];
        }
    }
}

/**
 * NaturalName heuristic: shown atoms sorted by name; every unnamed atom is
 * placed at the average position of the atoms it shares a rule with
 * (two refinement passes so that unnamed atoms can also follow each other).
 */
std::vector<Atom> natural_name_order(const Program& prog, const std::vector<Atom>& atoms,
                                     const std::map<Atom, std::string>& names, const Spines& spines) {
    Atom max_atom = atoms.empty() ? 0 : atoms.back();
    std::vector<Atom> named, unnamed;
    for (size_t i = 0; i < atoms.size(); ++i) (names.count(atoms[i]) ? named : unnamed).push_back(atoms[i]);
    NameLess nl = {&names};
    std::sort(named.begin(), named.end(), nl);
    apply_spines(named, spines);  // spine atoms are named; before the barycenters are computed
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

/**
 * Atom order from top to bottom: the user-given order first, then the heuristic
 * order, in which the atoms of every loaded DD keep their stored order (spines).
 */
std::vector<Atom> resolve_order(const Program& prog, const std::vector<Atom>& atoms,
                                const CompileOptions& options, const std::map<Atom, std::string>& names,
                                const Spines& spines) {
    std::set<Atom> known(atoms.begin(), atoms.end());
    std::vector<Atom> order;
    std::set<Atom> placed;
    for (size_t i = 0; i < options.order.size(); ++i) {
        Atom a = options.order[i];
        if (!known.count(a)) throw std::invalid_argument("order mentions unknown atom " + std::to_string(a));
        if (placed.insert(a).second) order.push_back(a);
    }
    std::vector<Atom> rest = atoms;
    if (options.order_heuristic == OrderHeuristic::NaturalName) {
        rest = natural_name_order(prog, atoms, names, spines);
    } else {
        apply_spines(rest, spines);
    }
    for (size_t i = 0; i < rest.size(); ++i) {
        if (!placed.count(rest[i])) order.push_back(rest[i]);
    }
    return order;
}

/**
 * A constraint to be conjoined. `defines` are the atoms whose definition the
 * constraint is part of (the head atoms of a rule, the atom of a support
 * constraint); `uses` are the other atoms it mentions, except those in the
 * same dependency component as a defined atom.
 */
struct Part {
    BDD bdd;
    std::vector<Atom> defines;
    std::vector<Atom> uses;
    bool first;  ///< conjoin before the other ready parts whatever its top level (loaded DDs)

    Part() : first(false) {}
};

/**
 * Conjoin the parts so that every atom's defining parts come before the parts
 * that use the atom and, among the parts whose used atoms are all defined, in
 * increasing order of their top level. The first rule keeps an intermediate
 * result from having to remember the values of atoms whose definitions are
 * still missing (which is exponential in their number); the second keeps the
 * intermediate results local. Atoms without defining parts (choice atoms,
 * externals) never block, and `uses` excludes the atoms of one dependency
 * component, so the order always exists.
 */
BDD conjoin_sorted(std::vector<Part>& parts) {
    const size_t P = parts.size();
    Atom max_atom = 0;
    for (size_t i = 0; i < P; ++i) {
        for (size_t k = 0; k < parts[i].defines.size(); ++k) max_atom = std::max(max_atom, parts[i].defines[k]);
        for (size_t k = 0; k < parts[i].uses.size(); ++k) max_atom = std::max(max_atom, parts[i].uses[k]);
    }
    std::vector<size_t> remaining_defs(max_atom + 1, 0);
    for (size_t i = 0; i < P; ++i) {
        for (size_t k = 0; k < parts[i].defines.size(); ++k) ++remaining_defs[parts[i].defines[k]];
    }
    std::vector<std::vector<size_t> > users(max_atom + 1);
    std::vector<size_t> pending(P, 0);
    for (size_t i = 0; i < P; ++i) {
        std::vector<Atom>& u = parts[i].uses;
        std::sort(u.begin(), u.end());
        u.erase(std::unique(u.begin(), u.end()), u.end());
        for (size_t k = 0; k < u.size(); ++k) {
            if (remaining_defs[u[k]] > 0) {
                users[u[k]].push_back(i);
                ++pending[i];
            }
        }
    }
    // Ready parts: the `first` ones, then the lowest top level, then the index (deterministic).
    typedef std::pair<std::pair<int, bddvar>, size_t> Key;
    struct KeyOf {
        Key operator()(const Part& p, size_t i) const {
            bddvar top = p.bdd.is_terminal() ? 0 : kyotodd::bddlevofvar(p.bdd.top());
            return Key(std::make_pair(p.first ? 0 : 1, top), i);
        }
    } key_of;
    std::priority_queue<Key, std::vector<Key>, std::greater<Key> > ready;
    for (size_t i = 0; i < P; ++i) {
        if (pending[i] == 0) ready.push(key_of(parts[i], i));
    }
    BDD f = BDD::True;
    size_t done = 0;
    while (done < P) {
        if (ready.empty()) {  // cannot happen (see above); conjoin the rest in top-level order
            for (size_t i = 0; i < P; ++i) {
                if (pending[i] > 0) {
                    pending[i] = 0;
                    ready.push(key_of(parts[i], i));
                }
            }
        }
        size_t i = ready.top().second;
        ready.pop();
        f &= parts[i].bdd;
        parts[i].bdd = BDD();
        ++done;
        for (size_t k = 0; k < parts[i].defines.size(); ++k) {
            Atom a = parts[i].defines[k];
            if (--remaining_defs[a] != 0) continue;
            for (size_t j = 0; j < users[a].size(); ++j) {
                size_t user = users[a][j];
                if (pending[user] > 0 && --pending[user] == 0) ready.push(key_of(parts[user], user));
            }
        }
    }
    return f;
}

/** Conjoin BDDs in increasing order of their top level (no dependencies). */
BDD conjoin_sorted(std::vector<BDD>& bdds) {
    std::vector<Part> parts(bdds.size());
    for (size_t i = 0; i < bdds.size(); ++i) parts[i].bdd = bdds[i];
    return conjoin_sorted(parts);
}

/**
 * Strongly connected components of the graph `succ` on the atoms 1..max_atom,
 * in reverse topological order (a component comes after every component
 * reachable from it). Iterative Tarjan.
 */
std::vector<std::vector<Atom> > tarjan_sccs(const std::vector<std::vector<Atom> >& succ, Atom max_atom) {
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
                sccs.push_back(comp);
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
 * Dependency component of every atom: the strongly connected components of the
 * graph with an edge from every body atom (positive or negative) to every head
 * atom of a rule, and with the head atoms of a disjunctive rule in one cycle
 * (the support of each of them depends on the others being false). Used to
 * order the conjunction of the constraints.
 */
std::vector<size_t> dependency_components(const Program& prog, Atom max_atom) {
    std::vector<std::vector<Atom> > succ(max_atom + 1);
    for (size_t r = 0; r < prog.rules().size(); ++r) {
        const Rule& rule = prog.rules()[r];
        for (size_t i = 0; i < rule.body.lits.size(); ++i) {
            Atom b = atom_of(rule.body.lits[i].lit);
            for (size_t h = 0; h < rule.head.size(); ++h) succ[b].push_back(rule.head[h]);
        }
        if (rule.head_type == HeadType::Disjunction && rule.head.size() > 1) {
            for (size_t h = 0; h < rule.head.size(); ++h) succ[rule.head[h]].push_back(rule.head[(h + 1) % rule.head.size()]);
        }
    }
    std::vector<std::vector<Atom> > sccs = tarjan_sccs(succ, max_atom);
    std::vector<size_t> comp(max_atom + 1, 0);
    for (size_t c = 0; c < sccs.size(); ++c) {
        for (size_t i = 0; i < sccs[c].size(); ++i) comp[sccs[c][i]] = c;
    }
    return comp;
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
    std::vector<std::vector<Atom> > all = tarjan_sccs(succ, max_atom), sccs;
    for (size_t c = 0; c < all.size(); ++c) {
        if (all[c].size() > 1 || self_loop[all[c][0]]) sccs.push_back(all[c]);
    }
    return sccs;
}

/** A loaded DD and the atom of the program that each of its names refers to. */
struct LoadedTarget {
    const LoadedDD* dd;
    std::vector<Atom> atoms;  ///< same order as dd->data.names
};

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
             const CompileOptions& options, const std::vector<LoadedTarget>* loaded = 0)
        : prog_(prog), order_(order), shown_(shown), options_(options), loaded_(loaded) {
        Atom max_atom = 0;
        for (size_t i = 0; i < order_.size(); ++i) max_atom = std::max(max_atom, order_[i]);
        if (options_.method == Method::TwoCopy) {
            groups_.push_back(order_);
        } else {
            groups_ = nontrivial_sccs(prog_, max_atom);
        }
        allocate_variables(max_atom);
        index_rules(max_atom);
        comp_ = dependency_components(prog_, max_atom);
    }

    CompileResult run(CompileResult& res) {
        res.var_to_atom = var_to_atom_;
        res.atom_to_var = atom_to_var_;
        CompileStats& st = res.stats;
        st.atoms = order_.size();
        st.groups = groups_.size();
        for (size_t i = 0; i < in_group_.size(); ++i) st.aux_vars += in_group_[i] ? 1 : 0;

        Timer t;
        std::vector<Part> parts;
        build_models(parts);
        st.models_time = t.lap();
        if (options_.method == Method::Scc) {
            build_supported(parts);
            st.supported_time = t.lap();
        }
        if (loaded_) {
            build_loaded(parts);
            st.load_time = t.lap();
            st.loaded_dds = loaded_->size();
        }
        build_assumptions(parts);
        if (options_.external_values) build_external_values(parts);
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
        if (!family.is_zero()) {
            if (options_.optimize) {
                for (size_t c = 0; c < res.costs.size(); ++c) {
                    std::vector<int> w = int_weights(res.costs[c].var_weights);
                    long long best = family.min_weight(w);
                    family = family.cost_bound_le(w, best);
                    res.optimum.push_back(best + res.costs[c].offset);
                }
            } else if (!options_.cost_le.empty() || !options_.cost_ge.empty() || !options_.cost_eq.empty()) {
                family = apply_cost_bounds(family, res);
            }
        }
        st.optimize_time = t.lap();

        res.vars = x_vars_;
        std::vector<bddvar> hidden;
        if (options_.project_to_shown) {
            std::vector<bddvar> shown;
            for (size_t i = 0; i < order_.size(); ++i) {
                Atom a = order_[i];
                (shown_.count(a) ? shown : hidden).push_back(atom_to_var_[a]);
            }
            res.vars = shown;
        }
        if (options_.cost_histogram) {
            res.cost_histogram = build_cost_histogram(family, hidden, res);
            st.histogram_time = t.lap();
        }
        res.family = family;
        if (!hidden.empty()) family = family.project(hidden);
        res.zdd = family;
        res.bdd = zdd_to_bdd_over(family, res.vars);
        st.project_time = t.lap();
        note_peak(st);
        if (options_.best != 0) {
            size_t taken = 0;
            enumerate_by_cost(res, [&](const std::vector<long long>& cost, const std::vector<bddvar>& set) -> bool {
                CostedSet s;
                s.cost = cost;
                for (size_t i = 0; i < set.size(); ++i) s.atoms.push_back(res.var_to_atom[set[i]]);
                res.best.push_back(s);
                return ++taken < options_.best;
            });
        }
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

        size_t needed = order_.size();
        for (size_t i = 0; i < in_group_.size(); ++i) needed += in_group_[i] ? 1 : 0;
        if (kyotodd::bddvarused() + needed > kMaxBddVariables) {
            throw std::invalid_argument(
                "the program needs " + std::to_string(needed) + " BDD variables (" +
                std::to_string(order_.size()) + " atoms + " + std::to_string(needed - order_.size()) +
                " auxiliary) but KyotoDD supports at most " + std::to_string(kMaxBddVariables) +
                " (" + std::to_string(kyotodd::bddvarused()) + " already in use)");
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
        // An atom is free when it is declared #external and no rule has it in
        // the head (as in clingo, a rule overrides the declaration), or when it
        // comes from a loaded DD (constrained by the DD only; the auxiliary
        // disjunctions gringo may put it in never support it, see compile()).
        is_external_.assign(max_atom + 1, false);
        for (size_t e = 0; e < prog_.externals().size(); ++e) {
            Atom a = prog_.externals()[e].atom;
            if (rules_of_head_[a].empty()) is_external_[a] = true;
        }
        if (loaded_) {
            for (size_t d = 0; d < loaded_->size(); ++d) {
                const std::vector<Atom>& atoms = (*loaded_)[d].atoms;
                for (size_t k = 0; k < atoms.size(); ++k) is_external_[atoms[k]] = true;
            }
        }
    }

    /** DD variable of an atom, 0 when it has none (an atom of no rule and no #external). */
    bddvar var_of(Atom a) const {
        return static_cast<size_t>(a) < atom_to_var_.size() ? atom_to_var_[a] : 0;
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

    /** Whether atom a is in the dependency component of one of the atoms. */
    bool same_component(Atom a, const std::vector<Atom>& atoms) const {
        for (size_t i = 0; i < atoms.size(); ++i) {
            if (comp_[atoms[i]] == comp_[a]) return true;
        }
        return false;
    }

    /** Add a part defining `defines` and mentioning the body atoms of `rule` (and `extra`). */
    void add_part(std::vector<Part>& parts, const BDD& bdd, const std::vector<Atom>& defines, const Rule& rule,
                  const std::vector<Atom>& extra = std::vector<Atom>()) const {
        Part p;
        p.bdd = bdd;
        p.defines = defines;
        for (size_t i = 0; i < rule.body.lits.size(); ++i) {
            Atom a = atom_of(rule.body.lits[i].lit);
            if (!same_component(a, defines)) p.uses.push_back(a);
        }
        for (size_t i = 0; i < extra.size(); ++i) {
            if (!same_component(extra[i], defines)) p.uses.push_back(extra[i]);
        }
        parts.push_back(p);
    }

    /** Models(X): X satisfies every rule classically (choice rules impose nothing). */
    void build_models(std::vector<Part>& parts) const {
        LitX lx = {this};
        for (size_t r = 0; r < prog_.rules().size(); ++r) {
            const Rule& rule = prog_.rules()[r];
            if (rule.head_type == HeadType::Choice) continue;
            BDD head = BDD::False;
            for (size_t i = 0; i < rule.head.size(); ++i) head |= x_of(rule.head[i]);
            add_part(parts, ~body_bdd(rule.body, lx) | head, rule.head, rule);
        }
    }

    /**
     * Supported(X): every atom a ∈ X has a rule with a in the head whose body is
     * true in X and whose other head atoms are false in X (free atoms, see
     * index_rules, are always supported). This is the unfounded-set condition
     * for singleton sets.
     */
    void build_supported(std::vector<Part>& parts) const {
        LitX lx = {this};
        for (size_t i = 0; i < order_.size(); ++i) {
            Atom a = order_[i];
            if (is_external_[a]) continue;
            BDD support = BDD::False;
            Part p;
            p.defines.push_back(a);
            const std::vector<size_t>& rs = rules_of_head_[a];
            for (size_t k = 0; k < rs.size(); ++k) {
                const Rule& rule = prog_.rules()[rs[k]];
                BDD s = body_bdd(rule.body, lx);
                for (size_t j = 0; j < rule.body.lits.size(); ++j) {
                    Atom b = atom_of(rule.body.lits[j].lit);
                    if (comp_[b] != comp_[a]) p.uses.push_back(b);
                }
                if (rule.head_type == HeadType::Disjunction) {
                    for (size_t h = 0; h < rule.head.size(); ++h) {
                        if (rule.head[h] == a) continue;
                        s &= ~x_of(rule.head[h]);
                        if (comp_[rule.head[h]] != comp_[a]) p.uses.push_back(rule.head[h]);
                    }
                }
                support |= s;
            }
            p.bdd = ~x_of(a) | support;
            parts.push_back(p);
        }
    }

    /**
     * One constraint per loaded DD: X restricted to the atoms of the DD is a set
     * of its family. Being integrity constraints over X only, they can be
     * conjoined with Models(X) before the unfounded sets are considered.
     */
    void build_loaded(std::vector<Part>& parts) const {
        for (size_t d = 0; d < loaded_->size(); ++d) {
            const LoadedTarget& target = (*loaded_)[d];
            std::vector<bddvar> to(target.atoms.size());
            for (size_t k = 0; k < to.size(); ++k) to[k] = atom_to_var_[target.atoms[k]];
            ZDD family = rename_zdd(target.dd->zdd, target.dd->vars, to);
            Part p;
            p.bdd = zdd_to_bdd_over(family, to);  // atoms never true in the family are false
            p.uses = target.atoms;
            p.first = true;  // a strong constraint over free atoms: prune everything else with it
            parts.push_back(p);
        }
    }

    /**
     * Assumptions: every answer set satisfies each assumed literal. Unit
     * integrity constraints over X, conjoined before everything else so that
     * they prune the intermediate results.
     */
    void build_assumptions(std::vector<Part>& parts) const {
        const std::vector<Literal>& lits = prog_.assumptions();
        for (size_t i = 0; i < lits.size(); ++i) {
            Atom a = atom_of(lits[i]);
            Part p;
            if (var_of(a) == 0) {  // an atom of no rule is false
                if (is_positive(lits[i])) p.bdd = BDD::False;
                else continue;
            } else {
                p.bdd = lit_x(lits[i]);
                p.uses.push_back(a);
            }
            p.first = true;
            parts.push_back(p);
        }
    }

    /**
     * Initial values of the #external atoms: a free external declared true
     * (false) is fixed to that value, the last declaration of an atom winning.
     * Atoms defined by a rule are not external (their declaration is ignored,
     * as in clingo), and loaded atoms carry no declaration value.
     */
    void build_external_values(std::vector<Part>& parts) const {
        std::map<Atom, ExternalValue> value;
        for (size_t e = 0; e < prog_.externals().size(); ++e) {
            value[prog_.externals()[e].atom] = prog_.externals()[e].value;
        }
        for (std::map<Atom, ExternalValue>::const_iterator it = value.begin(); it != value.end(); ++it) {
            Atom a = it->first;
            if (!is_external_[a] || !rules_of_head_[a].empty()) continue;
            if (it->second != ExternalValue::True && it->second != ExternalValue::False) continue;
            Part p;
            p.bdd = it->second == ExternalValue::True ? x_of(a) : ~x_of(a);
            p.uses.push_back(a);
            p.first = true;
            parts.push_back(p);
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
        // A free atom (#external without rules, loaded) behaves as the choice
        // rule {a}: it can never be unfounded.
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
        return f.exist(uvars);
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
                bddvar v = var_of(atom_of(l));
                if (v == 0) {  // an atom of no rule is false: only "not a" costs
                    if (!is_positive(l)) cf.offset += w;
                    continue;
                }
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

    /** Does any entry of the bound refer to the optimum? */
    static bool has_relative(const std::vector<CostBound>& b) {
        for (size_t i = 0; i < b.size(); ++i) {
            if (b[i].relative) return true;
        }
        return false;
    }

    /** A bound vector in KyotoDD weight space: value (+ the optimum of the level) - offset. */
    static std::vector<long long> resolve_bounds(const std::vector<CostBound>& bounds, const CompileResult& res) {
        std::vector<long long> b(bounds.size());
        for (size_t c = 0; c < bounds.size(); ++c) {
            long long v = bounds[c].value;
            if (bounds[c].relative) v += res.optimum[c];  // res.optimum includes the offset
            b[c] = v - res.costs[c].offset;
        }
        return b;
    }

    /**
     * The sets of @p family whose cost vector over the first b.size() levels is
     * lexicographically <= b (@p le) or >= b (!le): a level decides unless the
     * cost equals the bound there, in which case the next level is consulted;
     * the levels beyond the bound vector are free.
     */
    static ZDD lex_bound(const ZDD& family, const std::vector<std::vector<int> >& w, const std::vector<long long>& b,
                         bool le) {
        const long long kMin = std::numeric_limits<long long>::min(), kMax = std::numeric_limits<long long>::max();
        ZDD acc = family;
        for (size_t c = b.size(); c-- > 0;) {
            ZDD within, strict;  // cost <= / >= b[c], and cost < / > b[c]
            if (le) {
                kyotodd::CostBoundMemo memo;
                within = family.cost_bound_le(w[c], b[c], memo);
                strict = b[c] == kMin ? ZDD(0) : family.cost_bound_le(w[c], b[c] - 1, memo);
            } else {
                within = family.cost_bound_ge(w[c], b[c]);
                strict = b[c] == kMax ? ZDD(0) : family.cost_bound_ge(w[c], b[c] + 1);
            }
            acc = strict + ((within - strict) & acc);
        }
        return acc;
    }

    /** The sets of @p family whose cost equals b on every level b lists. */
    static ZDD lex_equal(const ZDD& family, const std::vector<std::vector<int> >& w, const std::vector<long long>& b) {
        ZDD acc = family;
        for (size_t c = 0; c < b.size(); ++c) acc = acc.cost_bound_eq(w[c], b[c]);
        return acc;
    }

    /**
     * Restrict @p family to the cost bounds of the options. A relative bound needs
     * the optimum, which is computed as optimize does and reported in res.optimum.
     */
    ZDD apply_cost_bounds(ZDD family, CompileResult& res) const {
        const CompileOptions& o = options_;
        std::vector<std::vector<int> > w;
        for (size_t c = 0; c < res.costs.size(); ++c) w.push_back(int_weights(res.costs[c].var_weights));
        if (has_relative(o.cost_le) || has_relative(o.cost_ge) || has_relative(o.cost_eq)) {
            ZDD best = family;
            for (size_t c = 0; c < res.costs.size(); ++c) {
                long long m = best.min_weight(w[c]);
                best = best.cost_bound_le(w[c], m);
                res.optimum.push_back(m + res.costs[c].offset);
            }
        }
        if (!o.cost_eq.empty()) return lex_equal(family, w, resolve_bounds(o.cost_eq, res));
        if (!o.cost_le.empty()) family = lex_bound(family, w, resolve_bounds(o.cost_le, res), true);
        if (!o.cost_ge.empty()) family = lex_bound(family, w, resolve_bounds(o.cost_ge, res), false);
        return family;
    }

    /**
     * The cost histogram of @p family (see CostCount): the distinct cost vectors
     * are visited from the smallest up, each time taking the lexicographically
     * optimal sets of what is left (as optimize finds them), projecting them
     * and adding them to the projected sets seen so far; the growth of that
     * union is the count of the vector.
     */
    std::vector<CostCount> build_cost_histogram(const ZDD& family, const std::vector<bddvar>& hidden,
                                                const CompileResult& res) const {
        std::vector<CostCount> hist;
        std::vector<std::vector<int> > w;
        for (size_t c = 0; c < res.costs.size(); ++c) w.push_back(int_weights(res.costs[c].var_weights));
        ZDD rest = family;
        ZDD seen(0);
        bigint::BigInt seen_count;
        while (!rest.is_zero()) {
            CostCount entry;
            ZDD best = rest;
            for (size_t c = 0; c < w.size(); ++c) {
                long long m = best.min_weight(w[c]);
                best = best.cost_bound_le(w[c], m);
                entry.cost.push_back(m + res.costs[c].offset);
            }
            seen = seen + (hidden.empty() ? best : best.project(hidden));
            bigint::BigInt total = seen.exact_count();
            entry.count = total - seen_count;
            if (!entry.count.is_zero()) hist.push_back(entry);
            seen_count = total;
            rest = rest - best;
        }
        return hist;
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
    std::vector<size_t> comp_;  ///< dependency component of every atom (dependency_components)
    std::vector<bool> is_external_;
    const std::vector<LoadedTarget>* loaded_;
};

/**
 * Replace the shown atoms by the atoms of the #project statements (those that
 * have a DD variable; the others are false anyway). Projected atoms without a
 * name get "__atom<id>" so that they are displayed, and the terms whose
 * condition leaves the projection are dropped with a warning.
 */
void apply_project_statements(const Program& prog, const std::vector<Atom>& atoms, std::set<Atom>& shown,
                              CompileResult& res) {
    std::set<Atom> known(atoms.begin(), atoms.end());
    shown.clear();
    for (size_t i = 0; i < prog.projections().size(); ++i) {
        Atom a = prog.projections()[i];
        if (!known.count(a)) continue;
        shown.insert(a);
        if (!res.names.count(a)) res.names[a] = res.atom_name(a);
    }
    std::vector<ShowTerm> kept;
    size_t dropped = 0;
    for (size_t t = 0; t < res.terms.size(); ++t) {
        const std::vector<Literal>& cond = res.terms[t].condition;
        bool inside = true;
        for (size_t i = 0; i < cond.size() && inside; ++i) {
            Atom a = atom_of(cond[i]);
            inside = shown.count(a) || !known.count(a);  // an atom of no rule is a constant
        }
        if (inside) kept.push_back(res.terms[t]);
        else ++dropped;
    }
    if (dropped != 0) {
        res.warnings.push_back("#project: " + std::to_string(dropped) +
                               " #show term(s) whose condition mentions an atom outside the projection are not shown");
    }
    res.terms = kept;
}

/** Reject cost options that cannot be applied (see CompileOptions::cost_le and cost_histogram). */
void validate_cost_options(const Program& program, const CompileOptions& o) {
    size_t n = std::max(o.cost_le.size(), std::max(o.cost_ge.size(), o.cost_eq.size()));
    if (n == 0 && !o.cost_histogram && o.best == 0) return;
    if (n != 0) {
        if (o.optimize) {
            throw std::invalid_argument("optimize (--opt) cannot be combined with a cost bound (cost_le/cost_ge/cost_eq)");
        }
        if (!o.cost_eq.empty() && (!o.cost_le.empty() || !o.cost_ge.empty())) {
            throw std::invalid_argument(
                "cost_eq (--cost-eq) cannot be combined with cost_le or cost_ge (--cost-le/--cost-ge)");
        }
    }
    std::set<int> priorities;
    for (size_t m = 0; m < program.minimizes().size(); ++m) priorities.insert(program.minimizes()[m].priority);
    if (priorities.empty()) {
        const char* what = n != 0 ? "a cost bound" : o.cost_histogram ? "the cost histogram (--cost-histogram)"
                                                                    : "the cost-ordered enumeration (best)";
        throw std::invalid_argument(std::string(what) + " needs a #minimize statement, but the program has none");
    }
    if (n > priorities.size()) {
        throw std::invalid_argument("the cost bound has " + std::to_string(n) + " entries, but the program has only " +
                                    std::to_string(priorities.size()) + " #minimize priority level(s)");
    }
}

CompileResult compile_with(const Program& program, const CompileOptions& options,
                           const std::vector<LoadedTarget>* loaded) {
    validate_cost_options(program, options);
    CompileResult res;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::set<Atom> shown = collect_names(program, res);
    std::vector<Atom> atoms = program_atoms(program);
    if (options.use_project_statements) {
        if (!options.project_to_shown) {
            throw std::invalid_argument("use_project_statements (--project) needs projection (not --all-atoms)");
        }
        if (!program.projections().empty()) apply_project_statements(program, atoms, shown, res);
    }
    Spines spines;
    if (loaded) {
        for (size_t d = 0; d < loaded->size(); ++d) spines.push_back((*loaded)[d].atoms);
    }
    std::vector<Atom> order = resolve_order(program, atoms, options, res.names, spines);
    res.stats.order_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Compiler c(program, order, shown, options, loaded);
    return c.run(res);
}

/** Atoms named by each #show output whose condition is a single positive atom. */
std::map<std::string, std::set<Atom> > atoms_by_name(const Program& prog) {
    std::map<std::string, std::set<Atom> > by_name;
    for (size_t o = 0; o < prog.outputs().size(); ++o) {
        const Output& out = prog.outputs()[o];
        if (out.condition.size() == 1 && is_positive(out.condition[0])) by_name[out.name].insert(out.condition[0]);
    }
    return by_name;
}

/**
 * Atoms defined by the program: the head atoms of its rules, except in the
 * auxiliary disjunctions gringo emits when it rewrites a recursive aggregate
 * (`!=`, negative weights): `aux | a :- B` together with `aux :- not a`, aux
 * being an unnamed atom. Given the second rule, the first can never be the
 * support of a (aux is true whenever a is false, and gringo's rewriting makes
 * aux true whenever B is), so an atom of a loaded DD may occur in it.
 */
std::set<Atom> defined_atoms(const Program& prog, const std::map<Atom, std::string>& primary) {
    std::set<std::pair<Atom, Atom> > complement;  // (aux, a) for every rule aux :- not a
    for (size_t r = 0; r < prog.rules().size(); ++r) {
        const Rule& rule = prog.rules()[r];
        if (rule.head_type == HeadType::Disjunction && rule.head.size() == 1 && rule.body.type == BodyType::Normal &&
            rule.body.lits.size() == 1 && !is_positive(rule.body.lits[0].lit)) {
            complement.insert(std::make_pair(rule.head[0], atom_of(rule.body.lits[0].lit)));
        }
    }
    std::set<Atom> defined;
    for (size_t r = 0; r < prog.rules().size(); ++r) {
        const Rule& rule = prog.rules()[r];
        for (size_t h = 0; h < rule.head.size(); ++h) {
            Atom a = rule.head[h];
            if (rule.head_type == HeadType::Disjunction && rule.head.size() == 2) {
                Atom aux = rule.head[1 - h];
                if (!primary.count(aux) && complement.count(std::make_pair(aux, a))) continue;
            }
            defined.insert(a);
        }
    }
    return defined;
}

std::string quoted(const std::string& s) { return "'" + s + "'"; }

std::invalid_argument load_error(const std::string& what) { return std::invalid_argument("loaded DD: " + what); }

}  // namespace

CompileResult compile(const Program& program, const CompileOptions& options) {
    return compile_with(program, options, 0);
}

CompileResult compile(const Program& program, const CompileOptions& options, const std::vector<LoadedDD>& loaded) {
    if (loaded.empty()) return compile(program, options);

    std::map<std::string, std::set<Atom> > by_name = atoms_by_name(program);
    std::set<std::string> program_facts;
    for (size_t o = 0; o < program.outputs().size(); ++o) {
        if (program.outputs()[o].condition.empty()) program_facts.insert(program.outputs()[o].name);
    }
    // A name must be an atom everywhere or a fact everywhere.
    std::set<std::string> dd_atoms, dd_facts;
    for (size_t d = 0; d < loaded.size(); ++d) {
        const StoredDDData& data = loaded[d].data;
        for (size_t k = 0; k < data.names.size(); ++k) {
            const std::string& n = data.names[k];
            if (program_facts.count(n)) throw load_error("atom " + quoted(n) + " is a fact of the program");
            if (dd_facts.count(n)) throw load_error("atom " + quoted(n) + " is a fact of another loaded DD");
            dd_atoms.insert(n);
        }
        for (size_t i = 0; i < data.fact_names.size(); ++i) {
            const std::string& f = data.fact_names[i];
            if (by_name.count(f)) throw load_error("fact " + quoted(f) + " is an atom of the program");
            if (dd_atoms.count(f)) throw load_error("fact " + quoted(f) + " is an atom of another loaded DD");
            dd_facts.insert(f);
        }
    }

    std::map<Atom, std::string> primary = atom_names(program);
    std::set<Atom> heads = defined_atoms(program, primary);
    std::set<Atom> externals;
    for (size_t e = 0; e < program.externals().size(); ++e) externals.insert(program.externals()[e].atom);

    // Match every stored atom with an atom of the program; the atoms the program
    // does not mention become fresh #external atoms of a copy of the program.
    Program augmented;
    const Program* prog = &program;
    Atom next_atom = program.max_atom() + 1;
    size_t synthesized = 0;
    std::vector<LoadedTarget> targets(loaded.size());
    for (size_t d = 0; d < loaded.size(); ++d) {
        const StoredDDData& data = loaded[d].data;
        targets[d].dd = &loaded[d];
        std::map<Atom, std::string> used;  // atom of the program -> stored name that took it
        for (size_t k = 0; k < data.names.size(); ++k) {
            const std::string& n = data.names[k];
            Atom a;
            std::map<std::string, std::set<Atom> >::const_iterator it = by_name.find(n);
            if (it == by_name.end()) {
                if (prog != &augmented) {
                    augmented = program;
                    prog = &augmented;
                }
                a = next_atom++;
                augmented.add_external(External(a, ExternalValue::Free));
                augmented.add_output(Output(n, std::vector<Literal>(1, a)));
                by_name[n].insert(a);
                externals.insert(a);  // a later DD may share the atom
                ++synthesized;
            } else if (it->second.size() > 1) {
                throw load_error("name " + quoted(n) + " is shown for several atoms of the program");
            } else {
                a = *it->second.begin();
                std::string shown_as = primary.count(a) && primary[a] != n ? " (atom " + quoted(primary[a]) + " of the program)" : "";
                if (heads.count(a)) {
                    throw load_error("atom " + quoted(n) + shown_as +
                                     " is defined by a rule of the program; loaded atoms may only occur in bodies");
                }
                if (!externals.count(a)) {
                    throw load_error("atom " + quoted(n) + shown_as + " is not declared #external in the program");
                }
            }
            if (!used.insert(std::make_pair(a, n)).second) {
                throw load_error("atoms " + quoted(used[a]) + " and " + quoted(n) + " both refer to atom " +
                                 quoted(primary.count(a) ? primary[a] : n) + " of the program");
            }
            targets[d].atoms.push_back(a);
        }
    }

    CompileResult res = compile_with(*prog, options, &targets);
    res.stats.synthesized_atoms = synthesized;
    std::set<std::string> have(res.fact_names.begin(), res.fact_names.end());
    for (size_t d = 0; d < loaded.size(); ++d) {
        const std::vector<std::string>& facts = loaded[d].data.fact_names;
        for (size_t i = 0; i < facts.size(); ++i) {
            if (have.insert(facts[i]).second) res.fact_names.push_back(facts[i]);
        }
    }
    return res;
}

}  // namespace ddasp
