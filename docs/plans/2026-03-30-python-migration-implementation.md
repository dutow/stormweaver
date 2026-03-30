# Python Migration Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:executing-plans to implement this plan task-by-task.

**Goal:** Get a minimal stormweaver running with Python scenarios — create/alter/insert/delete with multiple threads against vanilla PostgreSQL.

**Architecture:** Python package (`stormweaver/`) + C++ extension module (`_stormweaver.so` via nanobind) + bundled free-threaded CPython 3.14t from conan. Shell wrapper as entry point.

**Tech Stack:** C++23, nanobind, CPython 3.14t (free-threading), conan, cmake, pytest

**Design doc:** `docs/plans/2026-03-30-python-migration-design.md`

---

### Task 1: Remove Lua from build system

Remove Sol2, Lua modules, and tomlplusplus from the build. Add cpython and nanobind. The project will not compile after this task until Task 2 is completed — that's expected.

**Files:**
- Modify: `conanfile.py`
- Modify: `CMakeLists.txt`
- Modify: `libstormweaver/src/CMakeLists.txt`
- Delete build target: `stormweaver/src/CMakeLists.txt` (remove executable, will be replaced)

**Step 1: Update conanfile.py**

```python
def requirements(self):
    self.requires("boost/1.86.0")
    self.requires("reflect-cpp/0.17.0")
    #self.requires("libmysqlclient/8.1.0")
    self.requires("libpqxx/7.9.2")
    self.requires("nlohmann_json/3.11.3")
    self.requires("spdlog/1.15.1")
    self.requires("catch2/3.7.0")
    self.requires("fmt/11.1.3")
    self.requires("magic_enum/0.9.7")
    self.requires("cryptopp/8.9.0")
    self.requires("cpython/3.14.3")
    self.requires("nanobind/2.12.0")
```

Remove: `sol2/3.5.0`, `tomlplusplus/3.4.0`.
Add: `cpython/3.14.3`, `nanobind/2.12.0`.

Conan options for cpython need `free_threaded=True`. Add to `default_options`:
```python
default_options = {
    "asan": False,
    "ubsan": False,
    "tsan": False,
    "msan": False,
    "cpython/*:free_threaded": True,
    "cpython/*:shared": True,
}
```

**Step 2: Update top-level CMakeLists.txt**

Replace:
```cmake
find_package(Sol2 REQUIRED)
find_package(tomlplusplus REQUIRED)
```
With:
```cmake
find_package(Python3 REQUIRED COMPONENTS Development)
find_package(nanobind REQUIRED)
```

Remove:
```cmake
INCLUDE(LuaModules)
```

Keep the `add_subdirectory(stormweaver)` line — we'll repurpose this directory for the extension module.

**Step 3: Update libstormweaver/src/CMakeLists.txt**

Remove `scripting/luactx.cpp` from `LIBRARY_SOURCES`.

Remove from `TARGET_LINK_LIBRARIES`:
```
sol2::sol2
toml-lua
lfs-lua
```

**Step 4: Update stormweaver/src/CMakeLists.txt**

Replace the executable with the nanobind extension module (placeholder — actual bindings come in Task 3):

```cmake
nanobind_add_module(_stormweaver
    module.cpp
)
TARGET_LINK_LIBRARIES(_stormweaver PRIVATE
    libstormweaver
)
```

Create `stormweaver/src/module.cpp` with a minimal placeholder:
```cpp
#include <nanobind/nanobind.h>

namespace nb = nanobind;

NB_MODULE(_stormweaver, m) {
    m.attr("__version__") = "0.1.0";
}
```

Remove `stormweaver/src/main.cpp`.

**Step 5: Verify conan install works**

Run: `conan install . --build=missing`

This validates the dependency resolution. The cmake build will fail until Task 2 is done.

**Step 6: Commit**

```bash
git add -A
git commit -m "build: replace Lua/Sol2 with cpython+nanobind in build system"
```

---

### Task 2: Decouple C++ from Lua

Remove Lua dependencies from `workload.hpp` and `workload.cpp`. Remove `Node`, `SqlFactory`, `BackgroundThread`, `CommQueue` classes (Python handles their roles). Keep `Worker`, `RandomWorker`, `Workload`.

