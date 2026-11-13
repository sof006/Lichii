// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>

#include <dxgi.h>
#include <intrin.h>
#include <shellapi.h>

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QString>
#include <fmt/format.h>
#include <quazip.h>
#include <quazipfile.h>
#include <quazipnewinfo.h>
#include <zlib.h>

#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/windows/crash_handler.h"
#include "lichii/debug_report.h"

namespace DebugReport {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::system_clock;

constexpr size_t KeepReports = 10;
constexpr const char* ReportPrefix = "lichii-report-";

struct Session {
    DWORD pid{};
    Clock::time_point start{};
    std::vector<u64> games;
};

std::mutex g_mutex;
Session g_session; ///< This process's session (g_mutex)
bool g_began{};
std::optional<std::jthread> g_flusher;

bool ReadEnabled() {
    const char* const value = std::getenv("LICHII_DEBUG_REPORT");
    return value == nullptr || value[0] != '0';
}

void SetDefault(const char* name, const char* value) {
    if (std::getenv(name) == nullptr) {
        _putenv_s(name, value);
    }
}

std::string Utf8(const fs::path& path) {
    return Common::FS::PathToUTF8String(path);
}

QString QPath(const fs::path& path) {
    return QString::fromStdWString(path.wstring());
}

/// The Windows user name appears in every path under the profile; reports show "user" instead.
void Censor(std::string& text) {
    static const std::string name = [] {
        const char* const value = std::getenv("USERNAME");
        return value != nullptr && std::strlen(value) >= 3 ? std::string{value} : std::string{};
    }();
    if (name.empty()) {
        return;
    }
    for (size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 4)) {
        text.replace(at, name.size(), "user");
    }
}

std::optional<std::string> ReadFile(const fs::path& path) {
    QFile file{QPath(path)};
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray data = file.readAll();
    return std::string(data.constData(), static_cast<size_t>(data.size()));
}

bool CanWrite(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path probe = dir / ".write-test";
    {
        std::ofstream file{probe};
        if (!(file << "x")) {
            return false;
        }
    }
    fs::remove(probe, ec);
    return true;
}

fs::path ExePath() {
    std::wstring buffer(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return fs::path{buffer};
}

std::string LocalTime(Clock::time_point time) {
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
    return QDateTime::fromSecsSinceEpoch(seconds)
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
        .toStdString();
}

// Session markers: session-<pid>.txt in the report folder while that process runs.

fs::path MarkerPath(DWORD pid) {
    return ReportDirectory() / fmt::format("session-{}.txt", pid);
}

void WriteMarker(const Session& session) {
    std::ofstream file{MarkerPath(session.pid), std::ios::trunc};
    file << "pid=" << session.pid << '\n';
    file << "start="
         << std::chrono::duration_cast<std::chrono::seconds>(session.start.time_since_epoch())
                .count()
         << '\n';
    file << "games=";
    for (size_t index = 0; index < session.games.size(); ++index) {
        file << (index == 0 ? "" : ",") << fmt::format("{:016x}", session.games[index]);
    }
    file << '\n';
}

std::optional<Session> ReadMarker(const fs::path& path) {
    std::ifstream file{path};
    if (!file) {
        return std::nullopt;
    }
    Session session;
    std::string line;
    while (std::getline(file, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "pid") {
            session.pid = static_cast<DWORD>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "start") {
            session.start = Clock::time_point{std::chrono::seconds{std::strtoll(value.c_str(), nullptr, 10)}};
        } else if (key == "games") {
            for (size_t begin = 0; begin < value.size();) {
                const size_t end = std::min(value.find(',', begin), value.size());
                if (end > begin) {
                    session.games.push_back(
                        std::strtoull(value.substr(begin, end - begin).c_str(), nullptr, 16));
                }
                begin = end + 1;
            }
        }
    }
    return session.pid != 0 ? std::optional{session} : std::nullopt;
}

/// Whether pid is a running lichii.exe (process ids are reused).
bool IsRunningLichii(DWORD pid) {
    if (pid == GetCurrentProcessId()) {
        return true;
    }
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) {
        return false;
    }
    bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    if (running) {
        std::wstring name(32768, L'\0');
        DWORD length = static_cast<DWORD>(name.size());
        if (QueryFullProcessImageNameW(process, 0, name.data(), &length)) {
            name.resize(length);
            running = _wcsicmp(fs::path{name}.filename().c_str(),
                               ExePath().filename().c_str()) == 0;
        }
    }
    CloseHandle(process);
    return running;
}

