import argparse


def parse_args(argv):
    parser = argparse.ArgumentParser(prog="stormweaver")
    parser.add_argument("scenario", help="Scenario file to execute")
    parser.add_argument(
        "-c",
        "--config",
        default="config/stormweaver.toml",
        help="Configuration file",
    )
    parser.add_argument(
        "-i", "--install-dir", default="", help="PostgreSQL installation directory"
    )
    return parser.parse_args(argv)