**Files:**
- Modify: `libstormweaver/include/workload.hpp`
- Modify: `libstormweaver/src/workload.cpp`
- Do NOT delete: `libstormweaver/include/scripting/luactx.hpp` and `libstormweaver/src/scripting/luactx.cpp` — just excluded from build already

**Step 1: Rewrite workload.hpp**

Remove `#include "scripting/luactx.hpp"`. Remove `SqlFactory`, `Node`, `BackgroundThread`-related classes.

Modify `RandomWorker` to remove `LuaContext` parameter:
```cpp
class RandomWorker : public Worker {
public:
  RandomWorker(std::string const &name,
               Worker::sql_connector_t const &sql_connector,
               WorkloadParams const &config, metadata_ptr metadata,
               action::ActionRegistry const &actions);

  RandomWorker(RandomWorker &&) = default;
  ~RandomWorker() override;

  void run_thread(std::size_t duration_in_seconds);
  void join();

  action::ActionRegistry &possibleActions();
  const statistics::WorkerStatistics &statistics() const;

protected:
  action::ActionRegistry actions;
  std::thread thread;
  statistics::WorkerStatistics stats;
};
```

Key changes:
- Removed `std::unique_ptr<LuaContext> luaCtx` parameter and member
- Added `const statistics::WorkerStatistics &statistics() const;` public accessor

Simplify `Workload` to take a `sql_connector_t` instead of `SqlFactory` + `LuaContext`:
```cpp
class Workload {
public:
  Workload(WorkloadParams const &params,
           Worker::sql_connector_t const &sql_connector,
           metadata_ptr metadata, action::ActionRegistry const &actions);

  void run();
  void wait_completion();
  RandomWorker &worker(std::size_t idx);
  std::size_t worker_count() const;
  void reconnect_workers();

private:
  std::size_t duration_in_seconds;
  std::size_t repeat_times;
  std::vector<RandomWorker> workers;
  action::ActionRegistry actions;
};
```

Remove entirely from the header: `SqlFactory`, `Node`.

**Step 2: Update workload.cpp**

Update `RandomWorker` constructor — remove `luaCtx` parameter:
```cpp
RandomWorker::RandomWorker(std::string const &name,
                           Worker::sql_connector_t const &sql_connector,
                           WorkloadParams const &config, metadata_ptr metadata,
                           action::ActionRegistry const &actions)
    : Worker(name, sql_connector, config, metadata), actions(actions) {}
```

Add statistics accessor:
```cpp
const statistics::WorkerStatistics &RandomWorker::statistics() const {
  return stats;
}
```

Update `Workload` constructor — use `sql_connector` directly:
```cpp
Workload::Workload(WorkloadParams const &params,
                   Worker::sql_connector_t const &sql_connector,
                   metadata_ptr metadata, action::ActionRegistry const &actions)
    : duration_in_seconds(params.duration_in_seconds),
      repeat_times(params.repeat_times), actions(actions) {

  if (repeat_times == 0)
    return;

  for (std::size_t idx = 0; idx < params.number_of_workers; ++idx) {
    auto name = fmt::format("Worker {}", idx + 1);
    workers.emplace_back(name, sql_connector, params, metadata, actions);
  }
}
```

Remove `SqlFactory::connect()`, `SqlFactory::SqlFactory()`, `SqlFactory::params()`, `Node::Node()`, `Node::make_worker()`, `Node::init_random_workload()`, `Node::possibleActions()`, `Node::sql_params()`.

**Step 3: Update any files that reference removed types**

Check for includes of `workload.hpp` that use `Node` or `SqlFactory`. The Lua binding file (`luactx.cpp`) used them but is already excluded from the build.

**Step 4: Verify the library compiles**

Run: `cmake --build build`

The `libstormweaver` static library and C++ unit tests should compile. The nanobind module should also compile (it only uses `__version__` so far).

**Step 5: Run existing C++ unit tests**

Run: `ctest --test-dir build -R unit`

The metadata and statistics unit tests should still pass.

**Step 6: Commit**

```bash
git add -A
git commit -m "refactor: decouple workload classes from Lua"
```

---

### Task 3: Bind SQL layer to nanobind

