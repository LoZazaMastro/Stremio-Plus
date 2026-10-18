#!/usr/bin/env python3
"""Test real asynchronous App flows against a controlled loopback addon.

Build the host target first, then run this script. It links the existing
production objects with test_app_addons.cpp as the only main function.
No product algorithms, player classes or Tasks callbacks are replaced by mocks.
The harness does not call App::init and never opens a decoder, account or torrent.

    cmake --build build-host -j2
    python tests/run_app_addon_tests.py --build-dir build-host

The build must be complete: mixing objects compiled against different App/Player
headers would invalidate the test, so do not run it concurrently with a rebuild.
"""
import argparse
import http.server
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import time
from urllib.parse import parse_qs, unquote, urlsplit


class FixtureServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address):
        super().__init__(address, Handler)
        self.events_lock = threading.Lock()
        self.requests = []
        self.completed = []

    def handle_error(self, request, client_address):
        # Expected when App cancels a delayed fixture during navigation.
        pass


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, value, status=200):
        body = json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    @property
    def base(self):
        return f"http://127.0.0.1:{self.server.server_port}"

    def do_GET(self):
        parsed = urlsplit(self.path)
        if parsed.path == "/app/reset":
            with self.server.events_lock:
                self.server.requests.clear()
                self.server.completed.clear()
            self.reply({"ok": True})
            return
        if parsed.path == "/app/requests":
            with self.server.events_lock:
                result = {"requests": list(self.server.requests), "completed": list(self.server.completed)}
            self.reply(result)
            return
        with self.server.events_lock:
            self.server.requests.append(self.path)
        try:
            self.provider_response(parsed)
        finally:
            with self.server.events_lock:
                self.server.completed.append(self.path)

    def provider_response(self, parsed):
        parts = parsed.path.strip("/").split("/")
        if len(parts) < 5 or parts[0] != "app" or parsed.query != "opaque=A%2BB":
            self.reply({"error": {"code": 400}}, 400)
            return
        route, resource, content_type = parts[1:4]
        identifier = unquote(parts[4][:-5] if parts[4].endswith(".json") else parts[4])
        extras = parse_qs(parts[5][:-5], keep_blank_values=True) if len(parts) > 5 and parts[5].endswith(".json") else {}
        if route == "meta" and resource == "meta":
            if identifier == "meta:slow":
                time.sleep(0.35)
            meta = {"id": identifier, "type": content_type, "name": "Full " + identifier}
            if identifier in ("kitsu:inline", "kitsu:empty"):
                video_id = "opaque/video:+%?1"
                streams = [{"name": "Inline stream", "url": self.base + "/never-play"}] if identifier == "kitsu:inline" else []
                meta.update({
                    "behaviorHints": {"defaultVideoId": video_id},
                    "videos": [
                        {"id": video_id, "title": "Episode one", "season": 1, "episode": 1, "streams": streams},
                        {"id": "opaque/video:two", "title": "Episode two", "season": 1, "episode": 2},
                    ],
                })
            elif content_type == "tv":
                meta.update({"behaviorHints": {"defaultVideoId": "programme:now", "hasScheduledVideos": True},
                             "videos": [{"id": "programme:now", "title": "Programme"}]})
            else:
                video_id = "opaque/absent:+?" if identifier == "kitsu:absent" else "video:" + identifier
                meta["videos"] = [{"id": video_id, "title": "Video"}]
            self.reply({"meta": meta})
        elif route in ("slow", "fast") and resource == "stream":
            time.sleep(0.30 if route == "slow" else 0.02)
            if route == "slow" and identifier == "provider-failure":
                self.reply({"error": {"code": 503}}, 503)
                return
            streams = [
                {"name": route + " first · 720p", "description": "First configured preference", "fixtureId": identifier,
                 "url": self.base + "/never-play?source=" + route + "1"},
                {"name": route + " second · 4K", "description": "Second configured preference", "fixtureId": identifier,
                 "url": self.base + "/never-play?source=" + route + "2"},
            ]
            if identifier == "ambiguous:identity":
                for index, stream in enumerate(streams):
                    stream["name"] = f"Shared result {index}"
                    stream["url"] = self.base + f"/shared-source?index={index}"
                    stream["behaviorHints"] = {"proxyHeaders": {"request": {"X-Fixture-Provider": route}}}
            self.reply({"streams": streams})
        elif route == "external" and resource == "stream":
            self.reply({"streams": [{"name": "External fixture", "externalUrl": self.base + "/external-destination"}]})
        elif route == "catalog" and resource == "catalog":
            self.catalog_response(identifier, extras)
        elif route == "subtitles" and resource == "subtitles":
            valid = (identifier == "opaque/episode:one" and extras.get("videoSize") == ["50000000000"]
                     and extras.get("filename") == ["Episode & 1.mkv"] and extras.get("videoHash"))
            if not valid:
                self.reply({"error": {"code": 400}}, 400)
                return
            self.reply({"subtitles": [
                {"id": "eng-one", "lang": "eng", "label": "English", "url": self.base + "/eng.srt"},
                {"id": "it-one", "lang": "it-IT", "label": "Italiano alternativo [CC]", "url": self.base + "/ita.srt"},
            ]})
        else:
            self.reply({"error": {"code": 404}}, 404)

    def catalog_response(self, identifier, extras):
        if identifier == "my/list":
            if extras.get("region") not in (["IT"], ["US"]):
                self.reply({"error": {"code": 400}}, 400)
                return
            skip = extras.get("skip", ["0"])[0]
            if skip == "0":
                metas = [{"id": "opaqueA", "type": "other", "name": "A"}, {"name": "Missing ID"},
                         {"id": "opaqueA", "type": "other", "name": "Duplicate A"}, {"id": "opaqueB", "name": "B"}]
            elif skip == "4":
                metas = [{"id": "opaqueB", "type": "other", "name": "Duplicate B"},
                         {"id": "opaqueA", "type": "custom", "name": "Distinct typed A"},
                         {"id": "opaqueC", "type": "other", "name": "C"}]
            elif skip == "7":
                metas = []
            else:
                self.reply({"error": {"code": 416}}, 416)
                return
            self.reply({"metas": metas})
        elif identifier == "no-paging":
            if "skip" in extras:
                self.reply({"error": {"code": 400}}, 400)
                return
            self.reply({"metasDetailed": [{"id": "single", "type": "other", "name": "Single result",
                "videos": [{"id": "single:video", "title": "Single video",
                            "streams": [{"name": "Catalog inline", "url": self.base + "/never-play"}]}]}]})
        elif identifier == "free-filter":
            if not extras.get("collection"):
                self.reply({"error": {"code": 400}}, 400)
                return
            self.reply({"metas": [{"id": "custom", "type": "other", "name": extras["collection"][0]}]})
        elif identifier == "search-list":
            if not extras.get("search") or extras.get("region") != ["IT"]:
                self.reply({"error": {"code": 400}}, 400)
                return
            if extras["search"] == ["old search"]:
                time.sleep(0.35)
            self.reply({"metas": [{"id": "search-result", "type": "other", "name": extras["search"][0]}]})
        else:
            self.reply({"error": {"code": 404}}, 404)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default="build-host", help="Completed CMake host build directory")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    build_dir = Path(args.build_dir).resolve()
    link_file = build_dir / "CMakeFiles/stremio.dir/link.txt"
    if not link_file.exists():
        parser.error("Host objects are missing. Build the CMake host target first.")
    original = shlex.split(link_file.read_text())
    command = []
    skip_next = False
    for token in original:
        if skip_next:
            skip_next = False
            continue
        if token == "-o":
            skip_next = True
            continue
        if token.endswith("/src/main.cpp.o"):
            continue
        if token.endswith(".o") and not (build_dir / token).exists():
            parser.error(f"Incomplete host build: missing {token}")
        command.append(token)
    with tempfile.TemporaryDirectory(prefix="stremio-app-addon-tests-") as temp:
        obj = Path(temp) / "test_app_addons.o"
        executable = Path(temp) / "test_app_addons"
        sdl_flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "sdl2"], text=True))
        subprocess.run([command[0], "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-pthread",
                        "-I" + str(project / "src"), "-I" + str(project / "third_party"), *sdl_flags,
                        "-c", str(project / "tests/test_app_addons.cpp"), "-o", str(obj)], check=True)
        subprocess.run([*command, str(obj), "-o", str(executable)], cwd=build_dir, check=True)
        server = FixtureServer(("127.0.0.1", 0))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            subprocess.run([str(executable), f"http://127.0.0.1:{server.server_port}"], check=True, timeout=90)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    main()