// Zip writing.

class ZipWriter {
public:
    explicit ZipWriter(const fs::path& path) : zip{QPath(path)} {
        ok = zip.open(QuaZip::mdCreate);
    }

    bool AddText(const QString& name, const std::string& text) {
        return Add(name, QDateTime::currentDateTime(),
                   QByteArray(text.data(), static_cast<qsizetype>(text.size())));
    }

    /// A file copied as is, in 1 MiB pieces (crash dumps can be large).
    bool AddFile(const QString& name, const fs::path& path) {
        QFile in{QPath(path)};
        if (!ok || !in.open(QIODevice::ReadOnly)) {
            return false;
        }
        QuaZipFile out{&zip};
        QuaZipNewInfo info{name, in.fileName()};
        if (!out.open(QIODevice::WriteOnly, info, nullptr, 0, Z_DEFLATED, 6)) {
            return false;
        }
        bool written = true;
        while (written && !in.atEnd()) {
            const QByteArray piece = in.read(1 << 20);
            written = !piece.isEmpty() && out.write(piece) == piece.size();
        }
        out.close();
        return written && out.getZipError() == 0;
    }

    bool Close() {
        if (!ok) {
            return false;
        }
        zip.close();
        return zip.getZipError() == 0;
    }

private:
    bool Add(const QString& name, const QDateTime& time, const QByteArray& data) {
        if (!ok) {
            return false;
        }
        QuaZipFile out{&zip};
        QuaZipNewInfo info{name};
        info.dateTime = time;
        if (!out.open(QIODevice::WriteOnly, info, nullptr, 0, Z_DEFLATED, 6)) {
            return false;
        }
        const bool written = out.write(data) == data.size();
        out.close();
        return written && out.getZipError() == 0;
    }

    QuaZip zip;
    bool ok{};
};

// Report contents.

/// The settings file without accounts, tokens and network names.
std::string Redact(std::string text) {
    static constexpr std::array<std::string_view, 5> secret{"token", "password", "username",
                                                            "nickname", "room"};
    std::string result;
    result.reserve(text.size());
    size_t begin = 0;
    while (begin < text.size()) {
        const size_t end = std::min(text.find('\n', begin), text.size());
        std::string line = text.substr(begin, end - begin);
        const size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            std::ranges::transform(key, key.begin(),
                                   [](char c) { return static_cast<char>(std::tolower(c)); });
            // The name after the last group ("Multiplayer\ip"), without "\default".
            std::string_view leaf{key};
            if (leaf.ends_with("\\default")) {
                leaf.remove_suffix(8);
            }
            leaf = leaf.substr(leaf.find_last_of('\\') + 1);
            if (leaf == "ip" || leaf.starts_with("ip_") || leaf.ends_with("_ip") ||
                std::ranges::any_of(secret, [&](std::string_view word) {
                    return key.find(word) != std::string::npos;
                })) {
                line = line.substr(0, eq + 1) + "(removed)";
            }
        }
        result += line;
        result += '\n';
        begin = end + 1;
    }
    Censor(result);
    return result;
}

std::string CpuName() {
    int info[4]{};
    __cpuid(info, 0x80000000);
    if (static_cast<unsigned>(info[0]) < 0x80000004) {
        return "unknown";
    }
    char brand[49]{};
    for (int leaf = 0; leaf < 3; ++leaf) {
        __cpuid(info, 0x80000002 + leaf);
        std::memcpy(brand + leaf * 16, info, sizeof(info));
    }
    std::string name{brand};
    name.erase(name.find_last_not_of(' ') + 1);
    return name;
}