Expose `ServerParams`, `QueryResult`, `LoggedSQL`, and a `connect_pg()` factory function.

**Files:**
- Modify: `stormweaver/src/module.cpp`

**Step 1: Write the bindings**

```cpp
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>

#include "sql_variant/generic.hpp"
#include "sql_variant/postgresql.hpp"

namespace nb = nanobind;

using namespace sql_variant;

static std::unique_ptr<LoggedSQL> connect_pg(
    std::string host, uint16_t port, std::string dbname,
    std::string user, std::string password) {
  ServerParams params{dbname, host, "", user, password, port};
  auto sql = std::make_unique<PostgreSQL>(params);
  return std::make_unique<LoggedSQL>(std::move(sql), "python");
}

NB_MODULE(_stormweaver, m) {
    m.attr("__version__") = "0.1.0";

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
}
```

**Step 2: Update CMakeLists to link libpqxx**

In `stormweaver/src/CMakeLists.txt`, the `_stormweaver` target already links `libstormweaver` which links `libpqxx`. Verify this is sufficient — nanobind modules need shared linkage for Python types, so ensure `libstormweaver` headers are on the include path.

**Step 3: Build and verify import**

```bash
cmake --build build
# Find the built .so and test import:
PYTHONPATH=build/stormweaver/src python3.14t -c "import _stormweaver; print(_stormweaver.__version__)"
```

Expected: `0.1.0`

Note: the exact python executable path depends on conan's cpython install location. Use the conan-provided interpreter.

**Step 4: Write a smoke test**

Create `tests/test_bindings.py`:
```python
import _stormweaver as sw

def test_version():
    assert sw.__version__ == "0.1.0"

def test_connect_pg_exists():
    assert callable(sw.connect_pg)
```

These tests don't need a running PostgreSQL. Connection tests come in Task 10.

**Step 5: Commit**

```bash
git add -A
git commit -m "feat: add nanobind bindings for SQL layer"
```

---

### Task 4: Bind Metadata and ActionRegistry

Expose the metadata system and action registry so Python can configure workloads.

**Files:**
- Modify: `stormweaver/src/module.cpp`

**Step 1: Add metadata bindings**

```cpp
#include "metadata.hpp"
#include "action/action_registry.hpp"

// In NB_MODULE:

nb::class_<metadata::Metadata>(m, "Metadata")
    .def(nb::init<>())
    .def("size", &metadata::Metadata::size)
    .def("reset", &metadata::Metadata::reset);

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
```

**Step 2: Add smoke test**

Add to `tests/test_bindings.py`:
```python
def test_metadata():
    m = sw.Metadata()
    assert m.size() == 0

def test_default_registry():
    r = sw.default_action_registry()
    assert r.size() > 0
    assert r.has("CreateTable")
    assert r.has("InsertData")

def test_workload_params():
    wp = sw.WorkloadParams()
    wp.duration_in_seconds = 30
    wp.number_of_workers = 4
    assert wp.duration_in_seconds == 30
```

**Step 3: Build and run tests**

```bash
cmake --build build
PYTHONPATH=build/stormweaver/src python3.14t -m pytest tests/test_bindings.py -v
```

**Step 4: Commit**

```bash
git add -A
git commit -m "feat: add nanobind bindings for Metadata and ActionRegistry"
```

---

### Task 5: Bind Worker, RandomWorker, and Statistics

Expose the worker classes and statistics so Python can run workloads and inspect results.

**Files:**
- Modify: `stormweaver/src/module.cpp`

**Step 1: Add worker + statistics bindings**

