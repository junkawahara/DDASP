/**
 * @file main.cpp
 * @brief ddasp command line tool: ground ASP program -> ZDD of answer sets.
 */

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "bdd.h"
#include "ddasp/aspif_reader.h"
#include "ddasp/compiler.h"
#include "ddasp/consequences.h"
#include "ddasp/cost_order.h"
#include "ddasp/dd_util.h"
#include "ddasp/stored_dd.h"

using namespace ddasp;

namespace {

struct Options {
    std::vector<std::string> inputs;
    std::string gringo = "gringo";
    std::vector<std::string> constants;  // -c NAME=VALUE, passed to the grounder
    std::string output;       // -o
    std::string atoms_file;   // --atoms
    std::string save_file;    // --save
    std::vector<std::string> load_files;  // --load
    bool count = false;
    bool enumerate = false;
    bool by_cost = false;
    bool brave = false;
    bool cautious = false;
    bool frequencies = false;
    bool cost_histogram = false;
    bool stats = false;
    bool json = false;
    bool quiet = false;
    bool export_bdd = false;
    bool all_atoms = false;
    bool use_project = false;
    bool optimize = false;
    bool external_values = false;
    std::vector<CostBound> cost_le, cost_ge, cost_eq;  // --cost-le / --cost-ge / --cost-eq (empty = none)
    std::string method = "scc";
    std::string order = "name";
    size_t enumerate_limit = 0;  // 0 = unlimited
    uint64_t node_max = 0;       // --node-max, 0 = unlimited
};

void usage(std::ostream& os) {
    os << "Usage: ddasp [options] FILE...\n"
          "\n"
          "Compile a ground ASP program into a ZDD representing all answer sets.\n"
          "FILE is an aspif file (*.aspif, as produced by `gringo --output=intermediate`)\n"
          "and is read as it is; any other files are passed to gringo first (see\n"
          "--gringo). The two kinds cannot be mixed.\n"
          "\n"
          "By default the summary lines `answer sets: N` and (with --opt or a relative\n"
          "cost bound) `optimum: ...` are printed; -q suppresses them.\n"
          "\n"
          "Options:\n"
          "  --count            print the bare number of answer sets (meant for -q)\n"
          "  --enumerate        print every answer set (one per line, shown atoms in\n"
          "                     variable-order, i.e. name order)\n"
          "  --limit N          print at most N answer sets with --enumerate (0 = all)\n"
          "  --by-cost          enumerate in ascending #minimize cost order (implies\n"
          "                     --enumerate), one `K[,K..]: atoms` line per answer set;\n"
          "                     after projection a set is listed once, at the smallest\n"
          "                     cost of the answer sets it stands for; --limit N gives\n"
          "                     the N best\n"          "  --brave            print the brave consequences (the names displayed by\n"
          "                     some answer set) on one line, like an answer set\n"
          "  --cautious         print the cautious consequences (the names displayed\n"
          "                     by every answer set; nothing if there is none)\n"
          "  --frequencies      print `name count` per displayed name: the number of\n"
          "                     answer sets displaying it (after projection)\n"
          "  -o FILE            write the ZDD to FILE (KyotoDD binary format)\n"
          "  --bdd              write a BDD instead of a ZDD with -o\n"
          "  --atoms FILE       write the variable/level/atom/name table to FILE, followed\n"
          "                     by one `# minimize` line per priority (level:weight)\n"
          "  --save FILE        write the answer sets as a stored DD (ZDD + atom names)\n"
          "                     to FILE, for --load in a later run\n"
          "  --load FILE        load a stored DD: its atoms become #external atoms of\n"
          "                     the program (declared automatically, except for aspif\n"
          "                     input) and the answer sets are restricted to its sets;\n"
          "                     may be repeated\n"
          "  --all-atoms        do not project onto the shown atoms (every atom\n"
          "                     becomes a DD variable)\n"
          "  --project          project onto the atoms of the #project statements\n"
          "                     instead of the shown atoms (when the program has\n"
          "                     any; unnamed ones are displayed as __atomN)\n"
          "  --opt              keep only the optimal answer sets (#minimize)\n"
          "  --cost-le K[,K..]  keep only the answer sets whose #minimize cost is at most\n"
          "                     K: one K per priority level, most important first (the\n"
          "                     order of the `optimum:` line); cost vectors compare\n"
          "                     lexicographically, like clingo's --opt-mode=enum,K,..,\n"
          "                     and omitted lower levels are free. `+d` means the\n"
          "                     optimal cost plus d (the optimum is then printed)\n"
          "  --cost-ge K[,K..]  ... at least K (same syntax; combines with --cost-le)\n"
          "  --cost-eq K[,K..]  ... exactly K (same syntax; not with --cost-le/--cost-ge).\n"
          "                     None of the three combines with --opt\n"
          "  --cost-histogram   print `K[,K..] count` per #minimize cost vector, ascending:\n"
          "                     the number of answer sets with that cost (after projection\n"
          "                     each set counts once, at the smallest cost of the answer\n"
          "                     sets it stands for); costs of no answer set are omitted\n"          "  --external-values  fix every free #external atom to its declared initial\n"
          "                     value (`#external a. [true]`; gringo declares a plain\n"
          "                     `#external a.` as false); ignored by default\n"
          "  --method M         stable-model method: scc (default) or twocopy\n"
          "  --order H          variable order heuristic: name (default), id, or a\n"
          "                     file with one atom name (or __atomN) per line (top of\n"
          "                     the DD first; remaining atoms follow in name order)\n"
          "  --gringo CMD       grounder command (default: gringo); it is run as\n"
          "                     `CMD --output=intermediate [-c NAME=VALUE...] FILE...`\n"
          "  -c NAME=VALUE      define the constant NAME as VALUE for the grounder (like\n"
          "                     gringo's -c; may be repeated); not with aspif input\n"
          "  --node-max N       limit KyotoDD's node table to N nodes; the run fails with\n"
          "                     a clear error instead of using all memory when the\n"
          "                     diagrams need more (default: unlimited)\n"
          "  --stats            print node counts and timings to stderr\n"
          "  --json             print the results as one JSON object instead of text:\n"
          "                     count, optimum, and the items requested by --brave,\n"
          "                     --cautious, --frequencies, --cost-histogram, --stats and\n"
          "                     --enumerate (answer_sets, written as they are found; with\n"
          "                     --by-cost as {cost, atoms} objects); -q and --count are ignored\n"
          "  -q, --quiet        do not print the summary lines\n"
          "  -h, --help         show this help\n";
}

/**
 * Parse a cost bound `K[,K...]`: one entry per #minimize priority level, most
 * important first; an entry `+d` (d >= 0) is relative to the optimum of its level.
 */
bool parse_cost_bounds(const std::string& option, const std::string& text, std::vector<CostBound>& out) {
    out.clear();
    size_t start = 0;
    while (true) {
        size_t comma = text.find(',', start);
        std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        bool relative = !item.empty() && item[0] == '+';
        std::string digits = relative ? item.substr(1) : item;
        bool ok = !digits.empty() && (std::isdigit(static_cast<unsigned char>(digits[0])) ||
                                      (!relative && digits[0] == '-' && digits.size() > 1));
        long long v = 0;
        if (ok) {
            char* end = 0;
            errno = 0;
            v = std::strtoll(digits.c_str(), &end, 10);
            ok = *end == '\0' && errno == 0;
        }
        if (!ok) {
            std::cerr << "ddasp: " << option << " expects integers K or +d separated by commas, got '" << text << "'\n";
            return false;
        }
        out.push_back(CostBound(v, relative));
        if (comma == std::string::npos) return true;
        start = comma + 1;
    }
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "ddasp: option " << name << " requires a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage(std::cout);
            std::exit(0);
        } else if (a == "--count") {
            opt.count = true;
        } else if (a == "--enumerate") {
            opt.enumerate = true;
        } else if (a == "--by-cost") {
            opt.by_cost = true;
            opt.enumerate = true;
        } else if (a == "--brave") {
            opt.brave = true;
        } else if (a == "--cautious") {
            opt.cautious = true;
        } else if (a == "--frequencies") {
            opt.frequencies = true;
        } else if (a == "--cost-histogram") {
            opt.cost_histogram = true;
        } else if (a == "--limit") {
            std::string v = need_value("--limit");
            char* end = 0;
            errno = 0;
            unsigned long long n = std::strtoull(v.c_str(), &end, 10);
            if (v.empty() || *end != '\0' || errno != 0 || v[0] == '-') {
                std::cerr << "ddasp: --limit expects a non-negative integer, got '" << v << "'\n";
                return false;
            }
            opt.enumerate_limit = static_cast<size_t>(n);
        } else if (a == "-o") {
            opt.output = need_value("-o");
        } else if (a == "--bdd") {
            opt.export_bdd = true;
        } else if (a == "--all-atoms") {
            opt.all_atoms = true;
        } else if (a == "--project") {
            opt.use_project = true;
        } else if (a == "--opt") {
            opt.optimize = true;
        } else if (a == "--cost-le" || a == "--cost-ge" || a == "--cost-eq") {
            std::string v = need_value(a.c_str());
            std::vector<CostBound>& bounds = a == "--cost-le" ? opt.cost_le : a == "--cost-ge" ? opt.cost_ge : opt.cost_eq;
            if (!parse_cost_bounds(a, v, bounds)) return false;
        } else if (a == "--external-values") {
            opt.external_values = true;
        } else if (a == "--method") {
            opt.method = need_value("--method");
        } else if (a == "--order") {
            opt.order = need_value("--order");
        } else if (a == "--atoms") {
            opt.atoms_file = need_value("--atoms");
        } else if (a == "--save") {
            opt.save_file = need_value("--save");
        } else if (a == "--load") {
            opt.load_files.push_back(need_value("--load"));
        } else if (a == "--node-max") {
            std::string v = need_value("--node-max");
            char* end = 0;
            errno = 0;
            unsigned long long n = std::strtoull(v.c_str(), &end, 10);
            if (v.empty() || *end != '\0' || errno != 0 || v[0] == '-' || n == 0) {
                std::cerr << "ddasp: --node-max expects a positive integer, got '" << v << "'\n";
                return false;
            }
            opt.node_max = static_cast<uint64_t>(n);
        } else if (a == "-c" || a == "--const") {
            std::string v = need_value("-c");
            size_t eq = v.find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 == v.size()) {
                std::cerr << "ddasp: -c expects NAME=VALUE, got '" << v << "'\n";
                return false;
            }
            opt.constants.push_back(v);
        } else if (a == "--gringo") {
            opt.gringo = need_value("--gringo");
        } else if (a == "--json") {
            opt.json = true;
        } else if (a == "--stats") {
            opt.stats = true;
        } else if (a == "-q" || a == "--quiet") {
            opt.quiet = true;
        } else if (!a.empty() && a[0] == '-' && a != "-") {
            std::cerr << "ddasp: unknown option " << a << "\n";
            return false;
        } else {
            opt.inputs.push_back(a);
        }
    }
    if (opt.inputs.empty()) {
        std::cerr << "ddasp: no input file\n";
        return false;
    }
    if (opt.use_project && opt.all_atoms) {
        std::cerr << "ddasp: --project and --all-atoms cannot be combined\n";
        return false;
    }
    if (opt.enumerate_limit != 0 && !opt.enumerate) {
        std::cerr << "ddasp: warning: --limit has no effect without --enumerate\n";
    }
    bool bounded = !opt.cost_le.empty() || !opt.cost_ge.empty() || !opt.cost_eq.empty();
    if (opt.optimize && bounded) {
        std::cerr << "ddasp: --opt cannot be combined with --cost-le/--cost-ge/--cost-eq (--cost-eq +0 means --opt)\n";
        return false;
    }
    if (!opt.cost_eq.empty() && (!opt.cost_le.empty() || !opt.cost_ge.empty())) {
        std::cerr << "ddasp: --cost-eq cannot be combined with --cost-le or --cost-ge\n";
        return false;
    }
    return true;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string shell_quote(const std::string& s) {
    std::string q = "'";
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\'') q += "'\\''";
        else q += s[i];
    }
    return q + "'";
}

