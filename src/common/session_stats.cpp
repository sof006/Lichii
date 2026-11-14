// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unistd.h>
#endif

#include "common/logging.h"
#include "common/memory_detect.h"
#include "common/session_stats.h"
#include "common/thread.h"

namespace Common::SessionStats {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::chrono::seconds FirstDelay{60};
/// How long the stats thread waits for the publishers after asking.
constexpr std::chrono::seconds PublishWait{5};

u32 ReadInterval() {
    const char* const value = std::getenv("LICHII_SESSION_STATS_S");
    if (value == nullptr || value[0] == '\0') {
        return 300;
    }
    const long seconds = std::strtol(value, nullptr, 10);
    return seconds <= 0 ? 0 : static_cast<u32>((std::max)(seconds, 10L));
}

std::atomic<u32> g_request{0};

} // Anonymous namespace

struct Registry {
    std::mutex mutex;
    std::vector<Publisher*> publishers;
    struct Source {
        u64 id;
        std::string name;
        std::function<std::string()> read;
    };
    std::vector<Source> sources;
    u64 next_source{1};
    /// Held while the sources run, so RemoveSource returns only once its source is done.
    std::mutex source_mutex;

    std::mutex thread_mutex;
    std::condition_variable wake;
    std::thread thread;
    bool stop{};
    Clock::time_point start{};

    static Registry& Get() {
        static Registry registry;
        return registry;
    }
};

