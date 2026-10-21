// Steam in a helper process (steam_net.h). The game never loads Steam itself: Steam Input hides
// every controller from the process that initialises SteamAPI as app 480 (Spacewar's controller
// config then gives the game nothing), while processes Steam has not hooked keep seeing the pads.
//
// Start() runs this exe again as `<exe> --steam-bridge` with two anonymous pipes as its stdin and
// stdout, and its stderr = our log. The helper (RunBridge) runs steam_net.cpp's in-process code
// and relays. It dies with the game: EOF on its stdin, and a kill-on-close job as a backstop.
//
// Frames on both pipes: u32 size (type + payload), u8 type, payload; integers little-endian.
//   game -> helper: SEND u64 peer, u8 reliable, data | PUBLISH u64 session, record |
//                   REMOVE u64 session | LIST u32 request, i32 max, i32 timeoutMs
//   helper -> game: READY u8 ok, u64 steamId | RECV u64 peer, u8 reliable, data |
//                   LIST_RESULT u32 request, u32 n, n x (u32 size, record) | JOIN record
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include "steam_net.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace steamnet {
namespace {

enum : uint8_t { kSend = 1, kPublish = 2, kRemove = 3, kList = 4, kReady = 101, kRecv = 102, kListResult = 103, kJoin = 104 };
constexpr uint32_t kMaxFrame = 16u << 20;

void Put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void Put64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }
uint32_t Get32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t Get64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

bool ReadAll(HANDLE h, void* buf, size_t n)
{
    auto* p = static_cast<uint8_t*>(buf);
    while (n)
    {
        DWORD got = 0;
        if (!ReadFile(h, p, DWORD(n), &got, nullptr) || !got) return false;
        p += got;
        n -= got;
    }
    return true;
}

bool ReadFrame(HANDLE h, uint8_t& type, std::vector<uint8_t>& payload)
{
    uint8_t head[5];
    if (!ReadAll(h, head, 5)) return false;
    const uint32_t size = Get32(head);
    if (size == 0 || size > kMaxFrame) return false;
    type = head[4];
    payload.resize(size - 1);
    return size == 1 || ReadAll(h, payload.data(), payload.size());
}

// One writer per pipe; frames from several threads never interleave.
struct Writer
{
    HANDLE h = INVALID_HANDLE_VALUE;
    std::mutex m;

    bool WriteAll(const void* buf, size_t n)
    {
        auto* p = static_cast<const uint8_t*>(buf);
        while (n)
        {
            DWORD put = 0;
            if (!WriteFile(h, p, DWORD(n), &put, nullptr) || !put) return false;
            p += put;
            n -= put;
        }
        return true;
    }
    bool Frame(uint8_t type, const void* a, size_t an, const void* b = nullptr, size_t bn = 0)
    {
        std::lock_guard l(m);
        if (h == INVALID_HANDLE_VALUE || 1 + an + bn > kMaxFrame) return false;
        uint8_t head[5];
        Put32(head, uint32_t(1 + an + bn));
        head[4] = type;
        return WriteAll(head, 5) && (!an || WriteAll(a, an)) && (!bn || WriteAll(b, bn));
    }
};

Writer g_out;                                   // game: to the helper; helper: to the game

// ---- game side
bool g_inproc = false;                          // TTT2_STEAM_INPROC=1
std::atomic<bool> g_running{ false };
std::atomic<uint64_t> g_local{ 0 };
Receiver g_receiver = nullptr;
HANDLE g_in = INVALID_HANDLE_VALUE;             // from the helper
HANDLE g_helper = nullptr;
std::mutex g_mutex;
std::condition_variable g_cv;
int g_ready = 0;                                // 0 waiting, 1 Steam up, -1 failed
uint32_t g_nextRequest = 1;
std::map<uint32_t, std::vector<std::vector<uint8_t>>> g_lists;
std::deque<std::vector<uint8_t>> g_joins;
// Desired lobby state survives the helper process, whose Steam lobby IDs do not.
std::mutex g_hostedMutex;
std::map<uint64_t, std::vector<uint8_t>> g_hostedRecords;

