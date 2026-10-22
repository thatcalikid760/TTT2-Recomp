// In-game updates (see updater.h).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include "updater.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

#ifndef TTT2_VERSION_STR
#define TTT2_VERSION_STR "0.0.0"
#endif
#ifndef TTT2_UPDATE_REPO
#define TTT2_UPDATE_REPO ""
#endif

namespace updater {
namespace {

std::mutex g_mutex;
Status g_status;
std::string g_url, g_digest;            // the setup asset's download URL and "sha256:<hex>"
uint64_t g_size = 0;
std::wstring g_setupPath;               // the downloaded installer

std::wstring Wide(const std::string& s)
{
    std::wstring w(s.size() + 1, L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size()));
    w.resize(n > 0 ? size_t(n - 1) : 0);
    return w;
}

std::wstring Folder(const wchar_t* sub)
{
    wchar_t dir[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    std::wstring path = (n && n < MAX_PATH) ? dir : L".";
    path += L"\\TTT2Recomp";
    CreateDirectoryW(path.c_str(), nullptr);
    path += sub;
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

// ---- a minimal JSON reader (the release object: strings, numbers, objects, arrays, literals)
struct Json
{
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    std::string text;                                   // String; Number as written
    std::vector<Json> items;                            // Array
    std::map<std::string, Json> fields;                 // Object
    const Json& operator[](const char* key) const
    {
        static const Json none;
        const auto it = fields.find(key);
        return it == fields.end() ? none : it->second;
    }
};

struct Parser
{
    const std::string& s;
    size_t p = 0;
    bool ok = true;
    void Space() { while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p; }
    std::string Str()
    {
        std::string out;
        ++p;                                              // opening quote
        while (p < s.size() && s[p] != '"')
        {
            char c = s[p++];
            if (c == '\\' && p < s.size())
            {
                c = s[p++];
                switch (c)
                {
                case 'n': out += '\n'; continue;
                case 't': out += '\t'; continue;
                case 'r': continue;
                case 'b': case 'f': continue;
                case 'u':
                {
                    const unsigned cp = p + 4 <= s.size() ? unsigned(std::strtoul(s.substr(p, 4).c_str(), nullptr, 16)) : 0;
                    p += 4;
                    if (cp < 0x80) out += char(cp);
                    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                    else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                    continue;
                }
                default: break;                           // \" \\ \/
                }
            }
            out += c;
        }
        ++p;                                              // closing quote
        return out;
    }
    Json Value(int depth = 0)
    {
        Json v;
        Space();
        if (p >= s.size() || depth > 32) { ok = false; return v; }
        const char c = s[p];
        if (c == '{')
        {
            v.kind = Json::Object;
            ++p;
            Space();
            if (p < s.size() && s[p] == '}') { ++p; return v; }
            while (ok && p < s.size())
            {
                Space();
                if (s[p] != '"') { ok = false; break; }
                const std::string key = Str();
                Space();
                if (p >= s.size() || s[p] != ':') { ok = false; break; }
                ++p;
                v.fields[key] = Value(depth + 1);
                Space();
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == '}') { ++p; break; }
                ok = false;
            }
        }
        else if (c == '[')
        {
            v.kind = Json::Array;
            ++p;
            Space();
            if (p < s.size() && s[p] == ']') { ++p; return v; }
            while (ok && p < s.size())
            {
                v.items.push_back(Value(depth + 1));
                Space();
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == ']') { ++p; break; }
                ok = false;
            }
        }
        else if (c == '"') { v.kind = Json::String; v.text = Str(); }
        else
        {
            const size_t start = p;
            while (p < s.size() && s[p] != ',' && s[p] != '}' && s[p] != ']' && !std::isspace(static_cast<unsigned char>(s[p]))) ++p;
            v.text = s.substr(start, p - start);
            v.kind = v.text == "null" ? Json::Null : (v.text == "true" || v.text == "false") ? Json::Bool : Json::Number;
        }
        return v;
    }
};

// ---- versions: up to four numbers ("0.1.7", "v0.1.8", "TTT2 Installer 0.1.7.1")
std::vector<int> VersionIn(const std::string& text)
{
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (!std::isdigit(static_cast<unsigned char>(text[i])) || (i && std::isdigit(static_cast<unsigned char>(text[i - 1])))) continue;
        std::vector<int> parts;
        size_t p = i;
        while (parts.size() < 4 && p < text.size() && std::isdigit(static_cast<unsigned char>(text[p])))
        {
            int n = 0;
            while (p < text.size() && std::isdigit(static_cast<unsigned char>(text[p]))) n = n * 10 + (text[p++] - '0');
            parts.push_back(n);
            if (p + 1 < text.size() && text[p] == '.' && std::isdigit(static_cast<unsigned char>(text[p + 1]))) ++p;
            else break;
        }
        if (parts.size() >= 2) { parts.resize(4, 0); return parts; }
    }
    return {};
}
std::string VersionText(std::vector<int> v)
{
    while (v.size() > 3 && v.back() == 0) v.pop_back();
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? "." : "") + std::to_string(v[i]);
    return s;
}

