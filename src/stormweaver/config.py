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
