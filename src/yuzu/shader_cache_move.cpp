// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/shader_cache_move.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include <QCoreApplication>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QString>

#include "common/common_types.h"
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/logging.h"

namespace ShaderCacheMove {
namespace {

namespace fs = std::filesystem;

constexpr const char* MOVED_MARKER = "moved-from-home";
constexpr const char* MESA_DB_DIR = "mesa_shader_cache_db";

u64 FolderSize(const fs::path& path) {
    std::error_code error;
    if (fs::is_regular_file(path, error)) {
        const auto size = fs::file_size(path, error);
        return error ? 0 : size;
    }
    u64 total = 0;
    for (auto it = fs::recursive_directory_iterator(path, error);
         !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (it->is_regular_file(error)) {
            const auto size = it->file_size(error);
            total += error ? 0 : size;
        }
    }
    return total;
}

std::vector<fs::path> Entries(const fs::path& path) {
    std::vector<fs::path> entries;
    std::error_code error;
    for (auto it = fs::directory_iterator(path, error);
         !error && it != fs::directory_iterator(); it.increment(error)) {
        entries.push_back(it->path());
    }
    return entries;
}

bool HasEntries(const fs::path& path) {
    std::error_code error;
    return fs::is_directory(path, error) && !fs::is_empty(path, error) && !error;
}

QString Size(u64 bytes) {
    if (bytes >= (u64{1} << 30)) {
        return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / (1 << 30), 0, 'f', 1);
    }
    return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / (1 << 20), 0, 'f', 0);
}

QString Path(const fs::path& path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

void Touch(const fs::path& path) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream{path};
}

/// The folder Mesa uses without MESA_SHADER_CACHE_DIR (see disk_cache_os.c).
fs::path HomeMesaCacheDir() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] == '/') {
        return fs::path{xdg} / MESA_DB_DIR;
    }
    return Common::FS::GetHomeDirectory() / ".cache" / MESA_DB_DIR;
}

class Copier {
public:
    Copier(QProgressDialog& dialog_, u64 total_) : dialog{dialog_}, total{total_} {}

    bool CopyFile(const fs::path& from, const fs::path& to) {
        std::ifstream in{from, std::ios::binary};
        std::ofstream out{to, std::ios::binary | std::ios::trunc};
        if (!in || !out) {
            return false;
        }
        std::vector<char> buffer(8 << 20);
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto read = in.gcount();
            if (read <= 0) {
                break;
            }
            if (!out.write(buffer.data(), read)) {
                return false;
            }
            Advance(static_cast<u64>(read));
        }
        return !in.bad() && static_cast<bool>(out.flush());
    }

    bool CopyTree(const fs::path& from, const fs::path& to) {
        std::error_code error;
        if (fs::is_regular_file(from, error)) {
            return CopyFile(from, to);
        }
        fs::create_directories(to, error);
        if (error) {
            return false;
        }
        for (const auto& entry : Entries(from)) {
            if (!CopyTree(entry, to / entry.filename())) {
                return false;
            }
        }
        return true;
    }

    /// Moves a file or folder, merging into an existing folder. Of two files with the same name
    /// the larger is kept: these caches only grow.
    bool Move(const fs::path& from, const fs::path& to) {
        std::error_code error;
        const bool from_dir = fs::is_directory(from, error);
        if (fs::exists(to, error)) {
            if (from_dir && fs::is_directory(to, error)) {
                bool ok = true;
                for (const auto& entry : Entries(from)) {
                    ok &= Move(entry, to / entry.filename());
                }
                fs::remove(from, error);
                return ok;
            }
            if (from_dir || fs::is_directory(to, error) ||
                FolderSize(from) <= FolderSize(to)) {
                Advance(FolderSize(from));
                fs::remove_all(from, error);
                return !error;
            }
            fs::remove(to, error);
        }
        const u64 size = FolderSize(from);
        fs::rename(from, to, error);
        if (!error) {
            Advance(size);
            return true;
        }
        // Another filesystem: copy, then remove the original.
        if (!CopyTree(from, to)) {
            fs::remove_all(to, error);
            return false;
        }
        fs::remove_all(from, error);
        return true;
    }

private:
    void Advance(u64 bytes) {
        done += bytes;
        if (total != 0) {
            dialog.setValue(static_cast<int>(done * 1000 / total));
        }
        QCoreApplication::processEvents();
    }

    QProgressDialog& dialog;
    u64 total;
    u64 done{};
};

} // namespace