// ---- HTTPS GET through WinHTTP (redirects followed); `sink` gets the body in chunks.
template <typename Sink>
bool Get(const std::wstring& url, const wchar_t* accept, std::string& error, uint64_t& length, Sink sink)
{
    URL_COMPONENTS u{ sizeof u };
    wchar_t host[256] = {}, path[2048] = {};
    u.lpszHostName = host; u.dwHostNameLength = 256;
    u.lpszUrlPath = path; u.dwUrlPathLength = 2048;
    wchar_t extra[2048] = {};
    u.lpszExtraInfo = extra; u.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) { error = "bad address"; return false; }
    struct Handle { HINTERNET h; ~Handle() { if (h) WinHttpCloseHandle(h); } };
    Handle session{ WinHttpOpen(L"TTT2Recomp-Updater/" TTT2_VERSION_STR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0) };
    if (!session.h) { error = "no internet access"; return false; }
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
    Handle connect{ WinHttpConnect(session.h, host, u.nPort, 0) };
    const std::wstring object = std::wstring(path) + extra;
    Handle request{ connect.h ? WinHttpOpenRequest(connect.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                              : nullptr };
    if (!request.h) { error = "could not connect to " + std::string(host, host + wcslen(host)); return false; }
    std::wstring headers = L"Accept: ";
    headers += accept;
    headers += L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request.h, headers.c_str(), DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
    {
        error = "no answer from GitHub (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
    if (status == 404) { error = "no release found"; return false; }
    if (status == 403 || status == 429) { error = "GitHub is limiting requests; try again later"; return false; }
    if (status != 200) { error = "GitHub answered " + std::to_string(status); return false; }
    wchar_t len[32] = {};
    size = sizeof len;
    length = WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, len, &size, nullptr) ? _wcstoui64(len, nullptr, 10) : 0;
    std::vector<char> buffer(1 << 16);
    for (;;)
    {
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buffer.data(), DWORD(buffer.size()), &got)) { error = "the download broke off"; return false; }
        if (!got) return true;
        if (!sink(buffer.data(), size_t(got))) { error = "could not save the download"; return false; }
    }
}

void Set(State state, const std::string& error = {})
{
    std::lock_guard l(g_mutex);
    g_status.state = state;
    g_status.error = error;
}

