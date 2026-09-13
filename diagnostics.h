#pragma once

// Error categories and the structured reporting protocol shared by the worker.

#include <iostream>
#include <string>

enum class ErrorCategory {
    Microphone,
    Model,
    Recording,
    Transcription,
    FileSaving,
    Worker
};

inline const char* error_category_name(ErrorCategory category) {
    switch (category) {
        case ErrorCategory::Microphone: return "MICROPHONE";
        case ErrorCategory::Model: return "MODEL";
        case ErrorCategory::Recording: return "RECORDING";
        case ErrorCategory::Transcription: return "TRANSCRIPTION";
        case ErrorCategory::FileSaving: return "FILE_SAVING";
        case ErrorCategory::Worker: return "WORKER";
    }
    return "WORKER";
}

// Failures that end the current recording attempt.
inline void report_error(ErrorCategory category, const std::string& message) {
    std::cerr << "ERROR|" << error_category_name(category) << '|' << message << std::endl;
}

// Whisper prefixes every segment with a space, so a joined transcript begins
// with one. Trimmed once, at the point the text is finished, rather than in each
// of the places that go on to save, print or display it.
inline std::string trimmed(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// Advisories that do not stop processing.
inline void report_warning(ErrorCategory category, const std::string& message) {
    std::cerr << "WARN|" << error_category_name(category) << '|' << message << std::endl;
}

inline std::string as_single_line(const std::string& text) {
    std::string flattened;
    flattened.reserve(text.size());
    bool pending_space = false;
    for (const char character : text) {
        if (character == '\n' || character == '\r' || character == '\t') {
            pending_space = !flattened.empty();
            continue;
        }
        if (pending_space) {
            flattened.push_back(' ');
            pending_space = false;
        }
        flattened.push_back(character);
    }
    return flattened;
}
