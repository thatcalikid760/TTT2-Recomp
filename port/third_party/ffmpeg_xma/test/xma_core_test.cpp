// XMA context emulation (port/src/runtime/kernel/xma_core.h) driven the way TTT2 drives the
// hardware, against PCM from an independent full FFmpeg (ffmpeg -i x.xma -f s16le ref.pcm):
//   xma_core_test <file.xma> <ref.pcm> [--negative-control]
// A simulated title double-buffers the stream two packets at a time through input buffers 0/1,
// consumes the output ring after every kick (wrapping, re-validating it when full), and then
// starts the same sound again on the same context after resetting it as sub_82D3E488 does.
// Checks: one 512-sample frame per channel for every frame in the stream (the hardware's
// output, which TTT2's NUSound counts to end a one-shot); ring PCM == reference after the first
// frame and the 64-sample encoder delay, both of which the reference drops; the
// second play repeats the first exactly, valid bits clear and current_buffer swaps; a third
// play from one buffer loops the whole stream twice (frame-granular loop region).
// Before those, the context plays the stream briefly at another declared sample rate, as a
// reused game voice does. Negative control: the simulated title never re-validates a full
// ring; the stream stalls and the comparison must fail.
#include "xma_core.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

static uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
static uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

static std::vector<uint8_t> g_mem;
static uint32_t Rd(uint32_t a) { uint32_t v; std::memcpy(&v, &g_mem[a], 4); return __builtin_bswap32(v); }
static void Wr(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(&g_mem[a], &v, 4); }

// Bit position (buffer coordinates, packet headers included) where the stream's last frame
// starts, and the number of frames: walks the length-prefixed frames independently of
// xma_core.h.
static uint32_t LastFrameStart(const uint8_t* s, uint32_t packets, uint32_t* frames = nullptr)
{
    constexpr uint32_t PAYLOAD = xma::kPayloadBits;
    auto at = [&](uint64_t p) { return uint32_t(p / PAYLOAD * xma::kPacketBits + 32 + p % PAYLOAD); };
    auto bit = [&](uint64_t p) { const uint32_t b = at(p); return (s[b >> 3] >> (7 - (b & 7))) & 1u; };
    auto cont = [&](uint64_t packet) { return xma::PacketContinuation(s + size_t(packet) * xma::kPacketBytes); };
    const uint64_t total = uint64_t(packets) * PAYLOAD;
    uint64_t p = cont(0), last = p;
    if (frames) *frames = 0;
    while (p + 15 <= total)
    {
        const uint64_t packet = p / PAYLOAD;
        uint32_t length = 0;
        for (int i = 0; i < 15; ++i) length = (length << 1) | bit(p + i);
        const bool straddlesIntoPadding = PAYLOAD - p % PAYLOAD < 15 && (packet + 1 >= packets || !cont(packet + 1));
        if (length == 0x7FFF || length <= 15 || straddlesIntoPadding)
        {
            if (packet + 1 >= packets) break;
            p = (packet + 1) * PAYLOAD + cont(packet + 1);
            continue;
        }
        if (p + length > total) break;
        last = p;
        p += length;
        if (frames) ++*frames;
    }
    return at(last);
}

static unsigned checks = 0, failures = 0;
#define CHECK(cond, ...) do { if (cond) ++checks; else { ++failures; std::fprintf(stderr, "FAIL " __VA_ARGS__); std::fprintf(stderr, "\n"); } } while (0)

