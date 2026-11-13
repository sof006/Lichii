// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

#include "common/common_types.h"

// Windows and Linux: debug reports, one zip per session in lichii-reports next to lichii.exe or
// the AppImage, to share as is. Each holds the log (and the one before it), the settings with
// accounts and tokens taken out, nx2_switches.txt, a system and a shader cache description, a
// summary of the log and, after a crash on Windows, the crash dump. A report is written when
// Lichii closes after a game ran, right after a crash (Windows: by a second lichii.exe that waits
// for the first to end), and at the next start for a session that ended any other way (killed,
// frozen, and on Linux crashed). Help > Save Debug Report writes one at any time. The sessions
// also log more: hitches over 100 ms with their cause and, every 10 s, the shader builds
// (EDEN_NX2_STUTTER_TRACE). LICHII_DEBUG_REPORT=0 turns all of it off.
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
#define LICHII_HAS_DEBUG_REPORT 1
#endif

namespace DebugReport {

enum class Kind {
    Exit,   ///< Lichii closed normally
    Crash,  ///< The crash handler ran
    Unclean,///< The session ended without either (killed, frozen, power loss)
    Manual, ///< Help > Save Debug Report
};

/// main(), first thing: with `--debug-report <pid>`, waits for that process to end, writes its
/// crash report and returns true (the caller then exits). Windows only: false elsewhere.
bool RunCrashReportProcess(int argc, char** argv);

/// main(), before the log starts: turns on the detailed logging defaults, writes the report of an
/// earlier session that ended without a clean exit, and marks this session as running.
void BeginSession();

/// Once the log has started: flushes it every few seconds, so a killed session keeps its end.
void StartLogFlush();

/// A game boots (its title id, 0 if unknown).
void NoteGame(u64 title_id);

/// Writes a report of this session now. Empty if it could not.
std::filesystem::path WriteNow(Kind kind);

/// main(), after the event loop: writes the report if a game ran and ends the session.
void EndSession();

/// Where the reports go.
std::filesystem::path ReportDirectory();

/// Whether reports are on (LICHII_DEBUG_REPORT is not 0).
bool Enabled();

} // namespace DebugReport
