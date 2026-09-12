#pragma once

// Application data directory, path validation, and atomic file writes.

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include "diagnostics.h"

#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// Files are written only inside a directory the application owns.
inline fs::path application_data_directory() {
    const char* data_home = std::getenv("XDG_DATA_HOME");
    if (data_home != nullptr && data_home[0] == '/') {
        return fs::path(data_home) / "audio-to-text";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] == '/') {
        return fs::path(home) / ".local" / "share" / "audio-to-text";
    }
    return fs::current_path() / ".audio-to-text";
}

// Creates the directory if needed and restricts it to the owner.
inline bool ensure_private_directory(const fs::path& directory) {
    std::error_code error;
    fs::create_directories(directory, error);
    if (error && !fs::is_directory(directory)) {
        return false;
    }
    if (!fs::is_directory(directory)) {
        return false;
    }
#ifdef __linux__
    if (::chmod(directory.c_str(), S_IRWXU) != 0) {
        return false;
    }
#endif
    return true;
}

// True when candidate resolves to root itself or something beneath it.
inline bool path_is_within(const fs::path& candidate, const fs::path& root) {
    std::error_code error;
    const fs::path resolved_candidate = fs::weakly_canonical(candidate, error);
    if (error) {
        return false;
    }
    const fs::path resolved_root = fs::weakly_canonical(root, error);
    if (error) {
        return false;
    }

    auto candidate_part = resolved_candidate.begin();
    auto root_part = resolved_root.begin();
    for (; root_part != resolved_root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == resolved_candidate.end() || *candidate_part != *root_part) {
            return false;
        }
    }
    return true;
}

// Groups the artefacts of one recording: 20260912-154233-7f3a.
inline std::string session_stamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm parts{};
#ifdef __linux__
    localtime_r(&seconds, &parts);
#else
    parts = *std::localtime(&seconds);
#endif

    std::random_device device;
    std::ostringstream stream;
    stream << std::put_time(&parts, "%Y%m%d-%H%M%S") << '-'
           << std::hex << std::setw(4) << std::setfill('0') << (device() & 0xffff);
    return stream.str();
}

// Writes through a temporary file and renames, so readers never observe a
// partial file. Owner-only permissions; refuses to follow a symbolic link.
class AtomicFile {
public:
    explicit AtomicFile(fs::path target)
        : target_(std::move(target)),
          temporary_(target_.string() + ".tmp-" + std::to_string(::getpid())) {}

    ~AtomicFile() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
        if (!committed_) {
            std::error_code error;
            fs::remove(temporary_, error);
        }
    }

    AtomicFile(const AtomicFile&) = delete;
    AtomicFile& operator=(const AtomicFile&) = delete;
    AtomicFile(AtomicFile&&) = delete;
    AtomicFile& operator=(AtomicFile&&) = delete;

    bool open() {
        std::error_code error;
        fs::remove(temporary_, error);
        // O_EXCL with O_CREAT never follows a link at the final component.
        descriptor_ = ::open(
            temporary_.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
            S_IRUSR | S_IWUSR);
        return descriptor_ >= 0;
    }

    bool write(const void* data, std::size_t size) {
        const char* bytes = static_cast<const char*>(data);
        std::size_t written = 0;
        while (written < size) {
            const ssize_t count = ::write(descriptor_, bytes + written, size - written);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            written += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool commit() {
        if (descriptor_ < 0 || ::fsync(descriptor_) != 0) {
            return false;
        }
        const int closed = ::close(descriptor_);
        descriptor_ = -1;
        if (closed != 0 || ::rename(temporary_.c_str(), target_.c_str()) != 0) {
            return false;
        }
        sync_parent_directory();
        committed_ = true;
        return true;
    }

private:
    // Makes the rename itself durable, not just the file contents.
    void sync_parent_directory() const {
        const int directory = ::open(
            target_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory >= 0) {
            ::fsync(directory);
            ::close(directory);
        }
    }

    fs::path target_;
    fs::path temporary_;
    int descriptor_ = -1;
    bool committed_ = false;
};

// Defence in depth: every output must stay inside the data directory, and an
// existing symbolic link at the destination is never overwritten.
inline bool output_path_is_safe(const fs::path& path, const fs::path& root) {
    if (!path_is_within(path, root)) {
        report_error(ErrorCategory::FileSaving,
            "Refusing to write outside the data directory: " + path.string());
        return false;
    }
    std::error_code error;
    if (fs::is_symlink(fs::symlink_status(path, error))) {
        report_error(ErrorCategory::FileSaving,
            "Refusing to overwrite a symbolic link: " + path.string());
        return false;
    }
    return true;
}

// Announces a produced artefact by kind and path.
inline void report_saved(const char* kind, const fs::path& path) {
    std::cout << "SAVED|" << kind << '|' << path.string() << std::endl;
}
