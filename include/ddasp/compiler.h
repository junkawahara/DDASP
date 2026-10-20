#ifndef DDASP_COMPILER_H
#define DDASP_COMPILER_H

/**
 * @file compiler.h
 * @brief Compile a ground ASP program into a decision diagram of its answer sets.
 *
 * KyotoDD must be initialized (kyotodd::bddinit) before compiling. The compiler
 * allocates fresh BDD variables; variable numbers already in use are not touched.
 */

#include <map>
#include <string>
#include <vector>

#include "bdd.h"
#include "ddasp/program.h"

namespace ddasp {

/** @brief Algorithm used to enforce the stable model condition. */
enum class Method {
    TwoCopy  ///< Stable(X) = Models(X) ∧ ¬∃Y (Y ⊊ X ∧ Y ⊨ P^X), with two BDD variables per atom.
};

/** @brief Compilation options. */
struct CompileOptions {
    Method method;
    /**
     * Project the answer sets onto the shown atoms (#show). Hidden atoms are
     * existentially quantified away, so answer sets that differ only in hidden
     * atoms collapse into one set. When false, every atom becomes a DD variable.
     */
    bool project_to_shown;
    /** Keep only the optimal answer sets with respect to the #minimize statements. */
    bool optimize;
    /**
     * Atom order for the DD variables, listed from the top of the diagram down.
     * Empty = ascending atom id. Atoms of the program missing from the list are
     * appended (in ascending id order) below the listed ones.
     */
    std::vector<Atom> order;

    CompileOptions() : method(Method::TwoCopy), project_to_shown(true), optimize(false) {}
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

/** @brief Result of a compilation. */
struct CompileResult {
    /**
     * @brief Family of answer sets; each set is given by the DD variables of its true atoms.
     * Projected onto the shown atoms and/or restricted to the optimal answer sets as requested.
     */
    kyotodd::ZDD zdd;
    /** @brief Characteristic function of @c zdd over @c vars. */
    kyotodd::BDD bdd;
    /** @brief Variables of the answer-set family (atom variables), top level first. */
    std::vector<kyotodd::bddvar> vars;
    /** @brief Cost functions of the #minimize statements, most important priority first. */
    std::vector<CostFunction> costs;
    /** @brief Optimal cost per level (same order as @c costs); filled only when optimizing a consistent program. */
    std::vector<long long> optimum;
    /** @brief var_to_atom[v] = atom of DD variable v (0 for variables not used by this result). */
    std::vector<Atom> var_to_atom;
    /** @brief atom_to_var[a] = DD variable of atom a (0 if the atom does not occur in the program). */
    std::vector<kyotodd::bddvar> atom_to_var;
    /** @brief Atom names taken from #show output statements whose condition is a single positive atom. */
    std::map<Atom, std::string> names;
    /**
     * @brief Names shown in every answer set: #show output statements with an empty
     * condition (gringo emits facts this way, without naming their atom).
     */
    std::vector<std::string> fact_names;

    /** @brief Name of an atom: its #show name, or "__atom<id>" when it has none. */
    std::string atom_name(Atom a) const;
};

/**
 * @brief Compile a program into a ZDD (and BDD) of its answer sets.
 * @throws std::invalid_argument on unsupported input, e.g. a #show statement whose
 *         condition is not a single positive atom while projecting.
 */
CompileResult compile(const Program& program, const CompileOptions& options = CompileOptions());

}  // namespace ddasp

#endif  // DDASP_COMPILER_H
