#pragma once

#include <cstdarg>
#include <string>
#include "json.hpp"
#include "diagnostics_native.h"

using json = nlohmann::json;

// All files remain directly in log_directory; no extra subfolder is appended.
// The native title uses /data/Stremio. Call start once before worker creation,
// and stop after the player/network/renderer workers have been shut down.
bool diagnostics_start(const std::string& log_directory);
void diagnostics_stop();
void diagnostics_note(const std::string& category, const std::string& message);
void diagnostics_set_runtime(const json& context);
bool diagnostics_flush(unsigned timeout_ms = 5000); // worker/test use, never per-frame

// Used by every output sink. No raw URL paths, auth headers or account tokens
// are written to disk, stdout, or the console's kernel log.
std::string diagnostics_redact(const std::string& message);

// Optional FFmpeg callback; install with av_log_set_callback. Debug/trace
// verbosity is discarded and the remaining messages use the same safe sink.
void diagnostics_ffmpeg_log(void* context, int level, const char* format, va_list arguments);
