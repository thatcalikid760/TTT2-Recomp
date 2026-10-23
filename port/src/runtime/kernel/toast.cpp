// On-screen notifications (see toast.h).
#include "toast.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

namespace toast {
namespace {

using clock = std::chrono::steady_clock;
constexpr int kFadeMs = 350;

std::mutex g_mutex;
Note g_note;
clock::time_point g_start{}, g_end{};
bool g_forever = false;
uint32_t g_generation = 0;
bool g_active = false;
int g_rect[4] = {};
std::atomic<int> g_click{ 0 };

bool ActiveLocked(clock::time_point now) { return g_active && (g_forever || now < g_end); }

} // namespace

void Show(const Note& note, int milliseconds)
{
    std::lock_guard l(g_mutex);
    g_note = note;
    g_start = clock::now();
    g_forever = milliseconds <= 0;
    g_end = g_start + std::chrono::milliseconds(g_forever ? 0 : milliseconds);
    ++g_generation;
    g_active = true;
    g_click = 0;
}

void Update(const Note& note)
{
    std::lock_guard l(g_mutex);
    if (!ActiveLocked(clock::now())) return;
    g_note = note;
    ++g_generation;
}

void Hide()
{
    std::lock_guard l(g_mutex);
    const auto now = clock::now();
    if (!ActiveLocked(now)) return;
    g_forever = false;
    g_end = now + std::chrono::milliseconds(kFadeMs);
    g_note.clickable = false;
}

bool Showing() { std::lock_guard l(g_mutex); return ActiveLocked(clock::now()); }

bool Current(Note& note, float& opacity, uint32_t& generation)
{
    std::lock_guard l(g_mutex);
    const auto now = clock::now();
    if (!ActiveLocked(now))
    {
        g_active = false;
        g_rect[0] = g_rect[1] = g_rect[2] = g_rect[3] = 0;
        return false;
    }
    const double in = std::chrono::duration<double, std::milli>(now - g_start).count();
    const double out = g_forever ? 1e9 : std::chrono::duration<double, std::milli>(g_end - now).count();
    opacity = float(std::clamp((std::min)(in, out) / kFadeMs, 0.0, 1.0));
    note = g_note;
    generation = g_generation;
    return true;
}

void SetRect(int left, int top, int right, int bottom)
{
    std::lock_guard l(g_mutex);
    g_rect[0] = left; g_rect[1] = top; g_rect[2] = right; g_rect[3] = bottom;
}

bool Hovered(int x, int y)
{
    std::lock_guard l(g_mutex);
    return ActiveLocked(clock::now()) && g_note.clickable && x >= g_rect[0] && x < g_rect[2] && y >= g_rect[1] && y < g_rect[3];
}

bool OnClick(int x, int y, bool right)
{
    if (!Hovered(x, y)) return false;
    g_click = right ? 2 : 1;
    return true;
}

int TakeClick() { return g_click.exchange(0); }

} // namespace toast
