#!/usr/bin/env python3
"""Package the mod runtime's symbol namespace without discovery metadata.

The development JSON remains untouched. All names, aliases, kinds, addresses,
global layouts, events and the executable digest remain available to the loader.
"""
import argparse
import json
from pathlib import Path


def compact(symbols):
    result = {key: symbols[key] for key in ("exe_sha256", "image_base", "globals", "events")
              if key in symbols}
    result["functions"] = []
    for entry in symbols["functions"]:
        row = {key: entry[key] for key in ("addr", "name", "kind", "hookable") if key in entry}
        if entry.get("aliases"):
            row["aliases"] = entry["aliases"]
        result["functions"].append(row)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    symbols = json.loads(args.source.read_text(encoding="utf-8"))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(compact(symbols), ensure_ascii=False,
                                     separators=(",", ":")) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