std::string OsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtl_get_version == nullptr || rtl_get_version(&version) != 0) {
        return "unknown";
    }
    DWORD revision = 0;
    DWORD size = sizeof(revision);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                 RRF_RT_REG_DWORD, nullptr, &revision, &size);
    return fmt::format("Windows {}.{} build {}.{}", version.dwMajorVersion, version.dwMinorVersion,
                       version.dwBuildNumber, revision);
}

std::string GpuText() {
    std::string text;
    IDXGIFactory1* factory{};
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return "  (DXGI unavailable)\n";
    }
    IDXGIAdapter1* adapter{};
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        LARGE_INTEGER umd{};
        std::string driver = "unknown";
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            driver = fmt::format("{}.{}.{}.{}", HIWORD(umd.HighPart), LOWORD(umd.HighPart),
                                 HIWORD(umd.LowPart), LOWORD(umd.LowPart));
        }
        text += fmt::format("  {}: {} (vendor {:04x}, device {:04x}), {} MiB video memory, "
                            "driver {}{}\n",
                            index, QString::fromWCharArray(desc.Description).toStdString(),
                            desc.VendorId, desc.DeviceId, desc.DedicatedVideoMemory >> 20, driver,
                            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
        adapter->Release();
    }
    factory->Release();
    return text;
}

std::string SystemText(Kind kind, const Session& session) {
    std::string text;
    text += fmt::format("Lichii: {} | {} | {} | built {}\n", Common::g_build_fullname,
                        Common::g_scm_branch, Common::g_scm_desc, Common::g_build_date);
    text += fmt::format("Report: {} for process {}, session started {}, written {}\n",
                        kind == Kind::Exit      ? "normal exit"
                        : kind == Kind::Crash   ? "crash"
                        : kind == Kind::Unclean ? "session ended without closing (killed or frozen)"
                                                : "saved from the Help menu",
                        session.pid, LocalTime(session.start), LocalTime(Clock::now()));
    std::string games;
    for (const u64 game : session.games) {
        games += fmt::format("{}{:016x}", games.empty() ? "" : ", ", game);
    }
    text += fmt::format("Games: {}\n", games.empty() ? "none" : games);
    text += fmt::format("OS: {}\n", OsVersion());
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    text += fmt::format("CPU: {} ({} logical processors)\n", CpuName(),
                        system.dwNumberOfProcessors);
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        text += fmt::format("Memory: {} MiB, {} MiB free; commit limit {} MiB, {} MiB free\n",
                            memory.ullTotalPhys >> 20, memory.ullAvailPhys >> 20,
                            memory.ullTotalPageFile >> 20, memory.ullAvailPageFile >> 20);
    }
    text += "GPUs:\n" + GpuText();
    text += fmt::format("lichii.exe: {}\n", Utf8(ExePath()));
    text += fmt::format("Data folder: {}\n", Utf8(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir)));
    text += fmt::format("Shader folder: {}\n",
                        Utf8(Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir)));
    text += "Environment (EDEN_*, LICHII_*, AMD_*, VK_*, DXVK_*):\n";
    if (wchar_t* const block = GetEnvironmentStringsW()) {
        for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
            const std::string line = QString::fromWCharArray(entry).toStdString();
            for (const std::string_view prefix : {"EDEN_", "LICHII_", "AMD_", "VK_", "DXVK_"}) {
                if (line.starts_with(prefix)) {
                    text += "  " + line + '\n';
                    break;
                }
            }
        }
        FreeEnvironmentStringsW(block);
    }
    Censor(text);
    return text;
}

