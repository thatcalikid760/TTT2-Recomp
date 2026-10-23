// ttt2_setup.exe -- the installer for the Tekken Tag Tournament 2 PC port. Modeled on the Tom &
// Jerry port's installer (same dialogs, uninstaller guard and /silent test mode).
//
// IT SHIPS NO GAME DATA. The player supplies their own Xbox 360 disc image and Title Update 3, and
// everything that came off them is produced at install time:
//   - the disc's files (dataNNN.bin archives, nrfo.bin, ...), 26 archives LZX-expanded on the way;
//   - TU3's data100.bin (expanded) + data100.ofs into game\update (the game's "update:" device);
//   - game\tekken.img: the disc's Default.xex with TU3's Default.xexp applied, decrypted - checked
//     against the SHA-1 of the image the port's executable was recompiled from.
// The only files carried inside this executable are ours (the port's exe, the Steamworks client
// library, the VC++ runtime, a readme) and an original icon.
//
// Install layout:
//     <Program Files>\Tekken Tag Tournament 2\        exe + runtime + game\ (read-only)
//     %LOCALAPPDATA%\TTT2Recomp\                       saves, profiles, settings, shader cache, log
// Uninstalling leaves %LOCALAPPDATA%\TTT2Recomp alone unless the player ticks the box.
// Run again over an install, it offers an update instead (RunUpdate): only the files it carries
// are replaced, so no disc image or title update is needed; `/update [folder]` does that silently.
#include "setup/payload_ids.h"
#include "setup/payload.h"
#include "setup/game_data.h"
#include "setup/stfs.h"
#include "setup/xdvdfs.h"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <objbase.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <cmath>
#include <cwctype>

#include <algorithm>
#include <string>
#include <vector>

using namespace ttt2::setup;

#ifndef TTT2_VERSION_STR
#define TTT2_VERSION_STR "0.0.0"
#endif
#define TTT2_WIDE2(s) L##s
#define TTT2_WIDE(s) TTT2_WIDE2(s)
#define TTT2_VERSION_W TTT2_WIDE(TTT2_VERSION_STR)

#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_DONE     (WM_APP + 2)

static const wchar_t kAppName[]      = L"Tekken Tag Tournament 2 (PC port)";
static const wchar_t kFolderName[]   = L"Tekken Tag Tournament 2";
static const wchar_t kRegKey[]       = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TTT2Recomp";
static const wchar_t kShortcut[]     = L"Tekken Tag Tournament 2.lnk";
static const wchar_t kUninstallExe[] = L"uninstall.exe";
static const wchar_t kUserDataDir[]  = L"TTT2Recomp";      // under %LOCALAPPDATA%
static const wchar_t kImageName[]    = L"game\\tekken.img";

// ---------------------------------------------------------------- shared state

struct InstallJob {
    HWND         dlg = nullptr;
    std::wstring iso, tu, dest;
    bool         scDesktop = true, scStartMenu = true;
    volatile LONG cancel = 0;

    CRITICAL_SECTION lock;
    int          pct = 0;
    std::wstring status;
    bool         ok = false;
    std::wstring error;
    HANDLE       log = INVALID_HANDLE_VALUE;   // /silent only
    std::vector<std::wstring> written;         // files this run created, removed again on failure

    void Set(int p, const std::wstring& s) {
        EnterCriticalSection(&lock);
        pct = p; status = s;
        LeaveCriticalSection(&lock);
        if (dlg) {
            PostMessageW(dlg, WM_APP_PROGRESS, 0, 0);
        } else if (log != INVALID_HANDLE_VALUE) {
            std::wstring w = s + L"\r\n";
            int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string utf8((size_t)(n > 0 ? n : 0), '\0');
            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &utf8[0], n, nullptr, nullptr);
            char head[16];
            int hn = _snprintf_s(head, sizeof head, _TRUNCATE, "[%3d%%] ", p);
            DWORD put = 0;
            if (hn > 0) WriteFile(log, head, (DWORD)hn, &put, nullptr);
            if (!utf8.empty()) WriteFile(log, utf8.data(), (DWORD)utf8.size(), &put, nullptr);
        }
    }
};

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// ---------------------------------------------------------------- filesystem helpers

static bool MakeDirs(const std::wstring& dir) {
    for (size_t i = 3; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == L'\\') {
            std::wstring part = dir.substr(0, i);
            if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                if (GetFileAttributesW(part.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
            }
        }
    }
    return true;
}

static void UnLog(const wchar_t* fmt, ...);

// Is this folder one of our installs? The uninstaller deletes a whole tree, so it never takes a
// path on trust - not from its own location, and not from a registry value. Markers that never
// change: the game image the installer built and the Steamworks library beside the exe.
static bool LooksLikeInstall(const std::wstring& dir) {
    if (dir.size() < 4) return false;
    return GetFileAttributesW((dir + L"\\" + kImageName).c_str()) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesW((dir + L"\\steam_api64.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
}

static void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        UnLog(L"    FindFirstFile(\"%s\\*\") failed, err %lu", dir.c_str(), GetLastError());
        return;
    }
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(p);
        else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(dir.c_str());
}

static uint64_t DirSizeKb(const std::wstring& dir) {
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += DirSizeKb(p) * 1024ull;
        else total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total / 1024ull;
}

static std::wstring KnownDir(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) { out = p; CoTaskMemFree(p); }
    return out;
}

static bool MakeShortcut(const std::wstring& lnkPath, const std::wstring& target,
                         const std::wstring& workDir, const std::wstring& iconPath) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link)))
        return false;
    bool ok = false;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(workDir.c_str());
    link->SetDescription(L"Tekken Tag Tournament 2 (PC port)");
    if (!iconPath.empty()) link->SetIconLocation(iconPath.c_str(), 0);
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        ok = SUCCEEDED(pf->Save(lnkPath.c_str(), TRUE));
        pf->Release();
    }
    link->Release();
    return ok;
}

static bool WriteBytes(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = size == 0 || WriteFile(h, data, (DWORD)size, &put, nullptr);
    CloseHandle(h);
    return ok && put == size;
}

// ---------------------------------------------------------------- the title update