/** A temporary file holding generated program text; removed on destruction. */
struct TempProgram {
    std::string path;
    explicit TempProgram(const std::string& text) {
        const char* dir = std::getenv("TMPDIR");
        std::string tmpl = std::string(dir && *dir ? dir : "/tmp") + "/ddasp-load-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        int fd = mkstemp(&buf[0]);
        if (fd == -1) throw std::runtime_error("cannot create a temporary file in " + tmpl);
        path = &buf[0];
        size_t done = 0;
        while (done < text.size()) {
            ssize_t n = write(fd, text.data() + done, text.size() - done);
            if (n <= 0) {
                close(fd);
                throw std::runtime_error("cannot write the temporary file " + path);
            }
            done += static_cast<size_t>(n);
        }
        close(fd);
    }
    ~TempProgram() { unlink(path.c_str()); }
};

/**
 * Run the grounder on the input files (plus the declarations of the loaded DDs,
 * when there are any) and return the aspif text.
 */
std::string run_gringo(const Options& opt, const std::string& declarations) {
    std::string cmd = opt.gringo + " --output=intermediate";
    for (size_t i = 0; i < opt.constants.size(); ++i) cmd += " -c " + shell_quote(opt.constants[i]);
    for (size_t i = 0; i < opt.inputs.size(); ++i) cmd += " " + shell_quote(opt.inputs[i]);
    std::unique_ptr<TempProgram> extra;
    if (!declarations.empty()) {
        extra.reset(new TempProgram(declarations));
        cmd += " " + shell_quote(extra->path);
    }
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) throw std::runtime_error("cannot run grounder: " + cmd);
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) text.append(buf, n);
    int status = pclose(pipe);
    if (status == -1) throw std::runtime_error("cannot wait for grounder: " + cmd);
    if (WIFSIGNALED(status)) {
        throw std::runtime_error("grounder killed by signal " + std::to_string(WTERMSIG(status)) + ": " + cmd);
    }
    if (WEXITSTATUS(status) != 0) {
        throw std::runtime_error("grounder failed (exit status " + std::to_string(WEXITSTATUS(status)) + "): " + cmd);
    }
    // `python -m clingo --mode=gringo` exits with 0 even when it fails (unreadable file,
    // syntax error) and prints the reason on stderr only; an empty output is the sign.
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        std::string hint = extra ? " (the last input file declares the atoms of the loaded DDs)" : "";
        throw std::runtime_error("grounder produced no output (see its messages above)" + hint + ": " + cmd);
    }
    return text;
}

