// Expand a TTT2 disc archive.
//
// 26 of the game's 42 dataNNN.bin archives are stored on the disc in an Xbox LZX container
// whose magic is 0x0FF512ED; the other 16 are already plain "nrfi" images. nrfo.bin, the
// master name table, addresses every archive in its EXPANDED form (for the 16 plain archives
// max(ofsd)+256 equals the file size exactly), so the compressed ones must be expanded before
// the game can read a single asset out of them.
//
// CONTAINER FORMAT (derived from the bytes, then verified against nrfo for 17 of the 26):
//   0x00  u32be  magic 0x0FF512ED
//   0x04  u32be  0x01000000
//   0x08  u32be  content hash
//   0x0C  u32be  flags/count word
//   0x10  N x u32be  UNCOMPRESSED size of each chunk; they sum to the expanded file size.
//                    N == ceil(fileSize / 0x20000).
// The compressed chunks then sit at 0x20000-ALIGNED FILE OFFSETS: chunk k occupies
// [max(headerEnd, k*0x20000), min((k+1)*0x20000, fileSize)). Chunk 0 is short because the
// header eats into it. Each chunk is an independent LZX stream with a 2^17 (128 KiB) window,
// decoded by libmspack's lzxd; inside a chunk, blocks are framed by a big-endian 16-bit
// length, with 0xFFxx introducing an extra uncompressed length (the same framing Unleashed
// Recomp's x_decompress uses for the sibling 0x0FF512EE container).
//
// Nine archives (data004/005/006/008/016/018/019/051/052) pack that table as 20-bit fields
// instead of u32s - see ReadChunkTable. --probe brute-forces anything still unexplained.
//
// Usage:
//   x_decompress <in> <out> [expectedSize]   expand
//   x_decompress --probe <in> <expectedSize> report the layout, write nothing
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

extern "C" {
#include <mspack.h>
#include <lzx.h>
}

namespace
{

constexpr uint32_t CHUNK_ALIGN = 0x20000;

std::vector<uint8_t> ReadAll(const char* path)
{
    std::ifstream f{ path, std::ios::binary };
    std::vector<uint8_t> v;
    if (!f.good()) return v;
    f.seekg(0, std::ios::end);
    v.resize((size_t)f.tellg());
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(v.data()), v.size());
    return v;
}

uint32_t Be32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

struct ReadStream
{
    const uint8_t* data = nullptr;
    const uint8_t* end = nullptr;
    int size = 0;
    int pad = 0;      // a frame with an odd compressed length owes one alignment byte
};

int MspackRead(mspack_file* file, void* buffer, int bytes)
{
    auto* s = reinterpret_cast<ReadStream*>(file);
    if (s->size == 0)
    {
        // LZX reads its bitstream as 16-bit words and re-aligns to a word boundary at every
        // frame boundary, so each frame's compressed data has to start on an even byte. A
        // frame whose compressed length is ODD is not padded in the file - the next frame's
        // length prefix follows immediately - so the pad byte has to be SYNTHESISED here.
        // Without it the word reads slip by one byte and the next frame decodes as garbage:
        // measured, that is exactly what killed data028 chunk 440 after 3 of its 6 frames.
        if (s->pad)
        {
            s->pad = 0;
            if (bytes < 1) return 0;
            *reinterpret_cast<uint8_t*>(buffer) = 0;
            return 1;
        }
        if (s->data + 2 > s->end) return 0;
        uint16_t size = Be16(s->data);
        s->data += 2;
        if ((size & 0xFF00) == 0xFF00)
        {
            if (s->data + 3 > s->end) return 0;
            s->data += 1;
            size = Be16(s->data);
            s->data += 2;
        }
        s->size = size;
        s->pad = size & 1;
    }
    int n = (int)std::min<ptrdiff_t>(std::min(s->size, bytes), s->end - s->data);
    if (n <= 0) return 0;
    std::memcpy(buffer, s->data, n);
    s->data += n;
    s->size -= n;
    return n;
}

struct WriteStream
{
    uint8_t* data = nullptr;
    size_t size = 0;
};

int MspackWrite(mspack_file* file, void* buffer, int bytes)
{
    auto* s = reinterpret_cast<WriteStream*>(file);
    size_t n = std::min(s->size, (size_t)bytes);
    std::memcpy(s->data, buffer, n);
    s->data += n;
    s->size -= n;
    return (int)n;
}

