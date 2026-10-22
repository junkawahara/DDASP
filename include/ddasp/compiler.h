#ifndef DDASP_COMPILER_H
#define DDASP_COMPILER_H

/**
 * @file compiler.h
 * @brief Compile a ground ASP program into a decision diagram of its answer sets.
 *
 * KyotoDD must be initialized (kyotodd::bddinit) before compiling. The compiler
 * allocates fresh BDD variables; variable numbers already in use are not touched.
 */

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "bdd.h"
#include "ddasp/program.h"

namespace ddasp {

/**
 * @brief Algorithm used to enforce the stable model condition.
 *
 * Both methods express "X has no non-empty unfounded set U" with auxiliary BDD
 * variables u_a (a ∈ U) and existential quantification:
 * Stable(X) = Models(X) ∧ ¬∃U (U ≠ ∅ ∧ U ⊆ X ∧ U is unfounded w.r.t. X).
 */
enum class Method {
    TwoCopy,  ///< One auxiliary variable per atom, a single quantification (reference implementation).
    Scc       ///< Supportedness for every atom plus one quantification per non-trivial SCC of the
              ///< positive dependency graph; auxiliary variables only for atoms in such SCCs.
};

/** @brief Parse a method name ("twocopy" or "scc"); throws std::invalid_argument otherwise. */
Method parse_method(const std::string& name);

/**
 * @brief Which models of the ground program the result represents.
 *
 * All three read the rules of the ground program as gringo emits it, so the
 * auxiliary atoms gringo introduces (aggregates, `not not`) are ordinary atoms
 * of the Supported and Classical families.
 */
enum class Semantics {
    Stable,     ///< answer sets (the default)
    Supported,  ///< models of Clark's completion: every true atom has a rule with it in the head
                ///< whose body is true and whose other head atoms are false (`a :- a.` supports a;
                ///< free atoms, i.e. #external and loaded ones, are always supported)
    Classical   ///< models of the rules read as implications body -> head (choice rules impose nothing)
};

/** @brief Parse a semantics name ("stable", "supported" or "classical"); throws std::invalid_argument otherwise. */
Semantics parse_semantics(const std::string& name);

/** @brief How the DD variable order of the atoms is chosen. */
enum class OrderHeuristic {
    AtomId,       ///< ascending gringo atom id
    NaturalName   ///< shown atoms sorted by name with numeric-aware comparison
                  ///< (p(2) < p(10)); unnamed atoms are placed at the barycenter of
                  ///< the atoms they share rules with
};

/** @brief Parse an order heuristic name ("id" or "name"); throws std::invalid_argument otherwise. */
OrderHeuristic parse_order_heuristic(const std::string& name);

/** @brief Numeric-aware string comparison: digit runs compare as numbers, the rest as text. */
bool natural_less(const std::string& a, const std::string& b);

/**
 * @brief One entry of a cost bound (see CompileOptions::cost_le): a cost, given
 * either as an absolute value or relative to the optimal cost of its level.
 */
struct CostBound {
    long long value;
    bool relative;  ///< value is added to the optimum of the level (as optimize computes it)