// The player may point at the TU file or at the folder holding all three updates: pick the
// package of this game whose Default.xexp targets 0.0.3.2.
static bool OpenTitleUpdate(const std::wstring& path, StfsPackage& pkg, std::vector<uint8_t>& xexp, std::wstring& why) {
    std::vector<std::wstring> candidates;
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) { why = L"The title update file was not found."; return false; }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) candidates.push_back(path + L"\\" + fd.cFileName); }
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    } else candidates.push_back(path);

    std::wstring seen;
    for (const std::wstring& c : candidates) {
        std::string err;
        if (!pkg.Open(c.c_str(), err)) continue;
        if (pkg.TitleId() != kTitleId) { pkg.Close(); continue; }
        if (!pkg.ReadFile("Default.xexp", xexp)) { pkg.Close(); continue; }
        const std::string version = XexpTargetVersion(xexp);
        if (version == "0.0.3.2") return true;
        seen = Widen(version);
        pkg.Close();
    }
    if (!seen.empty())
        why = L"That is the title update for game version " + seen + L". The port needs Title Update 3 "
              L"(version 0.0.3.2, file name ending in \"O2\").";
    else
        why = L"No Tekken Tag Tournament 2 title update was found there. Choose the Title Update 3 "
              L"file (TU_174Q22P_0000008000000.00000000000O2) or the folder holding it.";
    return false;
}

// ---------------------------------------------------------------- the install worker

static bool Fail(InstallJob* job, const std::wstring& why) {
    job->error = why;
    job->Set(job->pct, L"Failed: " + why);
    return false;
}

// Add or remove programs (HKLM): written by an install, refreshed by an update.
static void Register(const std::wstring& dest) {
    const std::wstring game = dest + L"\\" + kGameExeName, uninst = dest + L"\\" + kUninstallExe;
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    auto S = [&](const wchar_t* n, const std::wstring& v) {
        RegSetValueExW(k, n, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    };
    auto D = [&](const wchar_t* n, DWORD v) { RegSetValueExW(k, n, 0, REG_DWORD, (const BYTE*)&v, sizeof v); };
    S(L"DisplayName", kAppName);
    S(L"DisplayVersion", TTT2_VERSION_W);
    S(L"Publisher", L"Native PC port");
    S(L"InstallLocation", dest);
    S(L"DisplayIcon", game);
    S(L"UninstallString", L"\"" + uninst + L"\" /uninstall");
    S(L"QuietUninstallString", L"\"" + uninst + L"\" /uninstall /quiet");
    D(L"NoModify", 1);
    D(L"NoRepair", 1);
    D(L"EstimatedSize", (DWORD)DirSizeKb(dest));
    RegCloseKey(k);
}

static std::wstring RegString(const wchar_t* name) {
    std::wstring out;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return out;
    wchar_t v[MAX_PATH] = {}; DWORD n = sizeof v - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, (BYTE*)v, &n) == ERROR_SUCCESS && type == REG_SZ) out = v;
    RegCloseKey(k);
    return out;
}

// The install Add or remove programs names, if it is really one of ours; empty otherwise.
static std::wstring ExistingInstall() {
    const std::wstring dir = RegString(L"InstallLocation");
    return LooksLikeInstall(dir) ? dir : std::wstring();
}

static bool ReadWhole(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1ll << 30);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

// ---------------------------------------------------------------- the update worker
//
// An update replaces only the files this installer carries (the port's exe and DLLs, the readme)
// and the uninstaller, and refreshes Add or remove programs. The game files built from the
// player's disc and title update stay; game\tekken.img must be the image this exe was recompiled
// from. New files are written next to the install first, then swapped in, so a failed write
// leaves the old version complete.
static bool RunUpdate(InstallJob* job) {
    std::string err;
    job->Set(5, L"Checking the installed game\x2026");
    if (!LooksLikeInstall(job->dest))
        return Fail(job, L"No installed copy of the game was found in that folder.");
    std::vector<uint8_t> image;
    if (!ReadWhole(job->dest + L"\\" + kImageName, image) || Sha1Hex(image.data(), image.size()) != kImageSha1)
        return Fail(job, L"The installed game files do not match this version. Run a full install instead.");

    // A running game holds its exe: nothing can be replaced until it is closed.
    const std::wstring game = job->dest + L"\\" + kGameExeName;
    HANDLE h = CreateFileW(game.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND)
        return Fail(job, L"The game is running. Close it, then update again.");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    job->Set(20, L"Writing the new version\x2026");
    const std::wstring staging = job->dest + L"\\update.tmp";
    DeleteTree(staging);
    if (!MakeDirs(staging)) return Fail(job, L"Could not write to the install folder.");
    std::vector<std::wstring> staged;
    if (!WritePayload(staging.c_str(), err, &staged)) {
        DeleteTree(staging);
        return Fail(job, Widen(err));
    }

    job->Set(70, L"Replacing the game's files\x2026");
    for (const std::wstring& from : staged) {
        const std::wstring to = job->dest + from.substr(staging.size());
        if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteTree(staging);
            return Fail(job, L"Could not replace " + to.substr(job->dest.size() + 1) +
                             L". Close the game and anything else using its folder, then update again.");
        }
    }
    DeleteTree(staging);

    job->Set(90, L"Updating the uninstaller\x2026");
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    CopyFileW(self, (job->dest + L"\\" + kUninstallExe).c_str(), FALSE);
    Register(job->dest);

    job->ok = true;
    job->Set(100, L"Done.");
    return true;
}

struct Planned { const IsoEntry* entry; uint64_t outBytes; bool expand; };

// The game's pixel shaders into the player's shader cache now, so play never waits for one to compile
// (a cold cache cost a real match 267 builds and dropped it to 28-36 fps in places). The installed game
// runs once as `--build-shaders`: its own translator and compiler, from the files just installed - a
// few minutes on all cores. Its "[shaders] <done>/<total>" lines move the bar from `from` to `to` %.
// Not fatal when it cannot run or fails: the game runs it again in the background at its next start.
// False: cancelled.
static bool PrepareShaders(InstallJob* job, int from, int to) {
    const std::wstring exe = job->dest + L"\\" + kGameExeName;
    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return true;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi = {};
    std::wstring line = L"\"" + exe + L"\" --build-shaders";
    std::vector<wchar_t> cmd(line.begin(), line.end());
    cmd.push_back(0);
    const BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                                        nullptr, job->dest.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!started) { CloseHandle(rd); return true; }
    CloseHandle(pi.hThread);
    job->Set(from, L"Preparing the game's shaders (one time, for smooth play)\x2026");
    std::string pending;
    char buf[4096];
    DWORD n = 0;
    int shown = -1;
    bool cancelled = false;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) {
        pending.append(buf, n);
        for (size_t eol; (eol = pending.find('\n')) != std::string::npos; pending.erase(0, eol + 1)) {
            unsigned done = 0, total = 0;
            if (sscanf_s(pending.substr(0, eol).c_str(), "[shaders] %u/%u", &done, &total) != 2 || !total) continue;
            const int pct = from + (int)((uint64_t)done * (to - from) / total);
            if (pct == shown && done != total) continue;
            shown = pct;
            wchar_t s[160];
            swprintf_s(s, L"Preparing the game's shaders (one time, for smooth play): %u of %u\x2026", done, total);
            job->Set(pct, s);
        }
        if (InterlockedCompareExchange(&job->cancel, 0, 0)) {
            TerminateProcess(pi.hProcess, 1);
            cancelled = true;
            break;
        }
    }
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return !cancelled;
}