```cpp
#include "workload.hpp"
#include "statistics.hpp"

// In NB_MODULE:

nb::class_<statistics::TimingStatistics>(m, "TimingStatistics")
    .def("avg_ms", &statistics::TimingStatistics::getAverageMs)
    .def("min_ms", &statistics::TimingStatistics::getMinMs)
    .def("max_ms", &statistics::TimingStatistics::getMaxMs)
    .def_ro("count", &statistics::TimingStatistics::count)
    .def("has_data", &statistics::TimingStatistics::hasData);

nb::class_<statistics::ActionStatistics>(m, "ActionStatistics")
    .def_ro("success_count", &statistics::ActionStatistics::successCount)
    .def_ro("action_failure_count", &statistics::ActionStatistics::actionFailureCount)
    .def_ro("sql_failure_count", &statistics::ActionStatistics::sqlFailureCount)
    .def_ro("other_failure_count", &statistics::ActionStatistics::otherFailureCount)
    .def("total_count", &statistics::ActionStatistics::getTotalCount)
    .def("success_rate", &statistics::ActionStatistics::getSuccessRate)
    .def_ro("execution_timing", &statistics::ActionStatistics::executionTiming)
    .def_ro("sql_timing", &statistics::ActionStatistics::sqlTiming);

nb::class_<statistics::WorkerStatistics>(m, "WorkerStatistics")
    .def("report", &statistics::WorkerStatistics::report)
    .def("report_summary", &statistics::WorkerStatistics::reportSummary)
    .def("report_detailed", &statistics::WorkerStatistics::reportDetailed)
    .def("total_duration_seconds", &statistics::WorkerStatistics::getTotalDurationSeconds)
    .def("total_action_count", &statistics::WorkerStatistics::getTotalActionCount)
    .def("total_success_count", &statistics::WorkerStatistics::getTotalSuccessCount)
    .def("total_failure_count", &statistics::WorkerStatistics::getTotalFailureCount)
    .def("success_rate", &statistics::WorkerStatistics::getOverallSuccessRate)
    .def("actions_per_second", &statistics::WorkerStatistics::getActionsPerSecond)
    .def("has_data", &statistics::WorkerStatistics::hasData);
```

For `Worker` and `RandomWorker`, we need to handle `sql_connector_t` which is a `std::function`. nanobind can wrap Python callables to `std::function`:

```cpp
#include <nanobind/stl/function.h>
#include <nanobind/stl/shared_ptr.h>

using sql_connector_t = Worker::sql_connector_t;

nb::class_<Worker>(m, "Worker")
    .def(nb::init<std::string const &, sql_connector_t const &,
         WorkloadParams const &, metadata_ptr>())
    .def("create_random_tables", &Worker::create_random_tables)
    .def("discover_existing_schema", &Worker::discover_existing_schema)
    .def("reset_metadata", &Worker::reset_metadata)
    .def("validate_metadata", &Worker::validate_metadata)
    .def("reconnect", &Worker::reconnect);

nb::class_<RandomWorker, Worker>(m, "RandomWorker")
    .def(nb::init<std::string const &, sql_connector_t const &,
         WorkloadParams const &, metadata_ptr,
         action::ActionRegistry const &>())
    .def("run_thread", &RandomWorker::run_thread)
    .def("join", &RandomWorker::join)
    .def("possible_actions", &RandomWorker::possibleActions,
         nb::rv_policy::reference)
    .def("statistics", &RandomWorker::statistics,
         nb::rv_policy::reference);
```

**Step 2: Build and verify**

```bash
cmake --build build
```

**Step 3: Add smoke test**

Add to `tests/test_bindings.py`:
```python
def test_worker_statistics():
    s = sw.WorkerStatistics()
    assert s.total_action_count() == 0
    assert not s.has_data()
```

Note: Testing actual Worker/RandomWorker requires a database connection, which is covered in Task 10.

**Step 4: Commit**

```bash
git add -A
git commit -m "feat: add nanobind bindings for Worker, RandomWorker, Statistics"
```

---

### Task 6: Create Python package structure

Create the `stormweaver` Python package with entry point, CLI, and config parsing.

**Files:**
- Create: `src/stormweaver/__init__.py`
- Create: `src/stormweaver/__main__.py`
- Create: `src/stormweaver/cli.py`
- Create: `src/stormweaver/config.py`

**Step 1: Create package directory**

```bash
mkdir -p src/stormweaver
```

**Step 2: Write `__init__.py`**

```python
from _stormweaver import (
    __version__,
    connect_pg,
    Metadata,
    ActionRegistry,
    default_action_registry,
    WorkloadParams,
    AllConfig,
    DdlConfig,
    DmlConfig,
    Worker,
    RandomWorker,
    WorkerStatistics,
    QueryResult,
    LoggedSQL,
)
from stormweaver.workload import Workload
from stormweaver.postgres import Postgres
from stormweaver.config import Config
```

