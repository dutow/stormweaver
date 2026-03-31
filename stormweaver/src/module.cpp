#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/function.h>
#include <nanobind/stl/shared_ptr.h>

#include "sql_variant/generic.hpp"
#include "sql_variant/postgresql.hpp"
#include "metadata.hpp"
#include "action/action_registry.hpp"
#include "workload.hpp"
#include "statistics.hpp"

namespace nb = nanobind;
using namespace sql_variant;

static std::unique_ptr<LoggedSQL> connect_pg(
    std::string host, uint16_t port, std::string dbname,
    std::string user, std::string password) {
  ServerParams params{dbname, host, "", user, password, port};
  auto sql = std::make_unique<sql_variant::PostgreSQL>(params);
  return std::make_unique<LoggedSQL>(std::move(sql), "python");
}

NB_MODULE(_stormweaver, m) {
    m.attr("__version__") = "0.1.0";

    // --- SQL Layer ---

    nb::class_<QueryResult>(m, "QueryResult")
        .def("success", &QueryResult::success)
        .def_ro("query", &QueryResult::query)
        .def_ro("affected_rows", &QueryResult::affectedRows)
        .def_prop_ro("error_code", [](const QueryResult &r) {
            return r.errorInfo.errorCode;
        })
        .def_prop_ro("error_message", [](const QueryResult &r) {
            return r.errorInfo.errorMessage;
        });

    nb::class_<LoggedSQL>(m, "LoggedSQL")
        .def("execute", &LoggedSQL::executeQuery)
        .def("reconnect", &LoggedSQL::reconnect);

    m.def("connect_pg", &connect_pg,
        nb::arg("host") = "localhost",
        nb::arg("port") = 5432,
        nb::arg("dbname") = "postgres",
        nb::arg("user") = "postgres",
        nb::arg("password") = "");

    // --- Metadata ---

    nb::class_<metadata::Metadata>(m, "Metadata")
        .def(nb::init<>())
        .def("size", &metadata::Metadata::size)
        .def("reset", &metadata::Metadata::reset);

    // --- Action ---

    nb::class_<action::ActionFactory>(m, "ActionFactory")
        .def_ro("name", &action::ActionFactory::name)
        .def_rw("weight", &action::ActionFactory::weight);

    nb::class_<action::ActionRegistry>(m, "ActionRegistry")
        .def(nb::init<>())
        .def("insert", &action::ActionRegistry::insert)
        .def("remove", &action::ActionRegistry::remove)
        .def("has", &action::ActionRegistry::has)
        .def("size", &action::ActionRegistry::size)
        .def("total_weight", &action::ActionRegistry::totalWeight)
        .def("get", &action::ActionRegistry::getReference, nb::rv_policy::reference)
        .def("make_custom_sql", &action::ActionRegistry::makeCustomSqlAction)
        .def("make_custom_table_sql", &action::ActionRegistry::makeCustomTableSqlAction)
        .def("use", &action::ActionRegistry::use);

    m.def("default_action_registry", &action::default_registy,
        nb::rv_policy::reference);

    // --- Configs ---

    nb::class_<action::DdlConfig>(m, "DdlConfig")
        .def(nb::init<>())
        .def_rw("min_table_count", &action::DdlConfig::min_table_count)
        .def_rw("max_table_count", &action::DdlConfig::max_table_count)
        .def_rw("max_column_count", &action::DdlConfig::max_column_count)
        .def_rw("access_methods", &action::DdlConfig::access_methods);

    nb::class_<action::DmlConfig>(m, "DmlConfig")
        .def(nb::init<>())
        .def_rw("delete_min", &action::DmlConfig::deleteMin)
        .def_rw("delete_max", &action::DmlConfig::deleteMax);

    nb::class_<action::AllConfig>(m, "AllConfig")
        .def(nb::init<>())
        .def_rw("ddl", &action::AllConfig::ddl)
        .def_rw("dml", &action::AllConfig::dml);

    nb::class_<WorkloadParams>(m, "WorkloadParams")
        .def(nb::init<>())
        .def_rw("action_config", &WorkloadParams::actionConfig)
        .def_rw("duration_in_seconds", &WorkloadParams::duration_in_seconds)
        .def_rw("repeat_times", &WorkloadParams::repeat_times)
        .def_rw("number_of_workers", &WorkloadParams::number_of_workers)
        .def_rw("max_reconnect_attempts", &WorkloadParams::max_reconnect_attempts);
}
