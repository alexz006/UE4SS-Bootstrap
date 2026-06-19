#include "updater.hpp"
#include "ini.hpp"
#include "progress.hpp"
#include "miniz.h"
#include "default_config.inc"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "version.lib")

namespace fs = std::filesystem;

namespace updater {
namespace {

// ----------------------------------------------------------------------------
// utils
// ----------------------------------------------------------------------------

std::wstring widen(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Named-mutex lock, one per install.
struct UpdateLock
{
    HANDLE h = nullptr;
    bool owned = false;
    ~UpdateLock()
    {
        if (owned && h) ReleaseMutex(h);
        if (h) CloseHandle(h);
    }
};

std::string now_string()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    wsprintfA(buf, "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// Days since a "YYYY-MM-DD HH:MM:SS" timestamp; huge value if unparseable.
double days_since(const std::string& ts)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (sscanf_s(ts.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3) return 1e9;

    SYSTEMTIME st{};
    st.wYear = (WORD)y; st.wMonth = (WORD)mo; st.wDay = (WORD)d;
    st.wHour = (WORD)h; st.wMinute = (WORD)mi; st.wSecond = (WORD)se;

    FILETIME ftLast, ftNow;
    if (!SystemTimeToFileTime(&st, &ftLast)) return 1e9;
    SYSTEMTIME now;
    GetLocalTime(&now);
    if (!SystemTimeToFileTime(&now, &ftNow)) return 1e9;

    ULARGE_INTEGER a{}, b{};
    a.LowPart = ftLast.dwLowDateTime; a.HighPart = ftLast.dwHighDateTime;
    b.LowPart = ftNow.dwLowDateTime;  b.HighPart = ftNow.dwHighDateTime;
    if (b.QuadPart < a.QuadPart) return 1e9;
    return (double)(b.QuadPart - a.QuadPart) / 1e7 / 86400.0;  // 100ns -> sec -> days
}

fs::path g_logPath;

void log_line(const std::string& msg)
{
    if (g_logPath.empty()) return;
    std::ofstream f(g_logPath, std::ios::app);
    if (f) f << "[" << now_string() << "] " << msg << "\n";
}

// ----------------------------------------------------------------------------
// HTTP (WinHTTP)
// ----------------------------------------------------------------------------

// Per-operation timeouts (ms).
constexpr int kResolveTimeout = 15000;
constexpr int kConnectTimeout = 20000;
constexpr int kSendTimeout = 30000;
constexpr int kReceiveTimeout = 30000;

// Exponential backoff capped at 8s: 1s, 2s, 4s, 8s...
int backoff_ms(int attempt)
{
    int ms = 1000;
    for (int i = 1; i < attempt && ms < 8000; ++i) ms *= 2;
    return ms > 8000 ? 8000 : ms;
}

struct HttpResult
{
    int status = 0;       // HTTP status, 0 on transport failure
    uint64_t total = 0;   // full resource size if known
    bool partial = false; // 206 Partial Content
};

std::optional<std::wstring> query_header_str(HINTERNET req, DWORD info)
{
    DWORD len = 0;
    WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &len, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return std::nullopt;
    std::wstring buf(len / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX, buf.data(), &len, WINHTTP_NO_HEADER_INDEX))
        return std::nullopt;
    buf.resize(wcslen(buf.c_str()));
    return buf;
}

// One GET attempt: on_meta after headers, on_chunk per body block (false aborts).
HttpResult http_request(const std::wstring& url, const std::wstring& extraHeaders,
                        const std::function<void(const HttpResult&)>& on_meta,
                        const std::function<bool(const char*, DWORD)>& on_chunk, std::string* err)
{
    HttpResult res;

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{}, path[4096]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc))
    {
        if (err) *err = "WinHttpCrackUrl failed";
        return res;
    }