**Step 3: Write `__main__.py`**

```python
import importlib.util
import sys
import logging

from stormweaver.cli import parse_args

logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)

def main():
    args = parse_args(sys.argv[1:])

    spec = importlib.util.spec_from_file_location("scenario", args.scenario)
    if spec is None or spec.loader is None:
        print(f"Error: cannot load scenario file: {args.scenario}", file=sys.stderr)
        sys.exit(1)

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    if not hasattr(module, "main"):
        print(f"Error: scenario {args.scenario} has no main() function", file=sys.stderr)
        sys.exit(4)

    module.main(args)

if __name__ == "__main__":
    main()
```

**Step 4: Write `cli.py`**

```python
import argparse

def parse_args(argv):
    parser = argparse.ArgumentParser(prog="stormweaver")
    parser.add_argument("scenario", help="Scenario file to execute")
    parser.add_argument("-c", "--config", default="config/stormweaver.toml",
                        help="Configuration file")
    parser.add_argument("-i", "--install-dir", default="",
                        help="PostgreSQL installation directory")
    return parser.parse_args(argv)
```

**Step 5: Write `config.py`**

```python
import tomllib
from pathlib import Path

class Config:
    def __init__(self, data):
        self._data = data
        defaults = data.get("default", {})
        self.pgroot = defaults.get("pgroot", "")
        self.datadir_root = defaults.get("datadir_root", "datadirs")
        self.port_start = defaults.get("port_start", 15432)
        self.port_end = defaults.get("port_end", 15531)

    @classmethod
    def load(cls, path):
        with open(path, "rb") as f:
            return cls(tomllib.load(f))

    def datadir(self, name):
        return str(Path(self.datadir_root) / f"datadir_{name}")
```

**Step 6: Commit**

```bash
git add -A
git commit -m "feat: create stormweaver Python package with CLI and config"
```

---

### Task 7: Python Postgres class

Rewrite the C++ `process::Postgres` class in Python using `subprocess`.

**Files:**
- Create: `src/stormweaver/postgres.py`

**Step 1: Write postgres.py**

```python
import logging
import subprocess
import time
from pathlib import Path

logger = logging.getLogger(__name__)

class Postgres:
    def __init__(self, install_dir, datadir, init=True, port=None):
        self.install_dir = Path(install_dir)
        self.datadir = Path(datadir)
        self._port = str(port) if port else None
        self._process = None

        if init:
            self._initdb()

    def _bin(self, name):
        return str(self.install_dir / "bin" / name)

    def _initdb(self):
        logger.info("Initializing datadir at %s", self.datadir)
        self.datadir.mkdir(parents=True, exist_ok=True)
        result = subprocess.run(
            [self._bin("initdb"), "-D", str(self.datadir), "--no-sync"],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"initdb failed: {result.stderr}")
        logger.info("initdb completed successfully")

        if self._port:
            self.add_config("port", self._port)
        else:
            self._port = "5432"

    @property
    def port(self):
        return int(self._port)

    def add_config(self, key_or_dict, value=None):
        conf_file = self.datadir / "postgresql.conf"
        with open(conf_file, "a") as f:
            if isinstance(key_or_dict, dict):
                for k, v in key_or_dict.items():
                    f.write(f"{k} = '{v}'\n")
            else:
                f.write(f"{key_or_dict} = '{value}'\n")

    def add_hba(self, host_type, database, user, address, method):
        hba_file = self.datadir / "pg_hba.conf"
        with open(hba_file, "a") as f:
            f.write(f"{host_type} {database} {user} {address} {method}\n")

    def start(self):
        logger.info("Starting PostgreSQL on port %s", self._port)
        result = subprocess.run(
            [self._bin("pg_ctl"), "start", "-D", str(self.datadir),
             "-l", str(self.datadir / "server.log"), "-w"],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"pg_ctl start failed: {result.stderr}")
        logger.info("PostgreSQL started")

    def stop(self, timeout=10):
        logger.info("Stopping PostgreSQL")
        result = subprocess.run(
            [self._bin("pg_ctl"), "stop", "-D", str(self.datadir),
             "-m", "fast", "-t", str(timeout)],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            logger.warning("pg_ctl stop failed: %s", result.stderr)

    def restart(self, timeout=10):
        self.stop(timeout)
        self.start()

    def is_ready(self):
        result = subprocess.run(
            [self._bin("pg_isready"), "-p", self._port],
            capture_output=True, text=True,
        )
        return result.returncode == 0

    def wait_ready(self, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.is_ready():
                return True
            time.sleep(0.2)
        return False

    def createdb(self, name):
        result = subprocess.run(
            [self._bin("createdb"), "-p", self._port, name],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"createdb failed: {result.stderr}")

    def dropdb(self, name):
        subprocess.run(
            [self._bin("dropdb"), "-p", self._port, name],
            capture_output=True, text=True,
        )

    def basebackup(self, target_datadir, extra_args=None):
        args = [
            self._bin("pg_basebackup"),
            "-D", str(target_datadir),
            "-p", self._port,
            "--no-sync",
        ]
        if extra_args:
            args.extend(extra_args)
        result = subprocess.run(args, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"pg_basebackup failed: {result.stderr}")
```