    CostBound() : value(0), relative(false) {}
    CostBound(long long v, bool rel) : value(v), relative(rel) {}
};

/** @brief Compilation options. */
struct CompileOptions {
    /** Stable-model method; used for Semantics::Stable only. */
    Method method;
    /**
     * The family to compute (see Semantics). The tautological rules are removed
     * (Program::without_tautologies) for the stable semantics only: supported
     * models may depend on them (`a :- a.`), and classical models do not change.
     */
    Semantics semantics;
    /** Heuristic for the variable order of atoms not listed in @c order. */
    OrderHeuristic order_heuristic;
    /**
     * Project the answer sets onto the shown atoms (#show). Hidden atoms are
     * existentially quantified away, so answer sets that differ only in hidden
     * atoms collapse into one set. When false, every atom becomes a DD variable.
     */
    bool project_to_shown;
    /**
     * Project onto the atoms of the #project statements instead of the shown
     * atoms when the program has such statements (like clingo's --project;
     * without #project statements this changes nothing). Every projected atom
     * becomes a variable of the result, named by its #show name or "__atom<id>";
     * #show terms whose condition mentions an atom outside the projection are
     * dropped with a warning (CompileResult::warnings). Requires project_to_shown.
     */
    bool use_project_statements;
    /** Keep only the optimal answer sets with respect to the #minimize statements. */
    bool optimize;
    /**
     * Fix every free #external atom whose declaration carries an initial value
     * (`#external a. [true]` / `[false]`; gringo declares `#external a.` as
     * false) to that value; atoms declared free or released stay free, and a
     * later declaration of the same atom overrides an earlier one. When false
     * (the default) every initial value is ignored.
     */
    bool external_values;
    /**
     * Bounds on the #minimize costs: keep only the answer sets whose cost vector
     * (one cost per priority level, most important first, as reported in
     * CompileResult::optimum) is lexicographically <= cost_le, >= cost_ge or
     * equal to cost_eq, each compared on the levels the bound lists (a shorter
     * bound leaves the lower priorities free); cost_le is clingo's
     * --opt-mode=enum,K,... cost_le and cost_ge combine, cost_eq excludes both,
     * and none of them combines with optimize. A relative entry stands for the
     * optimum of its level plus the value; the optimum is then computed as
     * optimize does and reported in CompileResult::optimum. Like optimize, the
     * bounds apply before projection. Empty: no bound.
     */
    std::vector<CostBound> cost_le;
    std::vector<CostBound> cost_ge;
    std::vector<CostBound> cost_eq;
    /**
     * Compute CompileResult::cost_histogram: the number of answer sets per
     * #minimize cost vector (requires a #minimize statement). The histogram
     * describes the result, i.e. the family after optimize / the cost bounds.
     */
    bool cost_histogram;
    /**
     * Fill CompileResult::best with the first `best` answer sets in ascending
     * #minimize cost order (enumerate_by_cost in cost_order.h); 0 = none.
     * Requires a #minimize statement.
     */
    size_t best;
    /**
     * Atom order for the DD variables, listed from the top of the diagram down.
     * Atoms of the program missing from the list are appended below the listed
     * ones in the order given by @c order_heuristic.
     */
    std::vector<Atom> order;

    CompileOptions()
        : method(Method::Scc), semantics(Semantics::Stable), order_heuristic(OrderHeuristic::NaturalName), project_to_shown(true),
          use_project_statements(false), optimize(false), external_values(false), cost_histogram(false),
          best(0) {}
};

/**
 * @brief One #minimize priority level as a linear cost over DD variables.
 *
 * cost(X) = offset + sum over atoms a in X of var_weights[atom_to_var[a]].
 * Negative literals "not a" with weight w contribute w to the offset and -w to
 * the weight of a. Costs refer to the unprojected answer sets.
 */
struct CostFunction {
    int priority;
    long long offset;
    std::vector<long long> var_weights;  ///< indexed by DD variable number

