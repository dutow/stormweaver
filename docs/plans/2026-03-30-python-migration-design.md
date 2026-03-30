# Stormweaver Python Migration Design

## Context

Stormweaver is a concurrent database testing tool currently written in C++23 with Lua scripting (Sol2).
This design describes migrating from Lua to free-threaded Python (CPython 3.14t) to:

- Lower the barrier for developers writing scenarios (Python vs Lua)
- Leverage the Python standard library and pip ecosystem
- Integrate better with the highly parallel engine via free-threading (no GIL)
- Move non-performance-critical code to Python while keeping the hot path in C++

This is a breaking change. Existing Lua scenarios will be rewritten in Python.

## Architecture

Python-first with a C++ extension module, bundled with a free-threaded CPython interpreter.

```
stormweaver scenario.py [args]
        |
  Shell wrapper script
  (sets PYTHONHOME, execs bundled python3.14t -m stormweaver)
        |
  Python package: stormweaver/
  (orchestration, process management, scenario loading)
        |
  C++ extension: _stormweaver.so (nanobind)
  (metadata, action registry, query building, SQL execution, worker hot loop)
```

### What stays in C++

Performance and mission-critical components exposed to Python via nanobind:

- **Metadata** — thread-safe concurrent schema tracking with atomic pointer swap
- **ActionRegistry** — weighted random action selection
- **Action implementations** — DDL/DML SQL generation from metadata
- **GenericSQL / PostgreSQL** — connection management, query execution, logging
- **Worker / RandomWorker** — hot loop for concurrent workload execution
- **Statistics** — per-action timing and error tracking
- **Checksum** — SHA256 table checksums

### What moves to Python

- **Process management** — `Postgres` class rewritten with `subprocess`/`pathlib`
- **Scenario loading and execution** — `importlib`
- **Argument parsing** — `argparse` (stdlib)
- **Configuration parsing** — `tomllib` (stdlib, replaces tomlplusplus)
- **Filesystem utilities** — `pathlib`/`shutil` (stdlib)
- **PgConf / PgManager** helpers
- **Background process orchestration** — `subprocess`
- **Workload coordination** — `threading.Thread` creating C++ workers

### What gets removed

- Sol2 / Lua entirely
- All `.lua` scripts (rewritten as `.py`)
- tomlplusplus conan dependency (Python has `tomllib`)
- Potentially some boost usage (process management and filesystem move to Python)

## Build System

Stays **conan + cmake**. Single build pass produces everything.

### Conan dependencies

**Keep:** boost, libpqxx, cryptopp, spdlog, catch2, fmt, magic_enum, reflect-cpp, nlohmann_json

**Add:** cpython/3.14.3 (free_threaded=True), nanobind

**Remove:** sol2, tomlplusplus

Boost usage should be reviewed after migration — if process management moves fully to Python, boost may be reducible or removable.

### CMake structure

```cmake
# Static C++ library (as today)
add_library(libstormweaver STATIC ...)

# Python extension module (replaces stormweaver executable)
nanobind_add_module(_stormweaver
    src/bindings/module.cpp
    src/bindings/metadata.cpp
    src/bindings/actions.cpp
    src/bindings/worker.cpp
    src/bindings/sql.cpp
)
target_link_libraries(_stormweaver PRIVATE libstormweaver)

# C++ unit tests (as today)
add_executable(stormweaver_tests ...)
```

## Install Layout

```
<prefix>/
├── bin/
│   └── stormweaver                              # generated wrapper script
├── python/
│   ├── bin/
│   │   └── python3.14t
│   ├── lib/
│   │   ├── libpython3.14t.so
│   │   └── python3.14t/
│   │       ├── ...                              # stdlib
│   │       └── site-packages/
│   │           ├── stormweaver/                  # Python package
│   │           │   ├── __init__.py
│   │           │   ├── __main__.py
│   │           │   ├── postgres.py
│   │           │   ├── pgconf.py
│   │           │   ├── pgmanager.py
│   │           │   ├── workload.py
│   │           │   ├── process.py
│   │           │   ├── cli.py
│   │           │   ├── config.py
│   │           │   └── util.py
│   │           └── _stormweaver.so              # C++ extension
│   └── include/
├── scenarios/                                    # shipped examples
│   ├── basic.py
│   └── ci/
│       ├── basic.py
│       ├── incremental.py
│       └── pitr.py
└── config/
    └── stormweaver.toml
```