    HINTERNET session = WinHttpOpen(L"UE4SSUpdater/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { if (err) *err = "WinHttpOpen failed"; return res; }
    WinHttpSetTimeouts(session, kResolveTimeout, kConnectTimeout, kSendTimeout, kReceiveTimeout);

    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    HINTERNET request = nullptr;
    if (connect)
    {
        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        request = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    }

    if (request)
    {
        std::wstring headers = L"User-Agent: UE4SSUpdater/1.0\r\nAccept: application/vnd.github+json\r\n";
        headers += extraHeaders;
        if (WinHttpSendRequest(request, headers.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr))
        {
            DWORD code = 0, codeSize = sizeof(code);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeSize, WINHTTP_NO_HEADER_INDEX);
            res.status = (int)code;
            res.partial = (res.status == 206);

            if (res.partial)
            {
                // Content-Range: bytes <start>-<end>/<total>
                if (auto cr = query_header_str(request, WINHTTP_QUERY_CONTENT_RANGE))
                {
                    auto slash = cr->find(L'/');
                    if (slash != std::wstring::npos) res.total = _wcstoui64(cr->c_str() + slash + 1, nullptr, 10);
                }
            }
            else
            {
                DWORD len64 = 0, lenSize = sizeof(len64);
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &len64, &lenSize, WINHTTP_NO_HEADER_INDEX))
                    res.total = len64;
            }

            if (on_meta) on_meta(res);

            if (res.status == 200 || res.status == 206)
            {
                std::vector<char> buf(64 * 1024);
                bool ok = true;
                for (;;)
                {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(request, &avail)) { ok = false; break; }
                    if (avail == 0) break;
                    DWORD toRead = avail < buf.size() ? avail : (DWORD)buf.size();
                    DWORD read = 0;
                    if (!WinHttpReadData(request, buf.data(), toRead, &read)) { ok = false; break; }
                    if (read == 0) break;
                    if (on_chunk && !on_chunk(buf.data(), read)) { ok = false; break; }
                }
                if (!ok && err) *err = "read interrupted, GLE=" + std::to_string(GetLastError());
            }
            else if (err)
            {
                *err = "HTTP status " + std::to_string(res.status);
            }
        }
        else if (err) { *err = "send/receive failed, GLE=" + std::to_string(GetLastError()); }
    }
    else if (err) { *err = "connect/open failed, GLE=" + std::to_string(GetLastError()); }

    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return res;
}

// GET into a string, retrying transient failures.
std::optional<std::string> http_get_string(const std::wstring& url, int retries, std::string* err)
{
    for (int attempt = 0; attempt <= retries; ++attempt)
    {
        if (attempt > 0) Sleep(backoff_ms(attempt));
        std::string body, e;
        auto res = http_request(
            url, L"", nullptr, [&](const char* p, DWORD n) { body.append(p, n); return true; }, &e);
        if (res.status >= 200 && res.status < 300) return body;
        if (err) *err = e.empty() ? ("HTTP " + std::to_string(res.status)) : e;
        log_line("api attempt " + std::to_string(attempt + 1) + "/" + std::to_string(retries + 1) +
                 " failed: " + (err ? *err : e));
    }
    return std::nullopt;
}

