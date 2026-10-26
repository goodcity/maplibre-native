#pragma once
#include "fence_diagnostic_core.hpp"
#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/sync_file.h>
#include <memory>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace fence_diagnostics {
class LinuxOperations final : public Operations {
public:
    std::uint64_t nowNs() override {
        timespec value{};
        if (clock_gettime(CLOCK_MONOTONIC, &value)) return 0;
        return std::uint64_t(value.tv_sec) * 1000000000ULL + value.tv_nsec;
    }
    Kind kind(int descriptor) override {
        char path[64], target[512];
        std::snprintf(path, sizeof(path), "/proc/self/fd/%d", descriptor);
        const auto count = readlink(path, target, sizeof(target)-1);
        if (count < 0) return errno == ENOENT || errno == EBADF ? Kind::Closed : Kind::Error;
        if (count == sizeof(target)-1) return Kind::Error;
        target[count] = 0;
        if (!std::strcmp(target, "anon_inode:[sync_file]") ||
            !std::strcmp(target, "anon_inode:sync_file")) return Kind::SyncFile;
        // Preserve visibility of the broader historical fence category without
        // assuming every matched device is a Linux sync_file.
        if (std::strstr(target, "sync_file") || std::strstr(target, "fence") ||
            !std::strcmp(target, "/dev/express_sync")) return Kind::LegacyFence;
        return Kind::Other;
    }
    int duplicate(int descriptor) override { return fcntl(descriptor, F_DUPFD_CLOEXEC, 0); }
    void closeDuplicate(int descriptor) override { close(descriptor); }
    Probe query(int descriptor) override {
        Probe out;
        sync_file_info info{};
        if (ioctl(descriptor, SYNC_IOC_FILE_INFO, &info)) {
            out.query = errno == ENOTTY ? Query::Unsupported :
                errno == EACCES || errno == EPERM ? Query::Denied : Query::Error;
            return out;
        }
        out.query = Query::Ok;
        out.status = info.status;
        out.components = info.num_fences;
        constexpr unsigned limit = 64;
        if (info.num_fences > limit) { out.detailsTooLarge = true; return out; }
        if (!info.num_fences) { out.detailsRead = true; return out; }
        std::array<sync_fence_info, limit> children{};
        sync_file_info details{};
        details.num_fences = info.num_fences;
        details.sync_fence_info = reinterpret_cast<std::uintptr_t>(children.data());
        if (ioctl(descriptor, SYNC_IOC_FILE_INFO, &details)) return out;
        if (details.num_fences > info.num_fences || details.num_fences > limit) return out;
        out.detailsRead = true;
        for (unsigned index = 0; index < details.num_fences; ++index) {
            const auto& child = children[index];
            if (child.status == 0) ++out.childActive;
            else if (child.status == 1) ++out.childSignaled;
            else if (child.status < 0) ++out.childError;
            else ++out.childInvalid;
            // Names returned by the kernel are intentionally never exported.
            if (child.timestamp_ns) {
                if (!out.timestamps || child.timestamp_ns < out.firstTimestamp)
                    out.firstTimestamp = child.timestamp_ns;
                if (child.timestamp_ns > out.lastTimestamp) out.lastTimestamp = child.timestamp_ns;
                ++out.timestamps;
            }
        }
        return out;
    }
};
inline Sample sample() {
    Sample out;
    LinuxOperations ops;
    out.beginNs = ops.nowNs();
    std::unique_ptr<DIR, decltype(&closedir)> ownedDirectory(opendir("/proc/self/fd"), &closedir);
    DIR* directory = ownedDirectory.get();
    if (directory) {
        out.directoryAvailable = true;
        const int ownDirectory = dirfd(directory);
        std::vector<int> descriptors;
        constexpr unsigned maximumDescriptors = 8192;
        // Enumerate before creating diagnostic duplicates so they cannot appear
        // in our own directory walk. Still a time interval, not atomic snapshot.
        while (true) {
            errno = 0;
            const auto* entry = readdir(directory);
            if (!entry) { out.directoryComplete = errno == 0; break; }
            if (entry->d_name[0] == '.') continue;
            char* end = nullptr;
            errno = 0;
            const long descriptor = std::strtol(entry->d_name, &end, 10);
            if (errno || !end || *end || descriptor < 0 || descriptor > INT_MAX) {
                ++out.linkErrors;
                continue;
            }
            if (descriptor != ownDirectory) {
                if (descriptors.size() == maximumDescriptors) {
                    out.directoryComplete = false;
                    break;
                }
                descriptors.push_back(static_cast<int>(descriptor));
            }
        }
        ownedDirectory.reset();
        for (const auto descriptor : descriptors) observe(descriptor, ops, out);
    }
    out.endNs = ops.nowNs();
    return out;
}
} // namespace fence_diagnostics