void* MspackAlloc(mspack_system*, size_t n) { return operator new(n); }
void MspackFree(void* p) { operator delete(p); }
void MspackCopy(void* src, void* dst, size_t n) { std::memcpy(dst, src, n); }

mspack_system g_sys = { nullptr, nullptr, MspackRead, MspackWrite, nullptr, nullptr, nullptr,
                        MspackAlloc, MspackFree, MspackCopy };

// Decode one LZX chunk. Returns bytes produced; `ok` receives whether the decoder ran to
// completion rather than running out of input.
size_t DecodeChunk(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen,
                   int windowBits, bool* ok = nullptr)
{
    ReadStream rs{ src, src + srcLen, 0 };
    WriteStream ws{ dst, dstLen };
    lzxd_stream* lzx = lzxd_init(&g_sys, reinterpret_cast<mspack_file*>(&rs),
                                 reinterpret_cast<mspack_file*>(&ws), windowBits, 0,
                                 (int)std::min<size_t>(srcLen, 0x10000), (off_t)dstLen, 0);
    if (!lzx) { if (ok) *ok = false; return 0; }
    int rc = lzxd_decompress(lzx, (off_t)dstLen);
    lzxd_free(lzx);
    if (ok) *ok = rc == MSPACK_ERR_OK;
    return dstLen - ws.size;
}

// Recover a chunk's uncompressed size when the header variant hides its size table.
//
// lzxd_decompress(n) succeeds for every n up to the chunk's real uncompressed size and fails
// once n exceeds it (the stream runs out of input), so "does it succeed" is monotonic in n and
// the size is simply the largest n that still succeeds. A decode with a generous bound gives
// the whole-frame count for free; only the final partial frame - under 32 KiB - has to be
// searched, which is about fifteen decodes per chunk.
size_t RecoverChunkSize(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& scratch,
                        size_t cap, int windowBits)
{
    scratch.assign(cap, 0);
    bool ok = false;
    size_t whole = DecodeChunk(src, srcLen, scratch.data(), cap, windowBits, &ok);
    if (ok) return whole;                      // the bound was not the limiting factor
    if (whole + 0x8000 > cap) return whole;
    size_t lo = whole, hi = whole + 0x8000;    // real size is in [whole, whole + one frame)
    while (lo + 1 < hi)
    {
        size_t mid = lo + (hi - lo) / 2;
        DecodeChunk(src, srcLen, scratch.data(), mid, windowBits, &ok);
        if (ok) lo = mid; else hi = mid;
    }
    return lo;
}

bool LooksLikeArchive(const uint8_t* p, size_t n)
{
    static const uint8_t magic[] = { 0x00, 0x00, 0x00, 0x14, 'f', 't', 'y', 'p',
                                     'n', 'r', 'f', 'i' };
    return n >= sizeof(magic) && std::memcmp(p, magic, sizeof(magic)) == 0;
}

// The chunk-size table. The u32 at 0x0C is a bitfield: bits 5..18 hold (2 * chunkCount + 1),
// and BIT 22 selects how the per-chunk UNCOMPRESSED sizes that follow at 0x10 are encoded.
//
//   bit 22 SET  - one big-endian u32 per chunk; the header ends at 16 + 4 * chunks.
//   bit 22 CLEAR- one big-endian 20-BIT field per chunk, packed without gaps; the header ends
//                 at 16 + ceil(20 * chunks / 8) rounded up to 4. A 20-bit field caps a chunk at
//                 1,048,575 bytes, which is why archives with a chunk bigger than that use the
//                 u32 form.
//
// Verified: with this rule all 26 compressed archives produce a table that sums EXACTLY to the
// expanded size nrfo.bin implies, and the derived header end matches - to the byte - the only
// offset at which chunk 0's block framing walks cleanly to the chunk boundary.
bool ReadChunkTable(const std::vector<uint8_t>& in, std::vector<uint32_t>& table,
                    size_t& headerEnd)
{
    uint32_t chunks = (uint32_t)((in.size() + CHUNK_ALIGN - 1) / CHUNK_ALIGN);
    uint32_t flags = Be32(in.data() + 12);
    table.assign(chunks, 0);
    if (flags & (1u << 22))
    {
        headerEnd = 16 + 4 * (size_t)chunks;
        if (headerEnd >= in.size()) return false;
        for (uint32_t i = 0; i < chunks; ++i) table[i] = Be32(in.data() + 16 + 4 * i);
        return true;
    }
    headerEnd = (16 + (20 * (size_t)chunks + 7) / 8 + 3) & ~(size_t)3;
    if (headerEnd >= in.size()) return false;
    for (uint32_t i = 0; i < chunks; ++i)
    {
        size_t bit = (size_t)i * 20;
        const uint8_t* p = in.data() + 16 + bit / 8;
        uint32_t w = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
        table[i] = (w >> (4 - bit % 8)) & 0xFFFFFu;
    }
    return true;
}