void Reader()
{
    uint8_t type = 0;
    std::vector<uint8_t> p;
    while (ReadFrame(g_in, type, p))
    {
        if (type == kRecv && p.size() >= 9)
        {
            if (g_receiver) g_receiver(Get64(p.data()), p[8] != 0, p.data() + 9, p.size() - 9);
        }
        else if (type == kReady && p.size() >= 9)
        {
            std::lock_guard l(g_mutex);
            g_ready = p[0] ? 1 : -1;
            g_local = Get64(p.data() + 1);
            g_cv.notify_all();
        }
        else if (type == kListResult && p.size() >= 8)
        {
            std::vector<std::vector<uint8_t>> records;
            const uint32_t n = Get32(p.data() + 4);
            size_t at = 8;
            for (uint32_t i = 0; i < n && at + 4 <= p.size(); ++i)
            {
                const uint32_t size = Get32(p.data() + at);
                at += 4;
                if (at + size > p.size()) break;
                records.emplace_back(p.begin() + at, p.begin() + at + size);
                at += size;
            }
            std::lock_guard l(g_mutex);
            g_lists[Get32(p.data())] = std::move(records);
            g_cv.notify_all();
        }
        else if (type == kJoin)
        {
            std::lock_guard l(g_mutex);
            g_joins.push_back(p);
        }
    }
    const bool wasUp = g_running.load();
    std::fprintf(stderr, "[steam] helper process ended - Steam online stopped\n");
    g_running = false;
    {
        std::lock_guard l(g_mutex);
        if (!g_ready) g_ready = -1;
        g_cv.notify_all();
    }
    // A helper that was up died mid-session: bring Steam back now (Start() republishes hosted
    // lobbies), not only on the game's next search or publish - a host just waits and would
    // otherwise stay invisible. A few tries, backing off; Start() serialises with other callers.
    if (wasUp)
        std::thread([] {
            for (int attempt = 0; attempt < 5 && !g_running; ++attempt)
            {
                Sleep(DWORD(2000u << attempt));
                if (g_running || Start(g_receiver)) break;
            }
        }).detach();
}

bool Launch()
{
    wchar_t self[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return false;
    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    HANDLE childIn = nullptr, toChild = nullptr, fromChild = nullptr, childOut = nullptr, childErr = nullptr;
    if (!CreatePipe(&childIn, &toChild, &sa, 1u << 20)) return false;
    if (!CreatePipe(&fromChild, &childOut, &sa, 1u << 20)) { CloseHandle(childIn); CloseHandle(toChild); return false; }
    SetHandleInformation(toChild, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(fromChild, HANDLE_FLAG_INHERIT, 0);
    // The helper logs into our log (file or redirected stderr).
    const HANDLE err = HANDLE(_get_osfhandle(_fileno(stderr)));
    if (err && err != INVALID_HANDLE_VALUE)
        DuplicateHandle(GetCurrentProcess(), err, GetCurrentProcess(), &childErr, 0, TRUE, DUPLICATE_SAME_ACCESS);

    // Inherit exactly these handles, nothing else the game has open.
    HANDLE inherit[3] = { childIn, childOut, childErr };
    const DWORD inheritCount = childErr ? 3 : 2;
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    const bool attrsOk = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                         UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                   inheritCount * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = childIn;
    si.StartupInfo.hStdOutput = childOut;
    si.StartupInfo.hStdError = childErr;
    if (attrsOk) si.lpAttributeList = attrs;
    std::wstring line = L"\"" + std::wstring(self) + L"\" --steam-bridge";
    PROCESS_INFORMATION pi = {};
    const BOOL created = CreateProcessW(self, line.data(), nullptr, nullptr, TRUE,
                                        CREATE_SUSPENDED | CREATE_NO_WINDOW | (attrsOk ? EXTENDED_STARTUPINFO_PRESENT : 0),
                                        nullptr, nullptr, &si.StartupInfo, &pi);
    if (attrsOk) DeleteProcThreadAttributeList(attrs);
    CloseHandle(childIn);
    CloseHandle(childOut);
    if (childErr) CloseHandle(childErr);
    if (!created)
    {
        std::fprintf(stderr, "[steam] could not start the helper process (error %lu)\n", GetLastError());
        CloseHandle(toChild);
        CloseHandle(fromChild);
        return false;
    }
    // Backstop: the helper is killed when the game goes, however it goes (the job handle is never
    // closed explicitly; the OS closes it at exit).
    if (HANDLE job = CreateJobObjectW(nullptr, nullptr))
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit = {};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof limit);
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_helper = pi.hProcess;
    g_out.h = toChild;
    g_in = fromChild;
    return true;
}

} // namespace

