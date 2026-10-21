/**
 * @file _binding.cpp
 * @brief pybind11 bindings of the DDASP core (ddasp._core).
 *
 * The extension embeds its own KyotoDD instance. Decision diagrams leave the
 * module as KyotoDD binary export bytes, which the kyotodd Python package can
 * import; the levels listed in the result identify the atoms.
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "bdd.h"
#include "ddasp/aspif_reader.h"
#include "ddasp/compiler.h"
#include "ddasp/program.h"
#include "ddasp/stored_dd.h"

namespace py = pybind11;
using namespace ddasp;

namespace {

bool g_initialized = false;
std::mutex g_compile_mutex;  // KyotoDD is a process-wide singleton

/**
 * (Re)initialize KyotoDD so that every compilation starts from level 1.
 * Returns false when the old instance had to be kept (levels then continue
 * from the previous compilation); the caller warns.
 */
bool reset_kyotodd() {
    bool fresh = true;
    if (g_initialized) {
        try {
            kyotodd::bddfinal();
        } catch (const std::exception&) {
            fresh = false;  // live objects remain; keep the current instance
        }
    }
    if (fresh) {
        kyotodd::bddinit(1 << 16);  // throws on failure
        g_initialized = true;
    }
    return fresh;
}

/** Plain C++ view of a compilation, built without the GIL. */
struct PlainResult {
    std::string zdd_bytes;
    std::string bdd_bytes;
    std::vector<int> levels;
    std::map<int, int> level_to_atom;
    std::map<int, int> atom_to_level;
    std::map<int, std::string> names;
    std::vector<std::string> fact_names;
    std::vector<std::pair<std::string, std::vector<int> > > terms;
    struct Cost {
        int priority;
        long long offset;
        std::map<int, long long> level_weights;
    };
    std::vector<Cost> costs;
    std::vector<long long> optimum;
    std::string count;
    CompileStats stats;
    uint64_t zdd_nodes;
    uint64_t bdd_nodes;
    bool fresh_instance;
    bool stored_ok;                           // stored_bytes is valid
    std::string stored_bytes;                 // the result as a stored DD file image
    std::string stored_error;                 // why it cannot be stored (when !stored_ok)
    std::vector<std::string> stored_warnings;
};

PlainResult compile_plain(const Program& program, const CompileOptions& opts, const std::vector<StoredDDData>& stored) {
    std::lock_guard<std::mutex> lock(g_compile_mutex);
    PlainResult out;
    out.fresh_instance = reset_kyotodd();
    std::vector<LoadedDD> loaded;
    for (size_t i = 0; i < stored.size(); ++i) loaded.push_back(import_stored_dd(stored[i]));
    CompileResult res = compile(program, opts, loaded);
    std::ostringstream zs, bs;
    res.zdd.export_binary(zs);
    res.bdd.export_binary(bs);
    out.zdd_bytes = zs.str();
    out.bdd_bytes = bs.str();
    try {
        std::ostringstream ss;
        write_stored_dd(ss, res, &out.stored_warnings);
        out.stored_bytes = ss.str();
        out.stored_ok = true;
    } catch (const std::invalid_argument& e) {
        out.stored_ok = false;
        out.stored_error = e.what();
    }
    for (size_t i = 0; i < res.vars.size(); ++i) {
        int lev = static_cast<int>(kyotodd::bddlevofvar(res.vars[i]));
        out.levels.push_back(lev);
        out.level_to_atom[lev] = res.var_to_atom[res.vars[i]];
    }
    for (size_t a = 1; a < res.atom_to_var.size(); ++a) {
        if (res.atom_to_var[a] != 0) {
            out.atom_to_level[static_cast<int>(a)] = static_cast<int>(kyotodd::bddlevofvar(res.atom_to_var[a]));
        }
    }
    out.names = res.names;
    out.fact_names = res.fact_names;
    for (size_t t = 0; t < res.terms.size(); ++t) {
        out.terms.push_back(std::make_pair(res.terms[t].name, res.terms[t].condition));
    }
    for (size_t c = 0; c < res.costs.size(); ++c) {
        PlainResult::Cost cost;
        cost.priority = res.costs[c].priority;
        cost.offset = res.costs[c].offset;
        for (size_t v = 1; v < res.costs[c].var_weights.size(); ++v) {
            if (res.costs[c].var_weights[v] != 0) {
                int lev = static_cast<int>(kyotodd::bddlevofvar(static_cast<kyotodd::bddvar>(v)));
                cost.level_weights[lev] = res.costs[c].var_weights[v];
            }
        }
        out.costs.push_back(cost);
    }
    out.optimum = res.optimum;
    out.count = res.zdd.exact_count().to_string();
    out.stats = res.stats;
    out.zdd_nodes = res.zdd.raw_size();
    out.bdd_nodes = res.bdd.raw_size();
    return out;
}

std::vector<WeightedLiteral> to_weighted(const std::vector<std::pair<int, long long> >& lits) {
    std::vector<WeightedLiteral> wl;
    wl.reserve(lits.size());
    for (size_t i = 0; i < lits.size(); ++i) wl.push_back(WeightedLiteral(lits[i].first, lits[i].second));
    return wl;
}