### Wrapper script

Generated at configure time (`stormweaver.sh.in`):

```bash
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SW_ROOT="$(dirname "$SCRIPT_DIR")"
export PYTHONHOME="${SW_ROOT}/python"
exec "${SW_ROOT}/python/bin/python3.14t" -m stormweaver "$@"
```

A second wrapper is generated for local builds, pointing at the build tree and conan's cpython in-place. `./build/stormweaver scenario.py` works without an install step.

### Distribution

The install tree is self-contained:
- `tar czf stormweaver.tar.gz <prefix>` produces a working tarball
- rpm/deb packages install the same tree to `/usr/lib/stormweaver/` with a symlink from `/usr/bin/stormweaver`

Users can install additional pip packages via the bundled Python:

```bash
stormweaver -m pip install somepackage
# or directly:
<prefix>/python/bin/python3.14t -m pip install somepackage
```

## C++ Extension API

The `_stormweaver` module (internal) is wrapped by the `stormweaver` Python package (public).

```python
import _stormweaver

# Connections
node = _stormweaver.connect_pg(host="localhost", port=5432, dbname="test", user="postgres")
result = node.execute("SELECT 1")
# result.success, result.rows, result.affected_rows, result.error, result.execution_time

# Metadata (thread-safe, shared across workers)
metadata = _stormweaver.Metadata()

# Action Registry
registry = _stormweaver.default_action_registry()
registry.set_weight("CreateTable", 5)
registry.set_weight("DropTable", 0)
registry.add_custom_sql("VACUUM {table}", weight=2)
registry.add_python_action(my_func, weight=3)  # Python callback, works with free-threading

# Worker (runs C++ hot loop on calling thread)
worker = _stormweaver.RandomWorker(
    node_factory=lambda: _stormweaver.connect_pg(...),
    metadata=metadata,
    registry=registry,
    duration=60,
)
stats = worker.run()

# Schema discovery
w = _stormweaver.Worker(node, metadata)
w.discover_schema()
w.validate_metadata()

# Checksums and statistics
checksum = _stormweaver.table_checksum(node, "my_table")
stats.print_report()
```

### Connection design

All queries go through the C++ `GenericSQL` wrapper — no psycopg or other Python pg library.
This ensures consistent logging, statistics tracking, and error handling for all queries,
including those from Python-defined custom actions.

libpq does not provide query-execution hooks, so intercepting queries through a Python library's
underlying `PGconn*` is not viable. The C++ wrapper is the single path for all database access.

## Python Package Structure

```
stormweaver/
├── __init__.py          # re-exports from _stormweaver + Python classes
├── __main__.py          # entry point: loads and executes scenario files
├── cli.py               # argument parsing (argparse)
├── config.py            # TOML config loading (tomllib)
├── postgres.py          # Postgres lifecycle (initdb, pg_ctl, basebackup, config, HBA)
├── pgconf.py            # PostgreSQL configuration builder
├── pgmanager.py         # Multi-node management (primary + replicas)
├── workload.py          # Workload class: creates threads, runs RandomWorkers
├── process.py           # Generic background process management (subprocess)
└── util.py              # Logging setup, filesystem helpers
```

### Scenario execution

Scenarios are plain Python scripts loaded via `importlib`:

```bash
stormweaver scenarios/ci/basic.py --config config/stormweaver.toml -i /usr/lib/postgresql/17
```

