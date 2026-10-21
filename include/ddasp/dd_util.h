#ifndef DDASP_DD_UTIL_H
#define DDASP_DD_UTIL_H

/**
 * @file dd_util.h
 * @brief Small decision-diagram helpers built on top of KyotoDD.
 */

#include <functional>
#include <vector>

#include "bdd.h"

namespace ddasp {

/**
 * @brief Convert a BDD to a ZDD over an explicit variable set.
 *
 * The result is the family { S ⊆ vars | f(S) = 1 }, where f is evaluated with
 * the variables in @p vars set according to S. Variables of @p f that are not
 * in @p vars must not occur in @p f (quantify them out first); if one does,
 * std::invalid_argument is thrown. Variables in @p vars on which @p f does not
 * depend appear as "don't care" (both with and without) in the family.
 *
 * Unlike BDD::to_zdd, this works on variable numbers rather than levels, so it
 * can be used when the variables of @p f are interleaved with other variables.
 *
 * @param f    Characteristic function.
 * @param vars Variables of the resulting family (any order).
 */
kyotodd::ZDD bdd_to_zdd_over(const kyotodd::BDD& f, const std::vector<kyotodd::bddvar>& vars);

/**
 * @brief Convert a ZDD to a BDD over an explicit variable set (inverse of bdd_to_zdd_over).
 *
 * The result is the characteristic function of the family: it is true for an
 * assignment iff the set of true variables (among @p vars) is in the family.
 * Variables in @p vars that never occur in the family are forced to false.
 * Variables of the ZDD outside @p vars cause std::invalid_argument.
 */
kyotodd::BDD zdd_to_bdd_over(const kyotodd::ZDD& f, const std::vector<kyotodd::bddvar>& vars);

/**
 * @brief Rename the variables of a ZDD: from[i] becomes to[i].
 *
 * The result is the family obtained by replacing, in every set of @p f, the
 * variable from[i] by to[i]. The renaming must be injective (no variable listed
 * twice in @p from or in @p to); a variable of @p f that is missing from
 * @p from causes std::invalid_argument, while variables of @p from that do not
 * occur in @p f are ignored.
 *
 * When the renaming keeps the level order of the variables (level(from[i]) <
 * level(from[j]) iff level(to[i]) < level(to[j])) the diagram is rebuilt node
 * by node in linear time. Otherwise it is rebuilt with set operations, which
 * costs as much as reordering the diagram (and needs variable numbers up to
 * 65536, the limit of ZDD::change).
 */
kyotodd::ZDD rename_zdd(const kyotodd::ZDD& f, const std::vector<kyotodd::bddvar>& from,
                        const std::vector<kyotodd::bddvar>& to);

/**
 * @brief Variable-set BDD of the given variables, in the encoding that
 * BDD::support() returns and BDD::exist(const BDD&) expects (a chain whose
 * 1-edges all lead to the true terminal).
 *
 * BDD::exist(cube) dispatches to KyotoDD's iterative implementation for deep
 * diagrams, whereas BDD::exist(std::vector<bddvar>) always recurses and hits
 * the recursion limit beyond ~8000 levels; quantify with this set instead.
 */
kyotodd::BDD variable_set(const std::vector<kyotodd::bddvar>& vars);

/**
 * @brief Visit the sets of a ZDD one by one without materializing the family.
 *
 * Each set is given as its variables from the top level down. The visitor
 * returns false to stop the enumeration early. Iterative (no recursion).
 */
void for_each_set(const kyotodd::ZDD& f, const std::function<bool(const std::vector<kyotodd::bddvar>&)>& visit);

/**
 * @brief Threshold constraint: sum(weights[i] * [terms[i]]) >= bound.
 *
 * @param terms   BDDs of the (pseudo-)literals.
 * @param weights Weights (may be negative).
 * @param bound   Lower bound.
 * @return A BDD that is true exactly when the weighted sum of true terms reaches @p bound.
 */
kyotodd::BDD threshold_bdd(const std::vector<kyotodd::BDD>& terms,
                           const std::vector<long long>& weights, long long bound);

}  // namespace ddasp

#endif  // DDASP_DD_UTIL_H