namespace {

/// Frame times in 0.25 ms bins up to 250 ms; the last bin holds the longer frames.
struct Frames {
    static constexpr size_t Bins = 1001;
    std::mutex mutex;
    std::array<u32, Bins> histogram{};
    u64 frames{};
    double total_ms{};
    double max_ms{};
    Clock::time_point last{};
};

Frames g_frames;

std::string FrameFields() {
    std::array<u32, Frames::Bins> histogram;
    u64 frames;
    double total_ms;
    double max_ms;
    {
        std::scoped_lock lock{g_frames.mutex};
        histogram = std::exchange(g_frames.histogram, {});
        frames = std::exchange(g_frames.frames, 0);
        total_ms = std::exchange(g_frames.total_ms, 0.0);
        max_ms = std::exchange(g_frames.max_ms, 0.0);
    }
    if (frames == 0) {
        return "frames=0";
    }
    const auto percentile = [&](double fraction) {
        const u64 wanted = static_cast<u64>(static_cast<double>(frames) * fraction);
        u64 seen = 0;
        for (size_t bin = 0; bin < histogram.size(); ++bin) {
            seen += histogram[bin];
            if (seen > wanted) {
                return static_cast<double>(bin + 1) * 0.25;
            }
        }
        return static_cast<double>(histogram.size()) * 0.25;
    };
    u64 over_20 = 0;
    u64 over_33 = 0;
    for (size_t bin = 0; bin < histogram.size(); ++bin) {
        const double ms = static_cast<double>(bin) * 0.25;
        over_20 += ms >= 20.0 ? histogram[bin] : 0;
        over_33 += ms >= 33.25 ? histogram[bin] : 0;
    }
    return fmt::format("frames={} fps={:.2f} avg_ms={:.2f} p50_ms={:.2f} p90_ms={:.2f} "
                       "p99_ms={:.2f} p999_ms={:.2f} max_ms={:.1f} over20={} over33={}",
                       frames, 1000.0 * static_cast<double>(frames) / total_ms,
                       total_ms / static_cast<double>(frames), percentile(0.5), percentile(0.9),
                       percentile(0.99), percentile(0.999), max_ms, over_20, over_33);
}

std::string Word(std::string text) {
    for (char& c : text) {
        if (c == ' ' || c == '=' || c == '\t' || c == '\n' || c == '\r') {
            c = '_';
        }
    }
    return text.empty() ? std::string{"?"} : text;
}

#ifdef __linux__

/// The first number after key in /proc-style "key: value" text, or -1.
long long Field(const std::string& text, std::string_view key) {
    size_t at = 0;
    while ((at = text.find(key, at)) != std::string::npos) {
        if (at == 0 || text[at - 1] == '\n') {
            return std::strtoll(text.c_str() + at + key.size(), nullptr, 10);
        }
        at += key.size();
    }
    return -1;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file{path};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

/// utime + stime of a /proc stat line in clock ticks, and its comm.
std::pair<std::string, u64> ThreadTimes(const std::string& stat) {
    const size_t open = stat.find('(');
    const size_t close = stat.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        return {};
    }
    std::string name = stat.substr(open + 1, close - open - 1);
    std::istringstream rest{stat.substr(close + 2)};
    std::string field;
    u64 utime = 0;
    u64 stime = 0;
    // After the comm: state(3) ppid ... utime is field 14, stime 15.
    for (int index = 3; index <= 15 && rest >> field; ++index) {
        if (index == 14) {
            utime = std::strtoull(field.c_str(), nullptr, 10);
        } else if (index == 15) {
            stime = std::strtoull(field.c_str(), nullptr, 10);
        }
    }
    return {std::move(name), utime + stime};
}

struct ProcessState {
    std::map<long, std::pair<std::string, u64>> threads;
    u64 process_ticks{};
    u64 gpu_ns{};
    Clock::time_point when{};
};

ProcessState g_process;

std::string GpuFields(u64& gpu_ns) {
    // DRM fdinfo of this process (Linux 5.19+): memory per region and the engines' busy time.
    namespace fs = std::filesystem;
    std::error_code ec;
    std::set<long long> clients;
    long long vram_kib = 0;
    long long gtt_kib = 0;
    u64 gfx_ns = 0;
    bool any = false;
    for (const auto& entry : fs::directory_iterator{"/proc/self/fdinfo", ec}) {
        const std::string text = ReadFile(entry.path());
        if (text.find("drm-driver:") == std::string::npos) {
            continue;
        }
        const long long client = Field(text, "drm-client-id:");
        if (!clients.insert(client).second) {
            continue;
        }
        any = true;
        long long vram = Field(text, "drm-memory-vram:");
        long long gtt = Field(text, "drm-memory-gtt:");
        vram = vram >= 0 ? vram : Field(text, "drm-resident-vram:");
        gtt = gtt >= 0 ? gtt : Field(text, "drm-resident-gtt:");
        vram_kib += vram > 0 ? vram : 0;
        gtt_kib += gtt > 0 ? gtt : 0;
        const long long gfx = Field(text, "drm-engine-gfx:");
        gfx_ns += gfx > 0 ? static_cast<u64>(gfx) : 0;
    }
    std::string text;
    if (any) {
        gpu_ns = gfx_ns;
        text += fmt::format(" drm_clients={} drm_vram_mib={} drm_gtt_mib={}", clients.size(),
                            vram_kib >> 10, gtt_kib >> 10);
    }
    // amdgpu's whole-device counters: the card with the most VRAM.
    u64 best_total = 0;
    fs::path best;
    for (const auto& entry : fs::directory_iterator{"/sys/class/drm", ec}) {
        const auto name = entry.path().filename().string();
        if (!name.starts_with("card") || name.find('-') != std::string::npos) {
            continue;
        }
        const u64 total =
            std::strtoull(ReadFile(entry.path() / "device/mem_info_vram_total").c_str(), nullptr, 10);
        if (total > best_total) {
            best_total = total;
            best = entry.path() / "device";
        }
    }
    if (best_total != 0) {
        const auto mib = [&best](const char* file) {
            return std::strtoull(ReadFile(best / file).c_str(), nullptr, 10) >> 20;
        };
        text += fmt::format(" card_vram_used_mib={} card_vram_total_mib={} card_gtt_used_mib={}",
                            mib("mem_info_vram_used"), best_total >> 20, mib("mem_info_gtt_used"));
    }
    return text;
}

/// The process line and the per-thread CPU line (ms of CPU per second since the last lines).
std::pair<std::string, std::string> ProcessLines() {
    namespace fs = std::filesystem;
    const auto now = Clock::now();
    const std::string status = ReadFile("/proc/self/status");
    const std::string meminfo = ReadFile("/proc/meminfo");
    const auto kib_to_mib = [](long long kib) { return kib < 0 ? -1 : kib >> 10; };

    std::error_code ec;
    size_t fds = 0;
    for (auto it = fs::directory_iterator{"/proc/self/fd", ec}; !ec && it != fs::directory_iterator{};
         it.increment(ec)) {
        ++fds;
    }
    const std::string stat = ReadFile("/proc/self/stat");
    long long minor_faults = -1;
    long long major_faults = -1;
    u64 process_ticks = 0;
    if (const size_t close = stat.rfind(')'); close != std::string::npos) {
        std::istringstream rest{stat.substr(close + 2)};
        std::string field;
        for (int index = 3; index <= 15 && rest >> field; ++index) {
            if (index == 10) {
                minor_faults = std::strtoll(field.c_str(), nullptr, 10);
            } else if (index == 12) {
                major_faults = std::strtoll(field.c_str(), nullptr, 10);
            } else if (index >= 14) {
                process_ticks += std::strtoull(field.c_str(), nullptr, 10);
            }
        }
    }
    const double tick_ms = 1000.0 / static_cast<double>(sysconf(_SC_CLK_TCK));
    const double elapsed_s =
        g_process.when == Clock::time_point{}
            ? 0.0
            : std::chrono::duration<double>(now - g_process.when).count();

    u64 gpu_ns = 0;
    const std::string gpu = GpuFields(gpu_ns);
    std::string process = fmt::format(
        "rss_mib={} anon_mib={} file_mib={} data_mib={} swap_mib={} threads={} fds={} "
        "minflt={} majflt={} cpu_ms_per_s={:.0f}{}{} mem_available_mib={} slab_unreclaim_mib={} "
        "swap_free_mib={}",
        kib_to_mib(Field(status, "VmRSS:")), kib_to_mib(Field(status, "RssAnon:")),
        kib_to_mib(Field(status, "RssFile:")), kib_to_mib(Field(status, "VmData:")),
        kib_to_mib(Field(status, "VmSwap:")), Field(status, "Threads:"), fds, minor_faults,
        major_faults,
        elapsed_s > 0 ? static_cast<double>(process_ticks - g_process.process_ticks) * tick_ms /
                            elapsed_s
                      : 0.0,
        elapsed_s > 0 && gpu_ns >= g_process.gpu_ns && g_process.gpu_ns != 0
            ? fmt::format(" gpu_gfx_ms_per_s={:.0f}",
                          static_cast<double>(gpu_ns - g_process.gpu_ns) / 1e6 / elapsed_s)
            : std::string{},
        gpu, kib_to_mib(Field(meminfo, "MemAvailable:")), kib_to_mib(Field(meminfo, "SUnreclaim:")),
        kib_to_mib(Field(meminfo, "SwapFree:")));

    // Threads: CPU time since the last lines, summed by name, largest first.
    std::map<long, std::pair<std::string, u64>> threads;
    for (auto it = fs::directory_iterator{"/proc/self/task", ec};
         !ec && it != fs::directory_iterator{}; it.increment(ec)) {
        const long tid = std::strtol(it->path().filename().string().c_str(), nullptr, 10);
        auto times = ThreadTimes(ReadFile(it->path() / "stat"));
        if (!times.first.empty()) {
            threads.emplace(tid, std::move(times));
        }
    }
    std::string cpu;
    if (elapsed_s > 0) {
        std::map<std::string, double> by_name;
        for (const auto& [tid, entry] : threads) {
            const auto last = g_process.threads.find(tid);
            const u64 before = last != g_process.threads.end() ? last->second.second : 0;
            const u64 ticks = entry.second >= before ? entry.second - before : entry.second;
            by_name[Word(entry.first)] += static_cast<double>(ticks) * tick_ms / elapsed_s;
        }
        std::vector<std::pair<double, std::string>> order;
        for (auto& [name, ms] : by_name) {
            order.emplace_back(ms, name);
        }
        std::ranges::sort(order, std::greater<>{});
        double rest = 0;
        for (size_t index = 0; index < order.size(); ++index) {
            if (index < 14 && order[index].first >= 1.0) {
                cpu += fmt::format("{}{}={:.0f}", cpu.empty() ? "" : " ", order[index].second,
                                   order[index].first);
            } else {
                rest += order[index].first;
            }
        }
        cpu += fmt::format("{}other={:.0f}", cpu.empty() ? "" : " ", rest);
    }
    g_process.threads = std::move(threads);
    g_process.process_ticks = process_ticks;
    g_process.gpu_ns = gpu_ns;
    g_process.when = now;
    return {std::move(process), std::move(cpu)};
}

#else

struct ProcessState {
    u64 cpu_100ns{};
    Clock::time_point when{};
};

ProcessState g_process;

std::pair<std::string, std::string> ProcessLines() {
    const auto usage = Common::GetProcessMemoryUsage();
    std::string process = fmt::format("rss_mib={} commit_mib={} system_commit_mib={}",
                                      usage.working_set >> 20, usage.commit >> 20,
                                      usage.system_commit >> 20);
#ifdef _WIN32
    const auto now = Clock::now();
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const auto value = [](const FILETIME& time) {
            return (u64{time.dwHighDateTime} << 32) | time.dwLowDateTime;
        };
        const u64 cpu = value(kernel) + value(user);
        if (g_process.when != Clock::time_point{}) {
            const double elapsed_s = std::chrono::duration<double>(now - g_process.when).count();
            process += fmt::format(" cpu_ms_per_s={:.0f}",
                                   static_cast<double>(cpu - g_process.cpu_100ns) / 1e4 /
                                       (std::max)(elapsed_s, 1e-3));
        }
        g_process.cpu_100ns = cpu;
        g_process.when = now;
    }
#endif
    return {std::move(process), std::string{}};
}