bool Start(Receiver receiver)
{
    static std::mutex m;
    std::lock_guard l(m);
    if (Running()) return true;
    if (const char* e = std::getenv("TTT2_STEAM_INPROC"); e && *e == '1')
    {
        g_inproc = true;
        std::fprintf(stderr, "[steam] TTT2_STEAM_INPROC=1: Steam in the game process (controllers will be hidden)\n");
        return direct::Start(receiver);
    }
    g_receiver = receiver;
    // The old Reader has exited before it clears g_running. Release the old
    // pipe/process handles before launching a new helper.
    {
        std::lock_guard writeLock(g_out.m);
        if (g_out.h != INVALID_HANDLE_VALUE) CloseHandle(g_out.h);
        g_out.h = INVALID_HANDLE_VALUE;
    }
    if (g_in != INVALID_HANDLE_VALUE) { CloseHandle(g_in); g_in = INVALID_HANDLE_VALUE; }
    if (g_helper) { CloseHandle(g_helper); g_helper = nullptr; }
    {
        std::lock_guard stateLock(g_mutex);
        g_ready = 0;
        g_lists.clear();
        g_joins.clear();
    }
    if (!Launch()) return false;
    std::thread(Reader).detach();
    std::unique_lock w(g_mutex);
    if (!g_cv.wait_for(w, std::chrono::seconds(20), [] { return g_ready != 0; }) || g_ready != 1)
    {
        std::fprintf(stderr, "[steam] Steam not available (helper %s)\n", g_ready ? "reported no Steam" : "timed out");
        if (!g_ready) TerminateProcess(g_helper, 1);
        return false;
    }
    g_running = true;
    std::fprintf(stderr, "[steam] Steam runs in helper process %lu (Steam id %016llX)\n", GetProcessId(g_helper),
                 (unsigned long long)g_local.load());
    // Republish all desired hosted sessions to the newly initialized helper.
    // Do not hold g_hostedMutex while writing to the helper pipe.
    std::map<uint64_t, std::vector<uint8_t>> hosted;
    {
        std::lock_guard hostedLock(g_hostedMutex);
        hosted = g_hostedRecords;
    }
    for (const auto& [id, record] : hosted)
    {
        uint8_t head[8];
        Put64(head, id);
        g_out.Frame(kPublish, head, 8, record.data(), record.size());
    }
    return true;
}

bool Running() { return g_inproc ? direct::Running() : g_running.load(); }
uint64_t LocalId() { return g_inproc ? direct::LocalId() : g_local.load(); }

bool Send(uint64_t peer, bool reliable, const void* data, size_t size)
{
    if (g_inproc) return direct::Send(peer, reliable, data, size);
    if (!g_running) return false;
    uint8_t head[9];
    Put64(head, peer);
    head[8] = reliable ? 1 : 0;
    return g_out.Frame(kSend, head, 9, data, size);
}

void PublishLobby(uint64_t sessionId, const std::vector<uint8_t>& record)
{
    if (g_inproc) return direct::PublishLobby(sessionId, record);
    {
        std::lock_guard l(g_hostedMutex);
        g_hostedRecords[sessionId] = record;
    }
    if (!g_running && !Start(g_receiver)) return;
    uint8_t head[8];
    Put64(head, sessionId);
    g_out.Frame(kPublish, head, 8, record.data(), record.size());
}

