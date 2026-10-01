// SPDX-License-Identifier: MIT
#include "util/fs.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

namespace mxlgw::util
{
    namespace
    {
        constexpr long tmpfsMagic = 0x01021994;
        constexpr long ramfsMagic = 0x858458f6;
        constexpr long overlayMagic = 0x794c7630;
        constexpr long fuseMagic = 0x65735546;

        std::string fsName(long magic)
        {
            switch (magic)
            {
                case tmpfsMagic: return "tmpfs";
                case ramfsMagic: return "ramfs";
                case overlayMagic: return "overlay";
                case fuseMagic: return "fuse";
                case 0xEF53: return "ext4";
                case 0x58465342: return "xfs";
                case 0x9123683E: return "btrfs";
                case 0x6969: return "nfs";
                case 0x01021997: return "v9fs";
                default: break;
            }
            char buf[24];
            std::snprintf(buf, sizeof(buf), "0x%lx", static_cast<unsigned long>(magic));
            return buf;
        }

        std::string nearestExisting(std::string const& path)
        {
            std::filesystem::path p{path};
            std::error_code ec;
            while (!p.empty() && !std::filesystem::exists(p, ec))
            {
                auto const parent = p.parent_path();
                if (parent == p)
                {
                    break;
                }
                p = parent;
            }
            return p.empty() ? std::string("/") : p.string();
        }

        void throwErrno(std::string const& what)
        {
            throw std::runtime_error(what + ": " + std::strerror(errno));
        }
    }

    std::optional<std::string> readFile(std::string const& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return std::nullopt;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    void atomicWrite(std::string const& path, std::string const& content, unsigned mode)
    {
        std::filesystem::path const target{path};
        auto const dir = target.has_parent_path() ? target.parent_path() : std::filesystem::path{"."};
        std::string tmpl = (dir / ("." + target.filename().string() + ".tmpXXXXXX")).string();
        int fd = ::mkstemp(tmpl.data());
        if (fd < 0)
        {
            throwErrno("mkstemp in " + dir.string());
        }
        std::size_t written = 0;
        while (written < content.size())
        {
            auto const n = ::write(fd, content.data() + written, content.size() - written);
            if (n < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                int const saved = errno;
                ::close(fd);
                ::unlink(tmpl.c_str());
                errno = saved;
                throwErrno("write " + tmpl);
            }
            written += static_cast<std::size_t>(n);
        }
        if (::fchmod(fd, mode) != 0 || ::fsync(fd) != 0)
        {
            int const saved = errno;
            ::close(fd);
            ::unlink(tmpl.c_str());
            errno = saved;
            throwErrno("fsync " + tmpl);
        }
        ::close(fd);
        if (::rename(tmpl.c_str(), path.c_str()) != 0)
        {
            int const saved = errno;
            ::unlink(tmpl.c_str());
            errno = saved;
            throwErrno("rename " + tmpl + " -> " + path);
        }
        int const dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0)
        {
            ::fsync(dfd);
            ::close(dfd);
        }
    }

    std::optional<std::int64_t> mtimeNs(std::string const& path)
    {
        struct stat st
        {};
        if (::stat(path.c_str(), &st) != 0)
        {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000LL + st.st_mtim.tv_nsec;
    }

    FsInfo inspectFs(std::string const& path)
    {
        FsInfo info;
        auto const existing = nearestExisting(path);
        struct statfs sfs
        {};
        if (::statfs(existing.c_str(), &sfs) != 0)
        {
            return info;
        }
        std::error_code ec;
        info.exists = std::filesystem::exists(path, ec);
        auto const magic = static_cast<long>(sfs.f_type);
        info.typeName = fsName(magic);
        info.tmpfs = magic == tmpfsMagic || magic == ramfsMagic;
        info.overlay = magic == overlayMagic || magic == fuseMagic;
        info.totalBytes = static_cast<std::uint64_t>(sfs.f_blocks) * static_cast<std::uint64_t>(sfs.f_bsize);
        info.freeBytes = static_cast<std::uint64_t>(sfs.f_bavail) * static_cast<std::uint64_t>(sfs.f_bsize);
        if (auto const dev = deviceOf(existing))
        {
            info.deviceId = *dev;
        }
        return info;
    }

    std::optional<std::uint64_t> deviceOf(std::string const& path)
    {
        struct stat st
        {};
        if (::stat(path.c_str(), &st) != 0)
        {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(st.st_dev);
    }
}
