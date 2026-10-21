#include "ddasp/dd_util.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
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

/*
 * The conversions and the threshold builder walk one level per step, so a
 * recursive formulation would need a stack frame per variable (tens of
 * thousands for large programs). They are written with an explicit stack of
 * frames instead; `stage` records how many children of the frame are done and
 * `ret` carries the result of the frame that was just popped.
 */

typedef std::unordered_map<bddp, std::unordered_map<size_t, ZDD> > ConvMemo;

struct ToZddFrame {
    BDD f;
    size_t i;
    int stage;
    ZDD lo;
    ToZddFrame(const BDD& f_, size_t i_) : f(f_), i(i_), stage(0) {}
};

void check_in_vars(const char* who, const BDD& f, bddvar v) {
    if (!f.is_terminal() && kyotodd::bddlevofvar(f.top()) > kyotodd::bddlevofvar(v)) {
        throw std::invalid_argument(std::string(who) + ": BDD depends on variable " + std::to_string(f.top()) +
                                    " which is not in the variable set");
    }
}

}  // namespace

ZDD bdd_to_zdd_over(const BDD& f, const std::vector<bddvar>& vars) {
    std::vector<bddvar> sorted(vars);
    std::sort(sorted.begin(), sorted.end(), LevelDesc());
    ConvMemo memo;
    std::vector<ToZddFrame> stack;
    stack.push_back(ToZddFrame(f, 0));
    ZDD ret;
    while (!stack.empty()) {
        ToZddFrame& fr = stack.back();
        if (fr.stage == 0) {
            if (fr.i == sorted.size()) {
                if (!fr.f.is_terminal()) {
                    throw std::invalid_argument("bdd_to_zdd_over: BDD depends on variable " +
                                                std::to_string(fr.f.top()) + " which is not in the variable set");
                }
                ret = fr.f.is_one() ? ZDD::Single : ZDD::Empty;
                stack.pop_back();
                continue;
            }
            std::unordered_map<size_t, ZDD>& m = memo[fr.f.id()];
            std::unordered_map<size_t, ZDD>::iterator it = m.find(fr.i);
            if (it != m.end()) {
                ret = it->second;
                stack.pop_back();
                continue;
            }
            bddvar v = sorted[fr.i];
            check_in_vars("bdd_to_zdd_over", fr.f, v);
            fr.stage = 1;
            BDD child = (!fr.f.is_terminal() && fr.f.top() == v) ? fr.f.child0() : fr.f;
            size_t next = fr.i + 1;
            stack.push_back(ToZddFrame(child, next));  // invalidates fr
            continue;
        }
        bddvar v = sorted[fr.i];
        bool split = !fr.f.is_terminal() && fr.f.top() == v;
        if (fr.stage == 1) {
            fr.lo = ret;
            if (split) {
                fr.stage = 2;
                BDD child = fr.f.child1();
                size_t next = fr.i + 1;
                stack.push_back(ToZddFrame(child, next));  // invalidates fr
                continue;
            }
            ret = ZDD::getnode(v, fr.lo, fr.lo);  // f does not depend on v
        } else {
            ret = ZDD::getnode(v, fr.lo, ret);
        }
        memo[fr.f.id()][fr.i] = ret;
        stack.pop_back();
    }
    return ret;
}

BDD variable_set(const std::vector<bddvar>& vars) {
    std::vector<bddvar> sorted(vars);
    std::sort(sorted.begin(), sorted.end(), LevelDesc());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    BDD set = BDD::False;
    for (size_t i = sorted.size(); i-- > 0;) set = BDD::getnode(sorted[i], set, BDD::True);  // lowest level first
    return set;
}

namespace {

typedef std::unordered_map<bddp, std::unordered_map<size_t, BDD> > ConvBackMemo;

struct ToBddFrame {
    ZDD f;
    size_t i;
    int stage;
    BDD lo;
    ToBddFrame(const ZDD& f_, size_t i_) : f(f_), i(i_), stage(0) {}
};

}  // namespace

