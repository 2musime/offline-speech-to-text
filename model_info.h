#pragma once

// Whisper model discovery, header inspection, and metadata.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "diagnostics.h"
#include "file_storage.h"

// Models must resolve inside an approved root; symbolic links are followed but
// the destination still has to land inside one of those roots.
inline bool resolve_model_path(
    const std::string& requested,
    const std::vector<fs::path>& roots,
    fs::path& resolved,
    std::string& reason) {
    std::error_code error;
    const fs::path canonical = fs::canonical(requested, error);
    if (error) {
        reason = "Model file does not exist or is not readable: " + requested;
        return false;
    }
    if (!fs::is_regular_file(canonical, error) || error) {
        reason = "Model path is not a regular file: " + requested;
        return false;
    }

    for (const fs::path& root : roots) {
        if (fs::is_directory(root, error) && path_is_within(canonical, root)) {
            resolved = canonical;
            return true;
        }
    }

    std::string allowed;
    for (const fs::path& root : roots) {
        allowed += (allowed.empty() ? "" : ", ") + root.string();
    }
    reason = "Model is outside the approved directories (" + allowed +
        "): " + requested + ". Pass --model-dir to approve another directory.";
    return false;
}

struct ModelMetadata {
    fs::path path;
    std::uintmax_t size_bytes = 0;
    std::string variant = "unknown";
    bool multilingual = false;
    std::int32_t audio_layers = 0;
    std::int32_t vocabulary_size = 0;
    std::int32_t mel_bands = 0;
    std::int32_t quantisation = 0;
};

// A ggml Whisper model begins with a magic number followed by eleven 32-bit
// hyper-parameters. Field order and the layer-count mapping were taken from
// third_party/whisper.cpp (src/whisper.cpp, whisper_model_load).
inline bool inspect_model(const fs::path& path, ModelMetadata& metadata, std::string& reason) {
    constexpr std::uint32_t ggml_magic = 0x67676d6c;
    constexpr std::size_t header_size = 48;
    // The smallest published Whisper model is tens of megabytes; anything under
    // a megabyte is truncated rather than merely unusual.
    constexpr std::uintmax_t smallest_plausible_model = 1024 * 1024;

    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error) {
        reason = "Could not determine the size of the model: " + path.string();
        return false;
    }
    if (size < smallest_plausible_model) {
        reason = "Model file is far too small to be a Whisper model and is probably "
            "truncated or incomplete: " + path.string();
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        reason = "Model file exists but could not be opened for reading: " + path.string();
        return false;
    }

    char header[header_size];
    if (!file.read(header, header_size)) {
        reason = "Could not read the model header: " + path.string();
        return false;
    }

    const auto field = [&header](std::size_t index) {
        std::uint32_t value = 0;
        std::memcpy(&value, header + index * sizeof(value), sizeof(value));
        return static_cast<std::int32_t>(value);
    };

    if (static_cast<std::uint32_t>(field(0)) != ggml_magic) {
        reason = "Not a ggml Whisper model; the file does not start with the ggml "
            "magic number: " + path.string();
        return false;
    }

    metadata.path = path;
    metadata.size_bytes = size;
    metadata.vocabulary_size = field(1);
    metadata.audio_layers = field(5);
    metadata.mel_bands = field(10);
    metadata.quantisation = field(11);

    if (metadata.vocabulary_size < 1000 || metadata.vocabulary_size > 1000000) {
        reason = "Model header is corrupted; the vocabulary size (" +
            std::to_string(metadata.vocabulary_size) + ") is implausible: " + path.string();
        return false;
    }
    if (metadata.mel_bands != 80 && metadata.mel_bands != 128) {
        reason = "Unsupported mel band count (" + std::to_string(metadata.mel_bands) +
            "); this build expects 80 or 128: " + path.string();
        return false;
    }

    switch (metadata.audio_layers) {
        case 4: metadata.variant = "tiny"; break;
        case 6: metadata.variant = "base"; break;
        case 12: metadata.variant = "small"; break;
        case 24: metadata.variant = "medium"; break;
        case 32: metadata.variant = "large"; break;
        default:
            reason = "Unrecognised Whisper model geometry (" +
                std::to_string(metadata.audio_layers) +
                " audio layers); the file may be corrupted or built for a different "
                "runtime: " + path.string();
            return false;
    }

    metadata.multilingual = metadata.vocabulary_size >= 51865;
    return true;
}

// The file is only ever opened for reading. These checks flag a model whose
// name or permissions disagree with what the header actually says.
inline void report_model_observations(const ModelMetadata& metadata) {
    std::error_code error;
    const fs::perms permissions = fs::status(metadata.path, error).permissions();
    constexpr fs::perms executable =
        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
    if (!error && (permissions & executable) != fs::perms::none) {
        report_warning(ErrorCategory::Model,
            "Model file is marked executable. It is only ever read as data, never run: " +
            metadata.path.string());
    }

    const std::string filename = metadata.path.filename().string();
    if (filename.find(metadata.variant) == std::string::npos) {
        report_warning(ErrorCategory::Model,
            "File name does not mention the model type in its header (" +
            metadata.variant + "): " + filename);
    }

    const bool name_claims_english = filename.find(".en.") != std::string::npos;
    if (name_claims_english && metadata.multilingual) {
        report_warning(ErrorCategory::Model,
            "File name claims an English-only model but the header is multilingual: " + filename);
    }
    if (!name_claims_english && !metadata.multilingual) {
        report_warning(ErrorCategory::Model,
            "Header reports an English-only model but the file name does not say so: " + filename);
    }
}

inline void report_model(const ModelMetadata& metadata) {
    std::cout << "MODEL|" << metadata.variant << '|'
              << (metadata.multilingual ? "multilingual" : "english") << '|'
              << metadata.size_bytes << '|' << metadata.path.string() << std::endl;
    std::cout << "Model: " << metadata.variant << ", "
              << (metadata.multilingual ? "multilingual" : "English-only") << ", "
              << metadata.size_bytes / (1024.0 * 1024.0) << " MB, "
              << metadata.audio_layers << " audio layers, "
              << metadata.mel_bands << " mel bands" << std::endl;
}
