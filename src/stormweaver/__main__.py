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
        print(
            f"Error: scenario {args.scenario} has no main() function",
            file=sys.stderr,
        )
        sys.exit(4)

    module.main(args)


if __name__ == "__main__":
    main()
