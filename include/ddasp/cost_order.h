#ifndef DDASP_COST_ORDER_H
#define DDASP_COST_ORDER_H

/**
 * @file cost_order.h
 * @brief Enumerate the answer sets of a compilation in ascending #minimize cost order.
 */

#include <functional>
#include <vector>

#include "bdd.h"
#include "ddasp/compiler.h"

namespace ddasp {

/** @brief How enumerate_by_cost visits a result. */
enum class CostOrderMethod {
    KBest,    ///< KyotoDD's ZddMinWeightIterator (Yen's k-best algorithm) on the result's ZDD:
              ///< one priority level and no weighted atom hidden by projection.
    Peeling   ///< The family before projection is peeled cost vector by cost vector (each time
              ///< the lexicographic optimum of the rest, as optimize finds it); handles several
              ///< priority levels and weights on hidden atoms.
};

/**
 * @brief The method enumerate_by_cost uses for @p res.
 * @throws std::invalid_argument when the program has no #minimize statement.
 */
CostOrderMethod cost_order_method(const CompileResult& res);

/** @brief Visitor of enumerate_by_cost: the cost vector and the set (variables from the top level down); returns false to stop. */
typedef std::function<bool(const std::vector<long long>&, const std::vector<kyotodd::bddvar>&)> CostVisitor;

/**
 * @brief Visit the answer sets of @p res in ascending order of their #minimize
 * cost (lexicographic over the priority levels, most important first; the order
 * among equal costs is unspecified).
 *
 * Every (projected) answer set is visited once, at the smallest cost vector of
 * the answer sets projecting to it: the reading of the cost bounds and of the
 * cost histogram, so the costs visited follow the histogram. The sets are given
 * as variables from the top level down, as for_each_set gives them.
 * @throws std::invalid_argument when the program has no #minimize statement or
 *         a weight is outside the 32-bit range.
 */
void enumerate_by_cost(const CompileResult& res, const CostVisitor& visit);

}  // namespace ddasp

#endif  // DDASP_COST_ORDER_H
