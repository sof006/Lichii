// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "common/common_types.h"

// Long-session statistics: every few minutes (LICHII_SESSION_STATS_S, 300 s by default, the first
// time after 60 s; 0 turns it off), one log line per section with the sizes of what could grow
// over hours of play, so a report from a long session shows what grew:
//
//   LichiiStats <section> t=<seconds since start> key=value key=value ...
//
// The process section (memory, threads, CPU time per thread, system memory, the amdgpu VRAM and
// GTT counters on Linux) is read by the stats thread itself. Other sections come from their owners
// in one of two ways:
//  - a Publisher, for state only its own thread may read (the texture cache on the GPU thread):
//    the owner calls Due() once per frame (one relaxed atomic load) and Publish() when it is true;
//  - a Source, for state behind a mutex: the stats thread calls it.
// The stats thread asks for new values, waits up to a few seconds for the publishers, then logs.
namespace Common::SessionStats {

/// Whether the statistics are on (LICHII_SESSION_STATS_S is not 0).
[[nodiscard]] bool Enabled();

/// Starts the stats thread (once; later calls do nothing until Stop).
void Start();

/// Stops the stats thread. Publishers and sources stay registered.
void Stop();

/// A section its owner fills on its own thread.
class Publisher {
public:
    explicit Publisher(std::string name);
    ~Publisher();

    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    /// True once per request of the stats thread. Cheap enough to call every frame.
    [[nodiscard]] bool Due();

    /// The section's fields, "key=value key=value".
    void Publish(std::string fields);

    // Read by the stats thread under its registry lock.
    std::string name;
    std::string fields;
    u32 seen_request{};
    u32 published_request{};
};

/// Registers a section the stats thread fills by calling `read`; it must be thread safe and quick.
/// Returns an id for RemoveSource.
u64 AddSource(std::string name, std::function<std::string()> read);
void RemoveSource(u64 id);

/// Frame times, for the "frames" section's rate and percentiles. Call once per presented frame,
/// from one thread.
void OnFrame();

/// For the debug report's summary: the LichiiStats lines of a log as a table over time, what changed
/// between the first and the last of them, and the frame rate over time from the stutter trace's
/// 10 s summaries (Nx2StutterSummary), in blocks of block_s seconds. Empty if the log has neither.
[[nodiscard]] std::string SummarizeLog(std::string_view log, double block_s = 600.0);

} // namespace Common::SessionStats
