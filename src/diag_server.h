// A small read-only HTTP server for testing from a computer on the same
// network: the app's log, its events, and a status page. Off unless Settings
// turns it on. Plain HTTP on one port, no authentication: a home network
// convenience, not a public service.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace diag_server {

constexpr int kDefaultPort = 9029;

// Starts (or keeps) the server for the log directory; stops it when
// `enabled` is false. Safe to call every frame.
void configure(bool enabled, const std::string& log_directory, int port = kDefaultPort);
void stop();
bool running();
int port();

// The JSON text GET /status returns; replaced whole, from the main thread.
void set_status(std::string json);

}  // namespace diag_server