/// The shader folders: each title's files with their sizes; folders of many files (the driver's
/// cache) as a count and a total.
std::string ShaderCacheText() {
    std::string text;
    const auto describe = [&text](const fs::path& root, std::string_view label) {
        std::error_code ec;
        text += fmt::format("{}: {}\n", label, Utf8(root));
        if (!fs::is_directory(root, ec)) {
            text += "  (none)\n";
            return;
        }
        const auto summarize = [](const fs::path& dir) {
            std::error_code ec2;
            u64 files = 0;
            u64 bytes = 0;
            Clock::time_point newest{};
            for (auto it = fs::recursive_directory_iterator(dir, ec2);
                 !ec2 && it != fs::recursive_directory_iterator(); it.increment(ec2)) {
                if (it->is_regular_file(ec2)) {
                    ++files;
                    bytes += it->file_size(ec2);
                    newest = std::max(newest, std::chrono::clock_cast<Clock>(
                                                  it->last_write_time(ec2)));
                }
            }
            return fmt::format("{} files, {} MiB, newest {}", files, bytes >> 20,
                               files != 0 ? LocalTime(newest) : std::string{"-"});
        };
        size_t root_files = 0;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            const std::string name = Utf8(entry.path().filename());
            if (entry.is_directory(ec)) {
                text += fmt::format("  {}\\: {}\n", name, summarize(entry.path()));
                // Title folders: their files one by one (a few dozen).
                size_t listed = 0;
                for (const auto& child : fs::directory_iterator(entry.path(), ec)) {
                    if (++listed > 40) {
                        text += "    ...\n";
                        break;
                    }
                    std::error_code ec3;
                    const std::string child_name = Utf8(child.path().filename());
                    if (child.is_directory(ec3)) {
                        text += fmt::format("    {}\\: {}\n", child_name, summarize(child.path()));
                    } else {
                        text += fmt::format(
                            "    {} {} bytes {}\n", child_name, child.file_size(ec3),
                            LocalTime(std::chrono::clock_cast<Clock>(child.last_write_time(ec3))));
                    }
                }
            } else if (++root_files <= 20) {
                text += fmt::format("  {} {} bytes\n", name, entry.file_size(ec));
            }
        }
        if (root_files > 20) {
            text += fmt::format("  ... {} files here in all: {}\n", root_files, summarize(root));
        }
    };
    const fs::path shader_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir);
    describe(shader_dir, "Shader folder");
    if (const fs::path driver_dir = Common::FS::GetDriverShaderCacheDir(); !driver_dir.empty()) {
        describe(driver_dir / "AMD" / "VkCache", "AMD driver cache files");
    }
    const fs::path home_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "cache" / "shader";
    std::error_code ec;
    if (!fs::equivalent(home_dir, shader_dir, ec)) {
        describe(home_dir, "Data folder shaders");
    }
    if (const char* const local = std::getenv("LOCALAPPDATA")) {
        describe(fs::path{local} / "AMD" / "VkCache", "AMD driver's own cache folder");
    }
    Censor(text);
    return text;
}

std::optional<double> NumberAfter(std::string_view line, std::string_view key) {
    const size_t at = line.find(key);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string rest{line.substr(at + key.size(), 32)};
    char* end{};
    const double value = std::strtod(rest.c_str(), &end);
    return end != rest.c_str() ? std::optional{value} : std::nullopt;
}

std::string_view WordAfter(std::string_view line, std::string_view key) {
    const size_t at = line.find(key);
    if (at == std::string_view::npos) {
        return {};
    }
    const std::string_view rest = line.substr(at + key.size());
    return rest.substr(0, rest.find(' '));
}

