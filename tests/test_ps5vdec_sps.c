/* Exercise production colour recovery with VLC's actual HEVC SPS parser. */
#include <vlc_common.h>
#include "hevc_nal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps5vdec_sps_under_test.h"

static unsigned checks, cases;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
typedef struct { uint8_t data[1024]; size_t bits; } Bits;
static void put(Bits *b, uint64_t value, unsigned count)
{
    CHECK(b->bits + count <= sizeof(b->data) * 8);
    for (unsigned i=count; i; --i,++b->bits)
        b->data[b->bits/8] |= ((value>>(i-1))&1u) << (7-b->bits%8);
}
static void ue(Bits *b, unsigned value)
{
    unsigned n=0, v=value+1;
    for (unsigned t=v;t>1;t>>=1)++n;
    put(b,0,n); put(b,v,n+1);
}
static size_t sps(uint8_t *out, bool vui, bool description,
                  unsigned primaries, unsigned transfer, unsigned matrix, bool full)
{
    Bits b={0};
    put(&b,0,4); put(&b,0,3); put(&b,1,1); /* one temporal layer */
    put(&b,0,2); put(&b,0,1); put(&b,2,5); /* Main10 */
    put(&b,0x20000000,32); put(&b,9,4); put(&b,0,44); put(&b,153,8);
    ue(&b,0); ue(&b,1); ue(&b,3840); ue(&b,1608); put(&b,0,1);
    ue(&b,2); ue(&b,2); ue(&b,4); put(&b,0,1);
    ue(&b,5); ue(&b,2); ue(&b,0); /* DPB, reorder and latency */
    ue(&b,0); ue(&b,3); ue(&b,0); ue(&b,3); ue(&b,0); ue(&b,0);
    put(&b,0,1); put(&b,1,1); put(&b,1,1); put(&b,0,1);
    ue(&b,0); put(&b,0,1); put(&b,1,1); put(&b,1,1); put(&b,vui,1);
    if(vui) {
        put(&b,0,1); put(&b,0,1); put(&b,1,1); /* aspect, overscan, signal */
        put(&b,5,3); put(&b,full,1); put(&b,description,1);
        if(description) { put(&b,primaries,8); put(&b,transfer,8); put(&b,matrix,8); }
        put(&b,0,1); put(&b,0,1); put(&b,0,1); put(&b,0,1);
        put(&b,0,1); put(&b,0,1); put(&b,0,1);
    }
    put(&b,0,1); put(&b,1,1); /* extension absent and RBSP stop */
    size_t n=0; unsigned zeros=0;
    out[n++]=0; out[n++]=0; out[n++]=0; out[n++]=1;
    out[n++]=0x42; out[n++]=1;
    for(size_t i=0;i<(b.bits+7)/8;++i) {
        if(zeros>=2 && b.data[i]<=3) { out[n++]=3; zeros=0; }
        out[n++]=b.data[i]; zeros=b.data[i]==0 ? zeros+1 : 0;
    }
    return n;
}
static video_format_t unknown(void)
{
    video_format_t v={0};
    v.primaries=COLOR_PRIMARIES_UNDEF; v.transfer=TRANSFER_FUNC_UNDEF; v.space=COLOR_SPACE_UNDEF;
    return v;
}
static void unchanged(video_format_t before, const uint8_t *data, size_t size)
{
    video_format_t v=before;
    CHECK(!hevc_recover_colorimetry(&v,data,size));
    CHECK(v.primaries==before.primaries && v.transfer==before.transfer && v.space==before.space);
    CHECK(v.b_color_range_full==before.b_color_range_full);
    ++cases;
}
int main(void)
{
    uint8_t data[40000];
    size_t n=sps(data,true,true,9,16,9,false);
    video_format_t v=unknown();
    CHECK(hevc_recover_colorimetry(&v,data,n));
    CHECK(v.primaries==COLOR_PRIMARIES_BT2020 && v.transfer==TRANSFER_FUNC_SMPTE_ST2084 && v.space==COLOR_SPACE_BT2020);
    CHECK(!v.b_color_range_full); ++cases;
    unchanged(v,data,n); /* Once complete, no reparsing or override. */
    v=unknown(); v.transfer=TRANSFER_FUNC_BT709;
    CHECK(hevc_recover_colorimetry(&v,data,n));
    CHECK(v.transfer==TRANSFER_FUNC_BT709 && v.space==COLOR_SPACE_BT2020); ++cases;
    v=unknown(); n=sps(data,true,true,9,18,9,true);
    CHECK(hevc_recover_colorimetry(&v,data,n));
    CHECK(v.transfer!=TRANSFER_FUNC_SMPTE_ST2084 && v.b_color_range_full); ++cases;
    v=unknown(); n=sps(data,true,true,1,1,1,false);
    CHECK(hevc_recover_colorimetry(&v,data,n));
    CHECK(v.primaries==COLOR_PRIMARIES_BT709 && v.transfer==TRANSFER_FUNC_BT709 && v.space==COLOR_SPACE_BT709); ++cases;
    n=sps(data,false,false,0,0,0,false); unchanged(unknown(),data,n);
    n=sps(data,true,false,0,0,0,false); unchanged(unknown(),data,n);
    n=sps(data,true,true,2,2,2,false); unchanged(unknown(),data,n);
    n=sps(data,true,true,9,16,9,false);
    unchanged(unknown(),data+4,n-4); /* A raw NAL is not an AnnexB AU. */
    for(unsigned header=0;header<4;++header) {
        uint8_t saved0=data[4],saved1=data[5];
        if(header==0)data[4]|=0x80; /* forbidden bit */
        if(header==1)data[4]|=1; /* enhancement layer */
        if(header==2)data[5]|=8; /* enhancement layer */
        if(header==3)data[5]=0; /* invalid temporal_id_plus1 */
        unchanged(unknown(),data,n); data[4]=saved0; data[5]=saved1;
    }
    v=unknown(); CHECK(hevc_recover_colorimetry(&v,data+1,n-1)); ++cases; /* 3-byte start */
    const uint8_t following[]={0,0,0,1,0x26,1,0x80};
    memcpy(data+n,following,sizeof(following)); v=unknown();
    CHECK(hevc_recover_colorimetry(&v,data,n+sizeof(following)) && v.transfer==TRANSFER_FUNC_SMPTE_ST2084); ++cases;
    for(size_t i=0;i<n;++i) {
        v=unknown(); (void)hevc_recover_colorimetry(&v,data,i);
        if(i<16)CHECK(v.transfer==TRANSFER_FUNC_UNDEF);
        ++cases;
    }
    memset(data+n,0,33000-n); unchanged(unknown(),data,33000);
    printf("PASS\t%u\t%u\n",cases,checks);
    return 0;
}
