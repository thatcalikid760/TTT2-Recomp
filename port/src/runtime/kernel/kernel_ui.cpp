// XamShowMessageBoxUI and XamShowKeyboardUI: the guest owns the result and XOVERLAPPED until
// completion, just as on Xbox. The box (or keyboard) is shown in the game's own dialog window by the system UI (sys_ui.h, menu_ext.cpp)
// - never a Win32 pop-up; all strings are copied before it is queued, and the answer completes the
// overlapped from the game's main thread without blocking it.
#include "kernel_shared.h"
#include "kernel_content.h"
#include "kernel_trace.h"
#include "sys_ui.h"
#include "phys_mirror.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

void XamNotifyEnqueueEvent(uint32_t id, uint32_t param);

namespace {
constexpr uint32_t kPending = 997, kInvalidParameter = 87;

bool ReadGuestBytes(uint8_t* base, uint32_t address, void* out, size_t size,
                    uint32_t alignment = 1) {
    if (!XamGuestRange(address, size, alignment)) return false;
    PhysPrepareHostAccess(address, uint32_t(size));      // ReadProcessMemory fails on a guard page
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), base + address, out, size, &read)
        && read == size;
}
bool ReadGuestWord(uint8_t* base, uint32_t address, uint32_t& word) {
    if (!ReadGuestBytes(base, address, &word, sizeof(word), 4)) return false;
    word = __builtin_bswap32(word);
    return true;
}
bool WritableGuestRange(uint8_t* base, uint32_t address, size_t size) {
    if (!XamGuestRange(address, size, 4)) return false;
    auto* at = base + address;
    const auto* end = at + size;
    while (at < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(at, &region, sizeof(region)) || region.State != MEM_COMMIT
            || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        switch (region.Protect & 0xFF) {
        case PAGE_READWRITE: case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: break;
        default: return false;
        }
        auto* next = static_cast<uint8_t*>(region.BaseAddress) + region.RegionSize;
        if (next <= at) return false;
        at = next;
    }
    return true;
}
bool CopyGuestText(uint8_t* base, uint32_t address, size_t limit,
                   bool optional, std::wstring& text) {
    text.clear();
    if (!address) return optional;
    for (size_t i = 0; i < limit; ++i) {
        const uint64_t at = uint64_t(address) + i * 2;
        uint16_t unit;
        if (at > UINT32_MAX || !ReadGuestBytes(base, uint32_t(at), &unit, 2, 2)) return false;
        unit = __builtin_bswap16(unit);
        if (!unit) return true;
        text.push_back(static_cast<wchar_t>(unit));
    }
    return false; // Reject unterminated / unbounded guest strings.
}
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// `text` (ASCII) into a guest wide-string buffer of `capacity` units, terminated.
void WriteGuestText(uint8_t* base, uint32_t buffer, uint32_t capacity, const std::string& text) {
    if (!capacity) return;
    const uint32_t n = (std::min)(uint32_t(text.size()), capacity - 1);
    for (uint32_t i = 0; i < n; ++i) GWrite16(base, buffer + i * 2, uint8_t(text[i]));
    GWrite16(base, buffer + n * 2, 0);
}

// GAME CHAT (the keyboard the game opens with no title for a 31-character buffer: 30 characters a message, the
// size of its messages in the lobby and on the wire): up to kChatLength characters are typed, and a longer message
// goes out as several of the game's own, split at spaces. The game reopens the keyboard after each message; the
// rest answers those reopenings, without showing the keyboard.
constexpr uint32_t kChatLength = 80;
std::mutex g_chatMutex;
std::deque<std::string> g_chatRest;               // the pieces still to send

std::vector<std::string> SplitChat(const std::string& text, size_t width) {
    std::vector<std::string> parts;
    size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && text[at] == ' ') ++at;
        if (at >= text.size()) break;
        size_t end = (std::min)(text.size(), at + width);
        if (end < text.size() && text[end] != ' ') {
            const size_t space = text.rfind(' ', end);
            if (space != std::string::npos && space > at) end = space;
        }
        parts.push_back(text.substr(at, end - at));
        at = end;
    }
    return parts;
}

struct Completion { uint8_t* base; uint32_t result, overlapped; };
void Finish(const Completion& c, int choice) {
    GWrite32(c.base, c.result, uint32_t(choice));
    XamCompleteOverlapped(c.base, c.overlapped, 0, 0, 0);
    // Match XAM's UI-close ordering: polling may observe the completed request before the
    // UI-hidden notification.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    XamNotifyEnqueueEvent(9, 0);
}
} // namespace

