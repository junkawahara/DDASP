#include "ddasp/cost_order.h"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "ddasp/dd_util.h"
#include "zdd_weight_iter.h"

namespace ddasp {

using kyotodd::ZDD;
using kyotodd::bddvar;

namespace {

/** 32-bit weights indexed by variable, sized as KyotoDD's weight functions require (> bddvarused()). */
std::vector<int> kyotodd_weights(const std::vector<long long>& w) {
    std::vector<int> r(std::max<size_t>(w.size(), kyotodd::bddvarused() + 1), 0);
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] > 0x7fffffffLL || w[i] < -0x7fffffffLL) {
            throw std::invalid_argument("#minimize weight out of 32-bit range");
        }
        r[i] = static_cast<int>(w[i]);
    }
    return r;
}

/** Variables of the family before projection that are not variables of the result. */
std::vector<bddvar> hidden_vars(const CompileResult& res) {
    std::set<bddvar> shown(res.vars.begin(), res.vars.end());
    std::vector<bddvar> hidden;
    for (size_t a = 1; a < res.atom_to_var.size(); ++a) {
        bddvar v = res.atom_to_var[a];
        if (v != 0 && !shown.count(v)) hidden.push_back(v);
    }
    return hidden;
}

/** Does any of the variables carry a non-zero weight on some level? */
bool carries_weight(const CompileResult& res, const std::vector<bddvar>& vars) {
    for (size_t c = 0; c < res.costs.size(); ++c) {
        const std::vector<long long>& w = res.costs[c].var_weights;
        for (size_t i = 0; i < vars.size(); ++i) {
            if (vars[i] < w.size() && w[vars[i]] != 0) return true;
        }
    }
    return false;
}

bool higher_level(bddvar a, bddvar b) { return kyotodd::bddlevofvar(a) > kyotodd::bddlevofvar(b); }

}  // namespace

CostOrderMethod cost_order_method(const CompileResult& res) {
    if (res.costs.empty()) {
        throw std::invalid_argument(
            "cost-ordered enumeration (--by-cost / best) needs a #minimize statement, but the program has none");
    }
    if (res.costs.size() == 1 && !carries_weight(res, hidden_vars(res))) return CostOrderMethod::KBest;
    return CostOrderMethod::Peeling;
}

void enumerate_by_cost(const CompileResult& res, const CostVisitor& visit) {
    CostOrderMethod method = cost_order_method(res);
    if (res.zdd.is_zero()) return;
    if (method == CostOrderMethod::KBest) {
        std::vector<int> w = kyotodd_weights(res.costs[0].var_weights);
        kyotodd::ZddMinWeightRange range(res.zdd, w);
        std::vector<long long> cost(1);
        for (kyotodd::ZddMinWeightIterator it = range.begin(); it != range.end(); ++it) {
            cost[0] = it->first + res.costs[0].offset;
            std::vector<bddvar> set = it->second;                // sorted by variable number
            std::sort(set.begin(), set.end(), higher_level);     // top level first, like for_each_set
            if (!visit(cost, set)) return;
        }
        return;
    }
    // Peeling: the lexicographic optimum of the rest is projected, its sets not
    // seen before are visited, and it is removed from the rest.
    std::vector<bddvar> hidden = hidden_vars(res);
    std::vector<std::vector<int> > w;
    for (size_t c = 0; c < res.costs.size(); ++c) w.push_back(kyotodd_weights(res.costs[c].var_weights));
    ZDD rest = res.family;
    ZDD seen(0);
    while (!rest.is_zero()) {
        ZDD best = rest;
        std::vector<long long> cost;
        for (size_t c = 0; c < w.size(); ++c) {
            long long m = best.min_weight(w[c]);
            best = best.cost_bound_le(w[c], m);
            cost.push_back(m + res.costs[c].offset);
        }
        ZDD fresh = (hidden.empty() ? best : best.project(hidden)) - seen;
        bool go_on = true;
        for_each_set(fresh, [&](const std::vector<bddvar>& set) -> bool {
            go_on = visit(cost, set);
            return go_on;
        });
        if (!go_on) return;
        seen = seen + fresh;
        rest = rest - best;
    }
}

}  // namespace ddasp
