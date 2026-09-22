"""Explicit evaluation of a frozen exported model, including final test on request."""
import argparse
import json
from pathlib import Path

from .dataset import metadata
from .model import load_model
from .schema import sha256
from .train import evaluate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--split", choices=["validation", "test"], default="validation")
    args = parser.parse_args()
    metadata(args.dataset)
    report = evaluate(load_model(args.model), args.dataset, args.split)
    print(json.dumps(dict(model_sha256=sha256(args.model), dataset_sha256=sha256(args.dataset),
                          split=args.split, metrics=report), indent=2))


if __name__ == "__main__":
    main()