static bool RunInstall(InstallJob* job) {
    std::string err;
    std::wstring why;

    job->Set(1, L"Reading the disc image\x2026");
    XdvdfsImage img;
    if (!img.Open(job->iso.c_str(), err)) return Fail(job, Widen(err));

    // Identity: the base executable must be the retail one the port was built from - the title
    // update is a delta against it and the image check below would fail anyway.
    const IsoEntry* xexEntry = img.Find("Default.xex");
    std::vector<uint8_t> baseXex;
    if (!xexEntry || !img.ReadEntry(*xexEntry, baseXex))
        return Fail(job, L"That disc image is not Tekken Tag Tournament 2 (no Default.xex).");
    if (Sha1Hex(baseXex.data(), baseXex.size()) != kBaseXexSha1)
        return Fail(job, L"That disc image is not the Tekken Tag Tournament 2 (USA, Europe) retail disc this port "
                         L"was made for, or the image is damaged.");

    job->Set(2, L"Reading the title update\x2026");
    StfsPackage tu;
    std::vector<uint8_t> xexp, tuArchive, tuIndex;
    if (!OpenTitleUpdate(job->tu, tu, xexp, why)) return Fail(job, why);
    if (!tu.ReadFile("data100.bin", tuArchive) || !tu.ReadFile("data100.ofs", tuIndex))
        return Fail(job, L"The title update file is incomplete or damaged.");
    const uint64_t tuExpanded = RecordExpandedSize(tuIndex);
    if (tuArchive.size() < 16 || !IsLzxContainer(tuArchive.data()) || !tuExpanded)
        return Fail(job, L"The title update file is incomplete or damaged.");

    job->Set(3, L"Applying Title Update 3 to the game\x2026");
    std::vector<uint8_t> image;
    if (!BuildGameImage(baseXex, xexp, image, err)) return Fail(job, Widen(err));

    // What to install: every top-level file of the game partition except the avatar pack (the
    // dashboard's, not the game's). Compressed archives are expanded to the size nrfo.bin gives.
    const IsoEntry* nrfoEntry = img.Find("nrfo.bin");
    std::vector<uint8_t> nrfo;
    if (!nrfoEntry || !img.ReadEntry(*nrfoEntry, nrfo)) return Fail(job, L"The disc image is incomplete (no nrfo.bin).");
    std::vector<std::string> archives;
    for (const IsoEntry& e : img.Entries())
        if (!e.dir && e.path.find('\\') == std::string::npos && e.path.size() == 11 &&
            _strnicmp(e.path.c_str(), "data", 4) == 0 && _stricmp(e.path.c_str() + 7, ".bin") == 0)
            archives.push_back(e.path);
    std::sort(archives.begin(), archives.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    if (archives.size() != 42) return Fail(job, L"The disc image is incomplete (expected 42 game archives).");
    const auto sizes = ArchiveSizes(nrfo, archives);

    std::vector<Planned> plan;
    uint64_t total = image.size() + tuExpanded + tuIndex.size();
    for (const IsoEntry& e : img.Entries()) {
        if (e.dir || e.path.find('\\') != std::string::npos || _stricmp(e.path.c_str(), "AvatarAssetPack") == 0) continue;
        uint8_t head[4] = {};
        if (e.size >= 4 && !img.ReadAt(e.offset, head, 4)) return Fail(job, L"The disc image could not be read.");
        Planned p{ &e, e.size, false };
        const auto it = sizes.find(e.path);
        if (e.size >= 16 && IsLzxContainer(head)) {
            if (it == sizes.end() || !it->second) return Fail(job, L"The disc image does not match the expected game data.");
            p.outBytes = it->second;
            p.expand = true;
        }
        plan.push_back(p);
        total += p.outBytes;
    }

    ULARGE_INTEGER freeAvail = {};
    if (GetDiskFreeSpaceExW(job->dest.substr(0, 3).c_str(), &freeAvail, nullptr, nullptr)) {
        const uint64_t need = total + PayloadBytes() + (256ull << 20);
        if (freeAvail.QuadPart < need) {
            wchar_t msg[256];
            swprintf_s(msg, L"There is not enough free space on that drive. About %.1f GB is needed.", need / 1e9);
            return Fail(job, msg);
        }
    }

    job->Set(4, L"Creating the install folder\x2026");
    const std::wstring gameDir = job->dest + L"\\game";
    if (!MakeDirs(gameDir + L"\\update"))
        return Fail(job, L"Could not create the install folder. Try a different location.");
    if (!WritePayload(job->dest.c_str(), err, &job->written)) return Fail(job, Widen(err));

    // The game files: 5..80% of the bar, by bytes written (the shaders take 82..97%).
    uint64_t done = 0;
    auto report = [&](const std::wstring& what, uint64_t extra) {
        if (InterlockedCompareExchange(&job->cancel, 0, 0)) return false;
        const int pct = 5 + (int)((done + extra) * 75 / (total ? total : 1));
        static uint64_t shown = 0;
        if (done + extra - shown > (16u << 20) || extra == 0) {
            shown = done + extra;
            job->Set(pct, what);
        }
        return true;
    };
    std::vector<uint8_t> buf(1u << 20);
    for (const Planned& p : plan) {
        const std::wstring name = Widen(p.entry->path), out = gameDir + L"\\" + name;
        job->written.push_back(out);
        if (p.expand) {
            const std::wstring what = L"Unpacking " + name + L"\x2026";
            const IsoEntry* e = p.entry;
            if (!ExpandContainer([&](uint64_t off, void* b, uint32_t n) { return img.ReadAt(e->offset + off, b, n); },
                                 e->size, p.outBytes, out, [&](uint64_t w) { return report(what, w); }, err))
                return Fail(job, err == "Installation cancelled." ? Widen(err) : L"Could not unpack " + name + L": " + Widen(err) + L".");
        } else {
            const std::wstring what = L"Copying " + name + L"\x2026";
            HANDLE oh = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (oh == INVALID_HANDLE_VALUE) return Fail(job, L"Could not write to the install folder.");
            uint64_t left = p.entry->size, at = 0;
            bool ok = true;
            while (left && ok) {
                const uint32_t n = (uint32_t)(std::min<uint64_t>)(left, buf.size());
                DWORD put = 0;
                ok = img.ReadAt(p.entry->offset + at, buf.data(), n) && WriteFile(oh, buf.data(), n, &put, nullptr) && put == n;
                at += n; left -= n;
                if (ok && !report(what, at)) { CloseHandle(oh); DeleteFileW(out.c_str()); return Fail(job, L"Installation cancelled."); }
            }
            CloseHandle(oh);
            if (!ok) { DeleteFileW(out.c_str()); return Fail(job, L"The disc image could not be read, or the disk is full."); }
        }
        done += p.outBytes;
    }

    job->Set(80, L"Installing Title Update 3 data\x2026");
    for (const wchar_t* f : { L"\\update\\data100.bin", L"\\update\\data100.ofs" }) job->written.push_back(gameDir + f);
    job->written.push_back(job->dest + L"\\" + kImageName);
    if (!ExpandContainer([&](uint64_t off, void* b, uint32_t n) {
                             if (off + n > tuArchive.size()) return false;
                             memcpy(b, tuArchive.data() + off, n);
                             return true;
                         }, tuArchive.size(), tuExpanded, gameDir + L"\\update\\data100.bin", {}, err))
        return Fail(job, L"Could not unpack the title update: " + Widen(err) + L".");
    if (!WriteBytes(gameDir + L"\\update\\data100.ofs", tuIndex.data(), tuIndex.size()) ||
        !WriteBytes(job->dest + L"\\" + kImageName, image.data(), image.size()))
        return Fail(job, L"Could not write to the install folder (the disk may be full).");

    if (!PrepareShaders(job, 82, 97)) return Fail(job, L"Installation cancelled.");

    job->Set(98, L"Creating shortcuts\x2026");
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::wstring uninst = job->dest + L"\\" + kUninstallExe;
    CopyFileW(self, uninst.c_str(), FALSE);
    const std::wstring game = job->dest + L"\\" + kGameExeName;
    if (job->scDesktop) {
        std::wstring d = KnownDir(FOLDERID_Desktop);
        if (!d.empty()) MakeShortcut(d + L"\\" + kShortcut, game, job->dest, game);
    }
    if (job->scStartMenu) {
        std::wstring s = KnownDir(FOLDERID_CommonPrograms);
        if (s.empty()) s = KnownDir(FOLDERID_Programs);
        if (!s.empty()) MakeShortcut(s + L"\\" + kShortcut, game, job->dest, game);
    }

    job->Set(99, L"Registering with Add or remove programs\x2026");
    Register(job->dest);

    job->ok = true;
    job->Set(100, L"Done.");
    return true;
}

// A failed or cancelled install takes back what it wrote (up to 13 GB), file by file - never a
// tree delete, since the folder may have held something else. Folders go only if left empty.
static bool Install(InstallJob* job) {
    if (RunInstall(job)) return true;
    for (const std::wstring& f : job->written) {
        SetFileAttributesW(f.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(f.c_str());
    }
    RemoveDirectoryW((job->dest + L"\\game\\update").c_str());
    RemoveDirectoryW((job->dest + L"\\game").c_str());
    RemoveDirectoryW(job->dest.c_str());
    return false;
}

static DWORD WINAPI InstallThread(LPVOID param) {
    InstallJob* job = (InstallJob*)param;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Install(job);
    PostMessageW(job->dlg, WM_APP_DONE, 0, 0);
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------- pickers

static std::wstring PickFile(HWND owner, const wchar_t* title, const COMDLG_FILTERSPEC* filters, UINT count) {
    IFileOpenDialog* dlg = nullptr;
    std::wstring out;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&dlg)))
        return out;
    dlg->SetFileTypes(count, filters);
    dlg->SetTitle(title);
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

static std::wstring PickFolder(HWND owner) {
    IFileOpenDialog* dlg = nullptr;
    std::wstring out;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&dlg)))
        return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose where to install the game");
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