bool ExpandTabled(const std::vector<uint8_t>& in, size_t expected, std::vector<uint8_t>& out,
                  std::string& why)
{
    uint32_t chunks = (uint32_t)((in.size() + CHUNK_ALIGN - 1) / CHUNK_ALIGN);
    std::vector<uint32_t> table;
    size_t headerEnd = 0;
    if (!ReadChunkTable(in, table, headerEnd)) { why = "header longer than file"; return false; }
    size_t sum = 0;
    for (uint32_t v : table) sum += v;
    if (expected && sum != expected)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "table sums to %zu, expected %zu", sum, expected);
        why = buf;
        return false;
    }
    out.assign(sum, 0);
    size_t written = 0;
    for (uint32_t k = 0; k < chunks; ++k)
    {
        size_t begin = std::max<size_t>(headerEnd, (size_t)k * CHUNK_ALIGN);
        size_t end = std::min<size_t>((size_t)(k + 1) * CHUNK_ALIGN, in.size());
        if (begin >= end) { why = "empty chunk"; return false; }
        // A chunk that did not compress is STORED: a two-byte marker of zero followed by the
        // bytes themselves, so its listed uncompressed size is exactly the chunk length minus
        // that marker. Copy those straight through - feeding them to the LZX decoder just
        // fails on a zero block length.
        if (table[k] + 2 == end - begin && Be16(in.data() + begin) == 0)
        {
            std::memcpy(out.data() + written, in.data() + begin + 2, table[k]);
            written += table[k];
            continue;
        }
        size_t got = DecodeChunk(in.data() + begin, end - begin, out.data() + written,
                                 table[k], 17);
        if (got != table[k])
        {
            char buf[176];
            std::snprintf(buf, sizeof(buf),
                          "chunk %u produced %zu of %u bytes (input %zu..%zu)",
                          k, got, table[k], begin, end);
            why = buf;
            return false;
        }
        written += got;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    bool probe = argc > 1 && std::string(argv[1]) == "--probe";
    int base = probe ? 2 : 1;
    if (argc < base + 2)
    {
        std::printf("usage: x_decompress <in> <out> [expectedSize]\n"
                    "       x_decompress --probe <in> <expectedSize>\n");
        return 1;
    }
    std::vector<uint8_t> in = ReadAll(argv[base]);
    if (in.empty()) { std::fprintf(stderr, "cannot read %s\n", argv[base]); return 1; }

    size_t expected = 0;
    if (probe) expected = std::strtoull(argv[base + 1], nullptr, 0);
    else if (argc > base + 2) expected = std::strtoull(argv[base + 2], nullptr, 0);

    uint32_t sig = Be32(in.data());
    if (sig != 0x0FF512EDu && sig != 0x0FF512EEu)
    {
        std::printf("%s: not compressed (%08X), pass through\n", argv[base], sig);
        if (!probe)
        {
            std::ofstream o(argv[base + 1], std::ios::binary);
            o.write(reinterpret_cast<const char*>(in.data()), in.size());
        }
        return 0;
    }

    std::vector<uint8_t> out;
    std::string why;
    if (ExpandTabled(in, expected, out, why))
    {
        bool magic = LooksLikeArchive(out.data(), out.size());
        std::printf("%s: %zu -> %zu bytes%s\n", argv[base], in.size(), out.size(),
                    magic ? "  (valid nrfi archive)" : "  WARNING: no nrfi magic");
        if (!probe)
        {
            std::ofstream o(argv[base + 1], std::ios::binary);
            o.write(reinterpret_cast<const char*>(out.data()), out.size());
            std::printf("wrote %s\n", argv[base + 1]);
        }
        return magic ? 0 : 4;
    }

    std::printf("%s: tabled layout failed (%s)\n", argv[base], why.c_str());

    // Fallback for the header variants whose chunk table is not at 0x10: the chunk boundaries
    // are still 0x20000-aligned, and every chunk decodes to a whole number of 32 KiB LZX
    // frames, so each one can be decoded "until its input runs out" without knowing its size
    // in advance. The expected total bounds the last chunk.
    if (expected)
    {
        std::vector<uint8_t> scratch;
        for (uint32_t hs = 8; hs <= 4096 && hs < in.size(); hs += 4)
        {
            std::vector<uint8_t> guess(expected, 0);
            std::vector<size_t> sizes;      // what each chunk actually yielded, for diagnosis
            size_t total = 0;
            uint32_t chunks = (uint32_t)((in.size() + CHUNK_ALIGN - 1) / CHUNK_ALIGN);
            for (uint32_t k = 0; k < chunks && total < expected; ++k)
            {
                size_t begin = std::max<size_t>(hs, (size_t)k * CHUNK_ALIGN);
                size_t end = std::min<size_t>((size_t)(k + 1) * CHUNK_ALIGN, in.size());
                if (begin >= end) continue;
                size_t room = expected - total;
                // A chunk that did not compress is stored raw behind a two-byte zero marker.
                if (Be16(in.data() + begin) == 0 && (end - begin) - 2 <= room)
                {
                    std::memcpy(guess.data() + total, in.data() + begin + 2, end - begin - 2);
                    total += end - begin - 2;
                    sizes.push_back(end - begin - 2);
                    continue;
                }
                // Cap the probe at everything that is left: a smaller bound would be reported
                // back as the chunk's size and silently truncate a highly compressible chunk.
                size_t size = RecoverChunkSize(in.data() + begin, end - begin, scratch, room, 17);
                if (!size) break;
                size_t got = DecodeChunk(in.data() + begin, end - begin,
                                         guess.data() + total, size, 17);
                if (!got) break;
                total += got;
                sizes.push_back(got);
            }
            if (total * 10 > (size_t)expected * 9 && total != expected)
            {
                std::printf("   header=%u per-chunk (%zu of %u):", hs, sizes.size(), chunks);
                for (size_t z : sizes) std::printf(" %zu", z);
                std::printf("   short by %zu\n", expected - total);
            }
            if (total == expected && LooksLikeArchive(guess.data(), guess.size()))
            {
                std::printf("%s: chunked-no-table layout, header=%u -> %zu bytes"
                            "  (valid nrfi archive)\n", argv[base], hs, total);
                if (!probe)
                {
                    std::ofstream o(argv[base + 1], std::ios::binary);
                    o.write(reinterpret_cast<const char*>(guess.data()), guess.size());
                    std::printf("wrote %s\n", argv[base + 1]);
                }
                return 0;
            }
            if (total)
                std::printf("  chunked header=%u -> %zu/%zu head=%08X%s\n", hs, total, expected,
                            Be32(guess.data()),
                            LooksLikeArchive(guess.data(), guess.size()) ? " nrfi" : "");
        }
    }
    if (!probe) return 2;

    // Probe: brute-force the header offset for the variant containers, decoding the whole
    // file as one stream.
    if (!expected) return 2;
    // Cheap pass first: decode only the opening bytes at each candidate offset and keep the
    // ones that produce the nrfi magic, then pay for a full decode only on those.
    std::vector<uint32_t> candidates;
    // The peek has to be at least one LZX frame (32 KiB) or the decoder never emits anything
    // and every candidate looks like a miss.
    std::vector<uint8_t> peek(65536);
    for (uint32_t hs = 8; hs <= 4096; hs += 4)
    {
        if (hs >= in.size()) break;
        std::fill(peek.begin(), peek.end(), 0);
        DecodeChunk(in.data() + hs, in.size() - hs, peek.data(),
                    std::min<size_t>(peek.size(), expected), 17);
        if (LooksLikeArchive(peek.data(), peek.size())) candidates.push_back(hs);
    }
    std::printf("  magic at header offsets:");
    for (uint32_t c : candidates) std::printf(" %u", c);
    std::printf("%s\n", candidates.empty() ? " (none)" : "");
    out.assign(expected, 0);
    for (uint32_t hs : candidates)
    {
        std::fill(out.begin(), out.end(), 0);
        size_t got = DecodeChunk(in.data() + hs, in.size() - hs, out.data(), expected, 17);
        std::printf("  header=%u -> %zu/%zu bytes%s\n", hs, got, expected,
                    got == expected ? "  COMPLETE" : "");
        if (got == expected) return 0;
    }
    std::printf("  no single-stream layout worked either\n");
    return 3;
}