**Step 2: Write test**

Create `tests/test_postgres.py`:
```python
from stormweaver.postgres import Postgres
from unittest.mock import patch, MagicMock
from pathlib import Path

def test_bin_path():
    pg = Postgres.__new__(Postgres)
    pg.install_dir = Path("/usr/lib/postgresql/17")
    assert pg._bin("initdb") == "/usr/lib/postgresql/17/bin/initdb"

def test_add_config(tmp_path):
    conf = tmp_path / "postgresql.conf"
    conf.write_text("")
    pg = Postgres.__new__(Postgres)
    pg.datadir = tmp_path
    pg.add_config("port", "5433")
    assert "port = '5433'" in conf.read_text()

def test_add_config_dict(tmp_path):
    conf = tmp_path / "postgresql.conf"
    conf.write_text("")
    pg = Postgres.__new__(Postgres)
    pg.datadir = tmp_path
    pg.add_config({"shared_buffers": "256MB", "max_connections": "100"})
    content = conf.read_text()
    assert "shared_buffers = '256MB'" in content
    assert "max_connections = '100'" in content
```

**Step 3: Run tests**

```bash
python3.14t -m pytest tests/test_postgres.py -v
```

**Step 4: Commit**

```bash
git add -A
git commit -m "feat: add Python Postgres process management class"
```

---

### Task 8: Python Workload class

Create the Python-side workload orchestration using `threading.Thread`.

**Files:**
- Create: `src/stormweaver/workload.py`

**Step 1: Write workload.py**

```python
import logging
import threading

import _stormweaver

logger = logging.getLogger(__name__)

class Workload:
    def __init__(self, workers, duration, registry, metadata, node_factory,
                 repeat=1, max_reconnect_attempts=5):
        self.num_workers = workers
        self.duration = duration
        self.repeat = repeat
        self.registry = registry
        self.metadata = metadata
        self.node_factory = node_factory
        self.max_reconnect_attempts = max_reconnect_attempts
        self._all_stats = []

    def run(self):
        for cycle in range(self.repeat):
            logger.info("Workload cycle %d/%d", cycle + 1, self.repeat)
            workers = []
            threads = []

            params = _stormweaver.WorkloadParams()
            params.duration_in_seconds = self.duration
            params.max_reconnect_attempts = self.max_reconnect_attempts

            for i in range(self.num_workers):
                name = f"worker-{cycle + 1}-{i + 1}"
                worker = _stormweaver.RandomWorker(
                    name, self.node_factory, params,
                    self.metadata, self.registry,
                )
                t = threading.Thread(
                    target=worker.run_thread,
                    args=(self.duration,),
                    name=name,
                )
                workers.append(worker)
                threads.append(t)

            for t in threads:
                t.start()
            for t in threads:
                t.join()

            for w in workers:
                self._all_stats.append(w.statistics())

            logger.info("Workload cycle %d complete", cycle + 1)

    def print_report(self):
        for i, stats in enumerate(self._all_stats):
            if stats.has_data():
                print(stats.report())
```

**Step 2: Write test**