void OfferMove(QWidget* parent) {
    if (!Common::FS::IsShaderDirBesideAppImage()) {
        return;
    }
    const fs::path new_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir);
    const fs::path old_dir = Common::FS::GetHomeShaderDir();
    std::error_code error;
    if (fs::exists(new_dir / MOVED_MARKER, error) || old_dir.empty() || old_dir == new_dir) {
        return;
    }

    const bool move_own = HasEntries(old_dir);
    // A MESA_SHADER_CACHE_DIR of the user's own is theirs to manage.
    const char* mesa_env = std::getenv("MESA_SHADER_CACHE_DIR");
    const fs::path old_mesa = HomeMesaCacheDir();
    const fs::path new_mesa = Common::FS::GetDriverShaderCacheDir() / MESA_DB_DIR;
    const u64 own_size = move_own ? FolderSize(old_dir) : 0;
    const u64 old_mesa_size = mesa_env == nullptr && HasEntries(old_mesa) ? FolderSize(old_mesa) : 0;
    const bool copy_mesa = old_mesa_size > FolderSize(new_mesa);
    if (!move_own && !copy_mesa) {
        return;
    }

    QString text = QObject::tr("Lichii now keeps its shader caches next to the AppImage, in:\n%1\n\n")
                       .arg(Path(new_dir));
    if (move_own) {
        text += QObject::tr("Lichii's shaders (%1) will be moved from:\n%2\n\n")
                    .arg(Size(own_size), Path(old_dir));
    }
    if (copy_mesa) {
        text += QObject::tr("Mesa's driver shader cache (%1) will be copied from:\n%2\n"
                            "It is copied, not moved, because other programs use it too.\n\n")
                    .arg(Size(old_mesa_size), Path(old_mesa));
    }
    text += QObject::tr("Without them, shaders are built again in the new folder.");

    QMessageBox box(QMessageBox::Question, QObject::tr("Move shader caches?"), text,
                    QMessageBox::NoButton, parent);
    auto* move_button = box.addButton(QObject::tr("Move them here"), QMessageBox::AcceptRole);
    auto* keep_button =
        box.addButton(QObject::tr("Keep using the old folder"), QMessageBox::RejectRole);
    auto* later_button = box.addButton(QObject::tr("Ask next time"), QMessageBox::DestructiveRole);
    box.setDefaultButton(move_button);
    box.setEscapeButton(later_button);
    box.exec();

    if (box.clickedButton() == keep_button) {
        Touch(new_dir / Common::FS::USE_HOME_SHADER_DIR_MARKER);
        Common::FS::UseHomeShaderDir();
        LOG_INFO(Frontend, "Shader caches stay in {}", Common::FS::PathToUTF8String(old_dir));
        return;
    }
    if (box.clickedButton() != move_button) {
        return;
    }

    const u64 needed = (copy_mesa ? old_mesa_size : 0) + (64ULL << 20);
    fs::create_directories(new_dir, error);
    const auto space = fs::space(new_dir, error);
    // Moving within one filesystem needs no space; a copy does.
    if (!error && space.available < needed && copy_mesa) {
        QMessageBox::warning(parent, QObject::tr("Move shader caches?"),
                             QObject::tr("There is not enough free space next to the AppImage "
                                         "(%1 needed, %2 free).")
                                 .arg(Size(needed), Size(space.available)));
        return;
    }

    QProgressDialog dialog(QObject::tr("Moving shader caches..."), QString{}, 0, 1000, parent);
    dialog.setWindowTitle(QObject::tr("Move shader caches"));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setCancelButton(nullptr);
    dialog.setMinimumDuration(0);
    dialog.setValue(0);
    Copier copier{dialog, own_size + (copy_mesa ? old_mesa_size : 0)};

    bool ok = true;
    if (move_own) {
        for (const auto& entry : Entries(old_dir)) {
            ok &= copier.Move(entry, new_dir / entry.filename());
        }
        fs::remove(old_dir, error); // only when it is empty now
    }
    if (copy_mesa) {
        fs::path part = new_mesa;
        part += ".part";
        fs::remove_all(part, error);
        if (copier.CopyTree(old_mesa, part)) {
            fs::remove_all(new_mesa, error);
            fs::rename(part, new_mesa, error);
            ok &= !error;
        } else {
            fs::remove_all(part, error);
            ok = false;
        }
    }
    dialog.setValue(1000);

    if (ok) {
        Touch(new_dir / MOVED_MARKER);
        LOG_INFO(Frontend, "Moved shader caches to {}", Common::FS::PathToUTF8String(new_dir));
    } else {
        QMessageBox::warning(parent, QObject::tr("Move shader caches"),
                             QObject::tr("Some caches could not be moved. Lichii asks again "
                                         "next time; anything already moved stays in:\n%1")
                                 .arg(Path(new_dir)));
    }
}

} // namespace ShaderCacheMove