// Download to dest with retries and resume-from-partial (Range); verifies size.
bool http_download(const std::wstring& url, const fs::path& dest, bool showProgress, int retries,
                   std::string* err)
{
    uint64_t expectedTotal = 0;  // learned from the first response

    for (int attempt = 0; attempt <= retries; ++attempt)
    {
        if (attempt > 0)
        {
            int wait = backoff_ms(attempt);
            log_line("download retry " + std::to_string(attempt) + "/" + std::to_string(retries) +
                     " in " + std::to_string(wait) + "ms");
            if (showProgress) progress::set_status(L"Connection issue - retrying...");
            Sleep(wait);
        }

        std::error_code ec;
        uint64_t existing = fs::exists(dest, ec) ? (uint64_t)fs::file_size(dest, ec) : 0;
        if (expectedTotal && existing >= expectedTotal) break;  // already complete

        std::wstring range;
        if (existing > 0) range = L"Range: bytes=" + std::to_wstring(existing) + L"-\r\n";

        std::ofstream out;
        uint64_t got = existing;
        uint64_t total = expectedTotal;
        int lastPct = -2;

        auto res = http_request(
            url, range,
            [&](const HttpResult& r) {
                if (r.total) { expectedTotal = r.total; total = r.total; }
                if (r.partial && existing > 0)
                {
                    out.open(dest, std::ios::binary | std::ios::app);  // resume
                }
                else
                {
                    out.open(dest, std::ios::binary | std::ios::trunc);  // full / restart
                    got = 0;
                }
            },
            [&](const char* p, DWORD n) {
                if (!out.is_open()) return false;
                out.write(p, n);
                got += n;
                if (showProgress)
                {
                    int pct = total ? (int)(got * 100 / total) : -1;
                    if (pct != lastPct) { progress::set_progress(pct); lastPct = pct; }
                    else progress::pump();
                }
                return (bool)out;
            },
            err);

        if (out.is_open()) out.close();

        if (res.status == 416)  // Range Not Satisfiable: stale partial, reset
        {
            fs::remove(dest, ec);
            continue;
        }

        bool httpOk = (res.status == 200 || res.status == 206);
        uint64_t finalSize = fs::exists(dest, ec) ? (uint64_t)fs::file_size(dest, ec) : 0;

        if (httpOk && expectedTotal && finalSize == expectedTotal)
        {
            if (showProgress) progress::set_progress(100);
            return true;
        }
        if (httpOk && !expectedTotal && finalSize > 0)
        {
            return true;  // no size advertised: accept a clean single pass
        }
        log_line("download incomplete: status=" + std::to_string(res.status) + " have=" +
                 std::to_string(finalSize) + "/" + std::to_string(expectedTotal));
        // keep the partial file for the next retry to resume
    }

    if (err && err->empty()) *err = "download failed after retries";
    return false;
}

// ----------------------------------------------------------------------------
// JSON (targeted extraction of the GitHub release fields)
// ----------------------------------------------------------------------------

// Find  "key" : "value"  starting at `from`; returns value and sets endPos past it.
std::optional<std::string> json_string(const std::string& j, const std::string& key, size_t from, size_t* endPos = nullptr)
{
    std::string token = "\"" + key + "\"";
    size_t k = j.find(token, from);
    if (k == std::string::npos) return std::nullopt;
    size_t colon = j.find(':', k + token.size());
    if (colon == std::string::npos) return std::nullopt;
    size_t q1 = j.find('"', colon);
    if (q1 == std::string::npos) return std::nullopt;
    std::string val;
    for (size_t i = q1 + 1; i < j.size(); ++i)
    {
        char c = j[i];
        if (c == '\\' && i + 1 < j.size()) { val.push_back(j[++i]); continue; }
        if (c == '"') { if (endPos) *endPos = i + 1; return val; }
        val.push_back(c);
    }
    return std::nullopt;
}

struct ReleaseInfo
{
    std::string tag;
    std::string assetName;
    std::wstring assetUrl;
};

bool asset_is_standard(const std::string& name)
{
    auto contains = [&](const char* s) { return name.find(s) != std::string::npos; };
    bool ok = name.rfind("UE4SS_", 0) == 0 &&
              name.size() >= 4 && name.compare(name.size() - 4, 4, ".zip") == 0;
    return ok && !contains("zDEV") && !contains("zCustom") && !contains("zMap") && !contains("Xinput");
}

std::optional<ReleaseInfo> parse_release(const std::string& json)
{
    ReleaseInfo info;
    auto tag = json_string(json, "tag_name", 0);
    if (!tag) return std::nullopt;
    info.tag = *tag;

    // walk assets: each asset has a "browser_download_url"; its "name" precedes it.
    size_t pos = 0, prev = 0;
    while (true)
    {
        size_t urlEnd = 0;
        std::string urlKey = "browser_download_url";
        size_t k = json.find("\"" + urlKey + "\"", pos);
        if (k == std::string::npos) break;
        auto url = json_string(json, urlKey, pos, &urlEnd);
        if (!url) break;

        // nearest "name" between prev asset end and this url
        std::string name;
        size_t scan = prev;
        while (true)
        {
            size_t np = 0;
            auto nm = json_string(json, "name", scan, &np);
            if (!nm || np > k) break;
            name = *nm;
            scan = np;
        }

        if (asset_is_standard(name))
        {
            info.assetName = name;
            info.assetUrl = widen(*url);
            return info;
        }
        pos = urlEnd;
        prev = urlEnd;
    }
    return std::nullopt;
}

