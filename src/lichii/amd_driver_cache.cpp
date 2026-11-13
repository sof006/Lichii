// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/amd_driver_cache.h"

#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/vulkan_common/vulkan.h"

namespace AmdDriverCache {
namespace {

namespace fs = std::filesystem;

constexpr wchar_t PROBE_ARG[] = L"--lichii-amd-cache-probe";
/// Beside the cache files: the path of lichii.exe, then the file names the driver gives it.
constexpr const char* OWNER_FILE = "lichii-exe.txt";
constexpr u32 AMD_VENDOR_ID = 0x1002;
constexpr DWORD PROBE_TIMEOUT_MS = 20000;

std::vector<std::string> result_lines;

fs::path ExePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (size == 0) {
            return {};
        }
        if (size < path.size()) {
            path.resize(size);
            return fs::path{path};
        }
        path.resize(path.size() * 2);
    }
}

/// <32 hex digits>.parc: the second 16 digits stand for the GPU and driver, the first for the
/// program.
bool IsCacheFile(const fs::path& path) {
    const std::string stem = path.stem().string();
    return path.extension() == ".parc" && stem.size() == 32 &&
           stem.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

std::vector<fs::path> CacheFiles(const fs::path& dir) {
    std::vector<fs::path> files;
    std::error_code error;
    for (auto it = fs::directory_iterator(dir, error); !error && it != fs::directory_iterator();
         it.increment(error)) {
        if (it->is_regular_file(error) && IsCacheFile(it->path())) {
            files.push_back(it->path());
        }
    }
    return files;
}

std::string Device(const fs::path& file) {
    return file.stem().string().substr(16);
}

u64 Size(const fs::path& file) {
    std::error_code error;
    const auto size = fs::file_size(file, error);
    return error ? 0 : size;
}

std::string Mib(u64 bytes) {
    return fmt::format("{:.1f} MiB", static_cast<double>(bytes) / (1 << 20));
}

/// The cache file names the driver gives this lichii.exe (one per AMD GPU), from a child run of it
/// with the driver's cache in an empty folder. Empty when it failed.
std::optional<std::vector<std::string>> ProbeOwnNames(const fs::path& exe, const fs::path& scratch) {
    std::error_code error;
    fs::remove_all(scratch, error);
    fs::create_directories(scratch, error);
    std::wstring command = L"\"" + exe.wstring() + L"\" " + PROBE_ARG + L" \"" +
                           scratch.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        fs::remove_all(scratch, error);
        return std::nullopt;
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, PROBE_TIMEOUT_MS);
    DWORD exit_code = 1;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exit_code);
    } else {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 2000);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::vector<std::string> names;
    for (const fs::path& file : CacheFiles(scratch / "AMD" / "VkCache")) {
        names.push_back(file.filename().string());
    }
    fs::remove_all(scratch, error);
    if (wait != WAIT_OBJECT_0 || exit_code != 0) {
        return std::nullopt;
    }
    return names;
}

/// The names recorded for `exe`, if the owner file is about it.
std::optional<std::vector<std::string>> RecordedNames(const fs::path& owner, const fs::path& exe) {
    std::ifstream file{owner};
    std::string line;
    if (!file || !std::getline(file, line) || line != Common::FS::PathToUTF8String(exe)) {
        return std::nullopt;
    }
    std::vector<std::string> names;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            names.push_back(line);
        }
    }
    return names;
}

void Record(const fs::path& owner, const fs::path& exe, const std::vector<std::string>& names) {
    std::ofstream file{owner, std::ios::trunc};
    file << Common::FS::PathToUTF8String(exe) << '\n';
    for (const std::string& name : names) {
        file << name << '\n';
    }
}

} // namespace