#endif

void LogLines(Registry& registry) {
    const long long t = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() -
                                                                         registry.start)
                            .count();
    auto [process, cpu] = ProcessLines();
    LOG_INFO(Common, "LichiiStats process t={} {}", t, process);
    if (!cpu.empty()) {
        LOG_INFO(Common, "LichiiStats cpu t={} {}", t, cpu);
    }
    LOG_INFO(Common, "LichiiStats frames t={} {}", t, FrameFields());

    std::vector<std::pair<std::string, std::string>> sections;
    std::scoped_lock source_lock{registry.source_mutex};
    std::vector<std::pair<std::string, std::function<std::string()>>> sources;
    {
        std::scoped_lock lock{registry.mutex};
        const u32 request = g_request.load(std::memory_order_relaxed);
        for (const Publisher* publisher : registry.publishers) {
            sections.emplace_back(publisher->name,
                                  publisher->published_request == request
                                      ? publisher->fields
                                      : (publisher->fields.empty() ? std::string{"stale=1"}
                                                                   : publisher->fields + " stale=1"));
        }
        for (const auto& source : registry.sources) {
            sources.emplace_back(source.name, source.read);
        }
    }
    for (auto& [name, read] : sources) {
        sections.emplace_back(std::move(name), read());
    }
    for (const auto& [name, fields] : sections) {
        LOG_INFO(Common, "LichiiStats {} t={} {}", name, t, fields);
    }
}