    CostFunction() : priority(0), offset(0) {}
};

/**
 * @brief One entry of the cost histogram: a cost vector (one cost per priority
 * level, most important first) and the number of answer sets of the result
 * whose cost it is. With projection, every projected set counts once, at the
 * lexicographically smallest cost vector of the answer sets projecting to it
 * (the same reading of costs as the cost bounds), so the counts add up to the
 * number of answer sets of the result.
 */
struct CostCount {
    std::vector<long long> cost;
    bigint::BigInt count;
};

/** @brief An answer set with its #minimize cost vector (CompileResult::best). */
struct CostedSet {
    std::vector<long long> cost;  ///< one cost per priority level, most important first
    std::vector<Atom> atoms;      ///< the atoms of the (projected) answer set, top level first
};

/** @brief Timing and size statistics of a compilation (seconds / node counts). */
struct CompileStats {
    double order_time;       ///< choosing the variable order
    double models_time;      ///< building the per-rule constraints of Models(X)
    double supported_time;   ///< building the per-atom constraints of Supported(X) (Scc method only)
    double conjoin_time;     ///< conjoining those constraints
    double unfounded_time;   ///< building and quantifying the unfounded-set conditions
    double convert_time;     ///< BDD -> ZDD conversion
    double optimize_time;    ///< #minimize filtering
    double histogram_time;   ///< cost histogram (CompileOptions::cost_histogram)
    double project_time;     ///< #show projection and the final ZDD -> BDD conversion
    uint64_t models_nodes;   ///< size of Models(X) (∧ Supported(X) for the Scc method)
    uint64_t stable_nodes;   ///< size of the stable-model BDD over all atoms
    uint64_t peak_nodes;     ///< largest value of kyotodd::bddused() observed (nodes in the table,
                             ///< including garbage not yet collected)
    size_t atoms;            ///< number of atoms of the program (one x variable each)
    size_t groups;           ///< number of unfounded-set groups (SCCs) quantified
    size_t aux_vars;         ///< number of auxiliary u variables allocated
    double load_time;        ///< mapping the loaded DDs onto the program's variables
    size_t loaded_dds;       ///< number of loaded DDs conjoined
    size_t synthesized_atoms;  ///< atoms of the loaded DDs that the program did not mention
    size_t simplified_rules;   ///< rules dropped or trimmed as tautologies before compiling (Program::without_tautologies)

    CompileStats()
        : order_time(0), models_time(0), supported_time(0), conjoin_time(0), unfounded_time(0), convert_time(0), optimize_time(0),
          histogram_time(0), project_time(0), models_nodes(0), stable_nodes(0), peak_nodes(0), atoms(0), groups(0), aux_vars(0),
          load_time(0), loaded_dds(0), synthesized_atoms(0), simplified_rules(0) {}
};

/**
 * @brief A #show output that is not the primary name of an atom: the name is
 * displayed with an answer set when every literal of the condition holds.
 * This covers additional names of an atom (`#show t : a.` when `a` is already
 * shown) and negated conditions (`#show t : not a.`).
 */
struct ShowTerm {
    std::string name;
    std::vector<Literal> condition;

    ShowTerm() {}
    ShowTerm(const std::string& n, const std::vector<Literal>& c) : name(n), condition(c) {}
};

/**
 * @brief Primary name of every named atom: the first #show output whose
 * condition is exactly that atom (positive). Shared by the compiler and the tools
 * that resolve atom names given by the user (order files).
 */
std::map<Atom, std::string> atom_names(const Program& program);

/**
 * @brief The atoms of a program, ascending: those of its rules and its #external
 * statements. Every other atom (one mentioned only by a #show condition or a
 * #minimize statement) has no rule, so it is false in every answer set and
 * gets no DD variable.
 */
std::vector<Atom> program_atoms(const Program& program);

/** @brief Result of a compilation. */
struct CompileResult {
    /**
     * @brief Family of answer sets; each set is given by the DD variables of its true atoms.
     * Projected onto the shown atoms and/or restricted to the optimal answer sets as requested.
     */
    kyotodd::ZDD zdd;
    /**
     * @brief The answer sets before projection, over every atom variable: @c zdd
     * is this family projected onto @c vars (the same family when nothing is
     * hidden). Kept for cost-ordered enumeration (cost_order.h).
     */
    kyotodd::ZDD family;
    /** @brief Characteristic function of @c zdd over @c vars. */
    kyotodd::BDD bdd;
    /** @brief Variables of the answer-set family (atom variables), top level first. */
    std::vector<kyotodd::bddvar> vars;
    /** @brief Cost functions of the #minimize statements, most important priority first. */
    std::vector<CostFunction> costs;
    /**
     * @brief Optimal cost per level (same order as @c costs); filled only when optimizing,
     * or bounding costs relative to the optimum, on a consistent program.
     */
    std::vector<long long> optimum;
    /**
     * @brief Cost histogram in ascending lexicographic order of the cost vectors,
     * without the vectors of no answer set; filled only with CompileOptions::cost_histogram.
     */
    std::vector<CostCount> cost_histogram;
    /** @brief The first CompileOptions::best answer sets in ascending cost order (empty unless requested). */
    std::vector<CostedSet> best;
    /** @brief Statistics of the compilation. */
    CompileStats stats;
    /** @brief var_to_atom[v] = atom of DD variable v (0 for variables not used by this result). */
    std::vector<Atom> var_to_atom;
    /** @brief atom_to_var[a] = DD variable of atom a (0 if the atom has none, see program_atoms()). */
    std::vector<kyotodd::bddvar> atom_to_var;
    /**
     * @brief Primary atom names (see atom_names()). With use_project_statements,
     * the projected atoms without a #show name are entered as "__atom<id>".
     */
    std::map<Atom, std::string> names;
    /**
     * @brief Names shown in every answer set: #show output statements with an empty
     * condition (gringo emits facts this way, without naming their atom).
     */
    std::vector<std::string> fact_names;
    /**
     * @brief The remaining #show outputs, in program order. The atoms of their
     * conditions count as shown atoms (they survive projection), so the terms
     * can be evaluated on the projected answer sets.
     */
    std::vector<ShowTerm> terms;
    /** @brief Messages about input that was ignored (e.g. #show terms dropped by #project). */
    std::vector<std::string> warnings;