// ---------------------------------------------------------------- artwork
//
// The install window is installer_bg.png (IDR_INSTALLER_BG): the art draws the fields and buttons,
// and the real controls sit on top of them. Edits get the field colour; buttons are owner-drawn
// from the art itself (plus a hover/press tint). Below the art a plain strip holds what the art has
// no place for: progress, status, the shortcut boxes and the credit. Everything scales with the art.

static const int kArtW = 1671, kArtH = 941, kStripH = 80;   // art pixels
static const COLORREF kFieldColor = RGB(16, 31, 41), kStripColor = RGB(9, 11, 16);
static const COLORREF kTextColor = RGB(226, 232, 238), kDimColor = RGB(150, 158, 170);
static const COLORREF kAccent = RGB(200, 24, 32);

// Interiors of the three drawn fields: painted over with kFieldColor (they hold sample text).
static const RECT kArtFields[] = { { 509, 556, 1041, 596 }, { 509, 646, 1041, 686 }, { 509, 738, 1041, 778 } };
static const int  kFieldIds[]  = { IDC_ISO_PATH, IDC_TU_PATH, IDC_DEST_PATH };
struct ArtRect { int id; RECT r; };
static const ArtRect kArtLayout[] = {
    { IDC_ISO_BROWSE,  { 1058, 553, 1239, 598 } },
    { IDC_TU_BROWSE,   { 1058, 643, 1239, 688 } },
    { IDC_DEST_BROWSE, { 1058, 735, 1239, 780 } },
    { IDC_INSTALL,     {  452, 801, 1220, 888 } },
    { IDCANCEL,        { 1626,   6, 1664,  38 } },   // the X drawn in the corner
};

static double  g_scale = 1;
static HDC     g_artDC = nullptr;      // the art at window size, field sample text painted out
static HBITMAP g_artBmp = nullptr, g_artOld = nullptr;
static HFONT   g_fieldFont = nullptr, g_uiFont = nullptr;
static HBRUSH  g_fieldBrush = nullptr, g_stripBrush = nullptr;

static int  S(int v) { return (int)lround(v * g_scale); }
static RECT S(const RECT& r) { return { S(r.left), S(r.top), S(r.right), S(r.bottom) }; }

static HBITMAP LoadArt(int w, int h) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_INSTALLER_BG), RT_RCDATA);
    HGLOBAL g = res ? LoadResource(nullptr, res) : nullptr;
    BYTE* data = g ? (BYTE*)LockResource(g) : nullptr;
    const DWORD size = res ? SizeofResource(nullptr, res) : 0;
    if (!data || !size) return nullptr;
    IWICImagingFactory* f = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    HBITMAP bmp = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromMemory(data, size)) &&
        SUCCEEDED(f->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(frame, (UINT)w, (UINT)h, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(f->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom))) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && FAILED(conv->CopyPixels(nullptr, (UINT)w * 4, (UINT)w * 4 * (UINT)h, (BYTE*)bits))) {
            DeleteObject(bmp);
            bmp = nullptr;
        }
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{ conv, scaler, frame, dec, stream, f }) if (u) u->Release();
    return bmp;
}

