#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Optional diagnostics for the serial command-processor thread. These are wall
// durations, including worker/GPU waits, not process CPU time. No guest data is
// retained. Shader words are hashed on entry because upload storage may be reused.
namespace gpu_draw_profile_detail {

inline bool EnabledValue(const char* value) noexcept
{
    return value && std::strcmp(value, "1") == 0;
}

inline uint64_t ShaderHash(const uint32_t* words, std::size_t count) noexcept
{
    // FNV-1a over every decoded PS word, least-significant byte first. The
    // explicit byte order gives the same fingerprint independent of host endian.
    uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < count; ++i)
        for (unsigned byte = 0; byte < 4; ++byte)
        {
            hash ^= (words[i] >> (byte * 8)) & 255;
            hash *= 1099511628211ull;
        }
    return hash;
}

struct Totals
{
    uint64_t draws = 0, indices = 0, wallNs = 0, maxNs = 0;

    void Add(uint32_t indexCount, uint64_t duration) noexcept
    {
        ++draws;
        indices += indexCount;
        wallNs += duration;
        maxNs = std::max(maxNs, duration);
    }
};

struct Family
{
    uint64_t shaderHash = 0;
    std::size_t shaderWords = 0;
    uint32_t primitive = 0;
    Totals totals;
};

struct Report
{
    uint64_t elapsedNs = 0;
    Totals total, overflow;
    std::size_t families = 0, topCount = 0;
    std::array<Family, 8> top{};
};

class Accumulator
{
public:
    static constexpr std::size_t MaxFamilies = 256;
    static constexpr uint64_t IntervalNs = 5'000'000'000ull;

    // Explicit timestamps keep the aggregation independently testable without
    // sleeps, a device, or a game process. Calls must remain on the CP thread.
    void Record(uint64_t hash, std::size_t words, uint32_t primitive,
                uint32_t indices, uint64_t startNs, uint64_t endNs) noexcept
    {
        if (!started_) { windowStart_ = startNs; started_ = true; }
        const uint64_t duration = endNs >= startNs ? endNs - startNs : 0;
        total_.Add(indices, duration);
        std::size_t slot = static_cast<std::size_t>(hash ^ (hash >> 32) ^
            (uint64_t(primitive) * 0x9E3779B97F4A7C15ull) ^ words) & (TableSize - 1);
        for (std::size_t probe = 0; probe < TableSize; ++probe)
        {
            Entry& entry = entries_[slot];
            if (!entry.used)
            {
                if (families_ == MaxFamilies) break;
                entry.used = true;
                entry.family.shaderHash = hash;
                entry.family.shaderWords = words;
                entry.family.primitive = primitive;
                ++families_;
            }
            if (entry.family.shaderHash == hash && entry.family.shaderWords == words &&
                entry.family.primitive == primitive)
            {
                entry.family.totals.Add(indices, duration);
                return;
            }
            slot = (slot + 1) & (TableSize - 1);
        }
        // New families beyond the fixed capacity stay visible in total/overflow
        // timing. Existing families still accumulate until the next report.
        overflow_.Add(indices, duration);
    }

    bool ReportIfDue(uint64_t nowNs, Report& report) noexcept
    {
        if (!started_ || nowNs < windowStart_ || nowNs - windowStart_ < IntervalNs)
            return false;
        report = {};
        report.elapsedNs = nowNs - windowStart_;
        report.total = total_;
        report.overflow = overflow_;
        report.families = families_;
        std::array<const Family*, MaxFamilies> ranked{};
        std::size_t count = 0;
        for (const Entry& entry : entries_)
            if (entry.used) ranked[count++] = &entry.family;
        std::sort(ranked.begin(), ranked.begin() + count, [](const Family* a, const Family* b) {
            if (a->totals.wallNs != b->totals.wallNs) return a->totals.wallNs > b->totals.wallNs;
            if (a->shaderHash != b->shaderHash) return a->shaderHash < b->shaderHash;
            if (a->shaderWords != b->shaderWords) return a->shaderWords < b->shaderWords;
            return a->primitive < b->primitive;
        });
        report.topCount = std::min(count, report.top.size());
        for (std::size_t i = 0; i < report.topCount; ++i) report.top[i] = *ranked[i];
        for (Entry& entry : entries_) entry = {};
        families_ = 0;
        total_ = {};
        overflow_ = {};
        windowStart_ = nowNs;
        return true;
    }

private:
    static constexpr std::size_t TableSize = MaxFamilies * 2;
    struct Entry { bool used = false; Family family; };
    std::array<Entry, TableSize> entries_{};
    Totals total_, overflow_;
    uint64_t windowStart_ = 0;
    std::size_t families_ = 0;
    bool started_ = false;
};