void Run(Registry& registry) {
    Common::SetCurrentThreadName("LichiiStats");
    const u32 interval = ReadInterval();
    auto next = registry.start + FirstDelay;
    // The CPU times of the first lines are since now.
    void(ProcessLines());
    std::unique_lock lock{registry.thread_mutex};
    while (!registry.stop) {
        if (registry.wake.wait_until(lock, next, [&registry] { return registry.stop; })) {
            break;
        }
        next += std::chrono::seconds{interval};
        g_request.fetch_add(1, std::memory_order_relaxed);
        // Give the publishers a few frames, then log what came.
        const auto deadline = Clock::now() + PublishWait;
        while (!registry.stop && Clock::now() < deadline) {
            bool all = true;
            {
                std::scoped_lock registry_lock{registry.mutex};
                const u32 request = g_request.load(std::memory_order_relaxed);
                for (const Publisher* publisher : registry.publishers) {
                    all = all && publisher->published_request == request;
                }
            }
            if (all) {
                break;
            }
            registry.wake.wait_for(lock, std::chrono::milliseconds{100},
                                   [&registry] { return registry.stop; });
        }
        if (registry.stop) {
            break;
        }
        lock.unlock();
        LogLines(registry);
        lock.lock();
    }
}

} // Anonymous namespace