Create `tests/test_workload.py`:
```python
from stormweaver.workload import Workload
import _stormweaver as sw

def test_workload_init():
    metadata = sw.Metadata()
    registry = sw.default_action_registry()
    w = Workload(
        workers=2,
        duration=10,
        registry=registry,
        metadata=metadata,
        node_factory=lambda: None,
    )
    assert w.num_workers == 2
    assert w.duration == 10
    assert w.repeat == 1
```

**Step 3: Run tests**

```bash
python3.14t -m pytest tests/test_workload.py -v
```

**Step 4: Commit**

```bash
git add -A
git commit -m "feat: add Python Workload class with threading"
```

---

### Task 9: Wrapper script and CMake install

Create the wrapper script template and CMake install rules for both local builds and installation.

**Files:**
- Create: `stormweaver.sh.in` (wrapper template for install)
- Create: `stormweaver-dev.sh.in` (wrapper template for local build)
- Modify: `CMakeLists.txt` (add install rules)
- Modify: `stormweaver/src/CMakeLists.txt` (extension install)

**Step 1: Create install wrapper template**

Create `stormweaver.sh.in`:
```bash
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SW_ROOT="$(dirname "$SCRIPT_DIR")"
export PYTHONHOME="${SW_ROOT}/python"
export PYTHONPATH="${SW_ROOT}/python/lib/python3.14t/site-packages"
exec "${SW_ROOT}/python/bin/python3.14t" -m stormweaver "$@"
```

**Step 2: Create dev wrapper template**

Create `stormweaver-dev.sh.in`:
```bash
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export PYTHONPATH="@CMAKE_SOURCE_DIR@/src:@CMAKE_BINARY_DIR@/stormweaver/src"
exec "@CPYTHON_EXECUTABLE@" -m stormweaver "$@"
```

**Step 3: Update top-level CMakeLists.txt**

Add after the `find_package` section:

```cmake
# Get cpython executable path from conan
set(CPYTHON_EXECUTABLE "${Python3_EXECUTABLE}")

# Generate dev wrapper for local builds
configure_file(
    "${CMAKE_SOURCE_DIR}/stormweaver-dev.sh.in"
    "${CMAKE_BINARY_DIR}/stormweaver"
    @ONLY
)
execute_process(COMMAND chmod +x "${CMAKE_BINARY_DIR}/stormweaver")
```

Add install rules at the end:

```cmake
# Install Python package
install(DIRECTORY src/stormweaver/ DESTINATION python/lib/python3.14t/site-packages/stormweaver)
install(PROGRAMS "${CMAKE_SOURCE_DIR}/stormweaver.sh.in" DESTINATION bin RENAME stormweaver)
install(DIRECTORY scenarios/ DESTINATION scenarios)
install(DIRECTORY config/ DESTINATION config)
```

**Step 4: Update extension CMakeLists**

In `stormweaver/src/CMakeLists.txt`, add install for the extension:

```cmake
install(TARGETS _stormweaver DESTINATION python/lib/python3.14t/site-packages/)
```

**Step 5: Verify dev wrapper works**

```bash
cmake --build build
./build/stormweaver --help
```

Expected: argparse help output from `stormweaver/cli.py`.

**Step 6: Commit**

```bash
git add -A
git commit -m "feat: add wrapper scripts and CMake install rules"
```

---

### Task 10: Minimal scenario and integration test

Write a basic scenario that starts PostgreSQL, runs a short workload (create/alter/insert/delete), and validates results. Wire it into CTest.

**Files:**
- Create: `scenarios/ci/basic.py`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the minimal scenario**