inline void PrintReport(const Report& report, std::FILE* stream = stderr) noexcept
{
    std::fprintf(stream, "[gpu-profile] interval_ms=%.3f draws=%llu indices=%llu draw_wall_ms=%.3f max_ms=%.3f families=%zu overflow_draws=%llu overflow_wall_ms=%.3f\n",
        double(report.elapsedNs) / 1e6, static_cast<unsigned long long>(report.total.draws),
        static_cast<unsigned long long>(report.total.indices), double(report.total.wallNs) / 1e6,
        double(report.total.maxNs) / 1e6, report.families,
        static_cast<unsigned long long>(report.overflow.draws), double(report.overflow.wallNs) / 1e6);
    for (std::size_t i = 0; i < report.topCount; ++i)
    {
        const Family& family = report.top[i];
        const Totals& totals = family.totals;
        std::fprintf(stream, "[gpu-profile] rank=%zu ps_hash=%016llX ps_words=%zu prim=%u draws=%llu indices=%llu draw_wall_ms=%.3f mean_ms=%.3f max_ms=%.3f share_pct=%.2f\n",
            i + 1, static_cast<unsigned long long>(family.shaderHash), family.shaderWords,
            family.primitive, static_cast<unsigned long long>(totals.draws),
            static_cast<unsigned long long>(totals.indices), double(totals.wallNs) / 1e6,
            totals.draws ? double(totals.wallNs) / (1e6 * double(totals.draws)) : 0.0,
            double(totals.maxNs) / 1e6,
            report.total.wallNs ? 100.0 * double(totals.wallNs) / double(report.total.wallNs) : 0.0);
    }
}

inline uint64_t NowNs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline Accumulator& SharedAccumulator() noexcept
{
    static Accumulator accumulator;
    return accumulator;
}

} // namespace gpu_draw_profile_detail

// Create after LoadUcode/VS validation in GpuExecuteDraw. One scope per draw,
// on the serial CP thread. Destruction covers early returns and synchronous
// GPU offload waits. The disabled path reads neither the shader nor the clock.
class GpuDrawProfileScope
{
public:
    GpuDrawProfileScope(const uint32_t* psWords, std::size_t wordCount,
                        uint32_t primitive, uint32_t indexCount) noexcept
    {
        static const bool enabled = gpu_draw_profile_detail::EnabledValue(std::getenv("TTT2_GPU_PROFILE"));
        if (!enabled) return;
        enabled_ = true;
        startNs_ = gpu_draw_profile_detail::NowNs();
        hash_ = gpu_draw_profile_detail::ShaderHash(psWords, wordCount);
        words_ = wordCount;
        primitive_ = primitive;
        indices_ = indexCount;
    }

    ~GpuDrawProfileScope() noexcept
    {
        if (!enabled_) return;
        const uint64_t end = gpu_draw_profile_detail::NowNs();
        auto& accumulator = gpu_draw_profile_detail::SharedAccumulator();
        accumulator.Record(hash_, words_, primitive_, indices_, startNs_, end);
        gpu_draw_profile_detail::Report report;
        if (accumulator.ReportIfDue(end, report)) gpu_draw_profile_detail::PrintReport(report);
    }

    GpuDrawProfileScope(const GpuDrawProfileScope&) = delete;
    GpuDrawProfileScope& operator=(const GpuDrawProfileScope&) = delete;

private:
    uint64_t startNs_ = 0, hash_ = 0;
    std::size_t words_ = 0;
    uint32_t primitive_ = 0, indices_ = 0;
    bool enabled_ = false;
};
