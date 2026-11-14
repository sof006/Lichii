// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/session_stats.h"

namespace {

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("SessionStats: a publisher is due once per request", "[common]") {
    Common::SessionStats::Publisher publisher{"test section"};
    // No request yet.
    REQUIRE_FALSE(publisher.Due());
    REQUIRE_FALSE(publisher.Due());
}

TEST_CASE("SessionStats: the summary shows what grew between the first and last lines",
          "[common]") {
    const std::string log =
        "[  60.000000] Common <Info> common/session_stats.cpp:1:LogLines: LichiiStats process t=60 "
        "rss_mib=4000 threads=40 cpu_ms_per_s=1500\n"
        "[  60.000100] Common <Info> common/session_stats.cpp:1:LogLines: LichiiStats frames t=60 "
        "frames=3600 fps=59.94 p50_ms=16.75 p99_ms=17.50 max_ms=30.0\n"
        "[  60.000200] Common <Info> common/session_stats.cpp:1:LogLines: LichiiStats cpu t=60 "
        "GPUBack=800 GPUFront=400 CPUCore_0=300 other=10\n"
        "[  60.000300] Common <Info> common/session_stats.cpp:1:LogLines: LichiiStats texture_cache "
        "t=60 images=1000 image_mib=2000 page_entries=5000\n"
        "[7260.000000] Common <Info> common/session_stats.cpp:1:LogLines: LichiiStats process t=7260 "
        "rss_mib=4100 threads=40 cpu_ms_per_s=1600\n"
        "[7260.000100] Common <Info> common/session_stats.cpp:1:LogLines: LichiiStats frames t=7260 "
        "frames=15000 fps=50.50 p50_ms=20.00 p99_ms=25.00 max_ms=40.0\n"
        "[7260.000300] Common <Info> common/session_stats.cpp:1:LogLines: LichiiStats texture_cache "
        "t=7260 images=1010 image_mib=2010 page_entries=95000\n";
    const std::string text = Common::SessionStats::SummarizeLog(log);
    INFO(text);
    REQUIRE(Contains(text, "== Long session"));
    // The table: one row per time, in minutes.
    REQUIRE(Contains(text, "   1.0  59.94"));
    REQUIRE(Contains(text, " 121.0  50.50"));
    REQUIRE(Contains(text, "GPUBack=800"));
    // The page entries grew the most, relative to where they started: listed first.
    const size_t page = text.find("texture_cache.page_entries: 5000 -> 95000");
    const size_t images = text.find("texture_cache.images: 1000 -> 1010");
    REQUIRE(page != std::string::npos);
    REQUIRE(images != std::string::npos);
    REQUIRE(page < images);
    REQUIRE(Contains(text, "+4.5e+04 per hour"));
    // Unchanged values are left out.
    REQUIRE_FALSE(Contains(text, "process.threads"));
}

TEST_CASE("SessionStats: the summary shows the stutter trace's frame rate over time",
          "[common]") {
    std::string log;
    for (int window = 0; window < 6; ++window) {
        log += "[ " + std::to_string(10 + window * 10) +
               ".000000] HW.GPU <Info> video_core/nx2_stutter_trace.cpp:176:Summarize: "
               "Nx2StutterSummary window_s=10.00 frames=600 over33=0 over100=0 hist=16:590,17:10 "
               "cat_ms=texture:10.0,buffer:20.0 builds=gfx:0,compute:0\n";
    }
    log += "[ 700.000000] HW.GPU <Info> video_core/nx2_stutter_trace.cpp:176:Summarize: "
           "Nx2StutterSummary window_s=10.00 frames=300 over33=30 over100=0 hist=33:270,40:30 "
           "cat_ms=texture:500.0,buffer:20.0 builds=gfx:0,compute:0\n";
    const std::string text = Common::SessionStats::SummarizeLog(log, 600.0);
    INFO(text);
    REQUIRE(Contains(text, "== Frame rate over time"));
    REQUIRE(Contains(text, "texture"));
    // First block: 60 fps, p50 17 ms (the 16 ms bin), p99 18 ms (the 17 ms bin).
    REQUIRE(Contains(text, "   60.0   17   18"));
    // Second block: 30 fps, 180 frames over 33 ms a minute, texture 50 ms per second.
    REQUIRE(Contains(text, "   30.0   34   41       180.0"));
    REQUIRE(Contains(text, "     50.0"));
}

TEST_CASE("SessionStats: nothing to say without the lines", "[common]") {
    REQUIRE(Common::SessionStats::SummarizeLog("[ 1.0] Core <Info> hello\n").empty());
}
