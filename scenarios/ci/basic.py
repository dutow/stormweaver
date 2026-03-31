import os
import shutil

import stormweaver as sw


def main(args):
    config = sw.Config.load(args.config)
    install_dir = args.install_dir or config.pgroot

    if not install_dir:
        raise RuntimeError(
            "PostgreSQL install dir required: use -i or set pgroot in config"
        )

    datadir = config.datadir("primary")

    shutil.rmtree(datadir, ignore_errors=True)

    pg = sw.Postgres(
        install_dir=install_dir, datadir=datadir, port=config.port_start
    )
    pg.add_config(
        {
            "log_min_messages": "warning",
            "max_connections": "100",
            "shared_buffers": "128MB",
        }
    )
    pg.start()
    pg.wait_ready()
    pg.createdb("testdb")

    metadata = sw.Metadata()
    registry = sw.default_action_registry()

    # Remove partition actions for simplicity
    for action_name in ["create_partition", "drop_partition"]:
        if registry.has(action_name):
            registry.remove(action_name)

    action_config = sw.AllConfig()
    action_config.ddl.access_methods = ["heap"]

    user = os.environ.get("PGUSER", os.getlogin())

    def make_connection():
        return sw.connect_pg(host="localhost", port=pg.port, dbname="testdb", user=user)

    # Create initial tables
    worker = sw.Worker("setup", make_connection, sw.WorkloadParams(), metadata)
    worker.create_random_tables(5)

    workload = sw.Workload(
        workers=4,
        duration=30,
        repeat=2,
        registry=registry,
        metadata=metadata,
        node_factory=make_connection,
        action_config=action_config,
    )
    workload.run()
    workload.print_report()

    # Validate metadata
    validator = sw.Worker(
        "validator", make_connection, sw.WorkloadParams(), metadata
    )
    valid = validator.validate_metadata()
    if not valid:
        print("WARNING: metadata validation failed")

    pg.stop()
    print("Scenario completed successfully")
