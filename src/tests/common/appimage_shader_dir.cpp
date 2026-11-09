// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"

#if defined(__linux__) && !defined(__ANDROID__)

namespace fs = std::filesystem;
using namespace Common::FS;

TEST_CASE("ShaderDir follows the AppImage folder", "[common][fs]") {
    const fs::path folder = fs::temp_directory_path() / "lichii-appimage-shader-dir-test";
    fs::remove_all(folder);
    fs::create_directories(folder);
    const std::string appimage = (folder / "lichii.AppImage").string();
    const fs::path expected = folder / "lichii-shaders";

    setenv("APPIMAGE", appimage.c_str(), 1);
    unsetenv("LICHII_APPIMAGE_SHADERS");
    SetAppDirectory({});
    CHECK(IsShaderDirBesideAppImage());
    CHECK(GetEdenPath(EdenPath::ShaderDir) == expected);
    CHECK(GetDriverShaderCacheDir() == expected / "mesa");
    CHECK(GetHomeShaderDir() != expected);

    UseHomeShaderDir();
    CHECK_FALSE(IsShaderDirBesideAppImage());
    CHECK(GetEdenPath(EdenPath::ShaderDir) == GetHomeShaderDir());
    CHECK(GetDriverShaderCacheDir().empty());

    // The prompt's "keep the old folder" answer lasts
    fs::create_directories(expected);
    std::ofstream{expected / USE_HOME_SHADER_DIR_MARKER};
    SetAppDirectory({});
    CHECK_FALSE(IsShaderDirBesideAppImage());
    fs::remove(expected / USE_HOME_SHADER_DIR_MARKER);

    setenv("LICHII_APPIMAGE_SHADERS", "0", 1);
    SetAppDirectory({});
    CHECK_FALSE(IsShaderDirBesideAppImage());

    unsetenv("LICHII_APPIMAGE_SHADERS");
    unsetenv("APPIMAGE");
    SetAppDirectory({});
    CHECK_FALSE(IsShaderDirBesideAppImage());
    fs::remove_all(folder);
}

#endif