// ABI reference: xenia/kernel/xam/xam_ui.cc XamShowMessageBoxUI_entry.
// Argument nine is at SP+0x54, after the eight register arguments.
PPC_FUNC(__imp__XamShowMessageBoxUI) {
    KTrace("XamShowMessageBoxUI");
    const uint32_t count = ctx.r6.u32, buttonArray = ctx.r7.u32;
    const uint32_t active = ctx.r8.u32, result = ctx.r10.u32;
    uint32_t overlapped;
    if (uint64_t(ctx.r1.u32) + 0x54 > UINT32_MAX
        || !ReadGuestWord(base, ctx.r1.u32 + 0x54, overlapped)) {
        ctx.r3.u32 = kInvalidParameter; return;
    }
    // The renderer must keep servicing frames while a player is choosing. The title uses the
    // asynchronous API; synchronous calls are explicitly unsupported.
    if (!overlapped) { ctx.r3.u32 = ERROR_NOT_SUPPORTED; return; }
    if (count < 1 || count > 4 || active >= count
        || !XamGuestRange(buttonArray, uint64_t(count) * 4, 4)
        || !WritableGuestRange(base, result, 4)
        || !WritableGuestRange(base, overlapped, 28)
        || (uint64_t(result) < uint64_t(overlapped) + 28 && uint64_t(overlapped) < uint64_t(result) + 4)) {
        ctx.r3.u32 = kInvalidParameter; return;
    }
    if (const uint32_t error = XamValidateOverlapped(base, overlapped)) {
        ctx.r3.u32 = error; return;
    }
    try {
        std::wstring title, text;
        if (!CopyGuestText(base, ctx.r4.u32, 256, true, title)
            || !CopyGuestText(base, ctx.r5.u32, 8192, true, text)) {
            ctx.r3.u32 = kInvalidParameter; return;
        }
        SysUiMessageBox box;
        box.text = Utf8(text);
        box.active = int(active);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t pointer;
            std::wstring label;
            if (!ReadGuestWord(base, buttonArray + i * 4, pointer)
                || !CopyGuestText(base, pointer, 256, false, label) || label.empty()) {
                ctx.r3.u32 = kInvalidParameter; return;
            }
            if (_wcsicmp(label.c_str(), L"Cancel") == 0) box.cancel = int(i);
            box.buttons.push_back(Utf8(label));
        }
        std::fprintf(stderr, "[xam] XamShowMessageBoxUI '%s' (%u buttons)\n", Utf8(title).c_str(), count);
        const Completion completion{ base, result, overlapped };
        box.done = [completion](int choice) {
            std::thread([completion, choice] { Finish(completion, choice); }).detach();
        };
        // Pending before the box is queued (the answer may complete it from the main thread at
        // once); a refused box restores the caller's words.
        const uint32_t saved[3] = { GRead32(base, overlapped), GRead32(base, overlapped + 4),
                                    GRead32(base, overlapped + 24) };
        GWrite32(base, overlapped + 4, 0);
        GWrite32(base, overlapped + 24, 0);
        std::atomic_thread_fence(std::memory_order_release);
        GWrite32(base, overlapped, kPending);
        if (!SysUiShowMessageBox(std::move(box))) {
            GWrite32(base, overlapped, saved[0]);
            GWrite32(base, overlapped + 4, saved[1]);
            GWrite32(base, overlapped + 24, saved[2]);
            ctx.r3.u32 = ERROR_ACCESS_DENIED; return;
        }
        XamNotifyEnqueueEvent(9, 1);
        ctx.r3.u32 = kPending;
    } catch (...) {
        ctx.r3.u32 = ERROR_NOT_ENOUGH_MEMORY;
    }
}

