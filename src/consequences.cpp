#include "ddasp/consequences.h"

#include <map>
#include <set>

#include "ddasp/dd_util.h"

namespace ddasp {

using kyotodd::ZDD;
using kyotodd::bddvar;
using bigint::BigInt;

namespace {

/** The sets of `family` that satisfy a #show condition (variables kept). */
ZDD satisfying(const ZDD& family, const std::vector<Literal>& condition, const CompileResult& res) {
    ZDD g = family;
    for (size_t i = 0; i < condition.size(); ++i) {
        Atom a = atom_of(condition[i]);
        bddvar v = static_cast<size_t>(a) < res.atom_to_var.size() ? res.atom_to_var[a] : 0;
        if (v == 0) {  // an atom of no rule is false
            if (is_positive(condition[i])) return ZDD(0);
            continue;
        }
        g = is_positive(condition[i]) ? g.onset(v).change(v) : g.offset(v);
    }
    return g;
}

}  // namespace

std::vector<NameCount> name_frequencies(const CompileResult& res) {
    const BigInt total = res.zdd.exact_count();
    std::map<bddvar, BigInt> var_count = zdd_variable_counts(res.zdd);

    // Display order of the names and, for every name, the conditions showing it.
    std::vector<std::string> order;
    std::map<std::string, std::vector<std::vector<Literal> > > conditions;
    std::set<std::string> facts(res.fact_names.begin(), res.fact_names.end());
    for (size_t i = 0; i < res.fact_names.size(); ++i) {
        if (conditions.insert(std::make_pair(res.fact_names[i], std::vector<std::vector<Literal> >())).second) {
            order.push_back(res.fact_names[i]);
        }
    }
    for (size_t i = 0; i < res.vars.size(); ++i) {
        Atom a = res.var_to_atom[res.vars[i]];
        std::map<Atom, std::string>::const_iterator it = res.names.find(a);
        if (it == res.names.end()) continue;
        if (conditions.find(it->second) == conditions.end()) order.push_back(it->second);
        conditions[it->second].push_back(std::vector<Literal>(1, a));
    }
    for (size_t t = 0; t < res.terms.size(); ++t) {
        if (conditions.find(res.terms[t].name) == conditions.end()) order.push_back(res.terms[t].name);
        conditions[res.terms[t].name].push_back(res.terms[t].condition);
    }

    std::vector<NameCount> out;
    for (size_t i = 0; i < order.size(); ++i) {
        const std::string& name = order[i];
        if (facts.count(name)) {
            out.push_back(NameCount(name, total));
            continue;
        }
        const std::vector<std::vector<Literal> >& conds = conditions[name];
        if (conds.size() == 1 && conds[0].size() == 1) {  // the common case: one literal, read off the counts
            Literal l = conds[0][0];
            Atom a = atom_of(l);
            bddvar v = static_cast<size_t>(a) < res.atom_to_var.size() ? res.atom_to_var[a] : 0;
            BigInt with = v == 0 ? BigInt(0) : var_count[v];
            out.push_back(NameCount(name, is_positive(l) ? with : total - with));
            continue;
        }
        ZDD shown(0);
        for (size_t c = 0; c < conds.size(); ++c) shown = shown + satisfying(res.zdd, conds[c], res);
        out.push_back(NameCount(name, shown.exact_count()));
    }
    return out;
}

std::vector<std::string> brave_consequences(const std::vector<NameCount>& frequencies) {
    std::vector<std::string> out;
    for (size_t i = 0; i < frequencies.size(); ++i) {
        if (!frequencies[i].count.is_zero()) out.push_back(frequencies[i].name);
    }
    return out;
}

std::vector<std::string> cautious_consequences(const std::vector<NameCount>& frequencies, const BigInt& total) {
    std::vector<std::string> out;
    if (total.is_zero()) return out;
    for (size_t i = 0; i < frequencies.size(); ++i) {
        if (frequencies[i].count == total) out.push_back(frequencies[i].name);
    }
    return out;
}

}  // namespace ddasp
