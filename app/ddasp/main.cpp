/**
 * @file main.cpp
 * @brief ddasp command line tool: ground ASP program -> ZDD of answer sets.
 */

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
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
#include "ddasp/dd_util.h"
#include "ddasp/stored_dd.h"

using namespace ddasp;

namespace {

struct Options {
    std::vector<std::string> inputs;
    std::string gringo = "gringo";
    std::string output;       // -o
    std::string atoms_file;   // --atoms
    std::string save_file;    // --save
    std::vector<std::string> load_files;  // --load
    bool count = false;
    bool enumerate = false;
    bool stats = false;
    bool quiet = false;
    bool export_bdd = false;
    bool all_atoms = false;
    bool optimize = false;
    bool external_values = false;
    std::string method = "scc";
    std::string order = "name";
    size_t enumerate_limit = 0;  // 0 = unlimited
};

void usage(std::ostream& os) {
    os << "Usage: ddasp [options] FILE...\n"
          "\n"
          "Compile a ground ASP program into a ZDD representing all answer sets.\n"
          "FILE is an aspif file (*.aspif, as produced by `gringo --output=intermediate`)\n"
          "and is read as it is; any other files are passed to gringo first (see\n"
          "--gringo). The two kinds cannot be mixed.\n"
          "\n"
          "By default the summary lines `answer sets: N` and (with --opt) `optimum: ...`\n"
          "are printed; -q suppresses them.\n"
          "\n"
          "Options:\n"
          "  --count            print the bare number of answer sets (meant for -q)\n"
          "  --enumerate        print every answer set (one per line, shown atoms in\n"
          "                     variable-order, i.e. name order)\n"
          "  --limit N          print at most N answer sets with --enumerate (0 = all)\n"
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
          "  --opt              keep only the optimal answer sets (#minimize)\n"
          "  --external-values  fix every free #external atom to its declared initial\n"
          "                     value (`#external a. [true]`; gringo declares a plain\n"
          "                     `#external a.` as false); ignored by default\n"
          "  --method M         stable-model method: scc (default) or twocopy\n"
          "  --order H          variable order heuristic: name (default), id, or a\n"
          "                     file with one atom name (or __atomN) per line (top of\n"
          "                     the DD first; remaining atoms follow in name order)\n"
          "  --gringo CMD       grounder command (default: gringo); it is run as\n"
          "                     `CMD --output=intermediate FILE...`\n"
          "  --stats            print node counts and timings to stderr\n"
          "  -q, --quiet        do not print the summary lines\n"
          "  -h, --help         show this help\n";
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
        } else if (a == "--opt") {
            opt.optimize = true;
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
        } else if (a == "--gringo") {
            opt.gringo = need_value("--gringo");
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
    if (opt.enumerate_limit != 0 && !opt.enumerate) {
        std::cerr << "ddasp: warning: --limit has no effect without --enumerate\n";
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
        double t_load = seconds_since(t0);

        kyotodd::bddinit(1 << 16);  // throws on failure
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
        copts.optimize = opt.optimize;
        copts.external_values = opt.external_values;
        copts.method = parse_method(opt.method);
        if (opt.order == "name" || opt.order == "id") {
            copts.order_heuristic = parse_order_heuristic(opt.order);
        } else {
            copts.order = read_order_file(opt.order, program);
        }
        CompileResult res = compile(program, copts, loaded);
        double t_compile = seconds_since(t1);

        if (opt.stats) {
            const CompileStats& st = res.stats;
            std::cerr << "rules: " << program.rules().size() << "\n"
                      << "atoms: " << st.atoms << "\n"
                      << "shown atoms: " << res.vars.size() << "\n"
                      << "aux vars: " << st.aux_vars << "\n"
                      << "groups: " << st.groups << "\n"
                      << "models nodes: " << st.models_nodes << "\n"
                      << "stable nodes: " << st.stable_nodes << "\n"
                      << "zdd nodes: " << res.zdd.raw_size() << "\n"
                      << "bdd nodes: " << res.bdd.raw_size() << "\n"
                      << "peak table nodes: " << st.peak_nodes << "\n";
            if (!loaded.empty()) {
                std::cerr << "loaded dds: " << st.loaded_dds << "\n"
                          << "synthesized atoms: " << st.synthesized_atoms << "\n";
            }
            std::cerr << "load time: " << t_load << " s\n"
                      << "order time: " << st.order_time << " s\n"
                      << "models time: " << st.models_time << " s\n"
                      << "supported time: " << st.supported_time << " s\n"
                      << "loaded dd time: " << st.load_time << " s\n"
                      << "conjoin time: " << st.conjoin_time << " s\n"
                      << "unfounded time: " << st.unfounded_time << " s\n"
                      << "convert time: " << st.convert_time << " s\n"
                      << "optimize time: " << st.optimize_time << " s\n"
                      << "project time: " << st.project_time << " s\n"
                      << "compile time: " << t_compile << " s\n";
        }

        bigint::BigInt count = res.zdd.exact_count();
        if (!opt.quiet) std::cout << "answer sets: " << count.to_string() << "\n";
        else if (opt.count) std::cout << count.to_string() << "\n";
        if (opt.optimize && !opt.quiet && !res.optimum.empty()) {
            std::cout << "optimum:";
            for (size_t i = 0; i < res.optimum.size(); ++i) std::cout << ' ' << res.optimum[i];
            std::cout << "\n";
        }

        if (opt.enumerate) {
            // Streamed from the ZDD: --limit stops early without expanding the family.
            size_t printed = 0;
            size_t limit = opt.enumerate_limit;
            for_each_set(res.zdd, [&](const std::vector<kyotodd::bddvar>& set) -> bool {
                std::vector<Atom> atoms;  // top level first = variable order
                for (size_t j = 0; j < set.size(); ++j) atoms.push_back(res.var_to_atom[set[j]]);
                std::vector<std::string> shown = res.shown_names(atoms);
                for (size_t j = 0; j < shown.size(); ++j) std::cout << (j ? " " : "") << shown[j];
                std::cout << "\n";
                ++printed;
                return limit == 0 || printed < limit;
            });
        }

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
    } catch (const std::exception& e) {
        std::cerr << "ddasp: error: " << e.what() << "\n";
        return 1;
    }
}
