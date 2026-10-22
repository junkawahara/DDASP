#ifndef DDASP_CONSEQUENCES_H
#define DDASP_CONSEQUENCES_H

/**
 * @file consequences.h
 * @brief Brave and cautious consequences and per-name frequencies of a compilation.
 *
 * All three are read off the ZDD of the answer sets: the frequency of a name
 * is the number of answer sets that display it, a name is a brave consequence
 * when some answer set displays it and a cautious consequence when every
 * answer set does. The names are the ones `--enumerate` prints: fact names,
 * the names of the atoms of the result (in variable order), then the #show
 * terms, each name once (a name shown through several outputs counts the
 * answer sets displaying it through any of them).
 */

#include <string>
#include <vector>

#include "bdd.h"
#include "ddasp/compiler.h"

namespace ddasp {

/** @brief A displayed name and the number of answer sets displaying it. */
struct NameCount {
    std::string name;
    bigint::BigInt count;

    NameCount() {}
    NameCount(const std::string& n, const bigint::BigInt& c) : name(n), count(c) {}
};

/**
 * @brief Number of answer sets displaying each name, in display order.
 *
 * One pass over the ZDD gives the count of every variable; names shown
 * through several outputs or compound conditions are counted with set
 * operations on the family.
 */
std::vector<NameCount> name_frequencies(const CompileResult& res);

/** @brief Names displayed by at least one answer set (display order), from name_frequencies(). */
std::vector<std::string> brave_consequences(const std::vector<NameCount>& frequencies);

/**
 * @brief Names displayed by every answer set (display order), from name_frequencies().
 * @param total Number of answer sets; when it is zero (inconsistent program) the result is empty.
 */
std::vector<std::string> cautious_consequences(const std::vector<NameCount>& frequencies,
                                               const bigint::BigInt& total);

}  // namespace ddasp

#endif  // DDASP_CONSEQUENCES_H