std::optional<ReleaseInfo> fetch_release(const std::string& repo, const std::string& channel,
                                         const std::string& pinnedTag, int retries, std::string* err)
{
    std::string api = "https://api.github.com/repos/" + repo + "/releases/";
    if (channel == "nightly")
        api += "tags/experimental-latest";
    else if (channel == "pinned")
        api += "tags/" + pinnedTag;
    else
        api += "latest";

    auto body = http_get_string(widen(api), retries, err);
    if (!body) return std::nullopt;
    auto rel = parse_release(*body);
    if (!rel && err) *err = "no matching UE4SS asset in release";
    return rel;
}

// ----------------------------------------------------------------------------
// installed version (from UE4SS.dll VERSIONINFO), for first-run seeding
// ----------------------------------------------------------------------------

std::vector<int> parse_version(const std::string& s)
{
    std::vector<int> parts;
    std::string cur;
    for (char c : s)
    {
        if (c == '.') { if (!cur.empty()) { parts.push_back(atoi(cur.c_str())); cur.clear(); } }
        else if (c >= '0' && c <= '9') cur.push_back(c);
        else if (!cur.empty()) { parts.push_back(atoi(cur.c_str())); cur.clear(); }
    }
    if (!cur.empty()) parts.push_back(atoi(cur.c_str()));
    return parts;
}

std::optional<std::string> installed_file_version(const fs::path& dll)
{
    std::error_code ec;
    if (!fs::exists(dll, ec)) return std::nullopt;
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(dll.c_str(), &dummy);
    if (!size) return std::nullopt;
    std::vector<BYTE> buf(size);
    if (!GetFileVersionInfoW(dll.c_str(), 0, size, buf.data())) return std::nullopt;
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", (void**)&ffi, &len) || !ffi) return std::nullopt;
    char out[64];
    wsprintfA(out, "%d.%d.%d.%d", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
              HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    return std::string(out);
}

// tag like "v3.0.1" vs file "3.0.1.0" -> true if leading components match
bool version_matches(const std::string& tag, const std::string& fileVer)
{
    auto a = parse_version(tag);
    auto b = parse_version(fileVer);
    if (a.empty() || b.empty()) return false;
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

// ----------------------------------------------------------------------------
// extraction
// ----------------------------------------------------------------------------

std::vector<std::string> split_path(const std::string& p)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : p)
    {
        if (c == '/' || c == '\\') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Strip a leading "ue4ss/" (nightly zips nest under it; stable zips are flat).
std::string strip_ue4ss_prefix(const std::string& rel)
{
    if (rel.size() > 6 && ini::lower(rel.substr(0, 6)) == "ue4ss/")
        return rel.substr(6);
    return rel;
}

// Whether a zip entry should be written to disk.
bool is_selected(const std::string& rel, const ini::Ini& cfg, bool overwriteSettings,
                 bool overwriteModsTxt, const fs::path& ue4ssDir)
{
    auto parts = split_path(rel);
    if (parts.empty()) return false;

    bool core = cfg.get_bool("modules", "core", true);

    auto absent = [&]() { std::error_code e; return !fs::exists(ue4ssDir / fs::path(widen(rel)), e); };

    if (parts[0] == "dwmapi.dll") return false;            // never replace our own proxy

    if (parts[0] == "Mods")
    {
        if (parts.size() < 2) return false;
        const std::string& mod = parts[1];
        // config files: only when missing or explicitly overwritten
        if (parts.size() == 2 && (mod == "mods.txt" || mod == "mods.json"))
            return overwriteModsTxt || absent();
        if (mod == "shared") return core;
        return cfg.get_bool("modules", mod, false);         // [modules] is an allow-list
    }

    // top-level files
    if (parts.size() == 1)
    {
        if (parts[0] == "UE4SS-settings.ini") return overwriteSettings || absent();
        return core;                                        // UE4SS.dll, README, Changelog...
    }
    return core;
}

bool write_file(const fs::path& path, const void* data, size_t size)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (size) f.write((const char*)data, (std::streamsize)size);
    return (bool)f;
}

bool extract_zip(const fs::path& zipPath, const fs::path& ue4ssDir, const ini::Ini& cfg,
                 bool overwriteSettings, bool overwriteModsTxt, bool showProgress, std::string* err)
{
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.string().c_str(), 0))
    {
        if (err) *err = "mz_zip_reader_init_file failed";
        return false;
    }

    // Phase 1: extract + validate all selected entries into memory.
    struct Item { fs::path dest; std::vector<unsigned char> data; };
    std::vector<Item> items;
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    bool ok = true;
    int skipped = 0;
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) { ok = false; break; }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;

        std::string rel = strip_ue4ss_prefix(st.m_filename);
        if (rel.empty()) continue;  // the top-level "ue4ss/" entry itself
        if (!is_selected(rel, cfg, overwriteSettings, overwriteModsTxt, ue4ssDir)) { ++skipped; continue; }

        size_t outSize = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &outSize, 0);
        if (!p) { ok = false; break; }

        Item it;
        it.dest = ue4ssDir / fs::path(widen(rel));
        it.data.assign((unsigned char*)p, (unsigned char*)p + outSize);
        mz_free(p);
        items.push_back(std::move(it));

        if (showProgress)
        {
            progress::set_progress(count ? (int)((i + 1) * 100 / count) : -1);
            progress::set_status(L"Verifying: " + widen(rel));
        }
    }
    mz_zip_reader_end(&zip);

    if (!ok)
    {
        log_line("extract: validation FAILED (nothing written)");
        if (err) *err = "archive validation failed";
        return false;
    }

    // Phase 2: write the validated files to disk.
    int written = 0;
    for (const auto& it : items)
    {
        if (!write_file(it.dest, it.data.data(), it.data.size())) { ok = false; break; }
        ++written;
        if (showProgress) progress::set_status(L"Installing (" + std::to_wstring(written) + L"/" +
                                               std::to_wstring(items.size()) + L")");
    }

    log_line("extract: written=" + std::to_string(written) + " skipped=" + std::to_string(skipped) +
             (ok ? " ok" : " WRITE-FAILED"));
    if (!ok && err) *err = "write failed";
    return ok;
}

