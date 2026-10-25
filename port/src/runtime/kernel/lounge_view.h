// Fighting Lounge presentation shared by the game thread and the hardware presenter.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace lounge_view {
constexpr int Width = 1280, Height = 720, Cabinets = 20;
constexpr float Pi = 3.14159265f;
struct Vec3 {
    float x = 0, y = 0, z = 0;
    bool operator==(const Vec3&) const = default;
};
inline Vec3 CabinetPosition(int i) {
    return { (i < 10 ? -22.0f : 10.0f) + (i % 5) * 3.0f, 0, (i / 5) % 2 == 0 ? 10.0f : -14.0f };
}
inline float CabinetFacing(int i) { return (i / 5) % 2 == 0 ? Pi : 0; }
inline Vec3 SeatPosition(int i, int slot) {
    Vec3 p = CabinetPosition(i);
    p.x += slot == 0 ? -0.45f : 0.45f;
    p.y = .55f;
    p.z += (i / 5) % 2 == 0 ? -1.1f : 1.1f;
    return p;
}
inline Vec3 BenchPosition(int i) { return {i < 3 ? -18.0f : 18.0f, .5f, -4.0f + (i % 3) * 2.0f}; }
inline float DistanceSq(Vec3 a, Vec3 b) { return (a.x-b.x)*(a.x-b.x) + (a.z-b.z)*(a.z-b.z); }
inline int NearestCabinet(Vec3 p) {
    int nearest = -1; float distance = 2.5f * 2.5f;
    for (int i = 0; i < Cabinets; ++i)
        if (float d = DistanceSq(p, CabinetPosition(i)); d < distance) { distance = d; nearest = i; }
    return nearest;
}
inline int NearestBench(Vec3 p) {
    int nearest = -1; float distance = 1.5f * 1.5f;
    for (int i = 0; i < 6; ++i)
        if (float d = DistanceSq(p, BenchPosition(i)); d < distance) { distance = d; nearest = i; }
    return nearest;
}
inline int Select(int selected, int dx, int dy) {
    return ((selected / 5 + dy + 4) % 4) * 5 + (selected % 5 + dx + 5) % 5;
}
enum class CabinetStatus { Empty, Waiting, Ready, Playing };
inline CabinetStatus Status(bool playing, int seated, bool ready0, bool ready1) {
    if (playing) return CabinetStatus::Playing;
    if (!seated) return CabinetStatus::Empty;
    return seated == 2 && ready0 && ready1 ? CabinetStatus::Ready : CabinetStatus::Waiting;
}
struct Card {
    CabinetStatus status = CabinetStatus::Empty;
    std::array<std::string, 2> players;
    std::array<bool, 2> ready{};
    std::array<std::string, 3> next;
    int queue = 0, mine = -1, queued = 0;
    std::string last;
    bool operator==(const Card&) const = default;
};
inline int Featured(const std::array<Card, Cabinets>& cards) {
    int best = -1;
    for (int i = 0; i < Cabinets; ++i)
        if (cards[i].status == CabinetStatus::Playing && (best < 0 || cards[i].queue > cards[best].queue)) best = i;
    return best;
}
struct Local {
    Vec3 position{0, 0, -28};
    float yaw = 0, cameraYaw = 0;
    int bench = -1;
    float emote = 0;
    bool operator==(const Local&) const = default;
};
inline bool Walkable(Vec3 p) {
    if (std::abs(p.x) > 27 || p.z < -31 || p.z > 21) return false;
    for (int i = 0; i < Cabinets; ++i) {
        Vec3 c = CabinetPosition(i);
        if (std::abs(p.x - c.x) < 1.15f && std::abs(p.z - c.z) < .95f) return false;
    }
    for (float x : {-18.0f, 18.0f})
        if (std::abs(p.x - x) < .8f && p.z > -5 && p.z < 1) return false;
    return true;
}
inline void Move(Local& local, float x, float z, bool sprint, float dt) {
    if (!std::isfinite(dt) || dt <= 0 || !std::isfinite(x) || !std::isfinite(z)) return;
    float length = std::hypot(x, z);
    if (length < .01f) return;
    x /= length; z /= length;
    float s = std::sin(local.cameraYaw), c = std::cos(local.cameraYaw);
    float dx = x*c + z*s, dz = -x*s + z*c;
    float step = (sprint ? 6.0f : 3.0f) * (std::min)(dt, .1f);
    Vec3 p = local.position; p.x += dx * step;
    if (Walkable(p)) local.position.x = p.x;
    p = local.position; p.z += dz * step;
    if (Walkable(p)) local.position.z = p.z;
    local.yaw = std::atan2(dx, dz);
    local.emote = 0;
}
struct View {
    bool visible = false, grid = true, connected = false, host = false;
    int selected = 0, members = 0;
    Local local;
    std::array<Card, Cabinets> cards;
    std::string connection, chat;
    bool operator==(const View&) const = default;
};
void Publish(const View& view);
// Paint runs only on the presenter thread. An unchanged serial means the caller can reuse its GPU texture.
bool Paint(std::vector<uint32_t>& pixels, uint64_t& serial);
bool Available();
void SetAvailable(bool available);
} // namespace lounge_view
