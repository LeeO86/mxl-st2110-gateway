// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace mxlgw::util
{
    /// Reads a whole file; nullopt if it cannot be opened.
    std::optional<std::string> readFile(std::string const& path);

    /// Atomic replace: temp file in the same directory, fsync, rename, fsync of the directory.
    /// Throws std::runtime_error on failure. `mode` applies to the new file.
    void atomicWrite(std::string const& path, std::string const& content, unsigned mode = 0644);

    /// Modification time in nanoseconds since the epoch; nullopt if the file does not exist.
    std::optional<std::int64_t> mtimeNs(std::string const& path);

    /// statfs-based filesystem test; same magic numbers MXL's mxlIsTmpFs uses (tmpfs, ramfs).
    struct FsInfo
    {
        bool exists = false;
        std::string typeName; // "tmpfs", "ramfs", "overlay", "ext4", ... or hex magic
        bool tmpfs = false;   // tmpfs or ramfs
        bool overlay = false; // overlayfs / fuse-overlayfs (container root)
        std::uint64_t totalBytes = 0;
        std::uint64_t freeBytes = 0;
        std::uint64_t deviceId = 0;
    };

    /// Inspects the filesystem holding `path`, or its nearest existing ancestor.
    FsInfo inspectFs(std::string const& path);

    /// Device id of "/" (to detect a path living on the container root filesystem).
    std::optional<std::uint64_t> deviceOf(std::string const& path);
}
