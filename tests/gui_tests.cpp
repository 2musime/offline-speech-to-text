#include "test_harness.h"

#include "partial_text.h"
#include "ui_state.h"

#include <QCoreApplication>

namespace {

QString merge(const QStringList& windows, int maximum_words = 3000) {
    QStringList accumulated;
    bool trimmed = false;
    for (const QString& window : windows) {
        partial_text::append_with_overlap(accumulated, window, 40, maximum_words, trimmed);
    }
    return accumulated.join(' ');
}

std::string text_of(const QString& value) { return value.toStdString(); }

void test_overlap_merging() {
    harness::begin("Streaming partial overlap");

    CHECK_TEXT("consecutive windows merge on their overlap",
        text_of(merge({"the quick brown fox jumps",
                       "brown fox jumps over the lazy",
                       "over the lazy dog while sarah"})),
        "the quick brown fox jumps over the lazy dog while sarah");

    CHECK_TEXT("punctuation and case differences still match",
        text_of(merge({"The quick brown fox.", "brown Fox jumps over"})),
        "The quick brown fox. jumps over");

    CHECK_TEXT("unrelated windows are concatenated",
        text_of(merge({"alpha beta", "gamma delta"})), "alpha beta gamma delta");

    CHECK_TEXT("a repeated window adds nothing",
        text_of(merge({"one two three", "one two three"})), "one two three");

    CHECK_TEXT("a single shared word is enough",
        text_of(merge({"hello world", "world again"})), "hello world again");

    CHECK_TEXT("an empty window is ignored",
        text_of(merge({"alpha beta", "   ", "beta gamma"})), "alpha beta gamma");

    // The longest-first search must not mistake an inner repeat for the overlap.
    CHECK_TEXT("a repeated word does not swallow text",
        text_of(merge({"very very good", "good morning"})), "very very good morning");

    CHECK_TEXT("no overlap when the tail differs",
        text_of(merge({"one two", "three four"})), "one two three four");
}

void test_partial_growth_is_bounded() {
    harness::begin("Partial text growth");

    QStringList accumulated;
    bool trimmed = false;
    for (int i = 0; i < 500; ++i) {
        partial_text::append_with_overlap(
            accumulated, QString("word%1 filler").arg(i), 40, 100, trimmed);
    }
    CHECK("a long session stays within the cap", accumulated.size() <= 100);
    CHECK("trimming is reported", trimmed);

    QStringList short_run;
    bool short_trimmed = false;
    partial_text::append_with_overlap(short_run, "just a few words", 40, 100, short_trimmed);
    CHECK_FALSE("a short session is never trimmed", short_trimmed);
}

void test_state_transitions() {
    harness::begin("GUI state transitions");

    // Only an idle state may begin a recording: this is what prevents a second
    // click or a stray timer from launching a concurrent worker.
    CHECK("a recording may start when ready", ui_state_may_start(UiState::Ready));
    CHECK("a recording may start after completing", ui_state_may_start(UiState::Completed));
    CHECK("a recording may start after an error", ui_state_may_start(UiState::Error));
    CHECK_FALSE("a second recording cannot start while loading",
                ui_state_may_start(UiState::LoadingModel));
    CHECK_FALSE("a second recording cannot start while recording",
                ui_state_may_start(UiState::Recording));
    CHECK_FALSE("a second recording cannot start while stopping",
                ui_state_may_start(UiState::Stopping));
    CHECK_FALSE("a second recording cannot start while processing",
                ui_state_may_start(UiState::Processing));

    // Closing must warn only while work would be lost.
    CHECK_FALSE("closing when ready needs no warning", ui_state_is_busy(UiState::Ready));
    CHECK("closing while loading warns", ui_state_is_busy(UiState::LoadingModel));
    CHECK("closing while recording warns", ui_state_is_busy(UiState::Recording));
    CHECK("closing while stopping warns", ui_state_is_busy(UiState::Stopping));
    CHECK("closing while processing warns", ui_state_is_busy(UiState::Processing));
    CHECK_FALSE("closing when complete needs no warning", ui_state_is_busy(UiState::Completed));

    const ControlStates ready = controls_for(UiState::Ready, true, false);
    CHECK("start is offered when ready", ready.start);
    CHECK("saved transcripts can be browsed when ready", ready.history);
    CHECK_FALSE("stop is not offered when ready", ready.stop);
    CHECK("settings are editable when ready", ready.model && ready.duration && ready.device);
    CHECK_FALSE("nothing is saveable before a transcription", ready.save || ready.copy);

    const ControlStates loading = controls_for(UiState::LoadingModel, true, false);
    CHECK_FALSE("start is withdrawn while loading", loading.start);
    CHECK_FALSE("stop is not yet available while loading", loading.stop);
    CHECK("loading can be cancelled", loading.cancel);
    CHECK("progress is indeterminate while loading", loading.progress_indeterminate);

    const ControlStates recording = controls_for(UiState::Recording, true, false);
    CHECK_FALSE("start is withdrawn while recording", recording.start);
    CHECK("stop is offered while recording", recording.stop);
    CHECK_FALSE("the model cannot change mid-recording", recording.model);
    CHECK_FALSE("the microphone cannot change mid-recording", recording.device);
    CHECK_FALSE("retention cannot change mid-recording", recording.keep_audio);
    CHECK_FALSE("deletion cannot race the worker", recording.delete_recordings);
    // The transcript pane shows live output while recording; swapping it for a
    // saved transcript would hide the thing being watched.
    CHECK_FALSE("history cannot be browsed while recording", recording.history);
    CHECK_FALSE("progress is measured while recording", recording.progress_indeterminate);

    const ControlStates stopping = controls_for(UiState::Stopping, true, false);
    CHECK_FALSE("stop is withdrawn once pressed", stopping.stop);
    CHECK("stopping can be cancelled", stopping.cancel);
    CHECK("progress is indeterminate while stopping", stopping.progress_indeterminate);

    const ControlStates processing = controls_for(UiState::Processing, true, false);
    CHECK_FALSE("the model cannot change while processing", processing.model);
    CHECK_FALSE("deletion cannot race processing", processing.delete_recordings);
    // A ten minute recording takes minutes to transcribe; the user must be able
    // to abandon it, and must see real progress rather than a spinning bar.
    CHECK("a long transcription can be cancelled", processing.cancel);
    CHECK_FALSE("history cannot be browsed while transcribing", processing.history);
    CHECK_FALSE("progress is measured while transcribing", processing.progress_indeterminate);

    const ControlStates completed = controls_for(UiState::Completed, true, true);
    CHECK("start returns after completion", completed.start);
    CHECK("the transcript is saveable", completed.save && completed.copy);
    CHECK("settings are editable again", completed.model && completed.duration);
    CHECK("deletion is available again", completed.delete_recordings);
    CHECK("history can be browsed once finished", completed.history);

    // After a failure, whatever text arrived stays saveable; nothing does not.
    const ControlStates failed_with_text = controls_for(UiState::Error, true, true);
    CHECK("text that arrived before a failure stays saveable", failed_with_text.save);
    const ControlStates failed_empty = controls_for(UiState::Error, true, false);
    CHECK_FALSE("a failure with no text offers nothing to save", failed_empty.save);
    CHECK("recovery from an error is possible", failed_empty.start);
    CHECK("history can be browsed after a failure", failed_empty.history);

    // The microphone list is only offered once enumeration has answered.
    CHECK_FALSE("the microphone list waits for enumeration",
                controls_for(UiState::Ready, false, false).device);
    CHECK("other settings do not wait for enumeration",
          controls_for(UiState::Ready, false, false).model);

    // Invariants that must hold in every state.
    for (const UiState state : {UiState::Ready, UiState::LoadingModel, UiState::Recording,
                                UiState::Stopping, UiState::Processing, UiState::Completed,
                                UiState::Error}) {
        const ControlStates controls = controls_for(state, true, true);
        harness::record(!(controls.start && controls.stop),
                        "start and stop are mutually exclusive", ui_state_name(state));
        harness::record(!(controls.stop && controls.cancel),
                        "stop and cancel never both apply", ui_state_name(state));
        // The button is shared, so every busy state must offer one or the other:
        // no phase may leave the user with nothing to press.
        harness::record(!ui_state_is_busy(state) || controls.stop || controls.cancel,
                        "every busy state offers a way out", ui_state_name(state));
        harness::record(ui_state_is_busy(state) || !controls.cancel,
                        "cancel is offered only while busy", ui_state_name(state));
        // Browsing and recording are mutually exclusive by decision: the pane
        // cannot show live output and a saved transcript at once.
        harness::record(!(controls.history && ui_state_is_busy(state)),
                        "history and busy work never overlap", ui_state_name(state));
        harness::record(controls.history == controls.start,
                        "history follows the same idleness as starting",
                        ui_state_name(state));
    }
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    test_overlap_merging();
    test_partial_growth_is_bounded();
    test_state_transitions();
    return harness::summary();
}