BDD zdd_to_bdd_over(const ZDD& f, const std::vector<bddvar>& vars) {
    std::vector<bddvar> sorted(vars);
    std::sort(sorted.begin(), sorted.end(), LevelDesc());
    ConvBackMemo memo;
    std::vector<ToBddFrame> stack;
    stack.push_back(ToBddFrame(f, 0));
    BDD ret;
    while (!stack.empty()) {
        ToBddFrame& fr = stack.back();
        if (fr.stage == 0) {
            if (fr.i == sorted.size()) {
                if (!fr.f.is_terminal()) {
                    throw std::invalid_argument("zdd_to_bdd_over: ZDD depends on variable " +
                                                std::to_string(fr.f.top()) + " which is not in the variable set");
                }
                ret = fr.f.is_one() ? BDD::True : BDD::False;
                stack.pop_back();
                continue;
            }
            std::unordered_map<size_t, BDD>& m = memo[fr.f.id()];
            std::unordered_map<size_t, BDD>::iterator it = m.find(fr.i);
            if (it != m.end()) {
                ret = it->second;
                stack.pop_back();
                continue;
            }
            bddvar v = sorted[fr.i];
            if (!fr.f.is_terminal() && kyotodd::bddlevofvar(fr.f.top()) > kyotodd::bddlevofvar(v)) {
                throw std::invalid_argument("zdd_to_bdd_over: ZDD depends on variable " +
                                            std::to_string(fr.f.top()) + " which is not in the variable set");
            }
            fr.stage = 1;
            ZDD child = (!fr.f.is_terminal() && fr.f.top() == v) ? fr.f.child0() : fr.f;
            size_t next = fr.i + 1;
            stack.push_back(ToBddFrame(child, next));  // invalidates fr
            continue;
        }
        bddvar v = sorted[fr.i];
        bool split = !fr.f.is_terminal() && fr.f.top() == v;
        if (fr.stage == 1) {
            fr.lo = ret;
            if (split) {
                fr.stage = 2;
                ZDD child = fr.f.child1();
                size_t next = fr.i + 1;
                stack.push_back(ToBddFrame(child, next));  // invalidates fr
                continue;
            }
            ret = BDD::ite(BDD::prime(v), BDD::False, fr.lo);  // zero-suppressed: v absent below
        } else {
            ret = BDD::ite(BDD::prime(v), ret, fr.lo);
        }
        memo[fr.f.id()][fr.i] = ret;
        stack.pop_back();
    }
    return ret;
}

namespace {

struct RenameFrame {
    ZDD f;
    int stage;
    ZDD lo;
    explicit RenameFrame(const ZDD& f_) : f(f_), stage(0) {}
};

struct IndexByLevel {
    const std::vector<bddvar>* vars;
    bool operator()(size_t a, size_t b) const {
        return kyotodd::bddlevofvar((*vars)[a]) < kyotodd::bddlevofvar((*vars)[b]);
    }
};

}  // namespace