Program load_program(const Options& opt, const std::string& declarations, std::vector<std::string>& warnings) {
    size_t aspif_count = 0;
    for (size_t i = 0; i < opt.inputs.size(); ++i) {
        if (ends_with(opt.inputs[i], ".aspif")) ++aspif_count;
    }
    if (aspif_count == opt.inputs.size()) {
        if (opt.inputs.size() != 1) throw std::runtime_error("only one aspif file can be given");
        if (!opt.constants.empty()) {
            throw std::runtime_error("-c cannot be used with aspif input (the program is already ground)");
        }
        return read_aspif_file(opt.inputs[0], &warnings);
    }
    if (aspif_count != 0) {
        throw std::runtime_error("cannot mix an aspif file with source files (aspif files are read as they are)");
    }
    std::string text = run_gringo(opt, declarations);
    return read_aspif_string(text, &warnings);
}

/** Atom id of a "__atomN" name (0 if the name has another form). */
Atom parse_unnamed_atom(const std::string& name) {
    const std::string prefix = "__atom";
    if (name.compare(0, prefix.size(), prefix) != 0 || name.size() == prefix.size()) return 0;
    char* end = 0;
    long v = std::strtol(name.c_str() + prefix.size(), &end, 10);
    if (*end != '\0' || v <= 0 || v > 0x7fffffffL) return 0;
    return static_cast<Atom>(v);
}

