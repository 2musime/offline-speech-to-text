#pragma once

// Reading back what the application has already produced: the saved
// transcripts, and the audio each was made from.
//
// Every path that leaves this header has been checked against the data
// directory. Reading is as much a place to leak files from as writing is, so
// the confinement rules are the same ones `file_storage.h` applies on the way
// out, reused rather than restated.

#include "file_storage.h"
#include "wav_io.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

// A stamp names one recording session. Every artefact of that session shares
// it, which is what links a transcript to the audio it came from.
struct SessionStamp {
    std::tm when{};
    std::string text;

    // Sorts newest first without needing a locale or a time zone: the stamp is
    // fixed-width and already in most-significant-first order.
    bool operator<(const SessionStamp& other) const { return text < other.text; }
};

struct TranscriptEntry {
    fs::path path;
    SessionStamp stamp;
    std::uintmax_t size_bytes = 0;
    std::string preview;
};

// Which artefacts of a session are still on disk. Audio can be deleted, or
// never written at all when retention is off, so a transcript having no audio
// is normal rather than an error.
struct CompanionAudio {
    fs::path recording;
    fs::path cleaned;
    fs::path speech;
    bool has_recording = false;
    bool has_cleaned = false;
    bool has_speech = false;

    bool any() const { return has_recording || has_cleaned || has_speech; }
};

inline constexpr std::size_t transcript_preview_length = 120;
inline constexpr const char* transcript_suffix = "-transcription.txt";

// "20260912-222240-04c4": eight date digits, six time digits, four hex.
// Rejected explicitly rather than guessed at, so a file the application did not
// write is never presented as though it had a meaningful date.
inline bool parse_session_stamp(const std::string& text, SessionStamp& stamp) {
    constexpr std::size_t expected_length = 20;
    if (text.size() != expected_length || text[8] != '-' || text[15] != '-') {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 15) {
            continue;
        }
        const unsigned char character = static_cast<unsigned char>(text[i]);
        const bool valid = i < 15 ? std::isdigit(character) != 0
                                  : std::isxdigit(character) != 0;
        if (!valid) {
            return false;
        }
    }

    const auto number = [&text](std::size_t offset, std::size_t length) {
        return std::stoi(text.substr(offset, length));
    };

    std::tm when{};
    when.tm_year = number(0, 4) - 1900;
    when.tm_mon = number(4, 2) - 1;
    when.tm_mday = number(6, 2);
    when.tm_hour = number(9, 2);
    when.tm_min = number(11, 2);
    when.tm_sec = number(13, 2);
    when.tm_isdst = -1;

    // A well-formed shape can still be an impossible date.
    if (when.tm_mon < 0 || when.tm_mon > 11 || when.tm_mday < 1 || when.tm_mday > 31 ||
        when.tm_hour > 23 || when.tm_min > 59 || when.tm_sec > 60) {
        return false;
    }

    stamp.when = when;
    stamp.text = text;
    return true;
}

// True when `path` is a regular file inside `root` and is not a symbolic link.
// A link is refused rather than followed: its destination is outside anything
// this application controls.
inline bool readable_inside(const fs::path& path, const fs::path& root) {
    std::error_code error;
    if (fs::is_symlink(fs::symlink_status(path, error)) || error) {
        return false;
    }
    if (!fs::is_regular_file(path, error) || error) {
        return false;
    }
    return path_is_within(path, root);
}

// First line, trimmed and capped. Whisper writes the whole transcript on one
// line, so this is a snippet rather than a paragraph.
inline std::string transcript_preview(const fs::path& path) {
    std::ifstream file(path);
    if (!file) {
        return {};
    }

    std::string line;
    std::getline(file, line);
    const std::size_t begin = line.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = line.find_last_not_of(" \t\r\n");
    line = line.substr(begin, end - begin + 1);

    if (line.size() > transcript_preview_length) {
        line.resize(transcript_preview_length);
        line += "...";
    }
    return line;
}

// Saved transcripts, newest first. Anything that does not parse, is not a
// regular file, or sits outside the data directory is skipped: one stray file
// must not cost the user the rest of their history.
inline std::vector<TranscriptEntry> list_transcripts(const fs::path& data_directory) {
    const fs::path directory = data_directory / "transcripts";
    std::vector<TranscriptEntry> entries;

    std::error_code error;
    if (!fs::is_directory(directory, error) || error) {
        return entries;
    }

    for (const fs::directory_entry& item : fs::directory_iterator(directory, error)) {
        if (error) {
            break;
        }
        if (!readable_inside(item.path(), data_directory)) {
            continue;
        }

        const std::string name = item.path().filename().string();
        const std::string suffix = transcript_suffix;
        if (name.size() <= suffix.size() ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }

        TranscriptEntry entry;
        if (!parse_session_stamp(name.substr(0, name.size() - suffix.size()), entry.stamp)) {
            continue;
        }

        std::error_code size_error;
        entry.size_bytes = fs::file_size(item.path(), size_error);
        if (size_error) {
            entry.size_bytes = 0;
        }
        entry.path = item.path();
        entry.preview = transcript_preview(item.path());
        entries.push_back(entry);
    }

    // Newest first: the transcript someone wants is almost always the last one.
    std::sort(entries.begin(), entries.end(),
              [](const TranscriptEntry& a, const TranscriptEntry& b) {
                  return b.stamp < a.stamp;
              });
    return entries;
}

