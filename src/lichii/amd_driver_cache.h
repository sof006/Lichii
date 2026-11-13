// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/// AMD's Windows driver names its shader cache file (<cache folder>\AMD\VkCache\<name>.parc) after
/// the path of lichii.exe. A release unzipped into a folder of its own, with the previous copy's
/// shaders moved in, would not read the previous copy's file and start with an empty driver cache.
namespace AmdDriverCache {

/// The child run by AdoptPreviousCopy: creates a device on each AMD GPU with the driver's cache in
/// the folder given, and exits. True when this process is that child.
bool RunProbeProcess(int* exit_code);

/// Gives the largest driver cache file of the previous copies this copy's name and removes the
/// rest, once the shaders are moved and before Vulkan is loaded. LICHII_AMD_CACHE_ADOPT=0 turns
/// it off.
void AdoptPreviousCopy();

/// Logs what AdoptPreviousCopy did, once the log is up.
void LogResult();

} // namespace AmdDriverCache