// Replay a context captured in game (xma_core.h writes xma_error_ctx<N>.bin under TTT2_XMA_LOG):
// 64 context bytes, then per input buffer a packet count and its packets. Plays it from the
// stream start with the captured loop setup for `kicks` kicks.
static int Replay(const char* path, int kicks)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 72) { std::fprintf(stderr, "FAIL short dump\n"); return 2; }
    uint32_t n0; std::memcpy(&n0, &d[64], 4);
    av_log_set_level(AV_LOG_QUIET);
    constexpr uint32_t ARRAY = 0x1000, OUT = 0x30000, IN0 = 0x100000;
    g_mem.assign(IN0 + size_t(n0) * xma::kPacketBytes, 0);
    std::memcpy(&g_mem[IN0], &d[68], size_t(n0) * xma::kPacketBytes);
    xma::Decoder dec;
    dec.Attach(g_mem.data(), ARRAY);
    const uint32_t c = dec.Allocate();
    std::memcpy(&g_mem[c], d.data(), 64);
    uint32_t dw0 = Rd(c);
    dw0 = xma::WithBits(dw0, 0, 12, n0);
    dw0 = xma::WithBits(dw0, 20, 2, 1);
    dw0 = xma::WithBits(dw0, 27, 5, 0);
    Wr(c, dw0);
    Wr(c + 4, xma::WithBits(xma::WithBits(Rd(c + 4), 0, 12, 0), 31, 1, 1));
    Wr(c + 8, 32);
    Wr(c + 16, xma::WithBits(Rd(c + 16), 31, 1, 0));
    Wr(c + 20, IN0); Wr(c + 24, 0); Wr(c + 28, OUT); Wr(c + 36, 0);
    for (int k = 0; k < kicks; ++k)
    {
        dec.Kick(c);
        Wr(c + 36, xma::WithBits(Rd(c + 36), 0, 5, xma::Bits(Rd(c), 27, 5)));
        Wr(c + 4, xma::WithBits(Rd(c + 4), 31, 1, 1));
    }
    const auto& st = dec.GetStats();
    std::printf("replay %s: %u packets, loop %X..%X x%u: frames %llu loops %llu errors %llu/%llu/%llu\n", path, n0,
                xma::Bits(Rd(c + 12), 0, 26), xma::Bits(Rd(c + 16), 0, 26), xma::Bits(Rd(c), 12, 8),
                (unsigned long long)st.frames, (unsigned long long)st.loops, (unsigned long long)st.sendErrors,
                (unsigned long long)st.decodeErrors, (unsigned long long)st.badFrames);
    return st.sendErrors || st.decodeErrors || st.badFrames || !st.loops ? 1 : 0;
}

