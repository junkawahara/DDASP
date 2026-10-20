/**
 * @file main.cpp
 * @brief ddasp command line tool: ground ASP program -> ZDD of answer sets.
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "bdd.h"
#include "ddasp/aspif_reader.h"
#include "ddasp/compiler.h"

using namespace ddasp;

namespace {

struct Options {
    std::vector<std::string> inputs;
    std::string gringo = "gringo";
    std::string output;       // -o
    std::string atoms_file;   // --atoms
    bool count = false;
    bool enumerate = false;
    bool stats = false;
    bool quiet = false;
    bool export_bdd = false;
    bool all_atoms = false;
    bool optimize = false;
    size_t enumerate_limit = 0;  // 0 = unlimited
};

void usage(std::ostream& os) {
    os << "Usage: ddasp [options] FILE...\n"
          "\n"
          "Compile a ground ASP program into a ZDD representing all answer sets.\n"
          "FILE is an aspif file (*.aspif, as produced by `gringo --output=intermediate`).\n"
          "Any other file is passed to gringo first (see --gringo).\n"
          "\n"
          "Options:\n"
          "  --count            print the number of answer sets\n"
          "  --enumerate        print every answer set (one per line, shown atoms)\n"
          "  --limit N          print at most N answer sets with --enumerate\n"
          "  -o FILE            write the ZDD to FILE (KyotoDD binary format)\n"
          "  --bdd              write a BDD instead of a ZDD with -o\n"
          "  --atoms FILE       write the variable/level/atom/name table to FILE\n"
          "  --all-atoms        do not project onto the shown atoms (every atom\n"
          "                     becomes a DD variable)\n"
          "  --opt              keep only the optimal answer sets (#minimize)\n"
          "  --gringo CMD       grounder command (default: gringo); it is run as\n"
          "                     `CMD --output=intermediate FILE...`\n"
          "  --stats            print node counts and timings to stderr\n"
          "  -q, --quiet        do not print the summary line\n"
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
            opt.enumerate_limit = static_cast<size_t>(std::strtoull(need_value("--limit").c_str(), 0, 10));
        } else if (a == "-o") {
            opt.output = need_value("-o");
        } else if (a == "--bdd") {
            opt.export_bdd = true;
        } else if (a == "--all-atoms") {
            opt.all_atoms = true;
        } else if (a == "--opt") {
            opt.optimize = true;
        } else if (a == "--atoms") {
            opt.atoms_file = need_value("--atoms");
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

/** Run the grounder on the input files and return the aspif text. */
std::string run_gringo(const Options& opt) {
    std::string cmd = opt.gringo + " --output=intermediate";
    for (size_t i = 0; i < opt.inputs.size(); ++i) cmd += " " + shell_quote(opt.inputs[i]);
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) throw std::runtime_error("cannot run grounder: " + cmd);
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) text.append(buf, n);
    int status = pclose(pipe);
    if (status != 0) throw std::runtime_error("grounder failed (exit status " + std::to_string(status) + "): " + cmd);
    return text;
}

Program load_program(const Options& opt, std::vector<std::string>& warnings) {
    bool all_aspif = true;
    for (size_t i = 0; i < opt.inputs.size(); ++i) {
        if (!ends_with(opt.inputs[i], ".aspif")) all_aspif = false;
    }
    if (all_aspif) {
        if (opt.inputs.size() != 1) throw std::runtime_error("only one aspif file can be given");
        return read_aspif_file(opt.inputs[0], &warnings);
    }
    std::string text = run_gringo(opt);
    return read_aspif_string(text, &warnings);
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
        std::vector<std::string> warnings;
        Program program = load_program(opt, warnings);
        for (size_t i = 0; i < warnings.size(); ++i) std::cerr << "ddasp: warning: " << warnings[i] << "\n";
        double t_load = seconds_since(t0);

        if (kyotodd::bddinit(1 << 16) != 0) {
            std::cerr << "ddasp: KyotoDD initialization failed\n";
            return 1;
        }

        Clock::time_point t1 = Clock::now();
        CompileOptions copts;
        copts.project_to_shown = !opt.all_atoms;
        copts.optimize = opt.optimize;
        CompileResult res = compile(program, copts);
        double t_compile = seconds_since(t1);

        if (opt.stats) {
            std::cerr << "rules: " << program.rules().size() << "\n"
                      << "atoms: " << res.vars.size() << "\n"
                      << "zdd nodes: " << res.zdd.raw_size() << "\n"
                      << "bdd nodes: " << res.bdd.raw_size() << "\n"
                      << "load time: " << t_load << " s\n"
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
            std::vector<std::vector<kyotodd::bddvar> > sets = res.zdd.enumerate();
            size_t limit = opt.enumerate_limit ? std::min(opt.enumerate_limit, sets.size()) : sets.size();
            std::string fact_prefix;
            for (size_t j = 0; j < res.fact_names.size(); ++j) {
                if (!fact_prefix.empty()) fact_prefix += " ";
                fact_prefix += res.fact_names[j];
            }
            for (size_t i = 0; i < limit; ++i) {
                std::string line = fact_prefix;
                for (size_t j = 0; j < sets[i].size(); ++j) {
                    Atom a = res.var_to_atom[sets[i][j]];
                    if (!res.names.count(a)) continue;  // only #show'n atoms
                    if (!line.empty()) line += " ";
                    line += res.names[a];
                }
                std::cout << line << "\n";
            }
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
            for (size_t c = 0; c < res.costs.size(); ++c) {
                out << "# minimize priority " << res.costs[c].priority << " offset " << res.costs[c].offset;
                for (size_t i = 0; i < res.vars.size(); ++i) {
                    kyotodd::bddvar v = res.vars[i];
                    if (res.costs[c].var_weights[v] != 0) out << ' ' << v << ':' << res.costs[c].var_weights[v];
                }
                out << "\n";
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ddasp: error: " << e.what() << "\n";
        return 1;
    }
}