    /** @brief Name of an atom: its primary #show name, or "__atom<id>" when it has none. */
    std::string atom_name(Atom a) const;

    /**
     * @brief Everything displayed for one answer set, given as the atoms it
     * contains: the fact names, the names of the named atoms in the order given,
     * then the terms whose conditions hold.
     */
    std::vector<std::string> shown_names(const std::vector<Atom>& atoms) const;
};

/** @brief Largest BDD variable number KyotoDD supports; every atom needs one variable, atoms in loops two. */
const size_t kMaxBddVariables = 65536;

/**
 * @brief Compile a program into a ZDD (and BDD) of its answer sets (or of its
 *        supported or classical models, see CompileOptions::semantics).
 *
 * For the stable semantics the program is compiled without its tautological
 * rules (Program::without_tautologies; the answer sets are the same), which
 * decides which #external atoms a rule defines, as in clingo.
 * @throws std::invalid_argument on unsupported input (unknown atom in the order,
 *         when optimizing or bounding costs with a #minimize weight outside the
 *         32-bit range, a cost bound with more entries than #minimize priority
 *         levels, a cost bound, cost_histogram or best without any #minimize
 *         statement, cost bounds combined as CompileOptions forbids, more variables needed than kMaxBddVariables,
 *         use_project_statements without project_to_shown).
 */
CompileResult compile(const Program& program, const CompileOptions& options = CompileOptions());

struct LoadedDD;  // stored_dd.h

/**
 * @brief Compile a program together with loaded (stored) DDs.
 *
 * Loading a stored DD D is the same as adding to the program `#external n.`
 * and `#show n : n.` for every atom name n of D, the fact `f.` for every fact
 * name f of D, and the constraint "the answer set restricted to the atoms of D
 * is a set of D". The atoms of D are matched by name with the atoms of the
 * program (every #show output whose condition is that single positive atom
 * counts as a name); atoms the program does not mention are added to a copy of
 * it as named #external atoms, so they stay free, constrained only by D, and
 * visible in the result. The fact names of D are appended to the result's.
 *
 * @throws std::invalid_argument when a stored atom is defined by the program
 *         (it occurs in a rule head, except in the auxiliary disjunctions gringo
 *         emits when it rewrites recursive aggregates, or is a fact), is not #external, has an
 *         ambiguous name, when two stored atoms refer to the same atom of the
 *         program, or when a name is an atom in one place and a fact in another.
 */
CompileResult compile(const Program& program, const CompileOptions& options, const std::vector<LoadedDD>& loaded);

}  // namespace ddasp

#endif  // DDASP_COMPILER_H
