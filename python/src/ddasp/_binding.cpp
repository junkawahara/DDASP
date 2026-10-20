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

#include <sstream>
#include <stdexcept>
#include <utility>

#include "bdd.h"
#include "ddasp/aspif_reader.h"
#include "ddasp/compiler.h"
#include "ddasp/program.h"

namespace py = pybind11;
using namespace ddasp;

namespace {

bool g_initialized = false;

/** (Re)initialize KyotoDD so that every compilation starts from level 1. */
void reset_kyotodd() {
    if (g_initialized) {
        try {
            kyotodd::bddfinal();
        } catch (const std::exception&) {
            return;  // live objects remain; keep the current instance
        }
    }
    if (kyotodd::bddinit(1 << 16) != 0) throw std::runtime_error("KyotoDD initialization failed");
    g_initialized = true;
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
    std::vector<py::dict> costs;
    std::vector<long long> optimum;
    std::string count;
    py::dict stats;
};

Result compile_py(const Program& program, const std::string& method, const std::string& order_heuristic,
                  const std::vector<int>& order, bool project, bool optimize) {
    CompileOptions opts;
    opts.method = parse_method(method);
    opts.order_heuristic = parse_order_heuristic(order_heuristic);
    opts.order = order;
    opts.project_to_shown = project;
    opts.optimize = optimize;

    reset_kyotodd();
    Result out;
    {
        CompileResult res = compile(program, opts);
        std::ostringstream zs, bs;
        res.zdd.export_binary(zs);
        res.bdd.export_binary(bs);
        out.zdd_bytes = py::bytes(zs.str());
        out.bdd_bytes = py::bytes(bs.str());
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
        for (size_t c = 0; c < res.costs.size(); ++c) {
            py::dict d;
            d["priority"] = res.costs[c].priority;
            d["offset"] = res.costs[c].offset;
            py::dict w;
            for (size_t v = 1; v < res.costs[c].var_weights.size(); ++v) {
                if (res.costs[c].var_weights[v] != 0) {
                    w[py::int_(static_cast<int>(kyotodd::bddlevofvar(static_cast<kyotodd::bddvar>(v))))] =
                        res.costs[c].var_weights[v];
                }
            }
            d["weights"] = w;
            out.costs.push_back(d);
        }
        out.optimum = res.optimum;
        out.count = res.zdd.exact_count().to_string();
        const CompileStats& st = res.stats;
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
        out.stats["zdd_nodes"] = res.zdd.raw_size();
        out.stats["bdd_nodes"] = res.bdd.raw_size();
        out.stats["peak_nodes"] = st.peak_nodes;
        out.stats["groups"] = st.groups;
        out.stats["aux_vars"] = st.aux_vars;
    }
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
        .def_readonly("costs", &Result::costs)
        .def_readonly("optimum", &Result::optimum)
        .def_readonly("count", &Result::count)
        .def_readonly("stats", &Result::stats);

    m.def("compile", &compile_py, py::arg("program"), py::arg("method") = "scc", py::arg("order") = "name",
          py::arg("order_atoms") = std::vector<int>(), py::arg("project") = true, py::arg("optimize") = false,
          "Compile a Program into decision diagrams of its answer sets.");
}
