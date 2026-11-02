/* Production Dolby Vision profile decisions and HEVC NAL filtering. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps5vdec_dovi_under_test.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

typedef struct { uint8_t bytes[128]; size_t bit; } Writer;
static void put(Writer *w, unsigned v, unsigned n)
{
    for (unsigned i = n; i; --i) {
        if ((v >> (i - 1)) & 1) w->bytes[w->bit / 8] |= 1u << (7 - (w->bit & 7));
        ++w->bit;
    }
}
static void ue(Writer *w, unsigned v)
{
    ++v;
    unsigned n = 0;
    for (unsigned t = v; t > 1; t >>= 1) ++n;
    put(w, 0, n); put(w, v, n + 1);
}
static size_t rpu(uint8_t *out, unsigned profile, bool full, unsigned depth,
                  bool resample, bool disable, unsigned coefficient, unsigned denominator)
{
    Writer w = {0};
    put(&w, 2, 6); put(&w, 18, 11); /* type / format */
    put(&w, profile, 4); put(&w, 0, 4); put(&w, 1, 1); /* level / sequence */
    put(&w, 0, 1); put(&w, coefficient, 2);
    if (!coefficient) ue(&w, denominator);
    put(&w, 1, 2); put(&w, full, 1);
    ue(&w, 2); ue(&w, 2); ue(&w, depth);
    put(&w, 0, 1); put(&w, 0, 3); put(&w, resample, 1); put(&w, disable, 1);
    put(&w, 1, 1); /* rbsp stop bit for the synthetic header */
    out[0] = 124; out[1] = 1; out[2] = 25;
    size_t count = 3;
    unsigned zeros = 0;
    for (size_t i = 0; i < (w.bit + 7) / 8; ++i) {
        uint8_t b = w.bytes[i];
        if (zeros >= 2 && b <= 3) { out[count++] = 3; zeros = 0; }
        out[count++] = b;
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return count;
}

static uint32_t rng = 0x74126;
static unsigned random_byte(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng & 255;
}

int main(void)
{
    uint8_t nal[256];
    size_t n = rpu(nal, 0, true, 4, false, true, 0, 23);
    CHECK(dv_rpu_profile(nal, n) == 5);
    for (size_t cut = 0; cut + 1 < n; ++cut) CHECK(dv_rpu_profile(nal, cut) == 0);
    n = rpu(nal, 1, false, 4, true, false, 0, 23);
    CHECK(dv_rpu_profile(nal, n) == 7);
    n = rpu(nal, 1, false, 4, false, true, 0, 23);
    CHECK(dv_rpu_profile(nal, n) == 8);
    n = rpu(nal, 1, false, 4, false, true, 1, 0);
    CHECK(dv_rpu_profile(nal, n) == 8);
    n = rpu(nal, 1, false, 2, true, false, 0, 23); /* profile 4 */
    CHECK(dv_rpu_profile(nal, n) == 0);
    n = rpu(nal, 0, false, 4, false, true, 0, 23);
    CHECK(dv_rpu_profile(nal, n) == 0);
    n = rpu(nal, 2, false, 4, false, true, 0, 23);
    CHECK(dv_rpu_profile(nal, n) == 0);
    n = rpu(nal, 1, false, 4, false, true, 2, 0);
    CHECK(dv_rpu_profile(nal, n) == 0);
    n = rpu(nal, 1, false, 4, false, true, 0, 12);
    CHECK(dv_rpu_profile(nal, n) == 0);
    n = rpu(nal, 1, false, 4, false, true, 0, 33);
    CHECK(dv_rpu_profile(nal, n) == 0);
    n = rpu(nal, 1, false, 4, false, true, 0, 23);
    uint8_t prefix = nal[2]; nal[2] = 0; CHECK(dv_rpu_profile(nal, n) == 0); nal[2] = prefix;
    nal[1] = 0; CHECK(dv_rpu_profile(nal, n) == 0); nal[1] = 1;
    nal[0] |= 0x80; CHECK(dv_rpu_profile(nal, n) == 0); nal[0] &= 0x7f;
    uint8_t short_nal[] = { 124, 1, 25 };
    CHECK(dv_rpu_profile(short_nal, sizeof(short_nal)) == 0);
    uint8_t invalid_escape[] = { 124, 1, 25, 0, 0, 3, 4 };
    CHECK(dv_rpu_profile(invalid_escape, sizeof(invalid_escape)) == 0);
    uint8_t long_zeroes[128] = { 124, 1, 25 };
    CHECK(dv_rpu_profile(long_zeroes, sizeof(long_zeroes)) == 0);

    /* A profile-7 length-prefixed packet must report its EL before filtering. */
    uint8_t packet[512] = { 0,0,0,3,2,1,85, 0,0,0,3,2,9,68 };
    size_t p = 14;
    packet[p++] = 0; packet[p++] = 0; packet[p++] = 0; packet[p++] = (uint8_t)n;
    memcpy(packet + p, nal, n); p += n;
    uint8_t annex[1024]; bool seen = false;
    size_t a = to_annexb(annex, sizeof(annex), packet, p, 4, true, &seen);
    CHECK(seen);
    size_t rlen = 0;
    const uint8_t *found = hevc_find_rpu(annex, a, &rlen);
    CHECK(found && rlen == n && dv_rpu_profile(found, rlen) == 8);
    a = hevc_base_layer(annex, a, &seen);
    const uint8_t wanted[] = { 0,0,0,1,2,1,85 };
    CHECK(a == sizeof(wanted) && !memcmp(annex, wanted, a));
    CHECK(hevc_find_rpu(annex, a, &rlen) == NULL);

    /* Filtering should preserve every selected byte with both start-code lengths. */
    for (unsigned test = 0; test < 10000; ++test) {
        uint8_t data[4096], expected[4096];
        size_t used = 0, kept = 0;
        bool expected_layers = false;
        unsigned count = 1 + random_byte() % 30;
        for (unsigned i = 0; i < count; ++i) {
            uint8_t unit[100];
            unsigned type = random_byte() % 64, layer = random_byte() % 3;
            size_t sc = 3 + random_byte() % 2;
            memset(unit, 0, sc); unit[sc - 1] = 1;
            unit[sc] = type << 1; unit[sc + 1] = (layer << 3) | 1;
            size_t bytes = sc + 2 + random_byte() % 60;
            for (size_t j = sc + 2; j < bytes; ++j) unit[j] = 1 + random_byte() % 255;
            memcpy(data + used, unit, bytes); used += bytes;
            if (layer) expected_layers = true;
            if (!layer && type != 62 && type != 63) {
                memcpy(expected + kept, unit, bytes); kept += bytes;
            }
        }
        seen = false;
        a = hevc_base_layer(data, used, &seen);
        CHECK(a == kept && !memcmp(data, expected, kept) && seen == expected_layers);
    }
    /* Bounds and bit-reader termination on adversarial/truncated RPUs. */
    for (unsigned test = 0; test < 20000; ++test) {
        n = random_byte();
        for (size_t i = 0; i < n; ++i) nal[i] = random_byte();
        if (n >= 3) { nal[0] = 124; nal[1] = 1; nal[2] = 25; }
        unsigned profile = dv_rpu_profile(nal, n);
        CHECK(profile == 0 || profile == 5 || profile == 7 || profile == 8);
    }
    printf("PS5 Dolby Vision helpers: checks=%u PASS (ASan, UBSan)\n", checks);
    return 0;
}
