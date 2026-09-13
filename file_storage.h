#pragma once

// Application data directory, path validation, and atomic file writes.

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
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

#if defined(_WIN32)
#include <cwctype>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// Files are written only inside a directory the application owns.
inline fs::path application_data_directory() {
#if defined(_WIN32)
    // Windows has no XDG convention. LOCALAPPDATA is the per-user directory for
    // application data that should not follow the user onto another machine,
    // which is what recordings are: large, private, and machine-local.
    const wchar_t* local_app_data = ::_wgetenv(L"LOCALAPPDATA");
    if (local_app_data != nullptr && local_app_data[0] != L'\0') {
        return fs::path(local_app_data) / L"audio-to-text";
    }
    return fs::current_path() / L".audio-to-text";
#else
    const char* data_home = std::getenv("XDG_DATA_HOME");
    if (data_home != nullptr && data_home[0] == '/') {
        return fs::path(data_home) / "audio-to-text";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] == '/') {
        return fs::path(home) / ".local" / "share" / "audio-to-text";
    }
    return fs::current_path() / ".audio-to-text";
#endif
}

// The directory this executable was started from. argv[0] is not a substitute:
// it is whatever the caller passed and need not be a path at all. Empty when
// the platform cannot answer, so callers must check before using it.
inline fs::path executable_directory() {
#if defined(_WIN32)
    // The C runtime's own record of the path the process started from. Wide,
    // so a path the active code page cannot express survives intact.
    wchar_t* program = nullptr;
    if (::_get_wpgmptr(&program) == 0 && program != nullptr && program[0] != L'\0') {
        return fs::path(program).parent_path();
    }
#elif defined(__linux__)
    std::error_code error;
    const fs::path self = fs::read_symlink("/proc/self/exe", error);
    if (!error) {
        return self.parent_path();
    }
#endif
    return {};
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
#if !defined(_WIN32)
    if (::chmod(directory.c_str(), S_IRWXU) != 0) {
        return false;
    }
#endif
    // Windows has no equivalent one-call restriction. LOCALAPPDATA is already
    // covered by an inherited per-user ACL, so a directory created beneath it
    // is private without further work; see docs/PRIVACY.md.
    return true;
}

// Windows filesystems are case insensitive, so two components differing only
// in case name the same directory and must compare equal; comparing them
// literally would reject a path that genuinely is inside the root. Folding
// cannot merge two distinct directories, because Windows does not let two names
// differ by case alone.
inline bool path_components_match(const fs::path& left, const fs::path& right) {
#if defined(_WIN32)
    const std::wstring& first = left.native();
    const std::wstring& second = right.native();
    if (first.size() != second.size()) {
        return false;
    }
    for (std::size_t index = 0; index < first.size(); ++index) {
        if (std::towlower(first[index]) != std::towlower(second[index])) {
            return false;
        }
    }
    return true;
#else
    return left == right;
#endif
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
        if (candidate_part == resolved_candidate.end() ||
            !path_components_match(*candidate_part, *root_part)) {
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
#if defined(__linux__)
    localtime_r(&seconds, &parts);
#else
    // The Windows CRT holds localtime's result in thread-local storage, so the
    // pointer it returns is not shared between threads the way POSIX's is.
    parts = *std::localtime(&seconds);
#endif

    std::random_device device;
    std::ostringstream stream;
    stream << std::put_time(&parts, "%Y%m%d-%H%M%S") << '-'
           << std::hex << std::setw(4) << std::setfill('0') << (device() & 0xffff);
    return stream.str();
}

inline unsigned long long current_process_id() {
#if defined(_WIN32)
    return static_cast<unsigned long long>(::_getpid());
#else
    return static_cast<unsigned long long>(::getpid());
#endif
}

// The suffix is appended to the path, not to a narrow rendering of it.
// fs::path::string() re-encodes, and on Windows it drops whatever the active
// code page cannot represent, which would point the temporary somewhere other
// than beside its target.
inline fs::path temporary_companion(const fs::path& target) {
    fs::path companion = target;
    companion += ".tmp-" + std::to_string(current_process_id());
    return companion;
}

// Writes through a temporary file and renames, so readers never observe a
// partial file. Owner-only permissions; refuses to follow a symbolic link.
class AtomicFile {
public:
    explicit AtomicFile(fs::path target)
        : target_(std::move(target)),
          temporary_(temporary_companion(target_)) {}

    ~AtomicFile() {
        if (descriptor_ >= 0) {
            close_descriptor(descriptor_);
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
#if defined(_WIN32)
        // _O_EXCL refuses any existing name, a reparse point included, which is
        // what O_NOFOLLOW buys on POSIX. _O_BINARY stops the CRT rewriting a
        // 0x0A byte of audio data into a line ending.
        descriptor_ = ::_wopen(
            temporary_.c_str(),
            _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
            _S_IREAD | _S_IWRITE);
#else
        // O_EXCL with O_CREAT never follows a link at the final component.
        descriptor_ = ::open(
            temporary_.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
            S_IRUSR | S_IWUSR);
#endif
        return descriptor_ >= 0;
    }

    bool write(const void* data, std::size_t size) {
        const char* bytes = static_cast<const char*>(data);
        std::size_t written = 0;
        while (written < size) {
#if defined(_WIN32)
            // _write counts in unsigned int, so a write larger than that has to
            // go round the loop more than once.
            const unsigned int batch = static_cast<unsigned int>(
                std::min<std::size_t>(size - written, 0x7fffffffu));
            const int count = ::_write(descriptor_, bytes + written, batch);
#else
            const ssize_t count = ::write(descriptor_, bytes + written, size - written);
#endif
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
        if (descriptor_ < 0 || flush_descriptor(descriptor_) != 0) {
            return false;
        }
        const int closed = close_descriptor(descriptor_);
        descriptor_ = -1;
        if (closed != 0) {
            return false;
        }
#if defined(_WIN32)
        // Windows rename fails outright when the target exists, so replacing it
        // has to be explicit. std::filesystem::rename replaces, and within one
        // volume -- which a sibling temporary always is -- it is atomic. There
        // is no directory handle to flush afterwards; Windows has no equivalent
        // of fsync on a directory.
        std::error_code error;
        fs::rename(temporary_, target_, error);
        if (error) {
            return false;
        }
#else
        if (::rename(temporary_.c_str(), target_.c_str()) != 0) {
            return false;
        }
        sync_parent_directory();
#endif
        committed_ = true;
        return true;
    }

private:
    static int flush_descriptor(int descriptor) {
#if defined(_WIN32)
        return ::_commit(descriptor);
#else
        return ::fsync(descriptor);
#endif
    }

    static int close_descriptor(int descriptor) {
#if defined(_WIN32)
        return ::_close(descriptor);
#else
        return ::close(descriptor);
#endif
    }

#if !defined(_WIN32)
    // Makes the rename itself durable, not just the file contents.
    void sync_parent_directory() const {
        const int directory = ::open(
            target_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory >= 0) {
            ::fsync(directory);
            ::close(directory);
        }
    }
#endif

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