/**
 * Read an order file (one atom name or __atomN per line) and map the names to
 * atoms. Every #show name of a single atom counts (`#show t : a.` as well as
 * the primary name); a name shown for several atoms is ambiguous and skipped.
 */
std::vector<Atom> read_order_file(const std::string& path, const Program& program) {
    std::ifstream in(path.c_str());
    if (!in) throw std::runtime_error("cannot open order file: " + path);
    std::map<std::string, std::set<Atom> > by_name;
    for (size_t o = 0; o < program.outputs().size(); ++o) {
        const Output& out = program.outputs()[o];
        if (out.condition.size() == 1 && is_positive(out.condition[0])) by_name[out.name].insert(out.condition[0]);
    }
    std::vector<Atom> atoms = program_atoms(program);
    std::set<Atom> known(atoms.begin(), atoms.end());
    std::vector<Atom> order;
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        size_t b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos || line[b] == '#' || line[b] == '%') continue;
        size_t e = line.find_last_not_of(" \t\r");
        std::string name = line.substr(b, e - b + 1);
        std::map<std::string, std::set<Atom> >::const_iterator it = by_name.find(name);
        if (it != by_name.end() && it->second.size() > 1) {
            std::cerr << "ddasp: warning: order file line " << line_no << ": name '" << name
                      << "' is shown for several atoms\n";
            continue;
        }
        Atom a = it != by_name.end() ? *it->second.begin() : parse_unnamed_atom(name);
        if (a == 0 || !known.count(a)) {
            std::cerr << "ddasp: warning: order file line " << line_no << ": unknown atom '" << name << "'\n";
            continue;
        }
        order.push_back(a);
    }
    return order;
}