bool Enabled() {
    static const bool enabled = ReadInterval() != 0;
    return enabled;
}

void Start() {
    if (!Enabled()) {
        return;
    }
    auto& registry = Registry::Get();
    std::scoped_lock lock{registry.thread_mutex};
    if (registry.thread.joinable()) {
        return;
    }
    registry.stop = false;
    registry.start = Clock::now();
    {
        std::scoped_lock frames_lock{g_frames.mutex};
        g_frames.histogram = {};
        g_frames.frames = 0;
        g_frames.total_ms = 0;
        g_frames.max_ms = 0;
        g_frames.last = {};
    }
    g_process = {};
    LOG_INFO(Common, "LichiiStats: session statistics every {} s (LICHII_SESSION_STATS_S)",
             ReadInterval());
    registry.thread = std::thread{[&registry] { Run(registry); }};
}

void Stop() {
    auto& registry = Registry::Get();
    std::thread thread;
    {
        std::scoped_lock lock{registry.thread_mutex};
        registry.stop = true;
        thread = std::move(registry.thread);
    }
    registry.wake.notify_all();
    if (thread.joinable()) {
        thread.join();
    }
}

Publisher::Publisher(std::string name_) : name{Word(std::move(name_))} {
    auto& registry = Registry::Get();
    std::scoped_lock lock{registry.mutex};
    seen_request = g_request.load(std::memory_order_relaxed);
    registry.publishers.push_back(this);
}

Publisher::~Publisher() {
    auto& registry = Registry::Get();
    std::scoped_lock lock{registry.mutex};
    std::erase(registry.publishers, this);
}

bool Publisher::Due() {
    const u32 request = g_request.load(std::memory_order_relaxed);
    if (request == seen_request) {
        return false;
    }
    seen_request = request;
    return true;
}

void Publisher::Publish(std::string fields_) {
    auto& registry = Registry::Get();
    std::scoped_lock lock{registry.mutex};
    fields = std::move(fields_);
    published_request = seen_request;
}

u64 AddSource(std::string name, std::function<std::string()> read) {
    auto& registry = Registry::Get();
    std::scoped_lock lock{registry.mutex};
    const u64 id = registry.next_source++;
    registry.sources.push_back({id, Word(std::move(name)), std::move(read)});
    return id;
}

void RemoveSource(u64 id) {
    auto& registry = Registry::Get();
    std::scoped_lock source_lock{registry.source_mutex};
    std::scoped_lock lock{registry.mutex};
    std::erase_if(registry.sources, [id](const auto& source) { return source.id == id; });
}

void OnFrame() {
    if (!Enabled()) {
        return;
    }
    const auto now = Clock::now();
    std::scoped_lock lock{g_frames.mutex};
    if (g_frames.last != Clock::time_point{}) {
        const double ms = std::chrono::duration<double, std::milli>(now - g_frames.last).count();
        const size_t bin = (std::min)(static_cast<size_t>(ms * 4.0), Frames::Bins - 1);
        ++g_frames.histogram[bin];
        ++g_frames.frames;
        g_frames.total_ms += ms;
        g_frames.max_ms = (std::max)(g_frames.max_ms, ms);
    }
    g_frames.last = now;
}

} // namespace Common::SessionStats