// A translucent wash over r (hover / press / disabled states of the drawn buttons).
static void Tint(HDC dc, const RECT& r, COLORREF c, BYTE alpha) {
    HDC src = CreateCompatibleDC(dc);
    HBITMAP px = CreateCompatibleBitmap(dc, 1, 1);
    HGDIOBJ old = SelectObject(src, px);
    SetPixel(src, 0, 0, c);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, alpha, 0 };
    AlphaBlend(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, src, 0, 0, 1, 1, bf);
    SelectObject(src, old);
    DeleteObject(px);
    DeleteDC(src);
}

// Hover tracking + hand cursor for the drawn buttons (hover flag in GWLP_USERDATA).
static LRESULT CALLBACK ArtButtonProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!GetWindowLongPtrW(h, GWLP_USERDATA)) {
            SetWindowLongPtrW(h, GWLP_USERDATA, 1);
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_SETCURSOR:
        if (IsWindowEnabled(h)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void DrawArtButton(HWND dlg, const DRAWITEMSTRUCT* di) {
    RECT r = di->rcItem;
    if (g_artDC) {
        RECT at;
        GetWindowRect(di->hwndItem, &at);
        MapWindowPoints(nullptr, dlg, (POINT*)&at, 2);
        BitBlt(di->hDC, 0, 0, r.right, r.bottom, g_artDC, at.left, at.top, SRCCOPY);
    } else {   // no art: plain dark buttons with their captions
        FillRect(di->hDC, &r, g_fieldBrush);
        wchar_t text[64];
        GetWindowTextW(di->hwndItem, text, 64);
        SetBkMode(di->hDC, TRANSPARENT);
        SetTextColor(di->hDC, kTextColor);
        HGDIOBJ of = SelectObject(di->hDC, g_uiFont);
        DrawTextW(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(di->hDC, of);
    }
    const bool hover = GetWindowLongPtrW(di->hwndItem, GWLP_USERDATA) != 0;
    if (di->itemState & ODS_DISABLED) Tint(di->hDC, r, RGB(0, 0, 0), 130);
    else if (di->itemState & ODS_SELECTED) Tint(di->hDC, r, RGB(0, 0, 0), 70);
    else if (hover) Tint(di->hDC, r, di->CtlID == IDCANCEL ? kAccent : RGB(255, 255, 255), 45);
    if ((di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT)) {
        RECT f = r;
        InflateRect(&f, -3, -3);
        DrawFocusRect(di->hDC, &f);
    }
}

// Size the window to the art (scaled to fit the screen), paint the art and lay the controls out.
static void SetUpArtWindow(HWND dlg) {
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int workW = work.right - work.left, workH = work.bottom - work.top;
    g_scale = (std::min)({ 1.0, 0.9 * workW / kArtW, 0.9 * workH / (kArtH + kStripH) });
    const int w = S(kArtW), artH = S(kArtH), h = S(kArtH + kStripH);
    // CAPTION in the .rc adds a title bar; the caption text is kept for the taskbar only.
    SetWindowLongPtrW(dlg, GWL_STYLE, GetWindowLongPtrW(dlg, GWL_STYLE) & ~(LONG_PTR)(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME));
    SetWindowLongPtrW(dlg, GWL_EXSTYLE, GetWindowLongPtrW(dlg, GWL_EXSTYLE) & ~(LONG_PTR)(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE));
    SetWindowPos(dlg, nullptr, work.left + (workW - w) / 2, work.top + (workH - h) / 2, w, h,
                 SWP_NOZORDER | SWP_FRAMECHANGED);

    g_fieldBrush = CreateSolidBrush(kFieldColor);
    g_stripBrush = CreateSolidBrush(kStripColor);
    g_fieldFont = CreateFontW(-(std::max)(12, S(19)), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                              CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_uiFont = CreateFontW(-(std::max)(12, S(16)), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Segoe UI");

    if (HBITMAP bmp = LoadArt(w, artH)) {
        HDC screen = GetDC(nullptr);
        g_artDC = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        g_artBmp = bmp;
        g_artOld = (HBITMAP)SelectObject(g_artDC, g_artBmp);
        for (const RECT& f : kArtFields) { RECT r = S(f); FillRect(g_artDC, &r, g_fieldBrush); }
    }

    for (const ArtRect& a : kArtLayout) {
        const RECT r = S(a.r);
        HWND c = GetDlgItem(dlg, a.id);
        MoveWindow(c, r.left, r.top, r.right - r.left, r.bottom - r.top, FALSE);
        SetWindowSubclass(c, ArtButtonProc, 0, 0);
    }
    const int fieldTextH = (std::max)(12, S(19)) + 6;
    for (int i = 0; i < 3; ++i) {
        const RECT r = S(kArtFields[i]);
        const int cy = (r.top + r.bottom) / 2;
        HWND e = GetDlgItem(dlg, kFieldIds[i]);
        SendMessageW(e, WM_SETFONT, (WPARAM)g_fieldFont, FALSE);
        MoveWindow(e, r.left + S(10), cy - fieldTextH / 2, r.right - r.left - S(20), fieldTextH, FALSE);
    }
    SendDlgItemMessageW(dlg, IDC_ISO_PATH, EM_SETCUEBANNER, TRUE, (LPARAM)L"No ISO selected\x2026");
    SendDlgItemMessageW(dlg, IDC_TU_PATH, EM_SETCUEBANNER, TRUE, (LPARAM)L"No Title Update selected\x2026");

    // The strip, lined up with the Install button.
    const int left = S(454), right = S(1218);
    HWND progress = GetDlgItem(dlg, IDC_PROGRESS);
    SetWindowTheme(progress, L"", L"");   // unthemed, so it takes the colours below
    SendMessageW(progress, PBM_SETBARCOLOR, 0, kAccent);
    SendMessageW(progress, PBM_SETBKCOLOR, 0, RGB(24, 28, 38));
    MoveWindow(progress, left, artH + S(12), right - left, (std::max)(6, S(10)), FALSE);
    const int rowH = (std::max)(12, S(16)) + 6;
    MoveWindow(GetDlgItem(dlg, IDC_STATUS), left, artH + S(28), right - left, rowH, FALSE);
    int x = left;
    HDC dc = GetDC(dlg);
    HGDIOBJ of = SelectObject(dc, g_uiFont);
    for (int id : { IDC_SC_DESKTOP, IDC_SC_STARTMENU }) {
        HWND c = GetDlgItem(dlg, id);
        SetWindowTheme(c, L"", L"");      // themed check boxes ignore WM_CTLCOLORSTATIC's text colour
        wchar_t text[64];
        GetWindowTextW(c, text, 64);
        SIZE sz = {};
        GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
        const int cw = sz.cx + GetSystemMetrics(SM_CXMENUCHECK) + S(12);
        MoveWindow(c, x, artH + S(52), cw, rowH, FALSE);
        x += cw + S(24);
    }
    SelectObject(dc, of);
    ReleaseDC(dlg, dc);
    MoveWindow(GetDlgItem(dlg, IDC_CREDIT), x, artH + S(52), right - x, rowH, FALSE);
    for (int id : { IDC_SC_DESKTOP, IDC_SC_STARTMENU, IDC_STATUS, IDC_CREDIT })
        SendDlgItemMessageW(dlg, id, WM_SETFONT, (WPARAM)g_uiFont, FALSE);
    InvalidateRect(dlg, nullptr, TRUE);
}

static void PaintArtBackground(HWND dlg, HDC dc) {
    RECT c;
    GetClientRect(dlg, &c);
    const int artH = S(kArtH);
    if (g_artDC) BitBlt(dc, 0, 0, c.right, artH, g_artDC, 0, 0, SRCCOPY);
    else { RECT a = { 0, 0, c.right, artH }; FillRect(dc, &a, g_stripBrush); }
    RECT strip = { 0, artH, c.right, c.bottom };
    FillRect(dc, &strip, g_stripBrush);
    HBRUSH line = CreateSolidBrush(kAccent);
    RECT l = { 0, artH, c.right, artH + (std::max)(1, S(2)) };
    FillRect(dc, &l, line);
    DeleteObject(line);
}

static void FreeArt() {
    if (g_artDC) { SelectObject(g_artDC, g_artOld); DeleteDC(g_artDC); g_artDC = nullptr; }
    if (g_artBmp) { DeleteObject(g_artBmp); g_artBmp = nullptr; }
    for (HGDIOBJ* o : { (HGDIOBJ*)&g_fieldFont, (HGDIOBJ*)&g_uiFont, (HGDIOBJ*)&g_fieldBrush, (HGDIOBJ*)&g_stripBrush })
        if (*o) { DeleteObject(*o); *o = nullptr; }
}

// ---------------------------------------------------------------- install dialog

static InstallJob g_job;
static HANDLE     g_thread = nullptr;
static bool       g_running = false;

static void SetControlsEnabled(HWND dlg, bool on) {
    for (int id : { IDC_ISO_PATH, IDC_ISO_BROWSE, IDC_TU_PATH, IDC_TU_BROWSE, IDC_DEST_PATH, IDC_DEST_BROWSE,
                    IDC_SC_DESKTOP, IDC_SC_STARTMENU, IDC_INSTALL })
        EnableWindow(GetDlgItem(dlg, id), on);
}

static INT_PTR CALLBACK MainProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_TTT2SETUP));
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        SetUpArtWindow(dlg);
        CheckDlgButton(dlg, IDC_SC_DESKTOP, BST_CHECKED);
        CheckDlgButton(dlg, IDC_SC_STARTMENU, BST_CHECKED);
        std::wstring pf = KnownDir(FOLDERID_ProgramFiles);
        SetDlgItemTextW(dlg, IDC_DEST_PATH, (pf + L"\\" + kFolderName).c_str());
        SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETRANGE32, 0, 100);
        return TRUE;
    }
    case WM_ERASEBKGND:
        PaintArtBackground(dlg, (HDC)wp);
        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, 1);
        return TRUE;
    case WM_DRAWITEM:
        DrawArtButton(dlg, (const DRAWITEMSTRUCT*)lp);
        return TRUE;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        // Edits (enabled ones send CTLCOLOREDIT, disabled ones CTLCOLORSTATIC) sit on the drawn
        // fields; everything else is on the strip.
        HDC dc = (HDC)wp;
        const int id = GetDlgCtrlID((HWND)lp);
        const bool field = id == IDC_ISO_PATH || id == IDC_TU_PATH || id == IDC_DEST_PATH;
        SetTextColor(dc, id == IDC_CREDIT || (field && !IsWindowEnabled((HWND)lp)) ? kDimColor : kTextColor);
        SetBkColor(dc, field ? kFieldColor : kStripColor);
        return (INT_PTR)(field ? g_fieldBrush : g_stripBrush);
    }
    case WM_LBUTTONDOWN:   // no title bar: drag the window by the art
        ReleaseCapture();
        SendMessageW(dlg, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return TRUE;
    case WM_DESTROY:
        FreeArt();
        return FALSE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ISO_BROWSE: {
            const COMDLG_FILTERSPEC f[] = { { L"Xbox 360 disc image (*.iso)", L"*.iso" }, { L"All files", L"*.*" } };
            std::wstring p = PickFile(dlg, L"Choose your Tekken Tag Tournament 2 disc image", f, 2);
            if (!p.empty()) SetDlgItemTextW(dlg, IDC_ISO_PATH, p.c_str());
            return TRUE;
        }
        case IDC_TU_BROWSE: {
            const COMDLG_FILTERSPEC f[] = { { L"Title update (TU_*)", L"TU_*" }, { L"All files", L"*.*" } };
            std::wstring p = PickFile(dlg, L"Choose Title Update 3 (TU_174Q22P_0000008000000.00000000000O2)", f, 2);
            if (!p.empty()) SetDlgItemTextW(dlg, IDC_TU_PATH, p.c_str());
            return TRUE;
        }
        case IDC_DEST_BROWSE: {
            std::wstring p = PickFolder(dlg);
            if (!p.empty()) SetDlgItemTextW(dlg, IDC_DEST_PATH, (p + L"\\" + kFolderName).c_str());
            return TRUE;
        }
        case IDC_INSTALL: {
            wchar_t iso[MAX_PATH] = {}, tu[MAX_PATH] = {}, dest[MAX_PATH] = {};
            GetDlgItemTextW(dlg, IDC_ISO_PATH, iso, MAX_PATH);
            GetDlgItemTextW(dlg, IDC_TU_PATH, tu, MAX_PATH);
            GetDlgItemTextW(dlg, IDC_DEST_PATH, dest, MAX_PATH);
            if (!iso[0] || GetFileAttributesW(iso) == INVALID_FILE_ATTRIBUTES) {
                MessageBoxW(dlg, L"Choose your disc image first.", kAppName, MB_ICONINFORMATION);
                return TRUE;
            }
            if (!tu[0] || GetFileAttributesW(tu) == INVALID_FILE_ATTRIBUTES) {
                MessageBoxW(dlg, L"Choose Title Update 3 (the file, or the folder holding it).", kAppName, MB_ICONINFORMATION);
                return TRUE;
            }
            if (!dest[0] || wcslen(dest) < 3 || dest[1] != L':') {
                MessageBoxW(dlg, L"Choose a full path to install to, for example C:\\Program Files\\Tekken Tag Tournament 2.",
                            kAppName, MB_ICONINFORMATION);
                return TRUE;
            }
            InitializeCriticalSection(&g_job.lock);
            g_job.dlg = dlg;
            g_job.iso = iso;
            g_job.tu = tu;
            g_job.dest = dest;
            g_job.scDesktop = IsDlgButtonChecked(dlg, IDC_SC_DESKTOP) == BST_CHECKED;
            g_job.scStartMenu = IsDlgButtonChecked(dlg, IDC_SC_STARTMENU) == BST_CHECKED;
            g_job.cancel = 0;
            SetControlsEnabled(dlg, false);
            g_running = true;
            g_thread = CreateThread(nullptr, 0, InstallThread, &g_job, 0, nullptr);
            return TRUE;
        }
        case IDCANCEL:
            if (g_running) {
                if (MessageBoxW(dlg, L"Stop the installation?", kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES)
                    InterlockedExchange(&g_job.cancel, 1);
                return TRUE;
            }
            EndDialog(dlg, 0);
            return TRUE;
        }
        return FALSE;

    case WM_APP_PROGRESS: {
        EnterCriticalSection(&g_job.lock);
        int pct = g_job.pct;
        std::wstring s = g_job.status;
        LeaveCriticalSection(&g_job.lock);
        SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, pct, 0);
        SetDlgItemTextW(dlg, IDC_STATUS, s.c_str());
        return TRUE;
    }

    case WM_APP_DONE: {
        g_running = false;
        if (g_thread) { WaitForSingleObject(g_thread, 5000); CloseHandle(g_thread); g_thread = nullptr; }
        if (g_job.ok) {
            SetDlgItemTextW(dlg, IDC_STATUS, L"Installed.");
            std::wstring msg = std::wstring(kAppName) +
                L" is installed.\n\nSaved games, profiles and settings are kept in your user profile, so they "
                L"survive an uninstall.\n\nPlay now?";
            if (MessageBoxW(dlg, msg.c_str(), kAppName, MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                // Through Explorer, so the game runs as the user does from its shortcut - started from here
                // directly it inherited the installer's administrator rights (MEASURED 2026-10-21: an elevated
                // game whose log and state the unelevated user's tools could not read).
                std::wstring game = L"\"" + g_job.dest + L"\\" + kGameExeName + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", game.c_str(), g_job.dest.c_str(), SW_SHOWNORMAL);
            }
            EndDialog(dlg, 1);
        } else {
            SetDlgItemTextW(dlg, IDC_STATUS, L"");
            SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 0, 0);
            MessageBoxW(dlg, g_job.error.empty() ? L"The installation did not finish." : g_job.error.c_str(),
                        kAppName, MB_ICONERROR);
            SetControlsEnabled(dlg, true);
        }
        return TRUE;
    }

    case WM_CLOSE:
        if (!g_running) EndDialog(dlg, 0);
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------- uninstall