bool RunProbeProcess(int* exit_code) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return false;
    }
    if (argc != 3 || std::wcscmp(argv[1], PROBE_ARG) != 0) {
        LocalFree(argv);
        return false;
    }
    *exit_code = 1;
    const bool cache_set = _wputenv_s(L"AMD_SHADER_DISK_CACHE_PATH", argv[2]) == 0;
    LocalFree(argv);
    HMODULE library = cache_set ? LoadLibraryW(L"vulkan-1.dll") : nullptr;
    if (library == nullptr) {
        return true;
    }
    const auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(library, "vkGetInstanceProcAddr")));
    const auto create_instance =
        get_proc ? reinterpret_cast<PFN_vkCreateInstance>(get_proc(nullptr, "vkCreateInstance"))
                 : nullptr;
    if (create_instance == nullptr) {
        return true;
    }
    // As Lichii's own instance (vulkan_wrapper.cpp); the driver's file name does not depend on it.
    const VkApplicationInfo application{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "yuzu Emulator",
        .applicationVersion = VK_MAKE_VERSION(1, 3, 0),
        .pEngineName = "yuzu Emulator",
        .engineVersion = VK_MAKE_VERSION(1, 3, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo instance_ci{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    VkInstance instance = VK_NULL_HANDLE;
    if (create_instance(&instance_ci, nullptr, &instance) != VK_SUCCESS) {
        return true;
    }
#define LOAD(name) const auto name = reinterpret_cast<PFN_##name>(get_proc(instance, #name))
    LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceProperties);
    LOAD(vkCreateDevice);
    LOAD(vkDestroyDevice);
    LOAD(vkDestroyInstance);
#undef LOAD
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physical_devices(count);
    vkEnumeratePhysicalDevices(instance, &count, physical_devices.data());
    bool ok = true;
    for (VkPhysicalDevice physical_device : physical_devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_device, &properties);
        if (properties.vendorID != AMD_VENDOR_ID) {
            continue;
        }
        // Creating the device writes the cache file.
        const float priority = 1.0f;
        const VkDeviceQueueCreateInfo queue_ci{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = 0,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };
        const VkDeviceCreateInfo device_ci{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue_ci,
        };
        VkDevice device = VK_NULL_HANDLE;
        if (vkCreateDevice(physical_device, &device_ci, nullptr, &device) == VK_SUCCESS) {
            vkDestroyDevice(device, nullptr);
        } else {
            ok = false;
        }
    }
    vkDestroyInstance(instance, nullptr);
    *exit_code = ok ? 0 : 1;
    return true;
}

void AdoptPreviousCopy() {
    if (const char* value = std::getenv("LICHII_AMD_CACHE_ADOPT"); value && value[0] == '0') {
        return;
    }
    // A cache folder the user chose is theirs to manage.
    if (std::getenv("AMD_SHADER_DISK_CACHE_PATH") != nullptr) {
        return;
    }
    const fs::path driver_dir = Common::FS::GetDriverShaderCacheDir();
    if (driver_dir.empty()) {
        return;
    }
    const fs::path cache_dir = driver_dir / "AMD" / "VkCache";
    const std::vector<fs::path> files = CacheFiles(cache_dir);
    if (files.empty()) {
        return;
    }
    const fs::path exe = ExePath();
    if (exe.empty()) {
        return;
    }
    const fs::path owner = cache_dir / OWNER_FILE;
    std::optional<std::vector<std::string>> own_names = RecordedNames(owner, exe);
    if (!own_names) {
        own_names = ProbeOwnNames(exe, driver_dir / fmt::format("probe-{}", GetCurrentProcessId()));
        if (!own_names) {
            result_lines.push_back("AMD driver cache: could not find this copy's cache file name");
            return;
        }
        Record(owner, exe, *own_names);
    }
    for (const std::string& own_name : *own_names) {
        const fs::path own = cache_dir / own_name;
        if (!IsCacheFile(own)) {
            continue;
        }
        // Of the previous copies' files for this GPU and driver, the largest: the one the
        // shaders were built into. The others can't be read by this copy.
        fs::path best;
        std::vector<fs::path> others;
        for (const fs::path& file : files) {
            if (file.filename() == own.filename() || Device(file) != Device(own)) {
                continue;
            }
            if (best.empty() || Size(file) > Size(best)) {
                if (!best.empty()) {
                    others.push_back(best);
                }
                best = file;
            } else {
                others.push_back(file);
            }
        }
        if (best.empty()) {
            continue;
        }
        std::error_code error;
        const u64 own_size = Size(own);
        const u64 best_size = Size(best);
        if (best_size > own_size) {
            fs::rename(best, own, error);
            if (error) {
                result_lines.push_back(fmt::format("AMD driver cache: could not rename {}: {}",
                                                   best.filename().string(), error.message()));
                continue;
            }
            result_lines.push_back(fmt::format(
                "AMD driver cache: the previous copy's {} ({}) is now this copy's {} (was {})",
                best.filename().string(), Mib(best_size), own_name, Mib(own_size)));
        } else {
            others.push_back(best);
        }
        u64 removed = 0;
        for (const fs::path& other : others) {
            const u64 size = Size(other);
            if (fs::remove(other, error)) {
                removed += size;
            }
        }
        if (!others.empty()) {
            result_lines.push_back(fmt::format(
                "AMD driver cache: removed {} files of older copies ({}) this copy can't read",
                others.size(), Mib(removed)));
        }
    }
}

void LogResult() {
    for (const std::string& line : result_lines) {
        LOG_INFO(Frontend, "{}", line);
    }
    result_lines.clear();
}

} // namespace AmdDriverCache
