// Stremio Plus libVLC input bridge.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int stremio_vlc_http_open(void* owner, const char* url, void** input, int64_t* size);
ssize_t stremio_vlc_input_read(void* input, unsigned char* data, size_t size);
int stremio_vlc_input_seek(void* input, uint64_t offset);
void stremio_vlc_input_close(void* input);

#ifdef __cplusplus
}
#endif
