#!/usr/bin/env python3
"""Production input bridge diagnostics with mock I/O and no VLC dependency."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / 'src/player_vlc.cpp').read_text()
types = source[source.index('struct InputOwner {'):source.index('int open_input(InputOwner*')]
callback = source[source.index('extern "C" ssize_t stremio_vlc_input_read('):
                  source.index('extern "C" int stremio_vlc_input_seek(')]
prefix = r'''
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <sys/types.h>
#define AVERROR(x) (-(x))
constexpr int AVERROR_EOF = -541478725, AVERROR_EXIT = -1414092869;
struct Player { struct Options { std::string url = "https://private.example/token"; }; };
struct AVIOContext { void* opaque; int (*read_packet)(void*, unsigned char*, int); };
struct GrowingFileStream {
 static GrowingFileStream* of(AVIOContext*) { return nullptr; }
 static void close_avio(AVIOContext**) {}
 void finish_opening() {}
 std::string failure() { return "Growing input failed"; }
};
struct TorrentStream {
 static TorrentStream* of(AVIOContext*) { return nullptr; }
 static bool is_ours(AVIOContext*) { return false; }
 static void close_avio(AVIOContext**) {}
 std::string failure() { return "Torrent input failed"; }
};
struct NetStream {
 static NetStream* of(AVIOContext*) { return nullptr; }
 static void close_avio(AVIOContext**) {}
 std::string failure() { return "HTTP input failed"; }
};
static std::vector<std::string> logs;
static void dlog(const char* format, ...) {
 char text[1024]; va_list args; va_start(args, format);
 vsnprintf(text, sizeof(text), format, args); va_end(args); logs.emplace_back(text);
}
'''
suffix = r'''
static unsigned checks, calls;
static int result;
static bool cancel_during_read;
static void check(bool value, const char* message) {
 ++checks; if (!value) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static int read_packet(void* opaque, unsigned char*, int size) {
 ++calls; check(size <= 256 * 1024, "bridge bounds read size");
 if (cancel_during_read) static_cast<InputOwner*>(opaque)->abort = true;
 return result;
}
int main() {
 unsigned char data[4]{};
 InputOwner owner;
 AVIOContext context{&owner, read_packet};
 Input input; input.owner = &owner; input.io = &context; input.strict = true;
 result = 4;
 check(stremio_vlc_input_read(&input, data, 4) == 4 && logs.empty(), "successful reads do not log");
 result = AVERROR_EOF;
 check(stremio_vlc_input_read(&input, data, 4) == 0 && stremio_vlc_input_read(&input, data, 4) == 0,
       "EOF preserves callback contract");
 check(logs.size() == 1 && logs.back().find("category=eof") != std::string::npos, "EOF logs once");
 result = AVERROR(EIO);
 check(stremio_vlc_input_read(&input, data, 4) == -1 && owner.fatal, "real I/O error remains fatal");
 stremio_vlc_input_read(&input, data, 4);
 check(logs.size() == 2 && logs.back().find("category=error") != std::string::npos, "I/O failure logs once");
 InputOwner closing; closing.cancel_reason = "stop_requested"; closing.abort = true;
 Input cancelled; cancelled.owner = &closing; cancelled.io = &context;
 const unsigned before = calls;
 check(stremio_vlc_input_read(&cancelled, data, 4) == -1 && calls == before && !closing.fatal,
       "requested stop bypasses I/O and cannot become an input failure");
 stremio_vlc_input_read(&cancelled, data, 4);
 check(logs.size() == 3 && logs.back().find("reason=stop_requested") != std::string::npos,
       "requested cancellation logs once with its fixed reason");
 InputOwner during; during.cancel_reason = "decoder_recovery";
 context.opaque = &during;
 Input blocked; blocked.owner = &during; blocked.io = &context; blocked.strict = true;
 result = AVERROR_EXIT; cancel_during_read = true;
 check(stremio_vlc_input_read(&blocked, data, 4) == -1 && !during.fatal && during.failure().empty(),
       "a cancelled blocked read cannot report a fatal source error");
 check(logs.size() == 4 && logs.back().find("abort=1") != std::string::npos &&
       logs.back().find("reason=decoder_recovery") != std::string::npos, "blocked-read cancellation is distinguished");
 for (const auto& line : logs)
  check(line.find("private.example") == std::string::npos && line.find("https://") == std::string::npos,
        "input location never enters diagnostics");
 printf("{\"result\":\"PASS\",\"checks\":%u}\n", checks);
}
'''
folder = root / 'build/tests/vlc-input-diagnostics'
folder.mkdir(parents=True, exist_ok=True)
harness = folder / 'harness.cpp'
harness.write_text(prefix + types + callback + suffix)
binary = folder / 'test'
subprocess.run([os.environ.get('HOST_CXX', 'clang++-18'), '-std=c++20', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
                str(harness), '-o', str(binary)], check=True)
run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10,
                     env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))
print(run.stdout, end='')
if run.returncode: print(run.stderr)
run.check_returncode()
report = json.loads(run.stdout)
report.update(scope='Production InputOwner/Input/read callback extracted unchanged, with mocked source I/O',
              limits='Does not reproduce VLC demuxer seeking or real torrent starvation.',
              sourceSha256=hashlib.sha256((root / 'src/player_vlc.cpp').read_bytes()).hexdigest())
(folder / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
