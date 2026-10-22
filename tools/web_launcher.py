#!/usr/bin/env python3
"""Build the web launcher: the hub page for one or more games.

    tools/web_launcher.py --game-dir /abs/game-a [--game-dir /abs/game-b ...] --out <dir>
        [--web-build <game id>=<build dir> ...] [--serve [port]]

Writes <dir>/index.html, app.js, core.js, worker.js and games.json (each game's
title, executable, digest, required folders, exclusions and store link from its
game.toml). A game's web build, when one exists, goes in <dir>/<game id>/.
--web-build copies a game's web build (build/web/recomp: index.html and the
app's .js/.wasm/.data) there, with .br/.gz copies a server can send instead.
--game-files <id>=<install> publishes an installation at <dir>/<id>/game/
(links, not copies, minus [bundle] exclude) with a manifest.txt: the player
then streams the game from there instead of asking for an import, fetching
only what the game reads and keeping it in the browser for the next visit.
Serve <dir> over HTTPS or from localhost with COOP/COEP headers (the game
needs a secure, cross-origin isolated context) and HTTP range requests;
--serve runs such a server, which also sends the compressed copies.

Only publish a game folder where you alone can reach it: it is the game.
"""

import argparse
import email.utils
import fnmatch
import functools
import gzip
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import game_config  # noqa: E402

PAGE_FILES = ("index.html", "app.js", "core.js", "worker.js")


def game_entry(cfg):
    game, launcher = cfg["game"], cfg["launcher"]
    return {
        "id": game["id"],
        "title": launcher["title"],
        "executable": game["executable"],
        "sha256": game["sha256"],
        "requiredDirs": cfg["setup"]["required_dirs"],
        "exclude": cfg["bundle"]["exclude"],
        "store": launcher["store"],
        "installNames": launcher["install_names"],
        "minFreeMb": launcher["min_free_mb"],
        "savesDir": launcher["saves_dir"],
        "saveExt": launcher["save_ext"],
    }