/// What the log says about hitches, shader builds and errors, totalled.
std::string Summary(const std::string& log) {
    struct Hitch {
        double ms;
        std::string_view line;
    };
    std::vector<Hitch> hitches;
    std::map<std::string, std::pair<u64, double>, std::less<>> causes;
    std::map<std::string, u64, std::less<>> lookups;
    u64 summary_lines = 0, frames = 0, over_100 = 0, gfx_builds = 0, compute_builds = 0;
    u64 compute_waits = 0, compute_waits_long = 0;
    double compute_wait_ms = 0, compute_wait_max = 0;
    u64 held_draws = 0;
    double held_ms = 0, held_max = 0;
    u64 async_builds = 0, compute_driver_builds = 0, fast_first = 0;
    double async_ms = 0, compute_driver_ms = 0;
    double unoptimized_blend = 0;
    std::vector<std::string_view> header, precache, crashes, errors;
    std::string_view cached_first;
    u64 compute_rebuilds = 0, compute_rebuilds_unwaited = 0, mesh_builds = 0, evicted = 0;
    double mesh_ms = 0;
    u64 error_count = 0, critical_count = 0, warning_count = 0;
    double last_time = 0;

    for (size_t begin = 0; begin < log.size();) {
        const size_t end = std::min(log.find('\n', begin), log.size());
        std::string_view line{log.data() + begin, end - begin};
        begin = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.starts_with('[')) {
            last_time = std::strtod(std::string{line.substr(1, 16)}.c_str(), nullptr);
        }
        if (line.find("Lichii Version:") != line.npos || line.find("Host CPU") != line.npos ||
            line.find("Host OS:") != line.npos ||
            (line.find("renderer_vulkan.cpp") != line.npos &&
             line.find(":Report: ") != line.npos) ||
            line.find("LogSettings: M-") != line.npos) {
            if (header.size() < 120) {
                header.push_back(line);
            }
        }
        if (line.find("Host crash") != line.npos) {
            crashes.push_back(line);
        }
        if (line.find(" <Critical> ") != line.npos) {
            ++critical_count;
            if (errors.size() < 60) {
                errors.push_back(line);
            }
        } else if (line.find(" <Error> ") != line.npos) {
            ++error_count;
            if (errors.size() < 60) {
                errors.push_back(line);
            }
        } else if (line.find(" <Warning> ") != line.npos) {
            ++warning_count;
        }
        if (line.find("Shader precache") != line.npos || line.find("ShaderWarm") != line.npos ||
            line.find("AMD driver cache") != line.npos ||
            line.find("AMD driver shader cache folder") != line.npos) {
            if (precache.size() < 60) {
                precache.push_back(line);
            }
        }
        if (line.find("Nx2Stutter frame=") != line.npos) {
            const double ms = NumberAfter(line, " ms=").value_or(0);
            auto& cause = causes[std::string{WordAfter(line, "cause=")}];
            ++cause.first;
            cause.second += ms;
            hitches.push_back({ms, line});
        } else if (line.find("Nx2StutterSummary ") != line.npos) {
            ++summary_lines;
            frames += static_cast<u64>(NumberAfter(line, " frames=").value_or(0));
            over_100 += static_cast<u64>(NumberAfter(line, " over100=").value_or(0));
            gfx_builds += static_cast<u64>(NumberAfter(line, "builds=gfx:").value_or(0));
            compute_builds += static_cast<u64>(NumberAfter(line, ",compute:").value_or(0));
            const std::string_view fields = WordAfter(line, " lookups=");
            for (size_t at = 0; at < fields.size();) {
                const size_t next = std::min(fields.find(',', at), fields.size());
                const std::string_view field = fields.substr(at, next - at);
                if (const size_t colon = field.find(':'); colon != field.npos) {
                    lookups[std::string{field.substr(0, colon)}] +=
                        std::strtoull(std::string{field.substr(colon + 1)}.c_str(), nullptr, 10);
                }
                at = next + 1;
            }
        } else if (line.find("compute first dispatch:") != line.npos) {
            const double ms = NumberAfter(line, " waited ").value_or(0);
            ++compute_waits;
            compute_waits_long += ms >= 100 ? 1 : 0;
            compute_wait_ms += ms;
            compute_wait_max = std::max(compute_wait_max, ms);
        } else if (line.find("Nx2Perf held_draw:") != line.npos) {
            const double ms = NumberAfter(line, ") held ").value_or(0);
            ++held_draws;
            held_ms += ms;
            held_max = std::max(held_max, ms);
        } else if (line.find("Nx2Perf async_pipelines:") != line.npos) {
            ++async_builds;
            async_ms += NumberAfter(line, "driver build ").value_or(0);
        } else if (line.find("Nx2Perf compute_build:") != line.npos) {
            ++compute_driver_builds;
            compute_driver_ms += NumberAfter(line, "driver build ").value_or(0);
        } else if (line.find("Nx2Perf fast_first:") != line.npos) {
            ++fast_first;
        } else if (line.find("Nx2Perf unoptimized_blend:") != line.npos) {
            unoptimized_blend = NumberAfter(line, "unoptimized_blend: ").value_or(0);
        } else if (line.find("Nx2Perf cached_first:") != line.npos) {
            cached_first = line.substr(line.find("cached_first:"));
        } else if (line.find("Rebuilding compute program") != line.npos) {
            ++compute_rebuilds;
        } else if (line.find("Nx2Perf compute rebuild:") != line.npos) {
            ++compute_rebuilds_unwaited;
        } else if (line.find("Nx2Perf mesh_emulation:") != line.npos) {
            ++mesh_builds;
            mesh_ms += NumberAfter(line, "driver build ").value_or(0);
        } else if (line.find("graphics pipelines unused for") != line.npos) {
            evicted += static_cast<u64>(NumberAfter(line, "Evicted ").value_or(0));
        }
    }

    std::string text;
    text += fmt::format("Log: {:.0f} s, {} critical, {} errors, {} warnings\n\n", last_time,
                        critical_count, error_count, warning_count);
    const auto section = [&text](std::string_view title, const std::vector<std::string_view>& lines) {
        if (lines.empty()) {
            return;
        }
        text += fmt::format("== {}\n", title);
        for (const std::string_view line : lines) {
            text += line;
            text += '\n';
        }
        text += '\n';
    };
    section("Crash", crashes);
    section("Lichii, system and changed settings", header);
    section("Shader precache", precache);

    text += "== Hitches (frames over the Nx2Stutter threshold, 100 ms by default)\n";
    if (summary_lines == 0 && hitches.empty()) {
        text += "No stutter trace in this log (EDEN_NX2_STUTTER_TRACE off).\n";
    } else {
        text += fmt::format("{} frames in the 10 s summaries, {} over 100 ms; {} hitch lines\n",
                            frames, over_100, hitches.size());
        for (const auto& [cause, totals] : causes) {
            text += fmt::format("  cause {}: {} frames, {:.0f} ms\n", cause, totals.first,
                                totals.second);
        }
        std::ranges::sort(hitches, [](const Hitch& a, const Hitch& b) { return a.ms > b.ms; });
        text += "Longest:\n";
        for (size_t index = 0; index < std::min<size_t>(hitches.size(), 20); ++index) {
            text += "  ";
            text += hitches[index].line;
            text += '\n';
        }
    }
    text += '\n';

    text += "== Shader builds during play\n";
    text += fmt::format("Pipelines first used (10 s summaries):");
    if (lookups.empty()) {
        text += " none recorded";
    }
    for (const auto& [name, count] : lookups) {
        text += fmt::format(" {}={}", name, count);
    }
    text += fmt::format("\n  (gfx_new: graphics pipelines the caches did not have, built in play; "
                        "gfx_prebuilt: built on the loading screen)\n");
    text += fmt::format("Built on the GPU thread: {} graphics, {} compute\n", gfx_builds,
                        compute_builds);
    text += fmt::format("Graphics builds logged: {}, {:.0f} ms in total; fast first builds: {}; "
                        "effect draws skipped until optimized: {:.0f}\n",
                        async_builds, async_ms, fast_first, unoptimized_blend);
    text += fmt::format("Optimized pipelines from the driver's cache (last count): {}\n",
                        cached_first.empty() ? std::string_view{"none asked"} : cached_first);
    text += fmt::format("Compute builds logged: {}, {:.0f} ms in total\n", compute_driver_builds,
                        compute_driver_ms);
    text += fmt::format("Compute dispatches that waited for their pipeline: {} ({} of 100 ms or "
                        "more), {:.0f} ms in total, longest {:.0f} ms\n",
                        compute_waits, compute_waits_long, compute_wait_ms, compute_wait_max);
    text += fmt::format("Draws held for their pipeline: {}, {:.0f} ms in total, longest {:.0f} ms\n",
                        held_draws, held_ms, held_max);
    text += fmt::format("Compute programs rebuilt for a pointer set later: {} ({} took over "
                        "without a wait)\n",
                        compute_rebuilds, compute_rebuilds_unwaited);
    text += fmt::format("Mesh emulation compute builds over 100 ms: {}, {:.0f} ms in total\n",
                        mesh_builds, mesh_ms);
    text += fmt::format("Graphics pipelines destroyed as unused: {}\n\n", evicted);
    section("Errors (first 60)", errors);
    return text;
}

