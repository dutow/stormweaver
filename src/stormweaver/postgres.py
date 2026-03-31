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
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"initdb failed: {result.stderr}")
        logger.info("initdb completed successfully")

        if self._port:
            self.add_config("port", self._port)
        else:
            self._port = "5432"

        # Use datadir for unix socket to avoid needing /run/postgresql
        self.add_config("unix_socket_directories", str(self.datadir.resolve()))

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
            [
                self._bin("pg_ctl"),
                "start",
                "-D",
                str(self.datadir),
                "-l",
                str(self.datadir / "server.log"),
                "-w",
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"pg_ctl start failed: {result.stderr}")
        logger.info("PostgreSQL started")

    def stop(self, timeout=10):
        logger.info("Stopping PostgreSQL")
        result = subprocess.run(
            [
                self._bin("pg_ctl"),
                "stop",
                "-D",
                str(self.datadir),
                "-m",
                "fast",
                "-t",
                str(timeout),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            logger.warning("pg_ctl stop failed: %s", result.stderr)

    def restart(self, timeout=10):
        self.stop(timeout)
        self.start()

    def is_ready(self):
        result = subprocess.run(
            [self._bin("pg_isready"), "-h", "localhost", "-p", self._port],
            capture_output=True,
            text=True,
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
            [self._bin("createdb"), "-h", "localhost", "-p", self._port, name],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"createdb failed: {result.stderr}")

    def dropdb(self, name):
        subprocess.run(
            [self._bin("dropdb"), "-h", "localhost", "-p", self._port, name],
            capture_output=True,
            text=True,
        )

    def basebackup(self, target_datadir, extra_args=None):
        args = [
            self._bin("pg_basebackup"),
            "-D",
            str(target_datadir),
            "-h", "localhost",
            "-p",
            self._port,
            "--no-sync",
        ]
        if extra_args:
            args.extend(extra_args)
        result = subprocess.run(args, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"pg_basebackup failed: {result.stderr}")