def build(game_dirs, out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    games = [game_entry(game_config.load(d)) for d in game_dirs]
    ids = [g["id"] for g in games]
    if len(set(ids)) != len(ids):
        raise ValueError("two games share an id: %s" % ", ".join(ids))
    for name in PAGE_FILES:
        shutil.copy2(ROOT / "web/launcher" / name, out / name)
    (out / "games.json").write_text(json.dumps(games, indent=2) + "\n")
    return games


WEB_BUILD_SUFFIXES = (".js", ".wasm", ".data")


def copy_web_build(game_id, build, out):
    build, dest = Path(build), Path(out) / game_id
    if not (build / "index.html").is_file():
        raise ValueError("no web build in %s (index.html is missing)" % build)
    dest.mkdir(parents=True, exist_ok=True)
    for f in build.iterdir():
        if f.is_file() and (f.name == "index.html" or f.suffix in WEB_BUILD_SUFFIXES):
            shutil.copy2(f, dest / f.name)
    for wasm in dest.glob("*.wasm"):
        strip_debug(wasm)
    compress_web_build(dest)


def strip_debug(wasm):
    """A release build keeps DWARF (about 70% of the module). The page never
    reads it, so the published copy goes without. llvm-objcopy, not wasm-opt:
    the latter rewrites imports in a form browsers reject."""
    tool = shutil.which("llvm-objcopy")
    if not tool and os.environ.get("EMSDK"):
        candidate = Path(os.environ["EMSDK"]) / "upstream/bin/llvm-objcopy"
        tool = str(candidate) if candidate.is_file() else None
    if not tool:
        print("note: no llvm-objcopy, %s keeps its debug info" % wasm.name)
        return
    before = wasm.stat().st_size
    subprocess.run([tool, "--strip-debug", str(wasm), str(wasm)], check=True)
    print("stripped %s: %.0f MB -> %.0f MB" % (wasm.name, before / 1e6, wasm.stat().st_size / 1e6))


def compress_web_build(dest):
    """Brotli and gzip copies of the app's .js/.wasm/.data. The wasm is mostly
    translated code and shrinks about twelvefold; a copy older than its source
    is made again."""
    for f in sorted(Path(dest).iterdir()):
        if f.suffix not in WEB_BUILD_SUFFIXES:
            continue
        for ext, make in ((".br", _brotli), (".gz", _gzip)):
            out = f.with_name(f.name + ext)
            if out.exists() and out.stat().st_mtime >= f.stat().st_mtime:
                continue
            if make(f, out):
                print("compressed %s: %.1f MB -> %.1f MB" % (out.name, f.stat().st_size / 1e6,
                                                            out.stat().st_size / 1e6), flush=True)


def _brotli(src, out):
    if not shutil.which("brotli"):
        return False
    subprocess.run(["brotli", "-f", "-q", "11", "-w", "24", "-o", str(out), str(src)], check=True)
    return True


def _gzip(src, out):
    with open(src, "rb") as fi, gzip.open(out, "wb", compresslevel=9) as fo:
        shutil.copyfileobj(fi, fo, 1 << 20)
    return True


def excluded(relative, patterns):
    """The launcher's rule (web/launcher/core.js): a pattern names the top
    folder or the file, case-insensitively."""
    parts = relative.split("/")
    return any(fnmatch.fnmatch(parts[0].lower(), p.lower()) or fnmatch.fnmatch(parts[-1].lower(), p.lower())
               for p in patterns)


SMALL_FILE = 1 << 20


def publish_game_files(game_id, install, out, patterns):
    """<out>/<id>/game/: the installation as links, and manifest.txt, one
    "<size>\t<mtime>\t<path>" line per file, for the player's stream."""
    install, dest = Path(install).resolve(), Path(out) / game_id / "game"
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    lines, total = [], 0
    for root, dirs, files in os.walk(install):
        dirs.sort()
        for name in sorted(files):
            src = Path(root) / name
            rel = src.relative_to(install).as_posix()
            if excluded(rel, patterns) or rel in ("manifest.txt", "small.pack", "hot.txt"):
                continue
            st = src.stat()
            link = dest / rel
            link.parent.mkdir(parents=True, exist_ok=True)
            link.symlink_to(src)
            lines.append("%d\t%d\t%s" % (st.st_size, int(st.st_mtime), rel))
            total += st.st_size
    # The small files (maps, campaigns, the odd table) in one pack: the
    # scenario list reads every map's header, and fetching ninety files one
    # request at a time is seconds of round trips. The player fetches the pack
    # once, in the background, and files its parts into its cache.
    small = [line for line in lines if int(line.split("\t")[0]) <= SMALL_FILE]
    with open(dest / "small.pack", "wb") as pack:
        for i, line in enumerate(lines):
            size, stamp, rel = line.split("\t", 2)
            if int(size) > SMALL_FILE:
                continue
            offset = pack.tell()
            with open(install / rel, "rb") as f:
                shutil.copyfileobj(f, pack)
            lines[i] = "%s\t%s\t%s\t%d" % (size, stamp, rel, offset)
        pack_size = pack.tell()
    header = "#pack\tsmall.pack\t%d" % pack_size
    (dest / "manifest.txt").write_text(header + "\n" + "\n".join(lines) + "\n", encoding="utf-8")
    print("published %d files (%.0f MB) of %s at %s; %d small ones packed (%.1f MB)"
          % (len(lines), total / 1e6, install, dest, len(small), pack_size / 1e6))


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
    # Keep-alive: the player's stream makes many small range requests.
    protocol_version = "HTTP/1.1"
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm",
                      ".js": "text/javascript", ".data": "application/octet-stream"}

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def send_head(self):
        """Files with validators, a compressed copy when the client takes one,
        and a single byte range (the player's stream reads by range)."""
        self._remaining = None
        path = self.translate_path(self.path)
        if os.path.isdir(path) or not os.path.isfile(path):
            return super().send_head()
        st = os.stat(path)
        etag = '"%x-%x"' % (st.st_size, int(st.st_mtime))
        if self.headers.get("If-None-Match") == etag:
            self.send_response(304)
            self.send_header("ETag", etag)
            self.end_headers()
            return None
        ctype = self.guess_type(path)
        encoding = None
        accepted = self.headers.get("Accept-Encoding", "")
        if "Range" not in self.headers:
            for token, ext in (("br", ".br"), ("gzip", ".gz")):
                if token in accepted and os.path.isfile(path + ext) and \
                        os.stat(path + ext).st_mtime >= st.st_mtime:
                    encoding, path = token, path + ext
                    break
        f = open(path, "rb")
        size = os.fstat(f.fileno()).st_size
        start, end = 0, size - 1
        status = 200
        spec = self.headers.get("Range", "")
        if spec.startswith("bytes=") and encoding is None:
            first, _, last = spec[6:].split(",")[0].partition("-")
            try:
                if first:
                    start, end = int(first), min(int(last) if last else size - 1, size - 1)
                else:
                    start, end = max(0, size - int(last)), size - 1
            except ValueError:
                start, end = 0, size - 1
            if start > end or start >= size:
                f.close()
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % size)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return None
            status = 206
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("ETag", etag)
        self.send_header("Last-Modified", email.utils.formatdate(st.st_mtime, usegmt=True))
        if encoding:
            self.send_header("Content-Encoding", encoding)
            self.send_header("Vary", "Accept-Encoding")
        if status == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        f.seek(start)
        self._remaining = end - start + 1
        return f

    def copyfile(self, source, outputfile):
        remaining = getattr(self, "_remaining", None)
        if remaining is None:
            return super().copyfile(source, outputfile)
        while remaining > 0:
            chunk = source.read(min(remaining, 1 << 20))
            if not chunk:
                break
            outputfile.write(chunk)
            remaining -= len(chunk)