void PruneReports(const fs::path& dir) {
    std::vector<std::pair<fs::file_time_type, fs::path>> reports;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const std::string name = Utf8(entry.path().filename());
        if (name.starts_with(ReportPrefix) && name.ends_with(".zip")) {
            reports.emplace_back(entry.last_write_time(ec), entry.path());
        }
    }
    std::ranges::sort(reports, std::greater{});
    for (size_t index = KeepReports; index < reports.size(); ++index) {
        fs::remove(reports[index].second, ec);
    }
}

fs::path Write(Kind kind, const Session& session) {
    const fs::path dir = ReportDirectory();
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string name = ReportPrefix + QDateTime::currentDateTime()
                                          .toString(QStringLiteral("yyyyMMdd-HHmmss"))
                                          .toStdString();
    if (!session.games.empty()) {
        name += fmt::format("-{:016x}", session.games.back());
    }
    name += kind == Kind::Exit      ? "-exit"
            : kind == Kind::Crash   ? "-crash"
            : kind == Kind::Unclean ? "-unclean"
                                    : "-manual";
    const fs::path path = dir / (name + ".zip");
    const fs::path part = dir / (name + ".zip.part");

    const fs::path log_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
    const fs::path config_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir);
    {
        ZipWriter zip{part};
        std::string log = ReadFile(log_dir / LOG_FILE).value_or(std::string{});
        Censor(log);
        zip.AddText(QStringLiteral("summary.txt"), Summary(log));
        zip.AddText(QStringLiteral("eden_log.txt"), log);
        log = {};
        if (auto old = ReadFile(log_dir / (std::string{LOG_FILE} + ".old.txt"))) {
            Censor(*old);
            zip.AddText(QStringLiteral("eden_log.txt.old.txt"), *old);
        }
        const fs::path dump = log_dir / "eden_crash.dmp";
        const auto dump_time = fs::last_write_time(dump, ec);
        if (!ec && std::chrono::clock_cast<Clock>(dump_time) + std::chrono::seconds{5} >= session.start) {
            zip.AddFile(QStringLiteral("eden_crash.dmp"), dump);
        }
        zip.AddText(QStringLiteral("system.txt"), SystemText(kind, session));
        zip.AddText(QStringLiteral("shader-cache.txt"), ShaderCacheText());
        if (const auto config = ReadFile(config_dir / "qt-config.ini")) {
            zip.AddText(QStringLiteral("config/qt-config.ini"), Redact(*config));
        }
        for (const u64 game : session.games) {
            const std::string file = fmt::format("{:016X}.ini", game);
            if (const auto config = ReadFile(config_dir / "custom" / file)) {
                zip.AddText(QString::fromStdString("config/custom/" + file), Redact(*config));
            }
        }
        if (const auto switches = ReadFile(Common::FS::GetExeDirectory() / "nx2_switches.txt")) {
            zip.AddText(QStringLiteral("nx2_switches.txt"), *switches);
        }
        if (const auto switches = ReadFile(Common::FS::GetAppDataRoamingDirectory() / EDEN_DIR /
                                           "nx2_switches.txt")) {
            zip.AddText(QStringLiteral("nx2_switches-appdata.txt"), *switches);
        }
        if (!zip.Close()) {
            fs::remove(part, ec);
            return {};
        }
    }
    fs::rename(part, path, ec);
    if (ec) {
        fs::remove(part, ec);
        return {};
    }
    PruneReports(dir);
    return path;
}

} // Anonymous namespace