void CheckNow()
{
    std::string body, error;
    uint64_t length = 0;
    const std::wstring url = L"https://api.github.com/repos/" + Wide(Repo()) + L"/releases/latest";
    if (Repo().empty()) { Set(State::Failed, "no update source in this build"); return; }
    if (!Get(url, L"application/vnd.github+json", error, length, [&](const char* d, size_t n) { body.append(d, n); return body.size() < (8u << 20); }))
    {
        std::fprintf(stderr, "[update] check failed: %s\n", error.c_str());
        Set(State::Failed, error);
        return;
    }
    Parser parser{ body };
    const Json release = parser.Value();
    if (!parser.ok || release.kind != Json::Object) { Set(State::Failed, "unexpected answer from GitHub"); return; }
    // The version: the tag when it holds one ("v0.1.8"), else the release name ("TTT2 Installer 0.1.8").
    std::vector<int> latest = VersionIn(release["tag_name"].text);
    if (latest.empty()) latest = VersionIn(release["name"].text);
    const Json* asset = nullptr;
    for (const Json& a : release["assets"].items)
    {
        std::string name = a["name"].text;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (name == "ttt2_setup.exe" || (!asset && name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0)) asset = &a;
    }
    const std::vector<int> current = VersionIn(CurrentVersion());
    std::lock_guard l(g_mutex);
    g_status.title = release["name"].text;
    g_status.latest = latest.empty() ? std::string() : VersionText(latest);
    if (latest.empty()) { g_status.state = State::Failed; g_status.error = "the latest release has no version number"; return; }
    // Strictly newer only: an equal or older release never replaces this build.
    if (!(latest > current))
    {
        g_status.state = State::UpToDate;
        std::fprintf(stderr, "[update] latest release %s, this build %s: up to date\n", g_status.latest.c_str(), CurrentVersion());
        return;
    }
    if (!asset) { g_status.state = State::Failed; g_status.error = "the release has no installer"; return; }
    g_url = (*asset)["browser_download_url"].text;
    g_digest = (*asset)["digest"].text;
    g_size = std::strtoull((*asset)["size"].text.c_str(), nullptr, 10);
    g_status.state = State::Available;
    std::fprintf(stderr, "[update] %s available (this build %s): %s\n", g_status.latest.c_str(), CurrentVersion(), g_url.c_str());
}

std::string Sha256Hex(const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0)
    {
        std::vector<UCHAR> buffer(1 << 20);
        DWORD got = 0;
        while (ReadFile(f, buffer.data(), DWORD(buffer.size()), &got, nullptr) && got) BCryptHashData(hash, buffer.data(), got, 0);
        UCHAR digest[32];
        if (BCryptFinishHash(hash, digest, 32, 0) == 0)
            for (UCHAR b : digest) { char t[3]; std::snprintf(t, 3, "%02x", b); hex += t; }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return hex;
}

void DownloadNow()
{
    std::string url, digest, version;
    uint64_t expected;
    {
        std::lock_guard l(g_mutex);
        url = g_url; digest = g_digest; expected = g_size; version = g_status.latest;
    }
    const std::wstring path = Folder(L"\\update") + L"\\ttt2_setup_" + Wide(version) + L".exe";
    const std::wstring part = path + L".part";
    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Set(State::Failed, "could not write the download"); return; }
    std::string error;
    uint64_t length = 0, done = 0;
    const bool ok = Get(Wide(url), L"application/octet-stream", error, length, [&](const char* d, size_t n) {
        DWORD put = 0;
        if (!WriteFile(f, d, DWORD(n), &put, nullptr) || put != n) return false;
        done += n;
        const uint64_t total = length ? length : expected;
        std::lock_guard l(g_mutex);
        g_status.percent = total ? int((std::min)(done * 100 / total, uint64_t(99))) : 0;
        return true;
    });
    CloseHandle(f);
    if (ok && expected && done != expected) { error = "the download is incomplete"; }
    if (ok && error.empty() && digest.compare(0, 7, "sha256:") == 0 && Sha256Hex(part) != digest.substr(7))
        error = "the download is damaged (checksum mismatch)";
    if (!ok || !error.empty())
    {
        DeleteFileW(part.c_str());
        std::fprintf(stderr, "[update] download failed: %s\n", error.c_str());
        Set(State::Failed, error);
        return;
    }
    DeleteFileW(path.c_str());
    if (!MoveFileW(part.c_str(), path.c_str())) { Set(State::Failed, "could not save the download"); return; }
    std::lock_guard l(g_mutex);
    g_setupPath = path;
    g_status.percent = 100;
    g_status.state = State::Downloaded;
    std::fprintf(stderr, "[update] %s downloaded (%llu bytes%s)\n", version.c_str(), (unsigned long long)done,
                 digest.empty() ? "" : ", checksum verified");
}

std::wstring ExeDir(std::wstring* exe = nullptr)
{
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir = self;
    if (exe) *exe = dir;
    dir.resize(dir.find_last_of(L'\\'));
    return dir;
}

