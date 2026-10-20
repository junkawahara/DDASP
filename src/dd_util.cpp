#include "ddasp/dd_util.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace ddasp {

using kyotodd::BDD;
using kyotodd::ZDD;
using kyotodd::bddp;
using kyotodd::bddvar;

namespace {

struct LevelDesc {
    bool operator()(bddvar a, bddvar b) const { return kyotodd::bddlevofvar(a) > kyotodd::bddlevofvar(b); }
};

typedef std::unordered_map<bddp, std::unordered_map<size_t, ZDD> > ConvMemo;

ZDD bdd_to_zdd_rec(const BDD& f, const std::vector<bddvar>& vars, size_t i, ConvMemo& memo) {
    if (i == vars.size()) {
        if (!f.is_terminal()) {
            throw std::invalid_argument("bdd_to_zdd_over: BDD depends on variable " +
                                        std::to_string(f.top()) + " which is not in the variable set");
        }
        return f.is_one() ? ZDD::Single : ZDD::Empty;
    }
    std::unordered_map<size_t, ZDD>& m = memo[f.id()];
    std::unordered_map<size_t, ZDD>::iterator it = m.find(i);
    if (it != m.end()) return it->second;

    bddvar v = vars[i];
    ZDD lo, hi;
    if (!f.is_terminal() && f.top() == v) {
        lo = bdd_to_zdd_rec(f.child0(), vars, i + 1, memo);
        hi = bdd_to_zdd_rec(f.child1(), vars, i + 1, memo);
    } else {
        if (!f.is_terminal() && kyotodd::bddlevofvar(f.top()) > kyotodd::bddlevofvar(v)) {
            throw std::invalid_argument("bdd_to_zdd_over: BDD depends on variable " +
                                        std::to_string(f.top()) + " which is not in the variable set");
        }
        lo = bdd_to_zdd_rec(f, vars, i + 1, memo);
        hi = lo;
    }
    ZDD r = ZDD::getnode(v, lo, hi);
    m[i] = r;
    return r;
}

}  // namespace

ZDD bdd_to_zdd_over(const BDD& f, const std::vector<bddvar>& vars) {
    std::vector<bddvar> sorted(vars);
    std::sort(sorted.begin(), sorted.end(), LevelDesc());
    ConvMemo memo;
    return bdd_to_zdd_rec(f, sorted, 0, memo);
}

namespace {

typedef std::unordered_map<bddp, std::unordered_map<size_t, BDD> > ConvBackMemo;

BDD zdd_to_bdd_rec(const ZDD& f, const std::vector<bddvar>& vars, size_t i, ConvBackMemo& memo) {
    if (i == vars.size()) {
        if (!f.is_terminal()) {
            throw std::invalid_argument("zdd_to_bdd_over: ZDD depends on variable " +
                                        std::to_string(f.top()) + " which is not in the variable set");
        }
        return f.is_one() ? BDD::True : BDD::False;
    }
    std::unordered_map<size_t, BDD>& m = memo[f.id()];
    std::unordered_map<size_t, BDD>::iterator it = m.find(i);
    if (it != m.end()) return it->second;

    bddvar v = vars[i];
    BDD lo, hi;
    if (!f.is_terminal() && f.top() == v) {
        lo = zdd_to_bdd_rec(f.child0(), vars, i + 1, memo);
        hi = zdd_to_bdd_rec(f.child1(), vars, i + 1, memo);
    } else {
        if (!f.is_terminal() && kyotodd::bddlevofvar(f.top()) > kyotodd::bddlevofvar(v)) {
            throw std::invalid_argument("zdd_to_bdd_over: ZDD depends on variable " +
                                        std::to_string(f.top()) + " which is not in the variable set");
        }
        lo = zdd_to_bdd_rec(f, vars, i + 1, memo);
        hi = BDD::False;  // zero-suppressed: v absent from every set below
    }
    BDD r = BDD::ite(BDD::prime(v), hi, lo);
    m[i] = r;
    return r;
}

}  // namespace

BDD zdd_to_bdd_over(const ZDD& f, const std::vector<bddvar>& vars) {
    std::vector<bddvar> sorted(vars);
    std::sort(sorted.begin(), sorted.end(), LevelDesc());
    ConvBackMemo memo;
    return zdd_to_bdd_rec(f, sorted, 0, memo);
}

namespace {

struct ThresholdBuilder {
    const std::vector<BDD>& terms;
    const std::vector<long long>& weights;
    std::vector<long long> max_rest;  // max achievable sum from index i onward
    std::vector<long long> min_rest;  // min achievable sum from index i onward
    std::map<std::pair<size_t, long long>, BDD> memo;

    ThresholdBuilder(const std::vector<BDD>& t, const std::vector<long long>& w)
        : terms(t), weights(w), max_rest(t.size() + 1, 0), min_rest(t.size() + 1, 0) {
        for (size_t i = t.size(); i-- > 0;) {
            max_rest[i] = max_rest[i + 1] + (w[i] > 0 ? w[i] : 0);
            min_rest[i] = min_rest[i + 1] + (w[i] < 0 ? w[i] : 0);
        }
    }

    // BDD for: sum_{j >= i} weights[j] * [terms[j]] >= need
    BDD build(size_t i, long long need) {
        if (min_rest[i] >= need) return BDD::True;
        if (max_rest[i] < need) return BDD::False;
        std::pair<size_t, long long> key(i, need);
        std::map<std::pair<size_t, long long>, BDD>::iterator it = memo.find(key);
        if (it != memo.end()) return it->second;
        BDD if_true = build(i + 1, need - weights[i]);
        BDD if_false = build(i + 1, need);
        BDD r = BDD::ite(terms[i], if_true, if_false);
        memo[key] = r;
        return r;
    }
};

}  // namespace

BDD threshold_bdd(const std::vector<BDD>& terms, const std::vector<long long>& weights, long long bound) {
    if (terms.size() != weights.size()) {
        throw std::invalid_argument("threshold_bdd: terms and weights differ in size");
    }
    ThresholdBuilder b(terms, weights);
    return b.build(0, bound);
}

}  // namespace ddasp
