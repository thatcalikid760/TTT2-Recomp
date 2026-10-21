// Decode an XMA2 RIFF file packet by packet with the port's minimal FFmpeg build and compare the
// 16-bit PCM against a reference produced by a full FFmpeg (ffmpeg -i x.xma -f s16le ref.pcm).
//   xma_decode_test <file.xma> <ref.pcm>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

static uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
static uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: xma_decode_test file.xma ref.pcm\n"); return 2; }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    std::ifstream rf(argv[2], std::ios::binary);
    std::vector<uint8_t> refBytes((std::istreambuf_iterator<char>(rf)), {});
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0) { std::fprintf(stderr, "FAIL not RIFF\n"); return 1; }
    const uint8_t* fmt = nullptr; uint32_t fmtSize = 0;
    const uint8_t* data = nullptr; uint32_t dataSize = 0;
    for (size_t o = 12; o + 8 <= d.size();)
    {
        const uint32_t size = Le32(&d[o + 4]);
        if (!std::memcmp(&d[o], "fmt ", 4)) { fmt = &d[o + 8]; fmtSize = size; }
        if (!std::memcmp(&d[o], "data", 4)) { data = &d[o + 8]; dataSize = size; }
        o += 8 + size + (size & 1);
    }
    if (!fmt || fmtSize < 18 + 34 || Le16(fmt) != 0x166 || !data) { std::fprintf(stderr, "FAIL not XMA2\n"); return 1; }
    const int channels = Le16(fmt + 2), rate = int(Le32(fmt + 4));

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    if (!codec) { std::fprintf(stderr, "FAIL no xma2 decoder\n"); return 1; }
    AVCodecContext* c = avcodec_alloc_context3(codec);
    av_channel_layout_default(&c->ch_layout, channels);
    c->sample_rate = rate;
    c->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(c->extradata, fmt + 18, 34);
    c->extradata_size = 34;
    if (avcodec_open2(c, codec, nullptr) < 0) { std::fprintf(stderr, "FAIL avcodec_open2\n"); return 1; }

    std::vector<int16_t> pcm;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    auto drain = [&] {
        while (avcodec_receive_frame(c, frame) == 0)
        {
            for (int s = 0; s < frame->nb_samples; ++s)
                for (int ch = 0; ch < channels; ++ch)
                {
                    const float v = reinterpret_cast<const float*>(frame->extended_data[ch])[s];
                    const long q = std::lrintf(v * 32768.0f);
                    pcm.push_back(int16_t(q > 32767 ? 32767 : q < -32768 ? -32768 : q));
                }
        }
    };
    for (uint32_t o = 0; o + 2048 <= dataSize; o += 2048)
    {
        av_new_packet(pkt, 2048);
        std::memcpy(pkt->data, data + o, 2048);
        if (avcodec_send_packet(c, pkt) < 0) { std::fprintf(stderr, "FAIL send_packet at %u\n", o); return 1; }
        av_packet_unref(pkt);
        drain();
    }
    avcodec_send_packet(c, nullptr);
    drain();

    if (argc > 3)
    {
        std::ofstream o(argv[3], std::ios::binary);
        o.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(pcm.size() * 2));
    }
    const size_t refSamples = refBytes.size() / 2;
    const int16_t* ref = reinterpret_cast<const int16_t*>(refBytes.data());
    const size_t n = std::min(refSamples, pcm.size());
    size_t mismatches = 0; int worst = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const int diff = std::abs(int(pcm[i]) - int(ref[i]));
        worst = std::max(worst, diff);
        mismatches += diff > 1;
    }
    std::printf("decoded %zu samples (reference %zu), compared %zu, >1 LSB mismatches %zu, worst %d\n",
                pcm.size(), refSamples, n, mismatches, worst);
    return (n && n + 2048 >= refSamples && mismatches == 0) ? 0 : 1;
}