std::string Base64(const std::string& bytes)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < bytes.size(); i += 3)
    {
        const uint32_t n = (uint8_t(bytes[i]) << 16) | (i + 1 < bytes.size() ? uint8_t(bytes[i + 1]) << 8 : 0) |
                           (i + 2 < bytes.size() ? uint8_t(bytes[i + 2]) : 0);
        out += t[(n >> 18) & 63];
        out += t[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? t[(n >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? t[n & 63] : '=';
    }
    return out;
}

std::wstring PsQuote(const std::wstring& s)
{
    std::wstring q = L"'";
    for (wchar_t c : s) { if (c == L'\'') q += L'\''; q += c; }
    return q + L"'";
}

} // namespace

// TTT2_VERSION_OVERRIDE=<version> (tests): pretend to be another build, e.g. an older one to try
// the download and install path against the current release.
const char* CurrentVersion()
{
    static const char* v = [] { const char* e = std::getenv("TTT2_VERSION_OVERRIDE"); return e && *e ? e : TTT2_VERSION_STR; }();
    return v;
}

const std::string& Repo()
{
    static const std::string repo = [] {
        // [Update] Repo=owner/name in settings.ini overrides the build's (TTT2_SETTINGS_INI for tests).
        std::string ini;
        if (const char* e = std::getenv("TTT2_SETTINGS_INI"); e && *e) ini = e;
        else
        {
            char dir[MAX_PATH] = {};
            const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH);
            ini = std::string(n && n < MAX_PATH ? dir : ".") + "\\TTT2Recomp\\settings.ini";
        }
        char value[256] = {};
        GetPrivateProfileStringA("Update", "Repo", TTT2_UPDATE_REPO, value, sizeof value, ini.c_str());
        return std::string(value);
    }();
    return repo;
}

Status Get() { std::lock_guard l(g_mutex); return g_status; }

void Check()
{
    {
        std::lock_guard l(g_mutex);
        if (g_status.state == State::Checking || g_status.state == State::Downloading) return;
        g_status.state = State::Checking;
        g_status.error.clear();
    }
    std::thread(CheckNow).detach();
}

void Download()
{
    {
        std::lock_guard l(g_mutex);
        if (g_status.state != State::Available && !(g_status.state == State::Failed && !g_url.empty())) return;
        g_status.state = State::Downloading;
        g_status.percent = 0;
        g_status.error.clear();
    }
    std::thread(DownloadNow).detach();
}

bool CanInstall()
{
    const std::wstring dir = ExeDir();
    return GetFileAttributesW((dir + L"\\game\\tekken.img").c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool Install(std::string& error)
{
    std::wstring setup;
    {
        std::lock_guard l(g_mutex);
        if (g_status.state != State::Downloaded) { error = "nothing downloaded"; return false; }
        setup = g_setupPath;
    }
    if (!CanInstall()) { error = "this copy is not an installed game"; return false; }
    std::wstring exe;
    const std::wstring dir = ExeDir(&exe);
    // A hidden PowerShell waits for the game to close, runs the installer's /update elevated
    // (Windows asks), then starts the game again - also when the update was declined or failed.
    const std::wstring script =
        L"$ErrorActionPreference = 'Stop'\n"
        L"Wait-Process -Id " + std::to_wstring(GetCurrentProcessId()) + L" -ErrorAction SilentlyContinue\n"
        L"$shell = New-Object -ComObject WScript.Shell\n"
        L"try {\n"
        L"  $p = Start-Process -FilePath " + PsQuote(setup) + L" -ArgumentList @('/update', " + PsQuote(L"\"" + dir + L"\"") +
        L") -Verb RunAs -Wait -PassThru\n"
        L"  if ($p.ExitCode -ne 0) { [void]$shell.Popup('The update could not be installed. Details: ' + " + PsQuote(setup + L".log") +
        L", 0, 'Tekken Tag Tournament 2', 48) }\n"
        L"} catch { [void]$shell.Popup('The update was cancelled.', 0, 'Tekken Tag Tournament 2', 64) }\n"
        L"Start-Process -FilePath " + PsQuote(exe) + L" -WorkingDirectory " + PsQuote(dir) + L"\n";
    std::string utf16(reinterpret_cast<const char*>(script.data()), script.size() * sizeof(wchar_t));
    std::wstring command = L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -EncodedCommand " +
                           Wide(Base64(utf16));
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr,
                        nullptr, &si, &pi) &&
        !CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        error = "could not start the installer (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::fprintf(stderr, "[update] installing %s: the game closes and restarts\n", g_status.latest.c_str());
    return true;
}

} // namespace updater