void RemoveLobby(uint64_t sessionId)
{
    if (g_inproc) return direct::RemoveLobby(sessionId);
    {
        std::lock_guard l(g_hostedMutex);
        g_hostedRecords.erase(sessionId);
    }
    if (!g_running) return;
    uint8_t head[8];
    Put64(head, sessionId);
    g_out.Frame(kRemove, head, 8);
}

std::vector<std::vector<uint8_t>> ListLobbies(int max, int timeoutMs)
{
    if (g_inproc) return direct::ListLobbies(max, timeoutMs);
    std::vector<std::vector<uint8_t>> out;
    if (!g_running && !Start(g_receiver)) return out;
    if (!g_running) return out;
    uint32_t request;
    {
        std::lock_guard l(g_mutex);
        request = g_nextRequest++;
    }
    uint8_t body[12];
    Put32(body, request);
    Put32(body + 4, uint32_t(max));
    Put32(body + 8, uint32_t(timeoutMs));
    if (!g_out.Frame(kList, body, 12)) return out;
    std::unique_lock l(g_mutex);
    g_cv.wait_for(l, std::chrono::milliseconds(timeoutMs + 3000), [&] { return g_lists.count(request) || !g_running; });
    const auto it = g_lists.find(request);
    if (it != g_lists.end())
    {
        out = std::move(it->second);
        g_lists.erase(it);
    }
    return out;
}

bool TakeJoinRequest(std::vector<uint8_t>& record)
{
    if (g_inproc) return direct::TakeJoinRequest(record);
    std::lock_guard l(g_mutex);
    if (g_joins.empty()) return false;
    record = std::move(g_joins.front());
    g_joins.pop_front();
    return true;
}

// ---- helper side
int RunBridge()
{
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    g_out.h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!in || in == INVALID_HANDLE_VALUE || !g_out.h || g_out.h == INVALID_HANDLE_VALUE) return 2;
    const bool ok = direct::Start([](uint64_t from, bool reliable, const uint8_t* data, size_t size) {
        uint8_t head[9];
        Put64(head, from);
        head[8] = reliable ? 1 : 0;
        g_out.Frame(kRecv, head, 9, data, size);
    });
    uint8_t ready[9] = { uint8_t(ok ? 1 : 0) };
    Put64(ready + 1, ok ? direct::LocalId() : 0);
    g_out.Frame(kReady, ready, 9);
    if (!ok) return 1;
    std::thread([] {
        for (;;)
        {
            std::vector<uint8_t> record;
            if (direct::TakeJoinRequest(record)) g_out.Frame(kJoin, record.data(), record.size());
            Sleep(50);
        }
    }).detach();
    uint8_t type = 0;
    std::vector<uint8_t> p;
    while (ReadFrame(in, type, p))
    {
        if (type == kSend && p.size() >= 9)
            direct::Send(Get64(p.data()), p[8] != 0, p.data() + 9, p.size() - 9);
        else if (type == kPublish && p.size() >= 8)
            direct::PublishLobby(Get64(p.data()), std::vector<uint8_t>(p.begin() + 8, p.end()));
        else if (type == kRemove && p.size() >= 8)
            direct::RemoveLobby(Get64(p.data()));
        else if (type == kList && p.size() >= 12)
        {
            const uint32_t request = Get32(p.data());
            const int max = int(Get32(p.data() + 4)), timeoutMs = int(Get32(p.data() + 8));
            std::thread([request, max, timeoutMs] {
                const auto records = direct::ListLobbies(max, timeoutMs);
                std::vector<uint8_t> body(8);
                Put32(body.data(), request);
                Put32(body.data() + 4, uint32_t(records.size()));
                for (const auto& r : records)
                {
                    const size_t at = body.size();
                    body.resize(at + 4 + r.size());
                    Put32(body.data() + at, uint32_t(r.size()));
                    if (!r.empty()) std::memcpy(body.data() + at + 4, r.data(), r.size());
                }
                g_out.Frame(kListResult, body.data(), body.size());
            }).detach();
        }
    }
    // The game closed its end (it exited or crashed): Steam leaves the lobbies with this process.
    return 0;
}

} // namespace steamnet