bool Enabled() {
    static const bool enabled = ReadEnabled();
    return enabled;
}

fs::path ReportDirectory() {
    static const fs::path dir = [] {
        const fs::path beside = Common::FS::GetExeDirectory() / "lichii-reports";
        if (CanWrite(beside)) {
            return beside;
        }
        return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "reports";
    }();
    return dir;
}

bool RunCrashReportProcess(int argc, char** argv) {
    if (argc < 3 || std::strcmp(argv[1], "--debug-report") != 0) {
        return false;
    }
    const DWORD pid = static_cast<DWORD>(std::strtoul(argv[2], nullptr, 10));
    // The crashed process flushes its log and ends; give it (and Windows Error Reporting) time.
    if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(process, 30000);
        CloseHandle(process);
    }
    const fs::path marker = MarkerPath(pid);
    Session session = ReadMarker(marker).value_or(Session{.pid = pid, .start = Clock::now() - std::chrono::hours{1}});
    const fs::path report = Write(Kind::Crash, session);
    std::error_code ec;
    fs::remove(marker, ec);
    const std::wstring message =
        report.empty()
            ? std::wstring{L"Lichii crashed, and the debug report could not be written.\n\nThe "
                           L"log is in %APPDATA%\\eden\\log."}
            : L"Lichii crashed. A debug report was saved:\n\n" + report.wstring() +
                  L"\n\nOpen its folder?";
    const int answer = MessageBoxW(nullptr, message.c_str(), L"Lichii",
                                   (report.empty() ? MB_OK : MB_YESNO) | MB_ICONERROR | MB_SETFOREGROUND);
    if (answer == IDYES) {
        const std::wstring arguments = L"/select,\"" + report.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
    }
    return true;
}