struct UninstallState { std::wstring dir; bool alsoSaves = false; };
static UninstallState g_un;

// Uninstalling deletes the folder it runs from, so it logs to %TEMP%.
static void UnLog(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, path)) return;
    wcscat_s(path, L"ttt2_uninstall.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    wchar_t line[1024];
    va_list ap; va_start(ap, fmt);
    int n = _vsnwprintf_s(line, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n > 0) {
        std::wstring w = std::wstring(line) + L"\r\n";
        int b = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string utf8((size_t)(b > 0 ? b : 0), '\0');
        if (b > 0) {
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &utf8[0], b, nullptr, nullptr);
            DWORD put = 0;
            WriteFile(h, utf8.data(), (DWORD)utf8.size(), &put, nullptr);
        }
    }
    CloseHandle(h);
}

static void DoUninstall(HWND dlg) {
    UnLog(L"--- uninstall: dir=\"%s\" alsoSaves=%d", g_un.dir.c_str(), g_un.alsoSaves ? 1 : 0);
    for (REFKNOWNFOLDERID id : { FOLDERID_Desktop, FOLDERID_CommonPrograms, FOLDERID_Programs }) {
        std::wstring d = KnownDir(id);
        if (!d.empty()) DeleteFileW((d + L"\\" + kShortcut).c_str());
    }
    if (dlg) SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 40, 0);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, kRegKey);
    if (dlg) SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 55, 0);
    if (g_un.alsoSaves) {
        std::wstring local = KnownDir(FOLDERID_LocalAppData);
        if (!local.empty()) DeleteTree(local + L"\\" + kUserDataDir);
    }
    if (dlg) SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 70, 0);
    // Re-check immediately before the destructive call: never delete a tree that is not ours.
    if (!g_un.dir.empty() && LooksLikeInstall(g_un.dir)) {
        DeleteTree(g_un.dir);
        UnLog(L"    tree deleted; dir still present = %d", GetFileAttributesW(g_un.dir.c_str()) != INVALID_FILE_ATTRIBUTES ? 1 : 0);
    } else {
        UnLog(L"    REFUSED to delete \"%s\" -- not an install folder", g_un.dir.c_str());
    }
    if (dlg) SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 100, 0);

    // Our own exe is still locked: a detached cmd removes it once we exit.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"/c ping 127.0.0.1 -n 3 >nul & del /f /q \"" + std::wstring(self) + L"\" & rmdir \"" + g_un.dir + L"\"";
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring shell = std::wstring(sys) + L"\\cmd.exe";
    std::wstring line = L"\"" + shell + L"\" " + cmd;
    std::vector<wchar_t> buf(line.begin(), line.end());
    buf.push_back(0);
    if (CreateProcessW(shell.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
}

static INT_PTR CALLBACK UninstallProc(HWND dlg, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG: {
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_TTT2SETUP)));
        std::wstring t = std::wstring(kAppName) + L" will be removed from:\n" + g_un.dir +
                         L"\n\nYour saved games and profiles are kept unless you tick the box below.";
        SetDlgItemTextW(dlg, IDC_UN_TEXT, t.c_str());
        SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETRANGE32, 0, 100);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            g_un.alsoSaves = IsDlgButtonChecked(dlg, IDC_UN_KEEPSAVES) == BST_CHECKED;
            if (g_un.alsoSaves &&
                MessageBoxW(dlg, L"Your saved games, profiles and settings will be permanently deleted. Continue?",
                            kAppName, MB_YESNO | MB_ICONWARNING) != IDYES)
                return TRUE;
            EnableWindow(GetDlgItem(dlg, IDOK), FALSE);
            EnableWindow(GetDlgItem(dlg, IDCANCEL), FALSE);
            DoUninstall(dlg);
            EndDialog(dlg, 1);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        return FALSE;
    case WM_CLOSE:
        EndDialog(dlg, 0);
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------- entry