/** Plain-data view of a CompileResult that survives the KyotoDD reset. */
struct Result {
    py::bytes zdd_bytes;
    py::bytes bdd_bytes;
    std::vector<int> levels;                 // shown atoms' levels, top of the DD first
    std::map<int, int> level_to_atom;
    std::map<int, int> atom_to_level;        // every atom of the program
    std::map<int, std::string> names;
    std::vector<std::string> fact_names;
    std::vector<std::pair<std::string, std::vector<int> > > terms;  // (name, condition literals)
    std::vector<py::dict> costs;
    std::vector<long long> optimum;
    std::string count;
    py::dict stats;
    py::object stored_bytes;  // bytes, or None when the result cannot be stored
    std::string stored_error;
    std::vector<std::string> stored_warnings;
};

/** Parse a stored DD file image; malformed input becomes a ValueError. */
StoredDDData parse_stored(const std::string& bytes) {
    try {
        std::istringstream is(bytes);
        return read_stored_dd(is);
    } catch (const std::exception& e) {
        throw py::value_error(e.what());
    }
}

Result compile_py(const Program& program, const std::string& method, const std::string& order_heuristic,
                  const std::vector<int>& order, bool project, bool optimize, const std::vector<py::bytes>& loaded) {
    CompileOptions opts;
    opts.method = parse_method(method);
    opts.order_heuristic = parse_order_heuristic(order_heuristic);
    opts.order = order;
    opts.project_to_shown = project;
    opts.optimize = optimize;
    std::vector<StoredDDData> stored;
    for (size_t i = 0; i < loaded.size(); ++i) stored.push_back(parse_stored(std::string(loaded[i])));

    PlainResult plain;
    {
        // The compilation only touches C++ data (the Program must not be modified
        // meanwhile); release the GIL so that other Python threads can run.
        py::gil_scoped_release release;
        plain = compile_plain(program, opts, stored);
    }
    if (!plain.fresh_instance) {
        PyErr_WarnEx(PyExc_RuntimeWarning,
                     "ddasp: KyotoDD could not be reset (live objects remain); levels continue from the "
                     "previous compilation", 1);
    }

    Result out;
    out.zdd_bytes = py::bytes(plain.zdd_bytes);
    out.bdd_bytes = py::bytes(plain.bdd_bytes);
    out.levels = plain.levels;
    out.level_to_atom = plain.level_to_atom;
    out.atom_to_level = plain.atom_to_level;
    out.names = plain.names;
    out.fact_names = plain.fact_names;
    out.terms = plain.terms;
    for (size_t c = 0; c < plain.costs.size(); ++c) {
        py::dict d;
        d["priority"] = plain.costs[c].priority;
        d["offset"] = plain.costs[c].offset;
        py::dict w;
        for (std::map<int, long long>::const_iterator it = plain.costs[c].level_weights.begin();
             it != plain.costs[c].level_weights.end(); ++it) {
            w[py::int_(it->first)] = it->second;
        }
        d["weights"] = w;
        out.costs.push_back(d);
    }
    out.optimum = plain.optimum;
    out.count = plain.count;
    const CompileStats& st = plain.stats;
    out.stats["order_time"] = st.order_time;
    out.stats["models_time"] = st.models_time;
    out.stats["supported_time"] = st.supported_time;
    out.stats["conjoin_time"] = st.conjoin_time;
    out.stats["unfounded_time"] = st.unfounded_time;
    out.stats["convert_time"] = st.convert_time;
    out.stats["optimize_time"] = st.optimize_time;
    out.stats["project_time"] = st.project_time;
    out.stats["models_nodes"] = st.models_nodes;
    out.stats["stable_nodes"] = st.stable_nodes;
    out.stats["zdd_nodes"] = plain.zdd_nodes;
    out.stats["bdd_nodes"] = plain.bdd_nodes;
    out.stats["peak_nodes"] = st.peak_nodes;
    out.stats["atoms"] = st.atoms;
    out.stats["groups"] = st.groups;
    out.stats["aux_vars"] = st.aux_vars;
    out.stats["load_time"] = st.load_time;
    out.stats["loaded_dds"] = st.loaded_dds;
    out.stats["synthesized_atoms"] = st.synthesized_atoms;
    out.stored_bytes = plain.stored_ok ? py::object(py::bytes(plain.stored_bytes)) : py::object(py::none());
    out.stored_error = plain.stored_error;
    out.stored_warnings = plain.stored_warnings;
    return out;
}

}  // namespace