```python
# __main__.py (simplified)
import importlib.util, sys
from stormweaver.cli import parse_args

def main():
    args = parse_args(sys.argv[1:])
    spec = importlib.util.spec_from_file_location("scenario", args.scenario)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.main(args)
```

### Workload class

Python manages threads, C++ runs the hot loop per thread:

```python
import threading
import _stormweaver

class Workload:
    def __init__(self, workers, duration, repeat, registry, metadata, node_factory):
        self.workers = workers
        self.duration = duration
        self.repeat = repeat
        self.registry = registry
        self.metadata = metadata
        self.node_factory = node_factory

    def run(self):
        all_stats = []
        for cycle in range(self.repeat):
            threads = []
            for i in range(self.workers):
                worker = _stormweaver.RandomWorker(
                    node_factory=self.node_factory,
                    metadata=self.metadata,
                    registry=self.registry,
                    duration=self.duration,
                )
                t = threading.Thread(target=worker.run, name=f"worker-{i}")
                threads.append((t, worker))
            for t, _ in threads:
                t.start()
            for t, _ in threads:
                t.join()
            all_stats.extend(w.stats for _, w in threads)
        return _stormweaver.merge_stats(all_stats)
```

With free-threading, `threading.Thread` gives real OS threads. Each thread enters C++ via
`worker.run()` and stays there for the duration — no Python contention.

### Example scenario

```python
import stormweaver as sw

def main(args):
    config = sw.Config.load(args.config)

    pg = sw.Postgres(install_dir=args.install_dir, datadir=config.datadir("primary"))
    pg.add_config({"shared_preload_libraries": "pg_tde"})
    pg.start()
    pg.wait_ready()
    pg.createdb("testdb")

    metadata = sw.Metadata()
    registry = sw.default_action_registry()

    workload = sw.Workload(
        workers=4,
        duration=30,
        repeat=3,
        registry=registry,
        metadata=metadata,
        node_factory=lambda: sw.connect_pg(port=pg.port, dbname="testdb"),
    )
    stats = workload.run()
    stats.print_report()

    pg.stop()
```

## Multi-Database Considerations

The design accommodates future MySQL support without changes:

```
Python layer (database-specific orchestration):
  stormweaver/postgres.py    # pg_ctl, initdb, basebackup, HBA
  stormweaver/mysql.py       # mysqld, mysql_install_db (future)

C++ layer (database-specific SQL):
  GenericSQL                 # base: connect, execute, results
  ├── PostgreSQL             # libpqxx
  └── MySQL                  # libmysqlclient (future)

  Action                     # base: build(metadata) → SQL string
  ├── pg::CreateTable        # PostgreSQL DDL syntax
  └── mysql::CreateTable     # MySQL DDL syntax (future)

Python connect functions:
  sw.connect_pg(...)         # returns GenericSQL backed by PostgreSQL
  sw.connect_mysql(...)      # future
```

- `node_factory` in `Workload` abstracts over connection type
- `ActionRegistry` accepts actions by interface — database-specific action sets can be registered
- Process management is naturally separate per database
- Scenarios are database-specific (a PostgreSQL PITR test won't apply to MySQL)

Adding MySQL later means: add the C++ `MySQL` + action implementations, add `stormweaver/mysql.py`, write MySQL scenarios.

## Decisions and Rationale

| Decision | Rationale |
|----------|-----------|
| Python-first with C++ extension (not embedded Python) | Aligns with goal of keeping more logic in Python; C++ extension is only for performance-critical path |
| Bundled cpython 3.14t | Free-threading not available in most distros; self-contained distribution for developer machines |
| Shell wrapper as build artifact | Works for local builds, tarball, and rpm/deb without separate packaging logic |
| nanobind over pybind11 | Same author, designed as successor; smaller binaries, better C++23 support, first-class free-threading |
| C++ GenericSQL for all DB access, no psycopg | libpq has no query hooks; need consistent logging/stats for all queries including Python-defined actions |
| conan + cmake stays | All new dependencies (cpython, nanobind) available in conan; no build system migration needed |
