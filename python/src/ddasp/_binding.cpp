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

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "bdd.h"
#include "ddasp/aspif_reader.h"
#include "ddasp/compiler.h"
#include "ddasp/consequences.h"
#include "ddasp/program.h"
#include "ddasp/stored_dd.h"

namespace py = pybind11;
using namespace ddasp;

namespace {

bool g_initialized = false;
std::mutex g_compile_mutex;  // KyotoDD is a process-wide singleton

/** Raised (as ddasp.NodeLimitError) when the node_max limit stops a compilation. */
struct NodeLimitError : std::runtime_error {
    explicit NodeLimitError(const std::string& what) : std::runtime_error(what) {}
};

/**
 * (Re)initialize KyotoDD so that every compilation starts from level 1, with
 * a node table limited to node_max nodes (0 = unlimited).
 * Returns false when the old instance had to be kept (levels then continue
 * from the previous compilation and the limit is not applied); the caller warns.
 */
bool reset_kyotodd(uint64_t node_max) {
    bool fresh = true;
    if (g_initialized) {
        try {
            kyotodd::bddfinal();
        } catch (const std::exception&) {
            fresh = false;  // live objects remain; keep the current instance
        }
    }
    if (fresh) {
        const uint64_t initial = 1 << 16;
        if (node_max == 0) kyotodd::bddinit(initial);  // throws on failure
        else kyotodd::bddinit(std::min(initial, node_max), node_max);
        g_initialized = true;
    }
    return fresh;
}

struct PlainResult;
PlainResult compile_plain_in(const Program& program, const CompileOptions& opts, const std::vector<StoredDDData>& stored,
                             PlainResult& out);

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
    std::vector<std::string> warnings;
    std::vector<std::pair<std::string, std::string> > frequencies;  // (name, decimal count) in display order
    std::vector<std::pair<std::vector<long long>, std::string> > cost_histogram;  // (cost vector, decimal count), ascending
    std::vector<std::pair<std::vector<long long>, std::vector<std::string> > > best;  // (cost vector, shown names), best first
};

PlainResult compile_plain(const Program& program, const CompileOptions& opts, const std::vector<StoredDDData>& stored,
                          uint64_t node_max) {
    std::lock_guard<std::mutex> lock(g_compile_mutex);
    PlainResult out;
    out.fresh_instance = reset_kyotodd(node_max);
    try {
        return compile_plain_in(program, opts, stored, out);
    } catch (const std::overflow_error& e) {
        if (node_max != 0 && std::string(e.what()).find("node table exhausted") != std::string::npos) {
            throw NodeLimitError("the node limit of " + std::to_string(node_max) +
                                 " nodes (node_max) was reached while building the diagrams");
        }
        throw;
    }
}