// XamShowKeyboardUI(user, flags, defaultText, title, description, buffer, bufferLength,
// pOverlapped): the Xbox virtual keyboard - TTT2's online GAME CHAT asks for its message with it.
// Shown in the game's dialog window by the system UI, typed on the PC keyboard; the answer writes
// the text into `buffer` (bufferLength wide characters, terminated) and completes the overlapped
// with ERROR_SUCCESS, or with ERROR_CANCELLED (extended error HRESULT_FROM_WIN32) and an empty
// buffer.
// ABI reference: xenia/kernel/xam/xam_ui.cc XamShowKeyboardUI_entry.
PPC_FUNC(__imp__XamShowKeyboardUI) {
    KTrace("XamShowKeyboardUI");
    constexpr uint32_t kCancelled = 1223;
    const uint32_t buffer = ctx.r8.u32, capacity = ctx.r9.u32, overlapped = ctx.r10.u32;
    if (!buffer || !capacity || capacity > 0x10000 || !WritableGuestRange(base, buffer, uint64_t(capacity) * 2)) {
        ctx.r3.u32 = kInvalidParameter; return;
    }
    try {
        std::wstring defaultText, title, description;
        if (!CopyGuestText(base, ctx.r5.u32, 0x10000, true, defaultText)
            || !CopyGuestText(base, ctx.r6.u32, 256, true, title)
            || !CopyGuestText(base, ctx.r7.u32, 2048, true, description)) {
            ctx.r3.u32 = kInvalidParameter; return;
        }
        std::fprintf(stderr, "[xam] XamShowKeyboardUI '%s' (flags %08X, %u characters)%s\n", Utf8(title).c_str(),
                     ctx.r4.u32, capacity, overlapped ? "" : " synchronous");
        if (!overlapped) {
            // Nobody can type while the caller waits: the default text, as Xenia's headless UI.
            WriteGuestText(base, buffer, capacity, Utf8(defaultText));
            ctx.r3.u32 = 0; return;
        }
        if (!WritableGuestRange(base, overlapped, 28)
            || (uint64_t(buffer) < uint64_t(overlapped) + 28 && uint64_t(overlapped) < uint64_t(buffer) + uint64_t(capacity) * 2)) {
            ctx.r3.u32 = kInvalidParameter; return;
        }
        if (const uint32_t error = XamValidateOverlapped(base, overlapped)) {
            ctx.r3.u32 = error; return;
        }
        const bool chat = title.empty() && capacity == 31;
        std::string next;
        if (chat) {
            std::lock_guard l(g_chatMutex);
            if (!g_chatRest.empty()) { next = std::move(g_chatRest.front()); g_chatRest.pop_front(); }
        }
        SysUiKeyboard keyboard;
        keyboard.title = Utf8(title);
        keyboard.description = Utf8(description);
        keyboard.text = Utf8(defaultText);
        keyboard.maxLength = chat ? kChatLength : capacity - 1;
        keyboard.done = [base, buffer, capacity, overlapped, chat](bool ok, const std::string& typed) {
            // A cancel empties the buffer: GAME CHAT reopens the keyboard after every message and
            // sends whatever the buffer holds when it is closed - the last message, again.
            std::string text = ok ? typed : std::string();
            if (chat && (!ok || text.size() > capacity - 1)) {
                // a cancel drops what was left of a long message; a long one is split
                std::vector<std::string> parts = SplitChat(text, capacity - 1);
                std::lock_guard l(g_chatMutex);
                g_chatRest.clear();
                if (parts.size() > 1) g_chatRest.assign(parts.begin() + 1, parts.end());
                if (!parts.empty()) text = parts[0];
            }
            WriteGuestText(base, buffer, capacity, text);
            std::thread([base, overlapped, ok] {
                XamCompleteOverlapped(base, overlapped, ok ? 0 : kCancelled, 0, ok ? 0 : 0x800704C7u);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                XamNotifyEnqueueEvent(9, 0);                  // XN_SYS_UI closed
            }).detach();
        };
        const uint32_t saved[3] = { GRead32(base, overlapped), GRead32(base, overlapped + 4),
                                    GRead32(base, overlapped + 24) };
        GWrite32(base, overlapped + 4, 0);
        GWrite32(base, overlapped + 24, 0);
        std::atomic_thread_fence(std::memory_order_release);
        GWrite32(base, overlapped, kPending);
        if (!next.empty()) {
            // the next piece of a long chat message: answered at once, the keyboard not shown
            keyboard.done(true, next);
            XamNotifyEnqueueEvent(9, 1);                      // XN_SYS_UI opened
            ctx.r3.u32 = kPending;
            return;
        }
        if (!SysUiShowKeyboard(std::move(keyboard))) {
            GWrite32(base, overlapped, saved[0]);
            GWrite32(base, overlapped + 4, saved[1]);
            GWrite32(base, overlapped + 24, saved[2]);
            ctx.r3.u32 = ERROR_ACCESS_DENIED; return;
        }
        XamNotifyEnqueueEvent(9, 1);                          // XN_SYS_UI opened
        ctx.r3.u32 = kPending;
    } catch (...) {
        ctx.r3.u32 = ERROR_NOT_ENOUGH_MEMORY;
    }
}