// Reads a saved transcript. Fails rather than reads when the path escapes the
// data directory, is a link, or is not a regular file.
inline bool read_transcript(
    const fs::path& path,
    const fs::path& data_directory,
    std::string& text,
    std::string& reason) {
    if (!readable_inside(path, data_directory)) {
        reason = "Refusing to read a file outside the application's own directory: " +
            path.string();
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        reason = "Could not open the transcript: " + path.string();
        return false;
    }

    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (file.bad()) {
        reason = "Could not read the transcript: " + path.string();
        return false;
    }
    return true;
}

// Deletes one transcript, under the same rules as deleting all of them.
inline bool delete_transcript(
    const fs::path& path,
    const fs::path& data_directory,
    std::string& reason) {
    if (!readable_inside(path, data_directory)) {
        reason = "Refusing to delete a file outside the application's own directory: " +
            path.string();
        return false;
    }

    std::error_code error;
    if (!fs::remove(path, error) || error) {
        reason = "Could not delete " + path.string() + ".";
        return false;
    }
    return true;
}

// The audio a transcript was made from. Missing audio is reported, not treated
// as a failure: retention can be off, or the recordings already deleted.
inline CompanionAudio companion_audio(
    const SessionStamp& stamp,
    const fs::path& data_directory) {
    const fs::path directory = data_directory / "recordings";

    CompanionAudio audio;
    audio.recording = directory / (stamp.text + "-recording.wav");
    audio.cleaned = directory / (stamp.text + "-cleaned.wav");
    audio.speech = directory / (stamp.text + "-speech.wav");
    audio.has_recording = readable_inside(audio.recording, data_directory);
    audio.has_cleaned = readable_inside(audio.cleaned, data_directory);
    audio.has_speech = readable_inside(audio.speech, data_directory);
    return audio;
}

// A recording a user can play back. This is the raw capture, not the cleaned
// copy and not the extracted speech: playback should give back what was said,
// exactly as the microphone heard it. The processed versions exist for Whisper.
struct RecordingEntry {
    fs::path path;
    SessionStamp stamp;
    std::uintmax_t size_bytes = 0;
    double seconds = 0.0;
    bool has_transcript = false;
};

inline constexpr const char* recording_suffix = "-recording.wav";

// Raw recordings, newest first. Same rules as the transcripts: anything
// malformed, linked, or outside the data directory is skipped rather than
// allowed to break the listing.
inline std::vector<RecordingEntry> list_recordings(const fs::path& data_directory) {
    const fs::path directory = data_directory / "recordings";
    std::vector<RecordingEntry> entries;

    std::error_code error;
    if (!fs::is_directory(directory, error) || error) {
        return entries;
    }

    const std::string suffix = recording_suffix;
    for (const fs::directory_entry& item : fs::directory_iterator(directory, error)) {
        if (error) {
            break;
        }
        if (!readable_inside(item.path(), data_directory)) {
            continue;
        }

        const std::string name = item.path().filename().string();
        if (name.size() <= suffix.size() ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }

        RecordingEntry entry;
        if (!parse_session_stamp(name.substr(0, name.size() - suffix.size()), entry.stamp)) {
            continue;
        }

        std::error_code size_error;
        entry.size_bytes = fs::file_size(item.path(), size_error);
        if (size_error) {
            entry.size_bytes = 0;
        }
        entry.path = item.path();
        if (!wav_duration_seconds(item.path(), entry.seconds)) {
            entry.seconds = 0.0;
        }
        const fs::path transcript =
            data_directory / "transcripts" / (entry.stamp.text + transcript_suffix);
        entry.has_transcript = readable_inside(transcript, data_directory);
        entries.push_back(entry);
    }

    std::sort(entries.begin(), entries.end(),
              [](const RecordingEntry& a, const RecordingEntry& b) {
                  return b.stamp < a.stamp;
              });
    return entries;
}

// Deletes one recording and the processed copies made from it. The transcript
// is deliberately left: text is small, and losing it with the audio would be a
// surprise.
inline bool delete_recording(
    const fs::path& path,
    const SessionStamp& stamp,
    const fs::path& data_directory,
    std::string& reason) {
    if (!readable_inside(path, data_directory)) {
        reason = "Refusing to delete a file outside the application's own directory: " +
            path.string();
        return false;
    }

    const fs::path directory = data_directory / "recordings";
    bool removed_any = false;
    for (const char* kind : {"-recording.wav", "-cleaned.wav", "-speech.wav"}) {
        const fs::path candidate = directory / (stamp.text + kind);
        if (!readable_inside(candidate, data_directory)) {
            continue;
        }
        std::error_code error;
        if (fs::remove(candidate, error) && !error) {
            removed_any = true;
        }
    }
    if (!removed_any) {
        reason = "Could not delete the recording: " + path.string();
    }
    return removed_any;
}
