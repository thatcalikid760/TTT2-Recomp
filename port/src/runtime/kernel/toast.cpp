// On-screen notifications (see toast.h).
#include "toast.h"
#include <algorithm>
#include <chrono>
#include <mutex>

namespace toast {
namespace {

using clock = std::chrono::steady_clock;
constexpr int kFadeMs = 350;

std::mutex g_mutex;
Note g_note;
clock::time_point g_start{}, g_end{};
uint32_t g_generation = 0;
bool g_active = false;

} // namespace

void Show(const Note& note, int milliseconds)
{
    std::lock_guard l(g_mutex);
    g_note = note;
    g_start = clock::now();
    g_end = g_start + std::chrono::milliseconds(milliseconds);
    ++g_generation;
    g_active = true;
}

bool Current(Note& note, float& opacity, uint32_t& generation)
{
    std::lock_guard l(g_mutex);
    if (!g_active) return false;
    const auto now = clock::now();
    if (now >= g_end) { g_active = false; return false; }
    const double in = std::chrono::duration<double, std::milli>(now - g_start).count();
    const double out = std::chrono::duration<double, std::milli>(g_end - now).count();
    opacity = float(std::clamp((std::min)(in, out) / kFadeMs, 0.0, 1.0));
    note = g_note;
    generation = g_generation;
    return true;
}

} // namespace toast