Create `scenarios/ci/basic.py`:
```python
import shutil
import stormweaver as sw

def main(args):
    config = sw.Config.load(args.config)
    install_dir = args.install_dir or config.pgroot

    if not install_dir:
        raise RuntimeError("PostgreSQL install dir required: use -i or set pgroot in config")

    datadir = config.datadir("primary")

    # Clean up any leftover datadir
    shutil.rmtree(datadir, ignore_errors=True)

    pg = sw.Postgres(install_dir=install_dir, datadir=datadir, port=config.port_start)
    pg.add_config({
        "log_min_messages": "warning",
        "max_connections": "100",
        "shared_buffers": "128MB",
    })
    pg.start()
    pg.wait_ready()
    pg.createdb("testdb")

    metadata = sw.Metadata()
    registry = sw.default_action_registry()

    # For vanilla postgres, remove tde_heap access method
    ddl_config = sw.DdlConfig()
    ddl_config.access_methods = ["heap"]

    # Remove partition actions for simplicity
    if registry.has("CreatePartition"):
        registry.remove("CreatePartition")
    if registry.has("DropPartition"):
        registry.remove("DropPartition")

    def make_connection():
        return sw.connect_pg(
            host="localhost",
            port=pg.port,
            dbname="testdb",
        )

    # Create initial tables
    conn = make_connection()
    worker = sw.Worker("setup", make_connection, sw.WorkloadParams(), metadata)
    worker.create_random_tables(5)

    workload = sw.Workload(
        workers=4,
        duration=30,
        repeat=2,
        registry=registry,
        metadata=metadata,
        node_factory=make_connection,
    )
    workload.run()
    workload.print_report()

    # Validate metadata
    validator = sw.Worker("validator", make_connection, sw.WorkloadParams(), metadata)
    valid = validator.validate_metadata()
    if not valid:
        print("WARNING: metadata validation failed")

    pg.stop()
    print("Scenario completed successfully")
```

**Step 2: Update DdlConfig access_methods in WorkloadParams**

The scenario sets `ddl_config.access_methods = ["heap"]` but we need to make sure this flows through to the actions. The `WorkloadParams` has an `actionConfig.ddl` field. Update the scenario to use it properly:

The `WorkloadParams` already has `actionConfig` which contains `DdlConfig`. The scenario should set it on the params, but in this case we're using the `Workload` Python class which creates its own `WorkloadParams`. We need to either:
- Pass `AllConfig` to the `Workload` class, or
- Set defaults on the `WorkloadParams` inside `Workload.__init__`

Update `src/stormweaver/workload.py` to accept `action_config`:
```python
def __init__(self, workers, duration, registry, metadata, node_factory,
             repeat=1, max_reconnect_attempts=5, action_config=None):
    ...
    self.action_config = action_config
```

And in the run loop:
```python
params = _stormweaver.WorkloadParams()
params.duration_in_seconds = self.duration
params.max_reconnect_attempts = self.max_reconnect_attempts
if self.action_config:
    params.action_config = self.action_config
```

Then in the scenario:
```python
action_config = sw.AllConfig()
action_config.ddl.access_methods = ["heap"]

workload = sw.Workload(
    ...
    action_config=action_config,
)
```

**Step 3: Update tests/CMakeLists.txt**

```cmake
if(NOT "${TEST_PG_DIR}" STREQUAL "")
    add_test(NAME stormweaver-basic
        COMMAND "${CMAKE_BINARY_DIR}/stormweaver"
                "${CMAKE_SOURCE_DIR}/scenarios/ci/basic.py"
                "-i" "${TEST_PG_DIR}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
```

**Step 4: Run the integration test**

```bash
cmake --build build
ctest --test-dir build -R stormweaver-basic -V
```

Expected: PostgreSQL starts, creates tables, runs workload with 4 workers for 30s x 2 cycles, prints statistics report, validates metadata, stops.

**Step 5: Commit**

```bash
git add -A
git commit -m "feat: add minimal Python scenario and integration test"
```

---

## Summary

| Task | Description | Dependencies |
|------|-------------|--------------|
| 1 | Remove Lua from build, add cpython+nanobind | None |
| 2 | Decouple C++ from Lua | Task 1 |
| 3 | Bind SQL layer | Task 2 |
| 4 | Bind Metadata + ActionRegistry | Task 2 |
| 5 | Bind Worker/RandomWorker/Statistics | Task 3, 4 |
| 6 | Python package (init, main, cli, config) | Task 5 |
| 7 | Python Postgres class | None (pure Python) |
| 8 | Python Workload class | Task 5 |
| 9 | Wrapper script + CMake install | Task 6, 8 |
| 10 | Minimal scenario + integration test | Task 7, 9 |

Tasks 3+4 can run in parallel. Task 7 can run in parallel with Tasks 1-5.