def serve(out, port, host="127.0.0.1"):
    handler = functools.partial(IsolatedHandler, directory=str(out))
    with http.server.ThreadingHTTPServer((host, port), handler) as httpd:
        print("serving %s at http://%s:%d/" % (out, "localhost" if host == "127.0.0.1" else host, port),
              flush=True)
        httpd.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game-dir", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--web-build", action="append", default=[], metavar="ID=DIR")
    parser.add_argument("--game-files", action="append", default=[], metavar="ID=INSTALL",
                        help="publish an installation for streaming (private servers only)")
    parser.add_argument("--serve", type=int, nargs="?", const=8000, metavar="PORT")
    parser.add_argument("--host", default="127.0.0.1",
                        help="address to serve on; anything but localhost needs HTTPS in front "
                             "(e.g. tailscale serve) for the browser to run the game")
    args = parser.parse_args()
    games = build(args.game_dir, args.out)
    ids = {g["id"] for g in games}
    for spec in args.web_build:
        game_id, sep, build_dir = spec.partition("=")
        if not sep or game_id not in ids:
            parser.error("--web-build wants <game id>=<dir> for one of: %s" % ", ".join(sorted(ids)))
        copy_web_build(game_id, build_dir, args.out)
    for game_id in sorted(ids):
        if (Path(args.out) / game_id).is_dir():
            compress_web_build(Path(args.out) / game_id)
    by_id = {g["id"]: g for g in games}
    for spec in args.game_files:
        game_id, sep, install = spec.partition("=")
        if not sep or game_id not in ids:
            parser.error("--game-files wants <game id>=<install> for one of: %s" % ", ".join(sorted(ids)))
        publish_game_files(game_id, install, args.out, by_id[game_id]["exclude"])
    print("web launcher for %s in %s" % (", ".join(g["title"] for g in games), args.out))
    if args.serve is not None:
        serve(args.out, args.serve, args.host)


if __name__ == "__main__":
    main()