PYBIND11_MODULE(_core, m) {
    m.doc() = "DDASP core: ground ASP programs -> decision diagrams of answer sets";

    py::class_<Program>(m, "Program", "A ground ASP program (aspif-like representation).")
        .def(py::init<>())
        .def("add_rule",
             [](Program& p, bool choice, const std::vector<int>& head, const std::vector<int>& body) {
                 p.add_rule(Rule(choice ? HeadType::Choice : HeadType::Disjunction, head, Body::normal(body)));
             },
             py::arg("choice"), py::arg("head"), py::arg("body"),
             "Add a rule with a normal body (list of signed literals).")
        .def("add_weight_rule",
             [](Program& p, bool choice, const std::vector<int>& head, long long lower_bound,
                const std::vector<std::pair<int, long long> >& body) {
                 p.add_rule(Rule(choice ? HeadType::Choice : HeadType::Disjunction, head,
                                 Body::weight(lower_bound, to_weighted(body))));
             },
             py::arg("choice"), py::arg("head"), py::arg("lower_bound"), py::arg("body"),
             "Add a rule whose body is sum(weight * [literal]) >= lower_bound.")
        .def("add_minimize",
             [](Program& p, int priority, const std::vector<std::pair<int, long long> >& lits) {
                 p.add_minimize(Minimize(priority, to_weighted(lits)));
             },
             py::arg("priority"), py::arg("literals"), "Add a #minimize statement.")
        .def("add_output",
             [](Program& p, const std::string& name, const std::vector<int>& condition) {
                 p.add_output(Output(name, condition));
             },
             py::arg("name"), py::arg("condition"),
             "Add a #show output: name is shown when all condition literals hold (empty = always).")
        .def("add_external",
             [](Program& p, int atom, int value) {
                 if (value < 0 || value > 3) throw std::invalid_argument("external value must be 0..3");
                 p.add_external(External(atom, static_cast<ExternalValue>(value)));
             },
             py::arg("atom"), py::arg("value") = 0,
             "Add an #external atom (value: 0 free, 1 true, 2 false, 3 release; all are treated as free).")
        .def("add_projection", &Program::add_projection, py::arg("atoms"), "Record a #project statement.")
        .def_property_readonly("num_rules", [](const Program& p) { return p.rules().size(); })
        .def_property_readonly("max_atom", &Program::max_atom)
        .def("__str__", &Program::to_string);

    m.def("read_aspif",
          [](const std::string& text) {
              std::vector<std::string> warnings;
              Program p = read_aspif_string(text, &warnings);
              return py::make_tuple(p, warnings);
          },
          py::arg("text"), "Parse aspif text; returns (Program, warnings).");

    py::class_<Result>(m, "CoreResult", "Compilation result (plain data; DDs as KyotoDD binary bytes).")
        .def_readonly("zdd_bytes", &Result::zdd_bytes)
        .def_readonly("bdd_bytes", &Result::bdd_bytes)
        .def_readonly("levels", &Result::levels)
        .def_readonly("level_to_atom", &Result::level_to_atom)
        .def_readonly("atom_to_level", &Result::atom_to_level)
        .def_readonly("names", &Result::names)
        .def_readonly("fact_names", &Result::fact_names)
        .def_readonly("terms", &Result::terms)
        .def_readonly("costs", &Result::costs)
        .def_readonly("optimum", &Result::optimum)
        .def_readonly("count", &Result::count)
        .def_readonly("stats", &Result::stats)
        .def_readonly("stored_bytes", &Result::stored_bytes)
        .def_readonly("stored_error", &Result::stored_error)
        .def_readonly("stored_warnings", &Result::stored_warnings);

    m.def("compile", &compile_py, py::arg("program"), py::arg("method") = "scc", py::arg("order") = "name",
          py::arg("order_atoms") = std::vector<int>(), py::arg("project") = true, py::arg("optimize") = false,
          py::arg("loaded") = std::vector<py::bytes>(),
          "Compile a Program into decision diagrams of its answer sets; `loaded` holds stored DD file images.");

    m.def("stored_dd_parts",
          [](const py::bytes& data) {
              StoredDDData dd = parse_stored(std::string(data));
              return py::make_tuple(dd.names, dd.fact_names, py::bytes(dd.payload), dd.terminal);
          },
          py::arg("data"),
          "Parse a stored DD file image: (names top first, fact names, KyotoDD ZDD bytes, terminal: 0 payload / 1 empty / 2 {∅}).");
    m.def("stored_dd_image",
          [](const std::vector<std::string>& names, const std::vector<std::string>& fact_names,
             const py::bytes& payload, int terminal) {
              StoredDDData dd;
              dd.names = names;
              dd.fact_names = fact_names;
              dd.payload = std::string(payload);
              dd.terminal = terminal;
              try {
                  std::ostringstream os;
                  write_stored_dd_data(os, dd);
                  return py::bytes(os.str());
              } catch (const std::invalid_argument& e) {
                  throw py::value_error(e.what());
              }
          },
          py::arg("names"), py::arg("fact_names"), py::arg("payload"), py::arg("terminal"),
          "Build a stored DD file image from its parts (names top first, fact names, KyotoDD ZDD bytes "
          "over levels 1..N, terminal: 0 payload / 1 empty / 2 {∅}).");
    m.def("stored_dd_declarations",
          [](const py::bytes& data) { return stored_dd_declarations(parse_stored(std::string(data))); },
          py::arg("data"), "Program text declaring the atoms (#external, #show) and facts of a stored DD.");
}
