// Independent AVFrame crop check against the same pinned FFmpeg static libraries.
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/sha.h>
#include <libavutil/mem.h>
}
#include <cstdio>
#include <stdexcept>
#include <string>

static std::string digest(const AVFrame* f, int w, int h) {
    AVSHA* sha = av_sha_alloc(); av_sha_init(sha, 256);
    for (int p = 0; p < 3; ++p) {
        int width = p ? (w + 1) / 2 : w, height = p ? (h + 1) / 2 : h;
        if (f->linesize[p] < width) throw std::runtime_error("bad pitch");
        for (int y = 0; y < height; ++y) av_sha_update(sha, f->data[p] + y * f->linesize[p], width);
    }
    unsigned char sum[32]; av_sha_final(sha, sum); av_free(sha);
    char text[65]; for (int i = 0; i < 32; ++i) snprintf(text + i * 2, 3, "%02x", sum[i]);
    return text;
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    AVFormatContext* f = nullptr;
    if (avformat_open_input(&f, argv[1], nullptr, nullptr) < 0 || avformat_find_stream_info(f, nullptr) < 0) return 3;
    const AVCodec* codec = nullptr; int track = av_find_best_stream(f, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    AVCodecContext* c = avcodec_alloc_context3(codec);
    if (track < 0 || avcodec_parameters_to_context(c, f->streams[track]->codecpar) < 0) return 4;
    c->thread_count = 2;
    c->apply_cropping = 0;
    if (avcodec_open2(c, codec, nullptr) < 0) return 5;
    AVPacket* packet = av_packet_alloc(); AVFrame* frame = av_frame_alloc(); unsigned count = 0;
    auto receive = [&] {
        while (avcodec_receive_frame(c, frame) == 0) {
            if (frame->format != AV_PIX_FMT_YUV420P) throw std::runtime_error("wrong reference format");
            const auto before = digest(frame, c->width, c->height);
            AVFrame* crop = av_frame_clone(frame);
            if (av_frame_apply_cropping(crop, AV_FRAME_CROP_UNALIGNED) < 0) throw std::runtime_error("crop failed");
            const auto after = digest(crop, crop->width, crop->height);
            printf("{\"frame\":%u,\"ffmpeg\":\"%s\",\"context_visible\":[%d,%d],\"avframe\":[%d,%d],"
                "\"remaining_crop\":[%zu,%zu,%zu,%zu],\"actual_crop_size\":[%d,%d],\"without_remaining_crop\":\"%s\",\"with_remaining_crop\":\"%s\"}\n",
                count++, av_version_info(), c->width, c->height, frame->width, frame->height,
                frame->crop_left, frame->crop_top, frame->crop_right, frame->crop_bottom,
                crop->width, crop->height, before.c_str(), after.c_str());
            av_frame_free(&crop); av_frame_unref(frame);
        }
    };
    while (av_read_frame(f, packet) >= 0) {
        if (packet->stream_index == track && avcodec_send_packet(c, packet) >= 0) receive();
        av_packet_unref(packet);
    }
    avcodec_send_packet(c, nullptr); receive();
    av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&c); avformat_close_input(&f);
    return count >= 60 ? 0 : 6;
}
