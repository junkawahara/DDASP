#ifndef DDASP_STORED_DD_H
#define DDASP_STORED_DD_H

/**
 * @file stored_dd.h
 * @brief Self-contained files of compiled answer-set families ("stored DDs").
 *
 * A stored DD is the ZDD of a compilation together with the names of its atoms
 * and its fact names, so that a later compilation can load it (see
 * compile() with loaded DDs in compiler.h). File format, version 1:
 *
 *     ddasp-dd 1
 *     atoms N
 *     <name>        N lines, top of the diagram first (names[k] sits at level N-k)
 *     facts M
 *     <name>        M lines
 *     zdd B         B bytes of KyotoDD binary (ZDD::export_binary) follow;
 *                   `zdd empty` / `zdd single` for the terminal families (no payload)
 *
 * The diagram is written with its variables compacted to levels 1..N, so that
 * importing it into a fresh KyotoDD instance needs exactly N variables. Names
 * are written as they come from the #show statements; a file is trusted input
 * (its declarations are passed to the grounder as program text).
 */

#include <iosfwd>
#include <string>
#include <vector>

#include "bdd.h"
#include "ddasp/compiler.h"

namespace ddasp {

/** @brief A stored DD as read from a file, before it is imported into KyotoDD. */
struct StoredDDData {
    std::vector<std::string> names;       ///< atom names, top of the diagram first
    std::vector<std::string> fact_names;  ///< names shown in every answer set
    std::string payload;                  ///< KyotoDD binary of the ZDD (empty when terminal != 0)
    int terminal;                         ///< 0: payload present, 1: the empty family, 2: the family {∅}

    StoredDDData() : terminal(0) {}
};

/**
 * @brief Write the answer-set family of a compilation as a stored DD.
 *
 * Every variable of @c res.vars must have a primary name; the names and the
 * fact names must be distinct and must be writable as atoms of a program
 * (they are declared as #external atoms and facts when the file is loaded).
 * Otherwise std::invalid_argument is thrown. Show terms (res.terms) are not
 * stored; a warning is appended to @p warnings when some are dropped.
 * @throws std::runtime_error when the stream fails.
 */
void write_stored_dd(std::ostream& os, const CompileResult& res, std::vector<std::string>* warnings = 0);

/**
 * @brief Write a stored DD from its parts (a diagram that did not come from a
 * compilation, e.g. one built directly with KyotoDD).
 *
 * @c dd.payload must be a KyotoDD ZDD export whose levels are 1..N
 * (N = dd.names.size(); a diagram built in a fresh KyotoDD instance with N
 * variables, or one renamed with rename_zdd), or empty with @c dd.terminal 1
 * (the empty family) or 2 (the family {∅}). The names are validated as in
 * write_stored_dd (distinct, writable as atoms; duplicate fact names are
 * dropped). @throws std::invalid_argument on a bad name or part,
 * std::runtime_error when the stream fails.
 */
void write_stored_dd_data(std::ostream& os, const StoredDDData& dd);

/**
 * @brief Parse a stored DD (the stream must be opened in binary mode).
 * @throws std::runtime_error on a malformed file.
 */
StoredDDData read_stored_dd(std::istream& is);

/**
 * @brief Program text declaring the atoms of a stored DD: `#external n.` and
 * `#show n : n.` for every atom name and `f.` for every fact name. Grounded
 * together with a program, it lets the program use the atoms of the DD and
 * keeps them named whatever the program's #show statements are.
 */
std::string stored_dd_declarations(const StoredDDData& dd);

/** @brief A stored DD imported into the current KyotoDD instance. */
struct LoadedDD {
    StoredDDData data;
    kyotodd::ZDD zdd;                   ///< the family, over @c vars
    std::vector<kyotodd::bddvar> vars;  ///< variable of each name (same order as data.names)
};

/**
 * @brief Import a stored DD; KyotoDD must be initialized.
 *
 * The diagram lands on the variables currently at levels 1..N (N = number of
 * atoms); variables are created when fewer exist. The variable numbers are
 * recorded in @c vars, so the result stays valid when variables are added later.
 * @throws std::runtime_error when the payload is not a valid KyotoDD ZDD of at most N levels;
 *         KyotoDD's std::overflow_error ("node table exhausted") passes through unchanged.
 */
LoadedDD import_stored_dd(const StoredDDData& dd);

}  // namespace ddasp

#endif  // DDASP_STORED_DD_H