// Apply [modules] flags to mods.txt and each mod's enabled.txt (listed, installed mods only).
void sync_mods_txt(const fs::path& ue4ssDir, const ini::Ini& cfg)
{
    auto sec = cfg.data.find("modules");
    if (sec == cfg.data.end()) return;

    const fs::path modsDir = ue4ssDir / "Mods";
    std::error_code ec;
    if (!fs::exists(modsDir, ec)) return;

    // canonical folder names (lower -> actual case)
    std::map<std::string, std::string> canon;
    for (auto& e : fs::directory_iterator(modsDir, ec))
        if (e.is_directory(ec))
        {
            std::string n = e.path().filename().string();
            canon[ini::lower(n)] = n;
        }

    // desired enable flags from [modules]
    struct Want { std::string name; bool on; };
    std::map<std::string, Want> want;  // keyed by lowercased name
    for (auto& kv : sec->second)
    {
        if (kv.first == "core") continue;  // not a mod
        std::string v = ini::lower(kv.second);
        bool on = (v == "1" || v == "true" || v == "yes" || v == "on");
        auto c = canon.find(kv.first);
        want[kv.first] = {c != canon.end() ? c->second : kv.first, on};
    }
    if (want.empty()) return;

    // enabled.txt per mod folder: create when on, remove when off (installed mods only)
    for (auto& kv : want)
    {
        auto c = canon.find(kv.first);
        if (c == canon.end()) continue;  // not installed -> leave alone
        const fs::path enabledTxt = modsDir / c->second / "enabled.txt";
        std::error_code e;
        if (kv.second.on)
        {
            if (!fs::exists(enabledTxt, e)) { std::ofstream create(enabledTxt); }
        }
        else
        {
            fs::remove(enabledTxt, e);
        }
    }

    const fs::path modsTxt = modsDir / "mods.txt";
    std::vector<std::string> lines;
    {
        std::ifstream f(modsTxt);
        std::string l;
        while (std::getline(f, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); lines.push_back(l); }
    }

    // rewrite flags on existing lines
    std::set<std::string> handled;
    for (auto& line : lines)
    {
        std::string t = ini::trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        auto colon = t.find(':');
        if (colon == std::string::npos) continue;
        std::string name = ini::trim(t.substr(0, colon));
        std::string lo = ini::lower(name);
        auto w = want.find(lo);
        if (w != want.end())
        {
            line = name + " : " + (w->second.on ? "1" : "0");
            handled.insert(lo);
        }
    }

    // insert modules not yet present (only those actually installed)
    std::vector<std::string> add;
    for (auto& kv : want)
    {
        if (handled.count(kv.first)) continue;
        if (!canon.count(kv.first)) continue;
        add.push_back(kv.second.name + " : " + (kv.second.on ? "1" : "0"));
    }
    if (!add.empty())
    {
        size_t at = lines.size();
        for (size_t i = 0; i < lines.size(); ++i)
            if (lines[i].find("Built-in keybinds") != std::string::npos) { at = i; break; }
        lines.insert(lines.begin() + at, add.begin(), add.end());
    }

    fs::create_directories(modsDir, ec);
    std::ofstream out(modsTxt, std::ios::trunc | std::ios::binary);
    if (!out) { log_line("mods.txt sync: write failed"); return; }
    for (auto& l : lines) out << l << "\r\n";
    log_line("mods.txt synced (" + std::to_string(want.size()) + " modules)");
}

} // namespace

