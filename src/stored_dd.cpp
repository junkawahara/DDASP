#include "ddasp/stored_dd.h"

#include <cstdlib>
#include <istream>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>

#include "ddasp/dd_util.h"

namespace ddasp {

using kyotodd::ZDD;
using kyotodd::bddvar;

namespace {

const char* const kMagicLine = "ddasp-dd 1";

bool is_lower(char c) { return c >= 'a' && c <= 'z'; }

/**
 * Whether a name can be written as an atom (or a fact) of a program: it starts
 * like a predicate symbol (optionally classically negated), its parentheses
 * and quotes balance, and outside quotes it contains neither whitespace nor
 * statement punctuation. Newlines are rejected everywhere (one name per line).
 */
bool writable_as_atom(const std::string& name) {
    size_t i = 0;
    if (i < name.size() && name[i] == '-') ++i;
    if (i >= name.size() || !(is_lower(name[i]) || name[i] == '_')) return false;
    int depth = 0;
    bool in_string = false;
    for (; i < name.size(); ++i) {
        char c = name[i];
        if (c == '\n' || c == '\r') return false;
        if (in_string) {
            if (c == '\\') {
                if (i + 1 >= name.size()) return false;
                ++i;  // escaped character
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        switch (c) {
            case '"': in_string = true; break;
            case '(': ++depth; break;
            case ')': if (--depth < 0) return false; break;
            case ' ': case '\t': case '.': case ':': case ';': case '%': return false;
            default: break;
        }
    }
    return depth == 0 && !in_string;
}

void check_name(const std::string& name, const char* what) {
    if (!writable_as_atom(name)) {
        throw std::invalid_argument("cannot store the DD: " + std::string(what) + " '" + name +
                                    "' cannot be written as an atom of a program");
    }
}

std::runtime_error malformed(const std::string& what) { return std::runtime_error("stored DD: " + what); }

/** Read one line; throws at end of input. */
std::string next_line(std::istream& is, const char* what) {
    std::string line;
    if (!std::getline(is, line)) throw malformed(std::string("unexpected end of file (expected ") + what + ")");
    return line;
}

/** Parse "<keyword> <count>" and return the count. */
size_t counted_line(std::istream& is, const char* keyword) {
    std::string line = next_line(is, keyword);
    std::string prefix = std::string(keyword) + " ";
    if (line.compare(0, prefix.size(), prefix) != 0) throw malformed("expected '" + prefix + "N', got '" + line + "'");
    std::string number = line.substr(prefix.size());
    if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos) {
        throw malformed("bad count in '" + line + "'");
    }
    return static_cast<size_t>(std::strtoull(number.c_str(), 0, 10));
}

void read_names(std::istream& is, size_t count, const char* what, std::vector<std::string>& out) {
    std::set<std::string> seen;
    for (size_t i = 0; i < count; ++i) {
        std::string name = next_line(is, what);
        if (name.empty()) throw malformed(std::string("empty ") + what + " name");
        if (!seen.insert(name).second) throw malformed(std::string(what) + " name '" + name + "' appears twice");
        out.push_back(name);
    }
}

}  // namespace

void write_stored_dd(std::ostream& os, const CompileResult& res, std::vector<std::string>* warnings) {
    const size_t n = res.vars.size();
    StoredDDData dd;
    dd.names.resize(n);
    for (size_t k = 0; k < n; ++k) {
        Atom a = res.var_to_atom[res.vars[k]];
        std::map<Atom, std::string>::const_iterator it = res.names.find(a);
        if (it == res.names.end()) {
            throw std::invalid_argument("cannot store the DD: atom " + res.atom_name(a) +
                                        " has no name (every stored atom needs a #show name)");
        }
        dd.names[k] = it->second;
    }
    dd.fact_names = res.fact_names;
    if (!res.terms.empty() && warnings) {
        warnings->push_back("stored DD: " + std::to_string(res.terms.size()) +
                            " #show term(s) (additional names, negated conditions) are not stored");
    }

    // Compact the diagram to levels 1..n: the top variable goes to level n.
    std::vector<bddvar> to(n);
    for (size_t k = 0; k < n; ++k) to[k] = kyotodd::bddvaroflev(static_cast<bddvar>(n - k));
    ZDD z = rename_zdd(res.zdd, res.vars, to);
    if (z.is_zero()) {
        dd.terminal = 1;
    } else if (z.is_one()) {
        dd.terminal = 2;
    } else {
        std::ostringstream buf;
        z.export_binary(buf);
        dd.payload = buf.str();
    }
    write_stored_dd_data(os, dd);
}

void write_stored_dd_data(std::ostream& os, const StoredDDData& dd) {
    std::set<std::string> seen;
    for (size_t k = 0; k < dd.names.size(); ++k) {
        check_name(dd.names[k], "atom name");
        if (!seen.insert(dd.names[k]).second) {
            throw std::invalid_argument("cannot store the DD: name '" + dd.names[k] + "' is shared by two atoms");
        }
    }
    std::vector<std::string> facts;
    std::set<std::string> fact_seen;
    for (size_t i = 0; i < dd.fact_names.size(); ++i) {
        const std::string& f = dd.fact_names[i];
        check_name(f, "fact name");
        if (seen.count(f)) throw std::invalid_argument("cannot store the DD: name '" + f + "' is both a fact and an atom");
        if (fact_seen.insert(f).second) facts.push_back(f);
    }
    if (dd.terminal < 0 || dd.terminal > 2) throw std::invalid_argument("cannot store the DD: bad terminal marker");
    if (dd.terminal != 0 && !dd.payload.empty()) {
        throw std::invalid_argument("cannot store the DD: a terminal family must not carry diagram data");
    }
    if (dd.terminal == 0 && dd.payload.empty()) {
        throw std::invalid_argument("cannot store the DD: the diagram data is empty");
    }

    os << kMagicLine << "\n";
    os << "atoms " << dd.names.size() << "\n";
    for (size_t k = 0; k < dd.names.size(); ++k) os << dd.names[k] << "\n";
    os << "facts " << facts.size() << "\n";
    for (size_t i = 0; i < facts.size(); ++i) os << facts[i] << "\n";
    if (dd.terminal == 1) {
        os << "zdd empty\n";
    } else if (dd.terminal == 2) {
        os << "zdd single\n";
    } else {
        os << "zdd " << dd.payload.size() << "\n";
        os.write(dd.payload.data(), static_cast<std::streamsize>(dd.payload.size()));
    }
    if (!os) throw std::runtime_error("stored DD: write error");
}

StoredDDData read_stored_dd(std::istream& is) {
    StoredDDData dd;
    std::string line = next_line(is, "header");
    if (line != kMagicLine) throw malformed("not a DDASP stored DD (first line must be '" + std::string(kMagicLine) + "')");
    size_t n = counted_line(is, "atoms");
    read_names(is, n, "atom", dd.names);
    size_t m = counted_line(is, "facts");
    read_names(is, m, "fact", dd.fact_names);
    std::set<std::string> atoms(dd.names.begin(), dd.names.end());
    for (size_t i = 0; i < dd.fact_names.size(); ++i) {
        if (atoms.count(dd.fact_names[i])) throw malformed("name '" + dd.fact_names[i] + "' is both a fact and an atom");
    }
    line = next_line(is, "zdd");
    if (line == "zdd empty") {
        dd.terminal = 1;
    } else if (line == "zdd single") {
        dd.terminal = 2;
    } else {
        std::istringstream rest(line);
        size_t size = counted_line(rest, "zdd");
        dd.payload.resize(size);
        if (size > 0) is.read(&dd.payload[0], static_cast<std::streamsize>(size));
        if (static_cast<size_t>(is.gcount()) != size) throw malformed("truncated diagram data");
    }
    return dd;
}

std::string stored_dd_declarations(const StoredDDData& dd) {
    std::string text;
    for (size_t k = 0; k < dd.names.size(); ++k) {
        const std::string& n = dd.names[k];
        text += "#external " + n + ".\n#show " + n + " : " + n + ".\n";
    }
    for (size_t i = 0; i < dd.fact_names.size(); ++i) text += dd.fact_names[i] + ".\n";
    return text;
}

LoadedDD import_stored_dd(const StoredDDData& dd) {
    LoadedDD out;
    out.data = dd;
    const size_t n = dd.names.size();
    if (dd.terminal == 1) {
        out.zdd = ZDD::Empty;
    } else if (dd.terminal == 2) {
        out.zdd = ZDD::Single;
    } else {
        std::istringstream is(dd.payload);
        try {
            out.zdd = ZDD::import_binary(is);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("stored DD: cannot import the diagram: ") + e.what());
        }
    }
    // The export records only the levels that have nodes, so fewer than n
    // variables may exist now (top atoms that are never true, terminal families).
    while (kyotodd::bddvarused() < n) kyotodd::bddnewvar();
    if (!out.zdd.is_terminal() && kyotodd::bddlevofvar(out.zdd.top()) > n) {
        throw std::runtime_error("stored DD: the diagram has more levels than the file has atoms");
    }
    out.vars.resize(n);
    for (size_t k = 0; k < n; ++k) out.vars[k] = kyotodd::bddvaroflev(static_cast<bddvar>(n - k));
    return out;
}

}  // namespace ddasp
