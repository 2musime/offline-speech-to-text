#pragma once

// The interface's state machine, kept free of widgets so the control table can
// be exercised directly.

enum class UiState {
    Ready,
    LoadingModel,
    Recording,
    Stopping,
    Processing,
    Completed,
    Error
};

// Which controls a state allows. Every enabled/disabled decision comes from here.
struct ControlStates {
    bool start = false;
    bool stop = false;
    // Abandoning work in progress. A ten minute recording can take minutes to
    // transcribe, and a user must never be left with no way out of a wait.
    bool cancel = false;
    bool model = false;
    bool duration = false;
    bool device = false;
    bool keep_audio = false;
    bool delete_recordings = false;
    bool save = false;
    bool copy = false;
    // Browsing saved transcripts. Disabled while a recording is in progress:
    // the transcript pane shows live output, and swapping it mid-recording
    // would hide the thing the user is watching.
    bool history = false;
    bool progress_indeterminate = false;
};

inline const char* ui_state_name(UiState state) {
    switch (state) {
        case UiState::Ready: return "Ready";
        case UiState::LoadingModel: return "LoadingModel";
        case UiState::Recording: return "Recording";
        case UiState::Stopping: return "Stopping";
        case UiState::Processing: return "Processing";
        case UiState::Completed: return "Completed";
        case UiState::Error: return "Error";
    }
    return "Unknown";
}

// Idle means no worker is doing anything we must wait for.
inline bool ui_state_is_idle(UiState state) {
    return state == UiState::Ready || state == UiState::Completed || state == UiState::Error;
}

// True while a worker is running and closing would discard a recording.
inline bool ui_state_is_busy(UiState state) {
    return state == UiState::LoadingModel || state == UiState::Recording ||
        state == UiState::Stopping || state == UiState::Processing;
}

// A recording may only begin from an idle state, which is what stops a second
// click, a stray shortcut, or a timer from launching a concurrent worker.
inline bool ui_state_may_start(UiState state) {
    return ui_state_is_idle(state);
}

inline ControlStates controls_for(UiState state, bool devices_ready, bool has_text) {
    const bool idle = ui_state_is_idle(state);

    ControlStates controls;
    controls.start = idle;
    controls.stop = state == UiState::Recording;
    controls.cancel = state == UiState::LoadingModel || state == UiState::Stopping ||
        state == UiState::Processing;
    // Changing these mid-run would not affect the worker already running.
    controls.model = idle;
    controls.duration = idle;
    controls.device = idle && devices_ready;
    controls.keep_audio = idle;
    // Deleting while the worker holds the directory open would race it.
    controls.delete_recordings = idle;
    controls.history = idle;
    controls.save = state == UiState::Completed || (state == UiState::Error && has_text);
    controls.copy = controls.save;
    // Processing is deliberately absent: the worker reports how many chunks it
    // has finished, so that phase shows real progress rather than a bar that
    // moves without meaning.
    controls.progress_indeterminate = state == UiState::LoadingModel ||
        state == UiState::Stopping;
    return controls;
}
