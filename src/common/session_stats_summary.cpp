// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The debug report's long-session part of summary.txt, from the LichiiStats lines
// (session_stats.cpp) and the stutter trace's Nx2StutterSummary lines.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "common/session_stats.h"

namespace Common::SessionStats {
namespace {

struct Series {
    double first_t{};
    double first{};
    double last_t{};
    double last{};
    double max{};
    double min{};
    u64 samples{};
};

std::optional<double> Number(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const std::string copy{text};
    char* end{};
    const double value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

/// The value of " key=" in a line up to the next space, or empty.
std::string_view Value(std::string_view line, std::string_view key) {
    size_t at = 0;
    while ((at = line.find(key, at)) != std::string_view::npos) {
        if (at == 0 || line[at - 1] == ' ') {
            std::string_view rest = line.substr(at + key.size());
            return rest.substr(0, rest.find(' '));
        }
        at += key.size();
    }
    return {};
}

double NumberOr(std::string_view line, std::string_view key, double fallback = -1.0) {
    return Number(Value(line, key)).value_or(fallback);
}

/// Seconds of the log's "[  12.345678]" prefix.
std::optional<double> LogTime(std::string_view line) {
    if (!line.starts_with('[')) {
        return std::nullopt;
    }
    const size_t close = line.find(']');
    if (close == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view inside = line.substr(1, close - 1);
    while (!inside.empty() && inside.front() == ' ') {
        inside.remove_prefix(1);
    }
    return Number(inside);
}

struct Row {
    std::map<std::string, std::string, std::less<>> process;
    std::string frames_line;
    std::string cpu_line;
};

struct Block {
    double seconds{};
    u64 frames{};
    u64 over_33{};
    u64 over_100{};
    std::map<u32, u64> histogram;
    std::map<std::string, double, std::less<>> category_ms;
    double rss_mib{-1.0};
};

double HistogramPercentile(const std::map<u32, u64>& histogram, u64 frames, double fraction) {
    const u64 wanted = static_cast<u64>(static_cast<double>(frames) * fraction);
    u64 seen = 0;
    for (const auto& [ms, count] : histogram) {
        seen += count;
        if (seen > wanted) {
            return static_cast<double>(ms) + 1.0;
        }
    }
    return histogram.empty() ? 0.0 : static_cast<double>(histogram.rbegin()->first) + 1.0;
}

std::string Short(double value) {
    if (std::fabs(value) >= 100.0 || value == std::floor(value)) {
        return fmt::format("{:.0f}", value);
    }
    return fmt::format("{:.2f}", value);
}

} // Anonymous namespace

std::string SummarizeLog(std::string_view log, double block_s) {
    // section -> key -> series
    std::map<std::string, std::map<std::string, Series>, std::less<>> series;
    std::map<double, Row> rows;
    std::map<long long, Block> blocks;

    for (size_t begin = 0; begin < log.size();) {
        const size_t end = (std::min)(log.find('\n', begin), log.size());
        std::string_view line = log.substr(begin, end - begin);
        begin = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (const size_t at = line.find("LichiiStats "); at != std::string_view::npos) {
            std::string_view rest = line.substr(at + 12);
            const size_t space = rest.find(' ');
            if (space == std::string_view::npos) {
                continue;
            }
            const std::string section{rest.substr(0, space)};
            rest.remove_prefix(space + 1);
            if (!rest.starts_with("t=")) {
                continue;
            }
            const auto t = Number(rest.substr(2, rest.find(' ') - 2));
            if (!t) {
                continue;
            }
            Row& row = rows[*t];
            if (section == "frames") {
                row.frames_line = std::string{rest};
            } else if (section == "cpu") {
                row.cpu_line = std::string{rest};
            }
            auto& keys = series[section];
            for (size_t at_token = 0; at_token < rest.size();) {
                const size_t next = (std::min)(rest.find(' ', at_token), rest.size());
                const std::string_view token = rest.substr(at_token, next - at_token);
                at_token = next + 1;
                const size_t equals = token.find('=');
                if (equals == std::string_view::npos || token.starts_with("t=")) {
                    continue;
                }
                const auto value = Number(token.substr(equals + 1));
                if (!value) {
                    continue;
                }
                const std::string key{token.substr(0, equals)};
                if (section == "process") {
                    row.process[key] = std::string{token.substr(equals + 1)};
                }
                Series& entry = keys[key];
                if (entry.samples == 0) {
                    entry.first_t = *t;
                    entry.first = *value;
                    entry.max = *value;
                    entry.min = *value;
                }
                entry.last_t = *t;
                entry.last = *value;
                entry.max = (std::max)(entry.max, *value);
                entry.min = (std::min)(entry.min, *value);
                ++entry.samples;
            }
        } else if (line.find("Nx2StutterSummary ") != std::string_view::npos) {
            const auto time = LogTime(line);
            if (!time) {
                continue;
            }
            Block& block = blocks[static_cast<long long>(*time / block_s)];
            block.seconds += NumberOr(line, "window_s=", 0.0);
            block.frames += static_cast<u64>(NumberOr(line, "frames=", 0.0));
            block.over_33 += static_cast<u64>(NumberOr(line, "over33=", 0.0));
            block.over_100 += static_cast<u64>(NumberOr(line, "over100=", 0.0));
            const std::string_view hist = Value(line, "hist=");
            for (size_t at = 0; at < hist.size();) {
                const size_t next = (std::min)(hist.find(',', at), hist.size());
                const std::string_view field = hist.substr(at, next - at);
                at = next + 1;
                const size_t colon = field.find(':');
                if (colon == std::string_view::npos) {
                    continue;
                }
                const auto ms = Number(field.substr(0, colon));
                const auto count = Number(field.substr(colon + 1));
                if (ms && count) {
                    block.histogram[static_cast<u32>(*ms)] += static_cast<u64>(*count);
                }
            }
            const std::string_view categories = Value(line, "cat_ms=");
            for (size_t at = 0; at < categories.size();) {
                const size_t next = (std::min)(categories.find(',', at), categories.size());
                const std::string_view field = categories.substr(at, next - at);
                at = next + 1;
                const size_t colon = field.find(':');
                if (colon == std::string_view::npos) {
                    continue;
                }
                if (const auto ms = Number(field.substr(colon + 1))) {
                    block.category_ms[std::string{field.substr(0, colon)}] += *ms;
                }
            }
            if (const double rss = NumberOr(line, "rss_mib="); rss >= 0) {
                block.rss_mib = rss;
            } else if (const double commit = NumberOr(line, "commit_mib="); commit >= 0) {
                block.rss_mib = commit;
            }
        }
    }

    std::string text;
    if (!rows.empty()) {
        text += "== Long session: LichiiStats lines (every LICHII_SESSION_STATS_S, 300 s by "
                "default)\n";
        text += "   min    fps  p50_ms  p99_ms  max_ms  rss_mib  cpu_ms/s  gpu_ms/s  "
                "vram_mib  busiest threads (ms of CPU per s)\n";
        for (const auto& [t, row] : rows) {
            const auto process = [&row](std::string_view key) -> std::string {
                const auto it = row.process.find(key);
                return it != row.process.end() ? it->second : std::string{"-"};
            };
            std::string threads;
            size_t shown = 0;
            for (size_t at = 0; at < row.cpu_line.size() && shown < 4;) {
                const size_t next = (std::min)(row.cpu_line.find(' ', at), row.cpu_line.size());
                const std::string_view token =
                    std::string_view{row.cpu_line}.substr(at, next - at);
                at = next + 1;
                if (token.starts_with("t=") || token.starts_with("other=")) {
                    continue;
                }
                threads += fmt::format("{}{}", threads.empty() ? "" : " ", token);
                ++shown;
            }
            const std::string_view frames = row.frames_line;
            const auto field = [&frames](std::string_view key) {
                const auto value = Number(Value(frames, key));
                return value ? Short(*value) : std::string{"-"};
            };
            std::string vram = process("drm_vram_mib");
            if (vram == "-") {
                vram = process("card_vram_used_mib");
            }
            text += fmt::format("{:>6.1f} {:>6} {:>7} {:>7} {:>7} {:>8} {:>9} {:>9} {:>9}  {}\n",
                                t / 60.0, field("fps="), field("p50_ms="), field("p99_ms="),
                                field("max_ms="), process("rss_mib"), process("cpu_ms_per_s"),
                                process("gpu_gfx_ms_per_s"), vram, threads);
        }
        // What changed: sizes that only grow are the suspects for a slowdown over hours.
        struct Change {
            double score;
            std::string line;
        };
        std::vector<Change> changes;
        for (const auto& [section, keys] : series) {
            for (const auto& [key, entry] : keys) {
                if (entry.samples < 2 || entry.first == entry.last) {
                    continue;
                }
                const double hours = (entry.last_t - entry.first_t) / 3600.0;
                const double delta = entry.last - entry.first;
                const double relative =
                    std::fabs(delta) / (std::max)(std::fabs(entry.first), 1.0);
                changes.push_back(
                    {relative,
                     fmt::format("  {}.{}: {} -> {} (min {}, max {}){}", section, key,
                                 Short(entry.first), Short(entry.last), Short(entry.min),
                                 Short(entry.max),
                                 hours > 0.05 ? fmt::format(", {:+.4g} per hour", delta / hours)
                                              : std::string{})});
            }
        }
        std::ranges::sort(changes, [](const Change& a, const Change& b) { return a.score > b.score; });
        text += fmt::format("\nFirst line to last, largest relative change first ({} values "
                            "changed):\n",
                            changes.size());
        for (size_t index = 0; index < (std::min<size_t>)(changes.size(), 120); ++index) {
            text += changes[index].line;
            text += '\n';
        }
        text += '\n';
    }
    if (!blocks.empty()) {
        text += fmt::format("== Frame rate over time (stutter trace, {:.0f} min blocks; "
                            "categories in ms per second)\n",
                            block_s / 60.0);
        std::vector<std::string> names;
        for (const auto& [index, block] : blocks) {
            for (const auto& [name, ms] : block.category_ms) {
                if (std::ranges::find(names, name) == names.end()) {
                    names.push_back(name);
                }
            }
        }
        text += "  from_min    fps  p50  p99  over33/min  rss_mib";
        for (const auto& name : names) {
            text += fmt::format("  {}", name);
        }
        text += '\n';
        for (const auto& [index, block] : blocks) {
            if (block.seconds <= 0) {
                continue;
            }
            text += fmt::format("  {:>8.0f} {:>6.1f} {:>4.0f} {:>4.0f} {:>11.1f} {:>8}",
                                static_cast<double>(index) * block_s / 60.0,
                                static_cast<double>(block.frames) / block.seconds,
                                HistogramPercentile(block.histogram, block.frames, 0.5),
                                HistogramPercentile(block.histogram, block.frames, 0.99),
                                static_cast<double>(block.over_33) * 60.0 / block.seconds,
                                block.rss_mib >= 0 ? Short(block.rss_mib) : std::string{"-"});
            for (const auto& name : names) {
                const auto it = block.category_ms.find(name);
                const double ms = it != block.category_ms.end() ? it->second / block.seconds : 0.0;
                text += fmt::format("  {:>{}.1f}", ms, name.size());
            }
            text += '\n';
        }
        text += '\n';
    }
    return text;
}

} // namespace Common::SessionStats