/** One `label: value` line of --stats (times in seconds); also a field of the JSON output. */
struct StatItem {
    std::string label;
    std::string value;
    bool seconds;
    StatItem() : seconds(false) {}
    /** JSON key: the label with underscores for spaces. */
    std::string key() const {
        std::string k = label;
        for (size_t i = 0; i < k.size(); ++i) {
            if (k[i] == ' ') k[i] = '_';
        }
        return k;
    }
};

StatItem stat_count(const char* label, uint64_t value) {
    StatItem item;
    item.label = label;
    item.value = std::to_string(value);
    return item;
}

StatItem stat_seconds(const char* label, double value) {
    StatItem item;
    item.label = label;
    std::ostringstream s;
    s << value;
    item.value = s.str();
    item.seconds = true;
    return item;
}

/** A JSON string literal: quotes, backslashes and control characters escaped, other bytes as they are. */
std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

void json_strings(std::ostream& os, const std::vector<std::string>& v) {
    os << "[";
    for (size_t i = 0; i < v.size(); ++i) os << (i ? ", " : "") << json_string(v[i]);
    os << "]";
}

void json_numbers(std::ostream& os, const std::vector<long long>& v) {
    os << "[";
    for (size_t i = 0; i < v.size(); ++i) os << (i ? ", " : "") << v[i];
    os << "]";
}

/** Writes the fields of the one JSON object of --json: `field(key)` then the value by the caller. */
struct JsonWriter {
    std::ostream& os;
    bool first;
    explicit JsonWriter(std::ostream& o) : os(o), first(true) {}
    void open() { os << "{"; }
    void field(const char* key) {
        os << (first ? "\n  " : ",\n  ") << json_string(key) << ": ";
        first = false;
    }
    void close() { os << "\n}\n"; }
};

void print_names_line(const std::vector<std::string>& names) {
    for (size_t i = 0; i < names.size(); ++i) std::cout << (i ? " " : "") << names[i];
    std::cout << "\n";
}

typedef std::chrono::steady_clock Clock;