int main(int argc, char** argv)
{
    if (argc >= 3 && !std::strcmp(argv[1], "--replay")) return Replay(argv[2], argc > 3 ? std::atoi(argv[3]) : 2000);
    if (argc < 3) { std::fprintf(stderr, "usage: xma_core_test file.xma ref.pcm [--negative-control]\n"); return 2; }
    const bool negative = argc > 3 && !std::strcmp(argv[3], "--negative-control");
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    std::ifstream rf(argv[2], std::ios::binary);
    std::vector<uint8_t> refBytes((std::istreambuf_iterator<char>(rf)), {});
    const uint8_t* fmt = nullptr; const uint8_t* data = nullptr; uint32_t dataSize = 0;
    for (size_t o = 12; o + 8 <= d.size();)
    {
        const uint32_t size = Le32(&d[o + 4]);
        if (!std::memcmp(&d[o], "fmt ", 4)) fmt = &d[o + 8];
        if (!std::memcmp(&d[o], "data", 4)) { data = &d[o + 8]; dataSize = size; }
        o += 8 + size + (size & 1);
    }
    if (!fmt || !data || Le16(fmt) != 0x166) { std::fprintf(stderr, "FAIL not an XMA2 RIFF\n"); return 2; }
    const int channels = Le16(fmt + 2);
    const uint32_t rate = Le32(fmt + 4);
    const uint32_t rateCode = rate == 24000 ? 0 : rate == 32000 ? 1 : rate == 44100 ? 2 : rate == 48000 ? 3 : 4;
    if (rateCode > 3) { std::fprintf(stderr, "FAIL sample rate %u has no XMA context code\n", rate); return 2; }
    const uint32_t packets = dataSize / xma::kPacketBytes;
    av_log_set_level(AV_LOG_QUIET);

    // Guest layout: context array 0x1000, input buffers 0x10000 / 0x20000 (two packets each),
    // output ring 0x30000 (6 blocks: frames wrap mid-frame), the whole stream from 0x100000.
    constexpr uint32_t ARRAY = 0x1000, IN0 = 0x10000, IN1 = 0x20000, OUT = 0x30000, STREAM = 0x100000;
    constexpr uint32_t PER_BUFFER = 2, BLOCKS = 6, LOOPS = 2;
    g_mem.assign(STREAM + size_t(packets) * xma::kPacketBytes, 0);
    std::memcpy(&g_mem[STREAM], data, size_t(packets) * xma::kPacketBytes);
    xma::Decoder dec;
    dec.Attach(g_mem.data(), ARRAY);
    const uint32_t c = dec.Allocate();
    CHECK(c == ARRAY, "first context at the array base (got %08X)", c);
    CHECK(dec.Allocate() == ARRAY + 64, "second context follows");

    // perBuffer = packets per input buffer (all of them: one buffer, read in place);
    // loopCount > 0 loops [loopStart, loopEnd) that many extra times; rateOverride >= 0 declares
    // another sample rate code; maxKicks bounds the play.
    auto play = [&](std::vector<int16_t>& pcm, unsigned& swaps, uint32_t perBuffer, uint32_t loopStart = 0,
                    uint32_t loopEnd = 0, uint32_t loopCount = 0, int rateOverride = -1, int maxKicks = 200000) {
        // sub_82D3E488-style reset, then the title's setup: stereo flag, rate, ring, buffer 0.
        for (int i = 0; i < 16; ++i) Wr(c + i * 4, 0);
        const uint32_t declaredRate = rateOverride >= 0 ? uint32_t(rateOverride) : rateCode;
        Wr(c + 4, (1u << 31) | (declaredRate << 27) | (channels == 2 ? 1u << 29 : 0));
        Wr(c + 7 * 4, OUT);
        Wr(c + 12, loopStart);
        Wr(c + 16, loopEnd);
        uint32_t next = 0;      // next stream packet to submit
        auto submit = [&](int which) {
            const uint32_t n = std::min(perBuffer, packets - next);
            if (!n) return;
            uint32_t address = which ? IN1 : IN0;
            if (n == packets) address = STREAM;
            else std::memcpy(&g_mem[address], &g_mem[STREAM + next * xma::kPacketBytes], n * xma::kPacketBytes);
            next += n;
            uint32_t dw0 = Rd(c), dw1 = Rd(c + 4);
            if (which) { dw1 = xma::WithBits(dw1, 0, 12, n); Wr(c + 24, address); }
            else { dw0 = xma::WithBits(dw0, 0, 12, n); Wr(c + 20, address); }
            dw0 = xma::WithBits(dw0, 20 + which, 1, 1);
            Wr(c, xma::WithBits(dw0, 22, 5, BLOCKS));
            Wr(c + 4, dw1);
        };
        Wr(c, xma::WithBits(xma::WithBits(0, 22, 5, BLOCKS), 12, 8, loopCount));
        submit(0);
        submit(1);
        uint32_t lastCurrent = 0;
        for (int frame = 0; frame < maxKicks; ++frame)
        {
            dec.Kick(c);
            const uint32_t dw0 = Rd(c), dw1 = Rd(c + 4), dw9 = Rd(c + 36);
            const uint32_t current = xma::Bits(Rd(c + 16), 31, 1);
            swaps += current != lastCurrent;
            lastCurrent = current;
            // Consume the ring: blocks [read, write), or all of it when "output valid" dropped.
            const uint32_t write = xma::Bits(dw0, 27, 5), read = xma::Bits(dw9, 0, 5);
            uint32_t blocks = (write + BLOCKS - read) % BLOCKS;
            if (!xma::Bits(dw1, 31, 1) && write == read) blocks = BLOCKS;
            for (uint32_t b = 0; b < blocks; ++b)
            {
                const uint8_t* p = &g_mem[OUT + ((read + b) % BLOCKS) * xma::kBlockBytes];
                for (uint32_t s = 0; s < xma::kBlockBytes; s += 2)
                    pcm.push_back(int16_t((p[s] << 8) | p[s + 1]));
            }
            Wr(c + 36, xma::WithBits(dw9, 0, 5, write));
            if (!negative) Wr(c + 4, xma::WithBits(Rd(c + 4), 31, 1, 1));
            // Refill consumed input buffers, as sub_82D3DFD0 + the stream code do: the one the
            // decoder reads next (current_buffer) first, so a stream that drained both buffers
            // in one kick continues in order.
            const uint32_t readNext = xma::Bits(Rd(c + 16), 31, 1);
            for (uint32_t k = 0; k < 2; ++k)
            {
                const int which = int(readNext ^ k);
                if (!xma::Bits(Rd(c), 20 + which, 1)) submit(which);
            }
            if (!blocks && next >= packets && !xma::Bits(Rd(c), 20, 2)) break;
        }
    };

    std::vector<int16_t> first, second, looped, other;
    unsigned swaps1 = 0, swaps2 = 0, swaps3 = 0, swaps0 = 0;
    // The voice first plays something at another sample rate (TTT2 reuses voices across 24 to
    // 48 kHz banks): the decoder must be reopened for the next sound's rate.
    play(other, swaps0, PER_BUFFER, 0, 0, 0, int((rateCode + 1) & 3), 40);
    const xma::Stats before = dec.GetStats();
    play(first, swaps1, PER_BUFFER);
    play(second, swaps2, PER_BUFFER);
    // The whole stream in one buffer, looping over every frame: the loop end is any position
    // after the last frame's start (the frame that ends there jumps back to the first frame).
    uint32_t streamFrames = 0;
    const uint32_t lastStart = LastFrameStart(&g_mem[STREAM], packets, &streamFrames);
    play(looped, swaps3, packets, xma::kPacketHeaderBits, lastStart + 1, LOOPS);

    if (const char* dump = std::getenv("XMA_CORE_DUMP"))
    {
        std::ofstream o(dump, std::ios::binary);
        o.write(reinterpret_cast<const char*>(first.data()), std::streamsize(first.size() * 2));
    }
    const int16_t* ref = reinterpret_cast<const int16_t*>(refBytes.data());
    const size_t F = 512 * size_t(channels);
    const size_t refSamples = refBytes.size() / 2, delay = F + 64 * size_t(channels);
    size_t mismatches = 0, compared = 0;
    for (size_t i = 0; i < refSamples && i + delay < first.size(); ++i, ++compared)
        mismatches += std::abs(int(first[i + delay]) - int(ref[i])) > 1;
    std::printf("packets %u, channels %d, %u Hz: ring PCM %zu samples, reference %zu, compared %zu, >1 LSB mismatches %zu; buffer swaps %u/%u\n",
                packets, channels, rate, first.size(), refSamples, compared, mismatches, swaps1, swaps2);
    CHECK(first.size() == streamFrames * F, "one frame out per frame in (%zu samples for %u frames)", first.size(), streamFrames);
    CHECK(compared + 2048 >= refSamples && compared, "ring delivered the whole stream (%zu of %zu)", compared, refSamples);
    CHECK(mismatches == 0, "ring PCM matches the reference (%zu mismatches)", mismatches);
    CHECK(swaps1 >= 2, "current_buffer alternated (%u swaps)", swaps1);
    CHECK(first == second, "a reset context replays the sound identically (%zu vs %zu samples)", first.size(), second.size());
    // Each pass after a loop jump repeats the first pass from its second frame on (a frame's
    // output also depends on the previous frame, which for a pass's first frame is the end of
    // the previous pass rather than silence).
    size_t loopMismatch = 0;
    for (uint32_t pass = 1; pass <= LOOPS; ++pass)
    {
        const size_t start = pass * first.size();
        for (size_t i = F; i < first.size() && start + i < looped.size(); ++i)
            loopMismatch += looped[start + i] != first[i];
    }
    const size_t loopExpected = (LOOPS + 1) * first.size();
    std::printf("loop [%X, %X) x%u: %zu samples (expected %zu), mismatches %zu, loop count left %u\n",
                xma::kPacketHeaderBits, lastStart + 1, LOOPS, looped.size(), loopExpected, loopMismatch,
                xma::Bits(Rd(c), 12, 8));
    CHECK(looped.size() == loopExpected, "loop passes are complete (%zu samples)", looped.size());
    CHECK(loopMismatch == 0, "loop passes repeat the stream (%zu mismatches)", loopMismatch);
    CHECK(xma::Bits(Rd(c), 12, 8) == 0, "loop count counted down");
    const auto& st = dec.GetStats();
    std::printf("stats: kicks %llu packets %llu frames %llu resets %llu ring-full %llu loops %llu send-errors %llu decode-errors %llu bad-frames %llu\n",
                (unsigned long long)st.kicks, (unsigned long long)st.packets, (unsigned long long)st.frames,
                (unsigned long long)st.resets, (unsigned long long)st.ringFull, (unsigned long long)st.loops,
                (unsigned long long)st.sendErrors, (unsigned long long)st.decodeErrors, (unsigned long long)st.badFrames);
    CHECK(st.resets - before.resets >= 2, "reset detected for each new sound");
    CHECK(st.loops - before.loops == LOOPS, "loop jumps taken (%llu)", (unsigned long long)(st.loops - before.loops));
    const uint64_t sendErrors = st.sendErrors - before.sendErrors, decodeErrors = st.decodeErrors - before.decodeErrors,
                   badFrames = st.badFrames - before.badFrames;
    CHECK(sendErrors == 0 && decodeErrors == 0 && badFrames == 0, "no decoder errors (%llu send, %llu decode, %llu bad frames)",
          (unsigned long long)sendErrors, (unsigned long long)decodeErrors, (unsigned long long)badFrames);
    if (failures) { std::fprintf(stderr, "%u of %u checks failed\n", failures, checks + failures); return 1; }
    std::printf("PASS %u XMA context checks\n", checks);
    return 0;
}