PlainResult compile_plain_in(const Program& program, const CompileOptions& opts, const std::vector<StoredDDData>& stored,
                             PlainResult& out) {
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
    out.warnings = res.warnings;
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
    std::vector<NameCount> freq = name_frequencies(res);
    for (size_t i = 0; i < freq.size(); ++i) {
        out.frequencies.push_back(std::make_pair(freq[i].name, freq[i].count.to_string()));
    }
    for (size_t i = 0; i < res.cost_histogram.size(); ++i) {
        out.cost_histogram.push_back(std::make_pair(res.cost_histogram[i].cost, res.cost_histogram[i].count.to_string()));
    }
    for (size_t i = 0; i < res.best.size(); ++i) {
        out.best.push_back(std::make_pair(res.best[i].cost, res.shown_names(res.best[i].atoms)));
    }
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
    std::vector<std::pair<std::string, std::string> > frequencies;
    std::vector<std::pair<std::vector<long long>, std::string> > cost_histogram;
    std::vector<std::pair<std::vector<long long>, std::vector<std::string> > > best;
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

/** Cost bound entries of one kind (see CompileOptions::cost_le). */
std::vector<CostBound> cost_bounds(const std::vector<long long>& values, bool relative) {
    std::vector<CostBound> b;
    for (size_t i = 0; i < values.size(); ++i) b.push_back(CostBound(values[i], relative));
    return b;
}

Result compile_py(const Program& program, const std::string& method, const std::string& order_heuristic,
                  const std::vector<int>& order, bool project, bool use_project_statements, bool optimize,
                  bool external_values, const std::vector<py::bytes>& loaded, uint64_t node_max,
                  const std::vector<long long>& cost_le, const std::vector<long long>& cost_ge,
                  const std::vector<long long>& cost_eq, const std::vector<long long>& cost_slack,
                  bool cost_histogram, uint64_t best, const std::string& semantics) {
    CompileOptions opts;
    opts.method = parse_method(method);
    opts.semantics = parse_semantics(semantics);
    opts.order_heuristic = parse_order_heuristic(order_heuristic);
    opts.order = order;
    opts.project_to_shown = project;
    opts.use_project_statements = use_project_statements;
    opts.optimize = optimize;
    opts.external_values = external_values;
    if (!cost_le.empty() && !cost_slack.empty()) throw py::value_error("cost_le and cost_slack cannot be combined");
    opts.cost_le = cost_slack.empty() ? cost_bounds(cost_le, false) : cost_bounds(cost_slack, true);
    opts.cost_ge = cost_bounds(cost_ge, false);
    opts.cost_eq = cost_bounds(cost_eq, false);
    opts.cost_histogram = cost_histogram;
    opts.best = static_cast<size_t>(best);
    std::vector<StoredDDData> stored;
    for (size_t i = 0; i < loaded.size(); ++i) stored.push_back(parse_stored(std::string(loaded[i])));

    PlainResult plain;
    {
        // The compilation only touches C++ data (the Program must not be modified
        // meanwhile); release the GIL so that other Python threads can run.
        py::gil_scoped_release release;
        plain = compile_plain(program, opts, stored, node_max);
    }
    if (!plain.fresh_instance) {
        PyErr_WarnEx(PyExc_RuntimeWarning,
                     "ddasp: KyotoDD could not be reset (live objects remain); levels continue from the "
                     "previous compilation and node_max is not applied", 1);
    }
    for (size_t i = 0; i < plain.warnings.size(); ++i) {
        PyErr_WarnEx(PyExc_RuntimeWarning, ("ddasp: " + plain.warnings[i]).c_str(), 1);
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
    out.frequencies = plain.frequencies;
    out.cost_histogram = plain.cost_histogram;
    out.best = plain.best;
    const CompileStats& st = plain.stats;
    out.stats["order_time"] = st.order_time;
    out.stats["models_time"] = st.models_time;
    out.stats["supported_time"] = st.supported_time;
    out.stats["conjoin_time"] = st.conjoin_time;
    out.stats["unfounded_time"] = st.unfounded_time;
    out.stats["convert_time"] = st.convert_time;
    out.stats["optimize_time"] = st.optimize_time;
    out.stats["histogram_time"] = st.histogram_time;
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
    out.stats["simplified_rules"] = st.simplified_rules;
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
             "Add an #external atom (value: 0 free, 1 true, 2 false, 3 release; used only with external_values).")
        .def("add_projection", &Program::add_projection, py::arg("atoms"),
             "Add a #project statement (used with project=\"project\").")
        .def("add_assumption", &Program::add_assumption, py::arg("literals"),
             "Add an assumption: every answer set must satisfy all of the literals.")
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
        .def_readonly("frequencies", &Result::frequencies)
        .def_readonly("cost_histogram", &Result::cost_histogram)
        .def_readonly("best", &Result::best)
        .def_readonly("stats", &Result::stats)
        .def_readonly("stored_bytes", &Result::stored_bytes)
        .def_readonly("stored_error", &Result::stored_error)
        .def_readonly("stored_warnings", &Result::stored_warnings);

    m.def("compile", &compile_py, py::arg("program"), py::arg("method") = "scc", py::arg("order") = "name",
          py::arg("order_atoms") = std::vector<int>(), py::arg("project") = true,
          py::arg("use_project_statements") = false, py::arg("optimize") = false,
          py::arg("external_values") = false, py::arg("loaded") = std::vector<py::bytes>(),
          py::arg("node_max") = static_cast<uint64_t>(0), py::arg("cost_le") = std::vector<long long>(),
          py::arg("cost_ge") = std::vector<long long>(), py::arg("cost_eq") = std::vector<long long>(),
          py::arg("cost_slack") = std::vector<long long>(), py::arg("cost_histogram") = false,
          py::arg("best") = static_cast<uint64_t>(0), py::arg("semantics") = "stable",
          "Compile a Program into decision diagrams of its answer sets (`semantics`: stable, supported or "
          "classical models); `loaded` holds stored DD file images, "
          "`node_max` limits KyotoDD's node table (0 = unlimited; NodeLimitError when reached); `cost_le`, "
          "`cost_ge` and `cost_eq` bound the #minimize cost vector lexicographically (one entry per priority "
          "level, most important first) and `cost_slack` is an upper bound relative to the optimum, which is "
          "then reported; `cost_histogram` fills the (cost vector, count) list of the result and `best` the list of "
          "its first `best` (cost vector, shown names) in ascending cost order.");
    py::register_exception<NodeLimitError>(m, "NodeLimitError", PyExc_RuntimeError);

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