double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        usage(std::cerr);
        return 2;
    }

    try {
        Clock::time_point t0 = Clock::now();
        std::vector<StoredDDData> stored;
        std::string declarations;
        for (size_t i = 0; i < opt.load_files.size(); ++i) {
            std::ifstream in(opt.load_files[i].c_str(), std::ios::binary);
            if (!in) throw std::runtime_error("cannot open stored DD file: " + opt.load_files[i]);
            try {
                stored.push_back(read_stored_dd(in));
            } catch (const std::exception& e) {
                throw std::runtime_error(opt.load_files[i] + ": " + e.what());
            }
            declarations += stored_dd_declarations(stored.back());
        }
        std::vector<std::string> warnings;
        Program program = load_program(opt, declarations, warnings);
        for (size_t i = 0; i < warnings.size(); ++i) std::cerr << "ddasp: warning: " << warnings[i] << "\n";
        if (opt.by_cost && program.minimizes().empty()) {
            throw std::runtime_error("--by-cost needs a #minimize statement, but the program has none");
        }
        double t_load = seconds_since(t0);

        // Initial table of 65536 nodes (or the limit, if smaller); unlimited growth unless --node-max.
        const uint64_t initial = 1 << 16;
        if (opt.node_max == 0) kyotodd::bddinit(initial);  // throws on failure
        else kyotodd::bddinit(std::min(initial, opt.node_max), opt.node_max);
        std::vector<LoadedDD> loaded;
        for (size_t i = 0; i < stored.size(); ++i) {
            try {
                loaded.push_back(import_stored_dd(stored[i]));
            } catch (const std::exception& e) {
                throw std::runtime_error(opt.load_files[i] + ": " + e.what());
            }
        }

        Clock::time_point t1 = Clock::now();
        CompileOptions copts;
        copts.project_to_shown = !opt.all_atoms;
        copts.use_project_statements = opt.use_project;
        copts.optimize = opt.optimize;
        copts.cost_le = opt.cost_le;
        copts.cost_ge = opt.cost_ge;
        copts.cost_eq = opt.cost_eq;
        copts.cost_histogram = opt.cost_histogram;
        copts.external_values = opt.external_values;
        copts.method = parse_method(opt.method);
        if (opt.order == "name" || opt.order == "id") {
            copts.order_heuristic = parse_order_heuristic(opt.order);
        } else {
            copts.order = read_order_file(opt.order, program);
        }
        CompileResult res = compile(program, copts, loaded);
        double t_compile = seconds_since(t1);
        for (size_t i = 0; i < res.warnings.size(); ++i) std::cerr << "ddasp: warning: " << res.warnings[i] << "\n";

        // Statistics: stderr lines, and the "stats" object of --json.
        std::vector<StatItem> stat_items;
        if (opt.stats) {
            const CompileStats& st = res.stats;
            stat_items.push_back(stat_count("rules", program.rules().size()));
            stat_items.push_back(stat_count("tautological rules", st.simplified_rules));
            stat_items.push_back(stat_count("atoms", st.atoms));
            stat_items.push_back(stat_count("shown atoms", res.vars.size()));
            stat_items.push_back(stat_count("aux vars", st.aux_vars));
            stat_items.push_back(stat_count("groups", st.groups));
            stat_items.push_back(stat_count("models nodes", st.models_nodes));
            stat_items.push_back(stat_count("stable nodes", st.stable_nodes));
            stat_items.push_back(stat_count("zdd nodes", res.zdd.raw_size()));
            stat_items.push_back(stat_count("bdd nodes", res.bdd.raw_size()));
            stat_items.push_back(stat_count("peak table nodes", st.peak_nodes));
            if (!loaded.empty()) {
                stat_items.push_back(stat_count("loaded dds", st.loaded_dds));
                stat_items.push_back(stat_count("synthesized atoms", st.synthesized_atoms));
            }
            stat_items.push_back(stat_seconds("load time", t_load));
            stat_items.push_back(stat_seconds("order time", st.order_time));
            stat_items.push_back(stat_seconds("models time", st.models_time));
            stat_items.push_back(stat_seconds("supported time", st.supported_time));
            stat_items.push_back(stat_seconds("loaded dd time", st.load_time));
            stat_items.push_back(stat_seconds("conjoin time", st.conjoin_time));
            stat_items.push_back(stat_seconds("unfounded time", st.unfounded_time));
            stat_items.push_back(stat_seconds("convert time", st.convert_time));
            stat_items.push_back(stat_seconds("optimize time", st.optimize_time));
            stat_items.push_back(stat_seconds("histogram time", st.histogram_time));
            stat_items.push_back(stat_seconds("project time", st.project_time));
            stat_items.push_back(stat_seconds("compile time", t_compile));
            for (size_t i = 0; i < stat_items.size(); ++i) {
                std::cerr << stat_items[i].label << ": " << stat_items[i].value << (stat_items[i].seconds ? " s" : "") << "\n";
            }
        }

        bigint::BigInt count = res.zdd.exact_count();
        JsonWriter json(std::cout);
        if (opt.json) {
            json.open();
            json.field("count");
            std::cout << count.to_string();
            if (!res.optimum.empty()) {  // --opt or a relative cost bound
                json.field("optimum");
                json_numbers(std::cout, res.optimum);
            }
        } else {
            if (!opt.quiet) std::cout << "answer sets: " << count.to_string() << "\n";
            else if (opt.count) std::cout << count.to_string() << "\n";
            if (!opt.quiet && !res.optimum.empty()) {  // --opt or a relative cost bound
                std::cout << "optimum:";
                for (size_t i = 0; i < res.optimum.size(); ++i) std::cout << ' ' << res.optimum[i];
                std::cout << "\n";
            }
        }

        if (opt.brave || opt.cautious || opt.frequencies) {
            std::vector<NameCount> freq = name_frequencies(res);
            if (opt.brave) {
                std::vector<std::string> names = brave_consequences(freq);
                if (opt.json) {
                    json.field("brave");
                    json_strings(std::cout, names);
                } else {
                    print_names_line(names);
                }
            }
            if (opt.cautious) {
                std::vector<std::string> names = cautious_consequences(freq, count);
                if (opt.json) {
                    json.field("cautious");
                    json_strings(std::cout, names);
                } else {
                    print_names_line(names);
                }
            }
            if (opt.frequencies) {
                if (opt.json) {
                    json.field("frequencies");
                    std::cout << "{";
                    for (size_t i = 0; i < freq.size(); ++i) {
                        std::cout << (i ? ", " : "") << json_string(freq[i].name) << ": " << freq[i].count.to_string();
                    }
                    std::cout << "}";
                } else {
                    for (size_t i = 0; i < freq.size(); ++i) std::cout << freq[i].name << ' ' << freq[i].count.to_string() << "\n";
                }
            }
        }

        if (opt.cost_histogram) {
            if (opt.json) {
                json.field("cost_histogram");
                std::cout << "[";
            }
            for (size_t i = 0; i < res.cost_histogram.size(); ++i) {
                const CostCount& entry = res.cost_histogram[i];
                if (opt.json) {
                    std::cout << (i ? ", " : "") << "{\"cost\": ";
                    json_numbers(std::cout, entry.cost);
                    std::cout << ", \"count\": " << entry.count.to_string() << "}";
                } else {
                    for (size_t c = 0; c < entry.cost.size(); ++c) std::cout << (c ? "," : "") << entry.cost[c];
                    std::cout << ' ' << entry.count.to_string() << "\n";
                }
            }
            if (opt.json) std::cout << "]";
        }

        if (opt.json && opt.stats) {
            json.field("stats");
            std::cout << "{";
            for (size_t i = 0; i < stat_items.size(); ++i) {
                std::cout << (i ? ", " : "") << json_string(stat_items[i].key()) << ": " << stat_items[i].value;
            }
            std::cout << "}";
        }

        if (opt.enumerate) {
            // Streamed from the ZDD: --limit stops early without expanding the family.
            size_t printed = 0;
            size_t limit = opt.enumerate_limit;
            auto emit_set = [&](const std::vector<long long>* cost, const std::vector<kyotodd::bddvar>& set) -> bool {
                std::vector<Atom> atoms;  // top level first = variable order
                for (size_t j = 0; j < set.size(); ++j) atoms.push_back(res.var_to_atom[set[j]]);
                std::vector<std::string> shown = res.shown_names(atoms);
                if (opt.json) {
                    std::cout << (printed ? ",\n    " : "\n    ");
                    if (cost) {
                        std::cout << "{\"cost\": ";
                        json_numbers(std::cout, *cost);
                        std::cout << ", \"atoms\": ";
                        json_strings(std::cout, shown);
                        std::cout << "}";
                    } else {
                        json_strings(std::cout, shown);
                    }
                } else {
                    std::string prefix;  // `K[,K..]:` with --by-cost
                    if (cost) {
                        for (size_t c = 0; c < cost->size(); ++c) prefix += (c ? "," : "") + std::to_string((*cost)[c]);
                        prefix += ":";
                    }
                    std::cout << prefix;
                    for (size_t j = 0; j < shown.size(); ++j) std::cout << (j || !prefix.empty() ? " " : "") << shown[j];
                    std::cout << "\n";
                }
                ++printed;
                return limit == 0 || printed < limit;
            };
            if (opt.json) {
                json.field("answer_sets");
                std::cout << "[";
            }
            if (opt.by_cost) {  // best first
                enumerate_by_cost(res, [&](const std::vector<long long>& cost, const std::vector<kyotodd::bddvar>& set) -> bool {
                    return emit_set(&cost, set);
                });
            } else {
                for_each_set(res.zdd, [&](const std::vector<kyotodd::bddvar>& set) -> bool { return emit_set(0, set); });
            }
            if (opt.json) std::cout << (printed ? "\n  ]" : "]");
        }
        if (opt.json) json.close();

        if (!opt.output.empty()) {
            std::ofstream out(opt.output.c_str(), std::ios::binary);
            if (!out) throw std::runtime_error("cannot open output file: " + opt.output);
            if (opt.export_bdd) res.bdd.export_binary(out);
            else res.zdd.export_binary(out);
        }

        if (!opt.atoms_file.empty()) {
            std::ofstream out(opt.atoms_file.c_str());
            if (!out) throw std::runtime_error("cannot open atoms file: " + opt.atoms_file);
            out << "# var level atom name\n";
            for (size_t i = 0; i < res.vars.size(); ++i) {
                kyotodd::bddvar v = res.vars[i];
                Atom a = res.var_to_atom[v];
                out << v << ' ' << kyotodd::bddlevofvar(v) << ' ' << a << ' ' << res.atom_name(a) << "\n";
            }
            // Cost functions by level (the table above maps levels to atoms). Costs
            // refer to the unprojected answer sets, so a weight on a hidden atom
            // cannot be expressed over the exported DD; such lines are marked.
            for (size_t c = 0; c < res.costs.size(); ++c) {
                const CostFunction& cf = res.costs[c];
                std::vector<bool> is_shown(cf.var_weights.size(), false);
                for (size_t i = 0; i < res.vars.size(); ++i) is_shown[res.vars[i]] = true;
                size_t hidden = 0;
                for (size_t v = 1; v < cf.var_weights.size(); ++v) {
                    if (cf.var_weights[v] != 0 && res.var_to_atom[v] != 0 && !is_shown[v]) ++hidden;
                }
                out << "# minimize priority " << cf.priority << " offset " << cf.offset;
                for (size_t i = 0; i < res.vars.size(); ++i) {
                    kyotodd::bddvar v = res.vars[i];
                    if (cf.var_weights[v] != 0) out << ' ' << kyotodd::bddlevofvar(v) << ':' << cf.var_weights[v];
                }
                if (hidden != 0) {
                    out << " # incomplete: " << hidden << " hidden atom(s) carry weight (use --all-atoms)";
                }
                out << "\n";
            }
        }

        if (!opt.save_file.empty()) {
            // The file is opened only once the image is complete, so a result
            // that cannot be stored leaves an existing file untouched.
            std::vector<std::string> save_warnings;
            std::ostringstream image;
            write_stored_dd(image, res, &save_warnings);
            for (size_t i = 0; i < save_warnings.size(); ++i) std::cerr << "ddasp: warning: " << save_warnings[i] << "\n";
            std::ofstream out(opt.save_file.c_str(), std::ios::binary);
            if (!out) throw std::runtime_error("cannot open stored DD file for writing: " + opt.save_file);
            out << image.str();
            if (!out) throw std::runtime_error("cannot write stored DD file: " + opt.save_file);
        }
        return 0;
    } catch (const std::overflow_error& e) {
        if (opt.node_max != 0 && std::string(e.what()).find("node table exhausted") != std::string::npos) {
            std::cerr << "ddasp: error: the node limit of " << opt.node_max
                      << " nodes (--node-max) was reached while building the diagrams\n";
        } else {
            std::cerr << "ddasp: error: " << e.what() << "\n";
        }
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "ddasp: error: " << e.what() << "\n";
        return 1;
    }
}