void BeginSession() {
    if (!Enabled()) {
        return;
    }
    // Detailed logging: hitches over 100 ms with their cause, and every 10 s the shader builds
    // and pipeline lookups. nx2_switches.txt or the environment can still set each one.
    SetDefault("EDEN_NX2_STUTTER_TRACE", "1");
    if (std::strcmp(std::getenv("EDEN_NX2_STUTTER_TRACE"), "1") == 0) {
        SetDefault("EDEN_NX2_STUTTER_MS", "100");
        SetDefault("EDEN_NX2_STUTTER_DETAIL_MS", "100");
    }

    const fs::path dir = ReportDirectory();
    // A session that ended without a clean exit or a crash report: its log is still
    // eden_log.txt, as the log starts (and renames it) only after this.
    std::optional<Session> stale;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const std::string name = Utf8(entry.path().filename());
        if (!name.starts_with("session-") || !name.ends_with(".txt")) {
            continue;
        }
        const auto session = ReadMarker(entry.path());
        if (session && IsRunningLichii(session->pid)) {
            continue;
        }
        if (session && (!stale || session->start > stale->start)) {
            stale = session;
        }
        fs::remove(entry.path(), ec);
    }
    if (stale && !stale->games.empty()) {
        Write(Kind::Unclean, *stale);
    }

    std::scoped_lock lock{g_mutex};
    g_session = Session{.pid = GetCurrentProcessId(), .start = Clock::now()};
    WriteMarker(g_session);
    g_began = true;
    Common::Windows::SetCrashCommand(
        L"\"" + ExePath().wstring() + L"\" --debug-report " + std::to_wstring(g_session.pid));
}

void StartLogFlush() {
    if (!Enabled() || g_flusher) {
        return;
    }
    g_flusher.emplace([](std::stop_token stop) {
        SetThreadDescription(GetCurrentThread(), L"LogFlush");
        while (!stop.stop_requested()) {
            for (int tick = 0; tick < 20 && !stop.stop_requested(); ++tick) {
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
            Common::Log::Stop(); // flushes
        }
    });
}

void NoteGame(u64 title_id) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    if (!g_began) {
        return;
    }
    if (std::ranges::find(g_session.games, title_id) == g_session.games.end()) {
        g_session.games.push_back(title_id);
    }
    WriteMarker(g_session);
}

fs::path WriteNow(Kind kind) {
    Session session;
    {
        std::scoped_lock lock{g_mutex};
        session = g_session;
        if (session.pid == 0) {
            session = Session{.pid = GetCurrentProcessId(), .start = Clock::now()};
        }
    }
    Common::Log::Stop(); // flushes
    return Write(kind, session);
}

void EndSession() {
    if (!Enabled()) {
        return;
    }
    g_flusher.reset();
    Session session;
    {
        std::scoped_lock lock{g_mutex};
        if (!g_began) {
            return;
        }
        session = g_session;
        g_began = false;
    }
    if (!session.games.empty()) {
        Common::Log::Stop(); // flushes
        Write(Kind::Exit, session);
    }
    Common::Windows::SetCrashCommand({});
    std::error_code ec;
    fs::remove(MarkerPath(session.pid), ec);
}

} // namespace DebugReport