ZDD rename_zdd(const ZDD& f, const std::vector<bddvar>& from, const std::vector<bddvar>& to) {
    if (from.size() != to.size()) throw std::invalid_argument("rename_zdd: from and to differ in size");
    std::unordered_map<bddvar, bddvar> target;
    std::unordered_set<bddvar> targets;
    for (size_t i = 0; i < from.size(); ++i) {
        if (!target.insert(std::make_pair(from[i], to[i])).second) {
            throw std::invalid_argument("rename_zdd: variable " + std::to_string(from[i]) + " is listed twice in from");
        }
        if (!targets.insert(to[i]).second) {
            throw std::invalid_argument("rename_zdd: variable " + std::to_string(to[i]) + " is listed twice in to");
        }
    }
    // An order-preserving renaming can create every node directly (getnode needs
    // the children strictly below the variable); any other one goes through set
    // operations, which reorder the diagram.
    std::vector<size_t> idx(from.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    IndexByLevel by_from_level = {&from};
    std::sort(idx.begin(), idx.end(), by_from_level);
    bool monotone = true;
    for (size_t k = 1; k < idx.size() && monotone; ++k) {
        monotone = kyotodd::bddlevofvar(to[idx[k - 1]]) < kyotodd::bddlevofvar(to[idx[k]]);
    }

    std::unordered_map<bddp, ZDD> memo;
    std::vector<RenameFrame> stack;
    stack.push_back(RenameFrame(f));
    ZDD ret;
    while (!stack.empty()) {
        RenameFrame& fr = stack.back();
        if (fr.stage == 0) {
            if (fr.f.is_terminal()) {
                ret = fr.f;
                stack.pop_back();
                continue;
            }
            std::unordered_map<bddp, ZDD>::iterator it = memo.find(fr.f.id());
            if (it != memo.end()) {
                ret = it->second;
                stack.pop_back();
                continue;
            }
            if (!target.count(fr.f.top())) {
                throw std::invalid_argument("rename_zdd: ZDD depends on variable " + std::to_string(fr.f.top()) +
                                            " which is not renamed");
            }
            fr.stage = 1;
            ZDD child = fr.f.child0();
            stack.push_back(RenameFrame(child));  // invalidates fr
            continue;
        }
        if (fr.stage == 1) {
            fr.lo = ret;
            fr.stage = 2;
            ZDD child = fr.f.child1();
            stack.push_back(RenameFrame(child));  // invalidates fr
            continue;
        }
        // ret holds the renamed 1-child: the sets containing the top variable, without it.
        bddvar w = target[fr.f.top()];
        ret = monotone ? ZDD::getnode(w, fr.lo, ret) : fr.lo + ret.change(w);
        memo[fr.f.id()] = ret;
        stack.pop_back();
    }
    return ret;
}

void for_each_set(const ZDD& f, const std::function<bool(const std::vector<bddvar>&)>& visit) {
    if (f.is_zero()) return;
    // Depth-first search over the paths to the 1-terminal; a frame is a node and
    // the edge taken next (0 then 1). `path` holds the variables of the 1-edges.
    struct Frame {
        ZDD node;
        int next_edge;
    };
    std::vector<Frame> stack;
    std::vector<bddvar> path;
    Frame root = {f, 0};
    stack.push_back(root);
    while (!stack.empty()) {
        Frame& fr = stack.back();
        if (fr.node.is_terminal()) {
            if (fr.node.is_one() && !visit(path)) return;
            stack.pop_back();
            if (!stack.empty() && stack.back().next_edge == 2) path.pop_back();
            continue;
        }
        if (fr.next_edge == 0) {
            fr.next_edge = 1;
            ZDD child = fr.node.child0();
            Frame next = {child, 0};
            stack.push_back(next);  // invalidates fr
        } else if (fr.next_edge == 1) {
            fr.next_edge = 2;
            path.push_back(fr.node.top());
            ZDD child = fr.node.child1();
            Frame next = {child, 0};
            stack.push_back(next);  // invalidates fr
        } else {
            stack.pop_back();
            if (!stack.empty() && stack.back().next_edge == 2) path.pop_back();
        }
    }
}

namespace {

struct ThresholdFrame {
    size_t i;
    long long need;
    int stage;
    BDD if_true;
    ThresholdFrame(size_t i_, long long need_) : i(i_), need(need_), stage(0) {}
};

struct TopLevelDesc {
    const std::vector<BDD>* terms;
    static bddvar level_of(const BDD& f) { return f.is_terminal() ? 0 : kyotodd::bddlevofvar(f.top()); }
    bool operator()(size_t a, size_t b) const { return level_of((*terms)[a]) > level_of((*terms)[b]); }
};

BDD threshold_bdd_ordered(const std::vector<BDD>& terms, const std::vector<long long>& weights, long long bound);

}  // namespace

BDD threshold_bdd(const std::vector<BDD>& terms, const std::vector<long long>& weights, long long bound) {
    if (terms.size() != weights.size()) {
        throw std::invalid_argument("threshold_bdd: terms and weights differ in size");
    }
    size_t n = terms.size();
    // The sum does not depend on the order of the terms, so process them from the
    // top level down: every ite() then combines a term with results that lie
    // entirely below it, which is constant-time instead of a full traversal.
    std::vector<size_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = i;
    TopLevelDesc by_level = {&terms};
    std::stable_sort(idx.begin(), idx.end(), by_level);
    std::vector<BDD> sorted_terms(n);
    std::vector<long long> sorted_weights(n);
    for (size_t i = 0; i < n; ++i) {
        sorted_terms[i] = terms[idx[i]];
        sorted_weights[i] = weights[idx[i]];
    }
    return threshold_bdd_ordered(sorted_terms, sorted_weights, bound);
}

namespace {

BDD threshold_bdd_ordered(const std::vector<BDD>& terms, const std::vector<long long>& weights, long long bound) {
    size_t n = terms.size();
    std::vector<long long> max_rest(n + 1, 0), min_rest(n + 1, 0);  // extreme sums over indices >= i
    for (size_t i = n; i-- > 0;) {
        max_rest[i] = max_rest[i + 1] + (weights[i] > 0 ? weights[i] : 0);
        min_rest[i] = min_rest[i + 1] + (weights[i] < 0 ? weights[i] : 0);
    }
    // Frame (i, need) computes: sum_{j >= i} weights[j] * [terms[j]] >= need.
    std::map<std::pair<size_t, long long>, BDD> memo;
    std::vector<ThresholdFrame> stack;
    stack.push_back(ThresholdFrame(0, bound));
    BDD ret;
    while (!stack.empty()) {
        ThresholdFrame& fr = stack.back();
        if (fr.stage == 0) {
            if (min_rest[fr.i] >= fr.need) {
                ret = BDD::True;
                stack.pop_back();
                continue;
            }
            if (max_rest[fr.i] < fr.need) {
                ret = BDD::False;
                stack.pop_back();
                continue;
            }
            std::map<std::pair<size_t, long long>, BDD>::iterator it = memo.find(std::make_pair(fr.i, fr.need));
            if (it != memo.end()) {
                ret = it->second;
                stack.pop_back();
                continue;
            }
            fr.stage = 1;
            size_t next = fr.i + 1;
            long long need = fr.need - weights[fr.i];
            stack.push_back(ThresholdFrame(next, need));  // invalidates fr
            continue;
        }
        if (fr.stage == 1) {
            fr.if_true = ret;
            fr.stage = 2;
            size_t next = fr.i + 1;
            long long need = fr.need;
            stack.push_back(ThresholdFrame(next, need));  // invalidates fr
            continue;
        }
        ret = BDD::ite(terms[fr.i], fr.if_true, ret);
        memo[std::make_pair(fr.i, fr.need)] = ret;
        stack.pop_back();
    }
    return ret;
}

}  // namespace

}  // namespace ddasp
