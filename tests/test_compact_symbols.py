"""The web package retains the mod API's complete symbol namespace."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("compact_symbols", ROOT / "tools/compact_symbols.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def test_compaction_preserves_runtime_fields_and_input():
    symbols = {
        "exe_sha256": "a" * 64, "image_base": "00400000",
        "functions": [
            {"addr": "00401000", "name": "entry", "kind": "entry", "hookable": True,
             "aliases": ["main"], "index": 0, "provenance": "listed", "evidence": []},
            {"addr": "00401004", "name": "continuation", "kind": "continuation",
             "hookable": False, "aliases": [], "index": 1, "evidence": ["return"]},
        ],
        "globals": [{"name": "items", "addr": "00402000", "stride": 16, "count": 7}],
        "events": {"on_frame": "00401000"},
    }
    before = copy.deepcopy(symbols)
    packed = module.compact(symbols)
    assert symbols == before
    for key in ("exe_sha256", "image_base", "globals", "events"):
        assert packed[key] == symbols[key]
    for original, compact in zip(symbols["functions"], packed["functions"]):
        for key in ("addr", "name", "kind", "hookable"):
            assert compact[key] == original[key]
        assert compact.get("aliases", []) == original["aliases"]
        assert not {"index", "provenance", "evidence"}.intersection(compact)


def test_cli_keeps_unicode_names_and_escapes(tmp_path):
    source = tmp_path / "symbols.json"
    target = tmp_path / "resources/symbols.json"
    symbols = {"exe_sha256": "abc", "functions": [
        {"addr": "00401000", "name": "Пример", "aliases": ['quoted\"name', "path\\name"]}]}
    source.write_text(json.dumps(symbols, ensure_ascii=False), encoding="utf-8")
    before = source.read_bytes()
    subprocess.run([sys.executable, module.__file__, str(source), str(target)], check=True)
    assert source.read_bytes() == before
    assert json.loads(target.read_text(encoding="utf-8")) == symbols
    # The runtime's compact parser accepts UTF-8 names directly.
    assert "Пример" in target.read_text(encoding="utf-8")