// ----------------------------------------------------------------------------
// orchestration
// ----------------------------------------------------------------------------

void run_blocking(const fs::path& baseDir)
{
    const fs::path ue4ssDir = baseDir / "ue4ss";
    const fs::path iniPath = ue4ssDir / "updater.ini";
    g_logPath = ue4ssDir / "updater.log";
    { std::ofstream truncate(g_logPath, std::ios::trunc); }  // fresh log each launch

  try
  {
    ini::Ini cfg;
    if (!cfg.load(iniPath.wstring()))
    {
        // no config: write the bundled default and continue with it
        std::error_code ec;
        fs::create_directories(ue4ssDir, ec);
        std::ofstream out(iniPath, std::ios::binary | std::ios::trunc);
        if (out) { out.write(kDefaultConfig, sizeof(kDefaultConfig) - 1); out.close(); }
        log_line("no config: created default updater.ini");
        if (!cfg.load(iniPath.wstring())) return;  // couldn't create/read it
    }
    if (!cfg.get_bool("updater", "enabled", false))
    {
        return;
    }

    const std::string repo = cfg.get("updater", "repo", "UE4SS-RE/RE-UE4SS");
    const std::string channel = ini::lower(cfg.get("updater", "channel", "stable"));
    const std::string pinned = cfg.get("updater", "pinned_tag", "");
    const bool overwriteSettings = cfg.get_bool("updater", "overwrite_settings", false);
    const bool overwriteModsTxt = cfg.get_bool("updater", "overwrite_mods_txt", false);
    const bool showProgress = cfg.get_bool("updater", "show_progress", true);
    int retries = atoi(cfg.get("updater", "download_retries", "5").c_str());
    if (retries < 0) retries = 0;
    const std::string installedTag = cfg.get("state", "installed_tag", "");
    const std::string installedAsset = cfg.get("state", "installed_asset", "");

    log_line("run: channel=" + channel + " repo=" + repo + " installed=" + installedTag + "/" + installedAsset);

    // Take the per-install update lock.
    UpdateLock lock;
    {
        std::wstring name = L"UE4SS-Bootstrap-Updater-" +
                            std::to_wstring(std::hash<std::wstring>{}(ue4ssDir.wstring()));
        lock.h = CreateMutexW(nullptr, FALSE, name.c_str());
        if (lock.h)
        {
            DWORD w = WaitForSingleObject(lock.h, 180000);  // wait up to 3 min
            lock.owned = (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED);
        }
    }
    if (lock.h && !lock.owned)
    {
        log_line("another instance holds the update lock; skipping update");
        sync_mods_txt(ue4ssDir, cfg);
        return;
    }

    // is UE4SS installed on disk?
    std::error_code ecPresent;
    const bool ue4ssPresent = fs::exists(ue4ssDir / "UE4SS.dll", ecPresent);

    // Throttle network checks to once per check_interval_days (0 = every launch).
    int intervalDays = atoi(cfg.get("updater", "check_interval_days", "0").c_str());
    if (intervalDays > 0 && ue4ssPresent && !installedAsset.empty())
    {
        const std::string lastCheck = cfg.get("state", "last_check", "");
        if (!lastCheck.empty() && days_since(lastCheck) < intervalDays)
        {
            log_line("within check interval (" + std::to_string(intervalDays) + "d, last=" + lastCheck + ") -> skip");
            sync_mods_txt(ue4ssDir, cfg);  // still apply local [modules] changes
            return;
        }
    }

    std::string err;
    auto rel = fetch_release(repo, channel, pinned, retries, &err);
    if (!rel)
    {
        log_line("fetch_release failed: " + err);
        sync_mods_txt(ue4ssDir, cfg);  // still apply [modules] -> mods.txt offline
        return;  // offline or API error -> proceed with current install
    }
    log_line("remote: tag=" + rel->tag + " asset=" + rel->assetName);

    auto write_state = [&]() {
        ini::write_section(iniPath.wstring(), "state",
                           {{"installed_tag", rel->tag},
                            {"installed_asset", rel->assetName},
                            {"last_check", now_string()}});
    };

    // already up to date AND actually installed?
    if (ue4ssPresent && rel->tag == installedTag && rel->assetName == installedAsset)
    {
        log_line("up to date");
        write_state();  // refresh last_check
        sync_mods_txt(ue4ssDir, cfg);
        return;
    }
    if (!ue4ssPresent)
    {
        log_line("UE4SS.dll missing on disk -> forcing install");
    }

    // first run with no state but a matching installed version: seed, don't redownload
    if (ue4ssPresent && installedAsset.empty() && channel != "nightly")
    {
        auto fv = installed_file_version(ue4ssDir / "UE4SS.dll");
        if (fv && version_matches(rel->tag, *fv))
        {
            log_line("first run: installed " + *fv + " matches " + rel->tag + ", seeding state");
            write_state();
            sync_mods_txt(ue4ssDir, cfg);
            return;
        }
    }

    // --- update needed ---
    log_line("update needed -> " + rel->tag + " (" + rel->assetName + ")");

    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    const fs::path zipPath = fs::path(tempDir) /
                             (std::to_wstring(GetCurrentProcessId()) + L"-" + widen(rel->assetName));
    { std::error_code ec; fs::remove(zipPath, ec); }  // clear any stale partial

    if (showProgress)
    {
        progress::show(L"UE4SS Updater");
        progress::set_status(L"Downloading " + widen(rel->tag) + L" ...");
        progress::set_progress(0);
    }

    bool ok = http_download(rel->assetUrl, zipPath, showProgress, retries, &err);
    if (!ok)
    {
        log_line("download failed: " + err);
        if (showProgress) progress::close();
        std::error_code ec;
        fs::remove(zipPath, ec);
        return;
    }
    log_line("downloaded to " + zipPath.string());

    if (showProgress) progress::set_status(L"Installing " + widen(rel->tag) + L" ...");
    ok = extract_zip(zipPath, ue4ssDir, cfg, overwriteSettings, overwriteModsTxt, showProgress, &err);

    std::error_code ec;
    fs::remove(zipPath, ec);  // delete the downloaded archive
    log_line("removed temp archive");

    if (ok)
    {
        write_state();
        sync_mods_txt(ue4ssDir, cfg);
        if (showProgress)
        {
            progress::set_status(L"Updated to " + widen(rel->tag));
            progress::set_progress(100);
        }
        log_line("update complete: " + rel->tag);
    }
    else
    {
        log_line("install failed: " + err);
    }

    if (showProgress) progress::close();
  }
  catch (const std::exception& e)
  {
      log_line(std::string("FATAL exception: ") + e.what());
      progress::close();
  }
  catch (...)
  {
      log_line("FATAL unknown exception");
      progress::close();
  }
}

} // namespace updater
