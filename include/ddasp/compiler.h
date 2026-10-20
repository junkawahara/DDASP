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
     * Atom order for the DD variables, listed from the top of the diagram down.
     * Empty = ascending atom id. Atoms of the program missing from the list are
     * appended (in ascending id order) below the listed ones.
     */
    std::vector<Atom> order;

    CompileOptions() : method(Method::TwoCopy) {}
};

/** @brief Result of a compilation. */
struct CompileResult {
    /** @brief Family of answer sets; each set is given by the DD variables of its true atoms. */
    kyotodd::ZDD zdd;
    /** @brief Characteristic function of the answer sets over the same variables. */
    kyotodd::BDD bdd;
    /** @brief Variables of the answer-set family (atom variables), top level first. */
    std::vector<kyotodd::bddvar> vars;
    /** @brief var_to_atom[v] = atom of DD variable v (0 for variables not used by this result). */
    std::vector<Atom> var_to_atom;
    /** @brief atom_to_var[a] = DD variable of atom a (0 if the atom does not occur in the program). */
    std::vector<kyotodd::bddvar> atom_to_var;
    /** @brief Atom names taken from #show output statements whose condition is a single positive atom. */
    std::map<Atom, std::string> names;

    /** @brief Name of an atom: its #show name, or "__atom<id>" when it has none. */
    std::string atom_name(Atom a) const;
};

/**
 * @brief Compile a program into a ZDD (and BDD) of its answer sets.
 * @throws std::invalid_argument on unsupported input.
 */
CompileResult compile(const Program& program, const CompileOptions& options = CompileOptions());

}  // namespace ddasp

#endif  // DDASP_COMPILER_H