static bool HasArg(const wchar_t* cmdline, const wchar_t* flag) {
    std::wstring c(cmdline);
    for (auto& ch : c) ch = (wchar_t)towlower(ch);
    return c.find(flag) != std::wstring::npos;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdline, int) {
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDPIAware();

    // The installer copies itself in as uninstall.exe: the file name decides which program we
    // are (a double-click passes no arguments), /uninstall too.
    wchar_t selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    const wchar_t* selfName = wcsrchr(selfPath, L'\\');
    selfName = selfName ? selfName + 1 : selfPath;
    const bool amUninstaller = _wcsicmp(selfName, kUninstallExe) == 0 || HasArg(cmdline, L"/uninstall");

    int rc = 0;
    // /silent <disc.iso> <title update> <install folder>: the same install with no UI and a log
    // next to the exe (scripted tests; unattended installs).
    if (HasArg(cmdline, L"/silent")) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring iso, tu, dest;
        for (int i = 1; i < argc; ++i)
            if (_wcsicmp(argv[i], L"/silent") == 0) {
                if (i + 3 < argc) { iso = argv[i + 1]; tu = argv[i + 2]; dest = argv[i + 3]; }
                break;
            }
        if (argv) LocalFree(argv);
        if (iso.empty() || tu.empty() || dest.empty()) {
            MessageBoxW(nullptr, L"usage: ttt2_setup.exe /silent <disc.iso> <title update file or folder> <install folder>",
                        kAppName, MB_ICONINFORMATION);
            CoUninitialize();
            return 2;
        }
        const std::wstring logPath = std::wstring(selfPath) + L".log";
        InitializeCriticalSection(&g_job.lock);
        g_job.dlg = nullptr;
        g_job.iso = iso;
        g_job.tu = tu;
        g_job.dest = dest;
        g_job.scDesktop = g_job.scStartMenu = false;
        g_job.log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_job.log != INVALID_HANDLE_VALUE) {
            DWORD put = 0;
            WriteFile(g_job.log, "\xEF\xBB\xBF", 3, &put, nullptr);
        }
        g_job.Set(0, L"[args] <" + std::wstring(cmdline ? cmdline : L"") + L">");
        const DWORD start = GetTickCount();
        const bool ok = Install(&g_job);
        wchar_t took[64];
        swprintf_s(took, L"[time] %.1f s", (GetTickCount() - start) / 1000.0);
        g_job.Set(g_job.pct, took);
        if (g_job.log != INVALID_HANDLE_VALUE) CloseHandle(g_job.log);
        CoUninitialize();
        return ok ? 0 : 1;
    }
    // /update [install folder]: update an existing install with no UI (the folder Add or remove
    // programs names when none is given), logging next to the exe like /silent.
    if (!amUninstaller && HasArg(cmdline, L"/update")) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring dest;
        for (int i = 1; i < argc; ++i)
            if (_wcsicmp(argv[i], L"/update") == 0) {
                if (i + 1 < argc) dest = argv[i + 1];
                break;
            }
        if (argv) LocalFree(argv);
        if (dest.empty()) dest = ExistingInstall();
        InitializeCriticalSection(&g_job.lock);
        g_job.dlg = nullptr;
        g_job.dest = dest;
        g_job.log = CreateFileW((std::wstring(selfPath) + L".log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_job.log != INVALID_HANDLE_VALUE) {
            DWORD put = 0;
            WriteFile(g_job.log, "\xEF\xBB\xBF", 3, &put, nullptr);
        }
        g_job.Set(0, L"[update] <" + dest + L">");
        const bool ok = !dest.empty() ? RunUpdate(&g_job) : Fail(&g_job, L"No installed copy of the game was found.");
        if (g_job.log != INVALID_HANDLE_VALUE) CloseHandle(g_job.log);
        CoUninitialize();
        return ok ? 0 : 1;
    }
    if (amUninstaller) {
        std::wstring dir = selfPath;
        size_t at = dir.find_last_of(L'\\');
        if (at != std::wstring::npos) dir.resize(at);
        if (!LooksLikeInstall(dir)) {
            std::wstring fromReg;
            HKEY k = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
                wchar_t v[MAX_PATH] = {}; DWORD n = sizeof v, type = 0;
                if (RegQueryValueExW(k, L"InstallLocation", nullptr, &type, (BYTE*)v, &n) == ERROR_SUCCESS) fromReg = v;
                RegCloseKey(k);
            }
            dir = LooksLikeInstall(fromReg) ? fromReg : std::wstring();
        }
        if (dir.empty()) {
            UnLog(L"--- refused: \"%s\" is not an install folder and the registry did not name a valid one", selfPath);
            MessageBoxW(nullptr,
                        L"This uninstaller could not find the installed game.\n\nNothing has been changed. Run the "
                        L"uninstaller from inside the game's own folder, or use Settings > Apps.",
                        kAppName, MB_ICONWARNING);
            CoUninitialize();
            return 1;
        }
        g_un.dir = dir;
        if (HasArg(cmdline, L"/quiet")) { DoUninstall(nullptr); rc = 0; }
        else rc = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_UNINSTALL), nullptr, UninstallProc, 0) == 1 ? 0 : 1;
    } else {
        // Already installed: offer to update just the port's files (no disc or title update needed).
        const std::wstring existing = ExistingInstall();
        int choice = IDNO;
        if (!existing.empty()) {
            const std::wstring have = RegString(L"DisplayVersion");
            const std::wstring ask = std::wstring(kAppName) + L" " + (have.empty() ? L"" : have + L" ") +
                L"is installed in:\n" + existing + L"\n\nUpdate it to version " TTT2_VERSION_W L"? Only the port's own "
                L"files are replaced, so your disc image and title update are not needed, and saved games, "
                L"profiles and settings are kept.\n\nYes: update\nNo: full reinstall (disc image and title update "
                L"needed)\nCancel: quit";
            choice = MessageBoxW(nullptr, ask.c_str(), kAppName, MB_YESNOCANCEL | MB_ICONQUESTION);
        }
        if (choice == IDCANCEL) {
            rc = 0;
        } else if (choice == IDYES) {
            InitializeCriticalSection(&g_job.lock);
            g_job.dlg = nullptr;
            g_job.dest = existing;
            HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            const bool ok = RunUpdate(&g_job);
            SetCursor(old);
            if (ok) {
                const std::wstring msg = std::wstring(kAppName) + L" is updated to version " TTT2_VERSION_W L".\n\nPlay now?";
                if (MessageBoxW(nullptr, msg.c_str(), kAppName, MB_YESNO | MB_ICONINFORMATION) == IDYES)
                    ShellExecuteW(nullptr, L"open", (existing + L"\\" + kGameExeName).c_str(), nullptr, existing.c_str(), SW_SHOWNORMAL);
                rc = 1;
            } else {
                MessageBoxW(nullptr, g_job.error.c_str(), kAppName, MB_ICONERROR);
                rc = 0;
            }
        } else {
            rc = (int)DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, MainProc, 0);
        }
    }
    CoUninitialize();
    return rc;
}
