#include "partial_text.h"
#include "transcript_library.h"
#include "ui_state.h"
#include "version.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFile>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QTime>
#include <QTextCursor>
#include <QListWidget>
#include <QToolButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>

class AudioToTextWindow final : public QMainWindow {
public:
    AudioToTextWindow() {
        setWindowTitle(QString("Audio to Text %1").arg(AUDIO_TO_TEXT_VERSION));
        resize(900, 620);
        // Below this the settings row wraps and the transcript stops being usable.
        setMinimumSize(640, 420);

        auto* central = new QWidget(this);
        auto* layout = new QVBoxLayout(central);
        layout->setSpacing(12);

        build_menus();

        // ---- Settings: sized to their content, not stretched across the window.
        model_selector_ = new QComboBox(central);
        // base.en first, so it is the default: it transcribes roughly 3.5x
        // faster than small.en, which matters most on long recordings.
        model_selector_->addItem("Speed: base.en", "models/ggml-base.en.bin");
        model_selector_->addItem("Accuracy: small.en", "models/ggml-small.en.bin");
        model_selector_->setSizeAdjustPolicy(QComboBox::AdjustToContents);

        device_selector_ = new QComboBox(central);
        device_selector_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        device_selector_->setMinimumContentsLength(24);

        duration_selector_ = new QComboBox(central);
        duration_selector_->addItem("15 seconds", 15);
        duration_selector_->addItem("45 seconds", 45);
        duration_selector_->addItem("60 seconds", 60);
        duration_selector_->addItem("5 minutes", 300);
        duration_selector_->addItem("10 minutes", 600);
        duration_selector_->setSizeAdjustPolicy(QComboBox::AdjustToContents);

        keep_audio_ = new QCheckBox("Keep audio files", central);
        keep_audio_->setChecked(true);
        keep_audio_->setToolTip("When off, audio is transcribed and discarded; no WAV file is written.");

        auto* settings = new QHBoxLayout();
        settings->setSpacing(8);
        settings->addWidget(new QLabel("Model:", central));
        settings->addWidget(model_selector_);
        settings->addSpacing(16);
        settings->addWidget(new QLabel("Microphone:", central));
        settings->addWidget(device_selector_);
        settings->addSpacing(16);
        settings->addWidget(new QLabel("Limit:", central));
        settings->addWidget(duration_selector_);
        settings->addSpacing(16);
        settings->addWidget(keep_audio_);
        settings->addStretch();
        layout->addLayout(settings);

        // ---- Recording: button, clock, status and progress read as one thing.
        auto* record_panel = new QGroupBox(central);
        auto* record_layout = new QVBoxLayout(record_panel);
        record_layout->setSpacing(8);

        start_button_ = new QPushButton("Start Recording", record_panel);
        start_button_->setMinimumHeight(36);
        QFont start_font = start_button_->font();
        start_font.setBold(true);
        start_button_->setFont(start_font);
        start_button_->setAccessibleName("Start recording");
        stop_button_ = new QPushButton("Stop Recording", record_panel);
        stop_button_->setMinimumHeight(36);
        stop_button_->setEnabled(false);

        duration_label_ = new QLabel(record_panel);
        QFont clock_font = duration_label_->font();
        clock_font.setPointSize(clock_font.pointSize() + 4);
        duration_label_->setFont(clock_font);

        auto* record_row = new QHBoxLayout();
        record_row->addWidget(start_button_);
        record_row->addWidget(stop_button_);
        record_row->addSpacing(20);
        record_row->addWidget(duration_label_);
        record_row->addStretch();
        record_layout->addLayout(record_row);

        // A coloured dot reads at a glance; a word has to be found and parsed.
        indicator_ = new QLabel("\u25cf", record_panel);
        indicator_->setToolTip("Idle");
        status_label_ = new QLabel("Ready", record_panel);
        progress_ = new QProgressBar(record_panel);
        progress_->setTextVisible(false);
        progress_->setRange(0, 100);
        progress_->setValue(0);
        progress_->setMaximumHeight(8);
        progress_->hide();

        auto* status_row = new QHBoxLayout();
        status_row->addWidget(indicator_);
        status_row->addWidget(status_label_, 1);
        record_layout->addLayout(status_row);
        record_layout->addWidget(progress_);

        latency_label_ = new QLabel("", record_panel);
        latency_label_->setEnabled(false);
        record_layout->addWidget(latency_label_);
        layout->addWidget(record_panel);

        // ---- Transcript: the output, and the actions that apply to it.
        auto* transcript_header = new QHBoxLayout();
        transcript_title_ = new QLabel("Transcript", central);
        transcript_title_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
        transcript_header->addWidget(transcript_title_);
        transcript_header->addStretch();
        save_button_ = new QPushButton("Save", central);
        copy_button_ = new QPushButton("Copy", central);
        save_button_->setEnabled(false);
        copy_button_->setEnabled(false);
        transcript_header->addWidget(save_button_);
        transcript_header->addWidget(copy_button_);
        layout->addLayout(transcript_header);

        transcript_ = new QPlainTextEdit(central);
        transcript_->setPlaceholderText(
            "Choose a microphone, then press Start Recording.\n\n"
            "Your speech is transcribed on this computer. Nothing is uploaded.");
        transcript_->setReadOnly(true);
        transcript_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        // Transcripts are read, not skimmed: a little more room per line helps.
        transcript_->document()->setDocumentMargin(10);
        transcript_->setAccessibleName("Transcript");
        transcript_->setAccessibleDescription("The text transcribed from your recording");
        layout->addWidget(transcript_, 1);
        // ---- The library is a separate screen. Recording controls are not
        // merely disabled there, they are absent: nothing on this screen can
        // start a recording, so there is nothing to reason about.
        history_list_ = new QListWidget(this);
        history_list_->setAlternatingRowColors(true);
        history_list_->setContextMenuPolicy(Qt::CustomContextMenu);
        history_list_->setAccessibleName("Saved transcripts");
        history_list_->setMinimumWidth(300);
        history_list_->setUniformItemSizes(false);
        // Elide rather than scroll sideways: a horizontal scrollbar in a list
        // of previews is a worse way to read them than a trimmed line.
        history_list_->setTextElideMode(Qt::ElideRight);
        history_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        auto* library_page = new QWidget(this);
        auto* library_layout = new QVBoxLayout(library_page);
        library_layout->setSpacing(12);

        back_button_ = new QPushButton("\u2190  Back to Recording", library_page);
        back_button_->setMinimumHeight(32);
        auto* library_header = new QHBoxLayout();
        library_header->addWidget(back_button_);
        library_header->addSpacing(16);
        auto* library_title = new QLabel("Saved transcripts", library_page);
        QFont library_font = library_title->font();
        library_font.setBold(true);
        library_title->setFont(library_font);
        library_header->addWidget(library_title);
        library_header->addStretch();
        library_layout->addLayout(library_header);

        auto* library_body = new QHBoxLayout();
        library_body->setSpacing(12);
        library_body->addWidget(history_list_);

        auto* reader = new QWidget(library_page);
        auto* reader_layout = new QVBoxLayout(reader);
        reader_layout->setContentsMargins(0, 0, 0, 0);

        saved_title_ = new QLabel("Select a transcript to read it", reader);
        saved_save_ = new QPushButton("Save a Copy", reader);
        saved_copy_ = new QPushButton("Copy", reader);
        saved_save_->setEnabled(false);
        saved_copy_->setEnabled(false);
        auto* reader_header = new QHBoxLayout();
        reader_header->addWidget(saved_title_);
        reader_header->addStretch();
        reader_header->addWidget(saved_save_);
        reader_header->addWidget(saved_copy_);
        reader_layout->addLayout(reader_header);

        // Its own view, so live output is never disturbed by browsing.
        saved_view_ = new QPlainTextEdit(reader);
        saved_view_->setReadOnly(true);
        saved_view_->setPlaceholderText("Choose a saved transcript from the list.");
        saved_view_->document()->setDocumentMargin(10);
        saved_view_->setAccessibleName("Saved transcript");
        reader_layout->addWidget(saved_view_, 1);

        library_body->addWidget(reader, 1);
        library_layout->addLayout(library_body, 1);

        stack_ = new QStackedWidget(this);
        stack_->addWidget(central);
        stack_->addWidget(library_page);
        setCentralWidget(stack_);

        // ---- Status bar: context that used to be four stacked labels.
        context_label_ = new QLabel(this);
        context_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        statusBar()->addWidget(context_label_, 1);
        auto* privacy_note = new QLabel("Processed on this computer", this);
        privacy_note->setEnabled(false);
        statusBar()->addPermanentWidget(privacy_note);
        update_context_bar();

        process_ = new QProcess(this);
        process_->setProcessChannelMode(QProcess::MergedChannels);
        timer_ = new QTimer(this);
        timer_->setInterval(250);
        limit_timer_ = new QTimer(this);
        limit_timer_->setSingleShot(true);
        // Refreshes the elapsed figure while a long transcription runs, so the
        // display keeps moving between chunk reports.
        processing_timer_ = new QTimer(this);
        processing_timer_->setInterval(1000);

        populate_devices();
        // Resolved here rather than waiting for the worker, so saved
        // transcripts can be read before anything has been recorded.
        data_directory_ = QString::fromStdString(application_data_directory().string());
        connect(back_button_, &QPushButton::clicked, this, [this] { show_recorder(); });
        connect(saved_copy_, &QPushButton::clicked, this, [this] { copy_saved(); });
        connect(saved_save_, &QPushButton::clicked, this, [this] { save_saved(); });
        connect(history_list_, &QListWidget::currentRowChanged, this,
                [this](int row) { show_history_entry(row); });
        connect(history_list_, &QListWidget::customContextMenuRequested, this,
                [this](const QPoint& at) { show_history_menu(at); });
        connect(duration_selector_, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int) { refresh_clock(0); });
        refresh_clock(0);

        connect(start_button_, &QPushButton::clicked, this, [this] { start_recording(); });
        connect(stop_button_, &QPushButton::clicked, this, [this] {
            if (state_ == UiState::Recording) {
                stop_recording();
            } else {
                cancel_work();
            }
        });
        connect(copy_button_, &QPushButton::clicked, this, [this] { copy_transcript(); });
        connect(save_button_, &QPushButton::clicked, this, [this] { save_transcript(); });
        connect(process_, &QProcess::readyRead, this, [this] { consume_worker_output(); });
        connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (closing_ || error == QProcess::Crashed) {
                return;
            }
            set_failed("Audio worker problem: " + process_->errorString());
        });
        connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                this, [this](int exit_code, QProcess::ExitStatus status) { handle_worker_exit(exit_code, status); });
        connect(timer_, &QTimer::timeout, this, [this] { refresh_elapsed(); });
        connect(processing_timer_, &QTimer::timeout, this, [this] { report_transcription_progress(); });
        connect(limit_timer_, &QTimer::timeout, this, [this] {
            if (state_ != UiState::Recording) {
                return;
            }
            set_status("Recording limit reached; finishing...");
            stop_recording();
        });

        apply_state(UiState::Ready);
    }

private:
    void start_recording() {
        // Only these states may begin a recording, so a second click, a stray
        // shortcut, or a timer cannot start a concurrent worker.
        if (!ui_state_may_start(state_)) {
            return;
        }
        if (process_->state() != QProcess::NotRunning) {
            return;
        }

        transcript_->clear();
        final_transcription_.clear();
        transcript_path_.clear();
        model_summary_ = "Validating model";
        input_summary_.clear();
        storage_summary_.clear();
        update_context_bar();
        latency_label_->clear();
        partial_words_.clear();
        partial_trimmed_ = false;
        completed_ = false;
        error_shown_ = false;
        cancelled_ = false;
        chunks_done_ = 0;
        chunks_total_ = 0;
        const QString model = model_selector_->currentData().toString();
        const QString duration = duration_selector_->currentData().toString();
        // No --threads: the worker's measured default is the single source of truth.
        QStringList arguments{model, "--stream", "--duration", duration};
        if (!keep_audio_->isChecked()) {
            arguments << "--no-retain-audio";
        }
        const int device = device_selector_->currentData().toInt();
        if (device >= 0) {
            arguments << "--device" << QString::number(device);
        }
        limit_seconds_ = duration_selector_->currentData().toInt();
        // No waitForStarted: blocking here would freeze the window. The state
        // stays LoadingModel until the worker says it is ready.
        process_->start(QCoreApplication::applicationDirPath() + "/audio_to_text_cli", arguments);
        apply_state(UiState::LoadingModel);
        set_status("Loading model...");
    }

    // The worker has validated the model, opened the device, and is waiting.
    // Recording, and the clock, start here rather than at the button press.
    void begin_recording() {
        if (state_ != UiState::LoadingModel) {
            return;
        }
        process_->write("\n");
        elapsed_.start();
        timer_->start();
        limit_timer_->start(limit_seconds_ * 1000);
        apply_state(UiState::Recording);
        set_status("Recording. Speak now.");
        refresh_elapsed();
    }

    // Shows how much is done and how long it has taken, so a long wait reads
    // as work rather than as a freeze.
    void report_transcription_progress() {
        if (state_ != UiState::Processing || chunks_total_ <= 0) {
            return;
        }
        progress_->setRange(0, chunks_total_);
        progress_->setValue(chunks_done_);

        const qint64 seconds = processing_elapsed_.isValid() ? processing_elapsed_.elapsed() / 1000 : 0;
        QString status = QString("Transcribing %1 of %2").arg(chunks_done_).arg(chunks_total_);
        if (seconds > 0) {
            status += QString(" (%1s elapsed").arg(seconds);
            // Once a chunk is done the remainder can be estimated honestly.
            if (chunks_done_ > 0 && chunks_done_ < chunks_total_) {
                const qint64 remaining = seconds * (chunks_total_ - chunks_done_) / chunks_done_;
                status += QString(", about %1s left").arg(remaining);
            }
            status += ")";
        }
        set_status(status);
    }

    // Abandons whatever the worker is doing. Available while loading, stopping
    // and transcribing, so no phase is a dead end.
    void cancel_work() {
        if (!ui_state_is_busy(state_)) {
            return;
        }
        timer_->stop();
        limit_timer_->stop();
        processing_timer_->stop();
        set_status("Cancelling...");
        if (process_->state() != QProcess::NotRunning) {
            process_->write("q\n");
            if (!process_->waitForFinished(2000)) {
                process_->kill();
                process_->waitForFinished(1000);
            }
        }
        cancelled_ = true;
        apply_state(UiState::Ready);
        set_status("Cancelled. Start recording to try again.");
    }

    void stop_recording() {
        if (state_ != UiState::Recording) {
            return;
        }
        if (process_->state() == QProcess::Running) {
            process_->write("\n");
        }
        timer_->stop();
        limit_timer_->stop();
        apply_state(UiState::Stopping);
        set_status("Finishing recording...");
    }

    void consume_worker_output() {
        output_buffer_ += QString::fromUtf8(process_->readAll());
        int newline_index = output_buffer_.indexOf('\n');
        while (newline_index >= 0) {
            const QString line = output_buffer_.left(newline_index).trimmed();
            output_buffer_.remove(0, newline_index + 1);
            handle_worker_line(line);
            newline_index = output_buffer_.indexOf('\n');
        }
    }

    void handle_worker_line(const QString& line) {
        if (line.isEmpty()) {
            return;
        }

        if (line.startsWith("ERROR|") || line.startsWith("WARN|")) {
            handle_worker_report(line);
            return;
        }

        if (line.startsWith("SAVED|")) {
            handle_saved_line(line);
            return;
        }

        if (line.startsWith("MODEL|")) {
            handle_model_line(line);
            return;
        }

        if (line.startsWith("INPUT|")) {
            input_summary_ = line.section('|', 1);
            update_context_bar();
            return;
        }

        if (line.startsWith("PARTIAL|")) {
            const QStringList parts = line.split('|');
            if (parts.size() >= 4) {
                append_partial_text(parts.mid(3).join('|'),
                                    parts.at(1).toLongLong(), parts.at(2).toDouble());
            }
            return;
        }

        if (line.startsWith("FINAL|")) {
            final_transcription_ = line.section('|', 1);
            render_final_transcription();
            // Completion is keyed off the transcription itself, not off a file
            // being written, so it still works when retention is off.
            completed_ = true;
            processing_timer_->stop();
            apply_state(UiState::Completed);
            set_status(QString("Transcription complete (%1s)")
                .arg(processing_elapsed_.isValid() ? processing_elapsed_.elapsed() / 1000 : 0));
            if (process_->state() == QProcess::Running) {
                process_->write("q\n");
            }
            return;
        }

        if (line.startsWith("DATADIR|")) {
            data_directory_ = line.section('|', 1);
            storage_summary_ = data_directory_;
            update_context_bar();
            return;
        }

        if (line.startsWith("PROGRESS|")) {
            const QStringList parts = line.split('|');
            if (parts.size() >= 3) {
                chunks_done_ = parts.at(1).toInt();
                chunks_total_ = parts.at(2).toInt();
                report_transcription_progress();
            }
            return;
        }

        if (line.startsWith("STREAMSTATS|")) {
            handle_stream_stats(line);
            return;
        }

        if (line.startsWith("READY|")) {
            // Emitted at every prompt; only the first one starts a recording.
            begin_recording();
            return;
        }

        if (line.startsWith("PROCESSING|")) {
            if (state_ == UiState::Stopping || state_ == UiState::Recording) {
                timer_->stop();
                limit_timer_->stop();
                chunks_done_ = 0;
                chunks_total_ = 0;
                // Live-streaming figures describe a phase that has ended.
                latency_label_->clear();
                processing_elapsed_.start();
                processing_timer_->start();
                apply_state(UiState::Processing);
                set_status("Preparing audio...");
            }
            return;
        }

    }

    // Secondary actions live here rather than in the toolbar. Deleting
    // recordings is destructive and should be reached deliberately, not brushed
    // past while choosing a microphone.
    void build_menus() {
        QMenu* file_menu = menuBar()->addMenu("&File");
        save_action_ = file_menu->addAction("&Save Transcript...", this, [this] { save_transcript(); });
        save_action_->setShortcut(QKeySequence::Save);
        // Ctrl+C belongs to the transcript for copying a selection, so the
        // whole-transcript copy takes the shifted form.
        copy_action_ = file_menu->addAction("&Copy Transcript", this, [this] { copy_transcript(); });
        copy_action_->setShortcut(QKeySequence("Ctrl+Shift+C"));
        file_menu->addSeparator();
        delete_action_ = file_menu->addAction("&Delete All Recordings...", this,
                                              [this] { delete_recordings(); });
        file_menu->addSeparator();
        file_menu->addAction("&Quit", this, [this] { close(); })
            ->setShortcut(QKeySequence::Quit);

        QMenu* recording_menu = menuBar()->addMenu("&Recording");
        // One action rather than two, so a single key both starts and stops.
        // The menu is what makes the shortcut discoverable.
        record_action_ = recording_menu->addAction("&Start Recording", this, [this] {
            if (state_ == UiState::Recording) {
                stop_recording();
            } else {
                start_recording();
            }
        });
        record_action_->setShortcut(QKeySequence("Ctrl+R"));
        cancel_action_ = recording_menu->addAction("&Cancel", this, [this] { cancel_work(); });
        cancel_action_->setShortcut(QKeySequence(Qt::Key_Escape));

        QMenu* view_menu = menuBar()->addMenu("&View");
        history_action_ = view_menu->addAction("Saved &Transcripts", this,
                                               [this] { toggle_library(); });
        history_action_->setCheckable(true);
        history_action_->setShortcut(QKeySequence("Ctrl+H"));

        QMenu* help_menu = menuBar()->addMenu("&Help");
        help_menu->addAction("&Privacy", this, [this] { show_privacy_notice(); });
        help_menu->addAction("&About", this, [this] { show_about(); });
    }

    // One line replacing four stacked labels. Only what is known is shown, so a
    // fresh window does not read as a list of things that failed.
    void update_context_bar() {
        QStringList parts;
        if (!model_summary_.isEmpty()) {
            parts << model_summary_;
        }
        if (!input_summary_.isEmpty()) {
            parts << input_summary_;
        }
        if (!storage_summary_.isEmpty()) {
            parts << storage_summary_;
        }
        context_label_->setText(parts.isEmpty() ? QString("Ready to record")
                                                : parts.join("   ·   "));
    }

    void copy_saved() {
        QApplication::clipboard()->setText(saved_view_->toPlainText());
        statusBar()->showMessage("Saved transcript copied", 3000);
    }

    void save_saved() {
        const int row = history_list_->currentRow();
        const QString suggested = (row >= 0 && row < static_cast<int>(entries_.size()))
            ? QString::fromStdString(entries_[static_cast<std::size_t>(row)].path.filename().string())
            : QString("transcription.txt");
        const QString path = QFileDialog::getSaveFileName(this, "Save a copy", suggested);
        if (path.isEmpty()) {
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, "Save failed", "The transcript could not be saved.");
            return;
        }
        file.write(saved_view_->toPlainText().toUtf8());
    }

    void copy_transcript() {
        QApplication::clipboard()->setText(transcript_->toPlainText());
    }

    // The single place any control's enabled state is decided.
    void apply_state(UiState state) {
        state_ = state;

        const bool has_text = !transcript_->toPlainText().trimmed().isEmpty();
        const ControlStates controls = controls_for(state, devices_ready_, has_text);
        start_button_->setEnabled(controls.start);
        stop_button_->setEnabled(controls.stop || controls.cancel);
        stop_button_->setText(controls.cancel ? "Cancel" : "Stop Recording");
        model_selector_->setEnabled(controls.model);
        duration_selector_->setEnabled(controls.duration);
        device_selector_->setEnabled(controls.device);
        keep_audio_->setEnabled(controls.keep_audio);

        save_button_->setEnabled(controls.save);
        copy_button_->setEnabled(controls.copy);
        save_action_->setEnabled(controls.save);
        copy_action_->setEnabled(controls.copy);
        delete_action_->setEnabled(controls.delete_recordings);
        record_action_->setEnabled(controls.start || controls.stop);
        record_action_->setText(controls.stop ? "&Stop Recording" : "&Start Recording");
        cancel_action_->setEnabled(controls.cancel);

        history_list_->setEnabled(controls.history);
        history_action_->setEnabled(controls.history);

        apply_indicator(state);
        explain_disabled_controls(state, controls);

        // Visible only while something is running, so an idle window does not
        // show a bar that looks like stalled work.
        progress_->setVisible(ui_state_is_busy(state));

        if (controls.progress_indeterminate) {
            // The worker gives no completion fraction for these phases.
            progress_->setRange(0, 0);
        } else if (state == UiState::Recording) {
            progress_->setRange(0, limit_seconds_ > 0 ? limit_seconds_ : 100);
        } else if (state == UiState::Processing) {
            // Filled in by PROGRESS| reports; start indeterminate only until
            // the first one arrives.
            progress_->setRange(0, chunks_total_ > 0 ? chunks_total_ : 0);
            progress_->setValue(chunks_done_);
        } else {
            progress_->setRange(0, 100);
            progress_->setValue(0);
        }
    }

    // Elapsed against the limit, so the limit is visible before you commit to it.
    // Colour carries the state; the tooltip spells it out for anyone who cannot
    // rely on colour alone.
    void apply_indicator(UiState state) {
        QString colour = "#9e9e9e";
        QString meaning = "Idle";
        switch (state) {
            case UiState::Recording:
                colour = "#d32f2f";
                meaning = "Recording";
                break;
            case UiState::LoadingModel:
            case UiState::Stopping:
            case UiState::Processing:
                colour = "#f9a825";
                meaning = "Working";
                break;
            case UiState::Completed:
                colour = "#2e7d32";
                meaning = "Finished";
                break;
            case UiState::Error:
                colour = "#c62828";
                meaning = "Stopped with a problem";
                break;
            default:
                break;
        }
        indicator_->setStyleSheet("color: " + colour + ";");
        indicator_->setToolTip(meaning);
        indicator_->setAccessibleName(meaning);
    }

    // A disabled control that does not say why is a dead end. Each one explains
    // itself in the state it is unavailable in.
    void explain_disabled_controls(UiState state, const ControlStates& controls) {
        const QString busy = "Not available while a recording is in progress.";

        start_button_->setToolTip(controls.start
            ? "Begin recording (Ctrl+R)"
            : busy);
        stop_button_->setToolTip(controls.stop
            ? "Stop recording and transcribe (Ctrl+R)"
            : (controls.cancel ? "Abandon this recording (Esc)"
                               : "Nothing is running."));

        const QString settings_tip = ui_state_is_idle(state)
            ? QString()
            : "Cannot be changed until the current recording finishes.";
        model_selector_->setToolTip(settings_tip);
        duration_selector_->setToolTip(settings_tip);
        keep_audio_->setToolTip(ui_state_is_idle(state)
            ? "When off, audio is transcribed and discarded; no WAV file is written."
            : settings_tip);
        device_selector_->setToolTip(!devices_ready_
            ? "Looking for microphones..."
            : settings_tip);

        const QString nothing_yet = "Available once a transcription has finished.";
        save_button_->setToolTip(controls.save ? "Save the transcript to a file (Ctrl+S)" : nothing_yet);
        copy_button_->setToolTip(controls.copy ? "Copy the transcript (Ctrl+Shift+C)" : nothing_yet);
    }

    // ------------------------------------------------------------- history

    void show_library() {
        if (!ui_state_may_start(state_)) {
            return;
        }
        refresh_history();
        stack_->setCurrentIndex(1);
        // The status bar belongs to whichever screen is showing.
        context_label_->setText(entries_.empty()
            ? QString("No saved transcripts")
            : QString("%1 saved transcript%2").arg(entries_.size())
                  .arg(entries_.size() == 1 ? "" : "s"));
        history_action_->setChecked(true);
        // Nothing on this screen records, so the recording keys are inert here.
        record_action_->setEnabled(false);
        cancel_action_->setEnabled(false);
        history_list_->setFocus();
    }

    void show_recorder() {
        stack_->setCurrentIndex(0);
        history_action_->setChecked(false);
        update_context_bar();
        apply_state(state_);
    }

    void toggle_library() {
        if (stack_->currentIndex() == 1) {
            show_recorder();
        } else {
            show_library();
        }
    }

    // Rebuilt from disk rather than tracked incrementally: the list is short,
    // and a stale list is worse than a cheap re-read.
    void refresh_history() {
        const int previous_row = history_list_->currentRow();
        const QSignalBlocker blocker(history_list_);
        history_list_->clear();
        entries_ = list_transcripts(fs::path(data_directory_.toStdString()));

        if (entries_.empty()) {
            auto* empty = new QListWidgetItem("No saved transcripts yet", history_list_);
            empty->setFlags(Qt::NoItemFlags);
            return;
        }

        for (const TranscriptEntry& entry : entries_) {
            char when[64];
            std::tm shown = entry.stamp.when;
            std::strftime(when, sizeof(when), "%d %b %Y, %H:%M", &shown);
            QString preview = entry.preview.empty()
                ? QString("(no text)")
                : QString::fromStdString(entry.preview);
            // The row is a label, not the transcript: keep it to one glance.
            if (preview.size() > 40) {
                preview = preview.left(40).trimmed() + "...";
            }

            auto* item = new QListWidgetItem(history_list_);
            item->setToolTip(QString::fromStdString(entry.path.string()));

            auto* row = new QWidget(history_list_);
            auto* row_layout = new QHBoxLayout(row);
            row_layout->setContentsMargins(6, 4, 4, 4);

            auto* text = new QVBoxLayout();
            text->setSpacing(1);
            auto* when_label = new QLabel(QString::fromUtf8(when), row);
            auto* preview_label = new QLabel(preview, row);
            preview_label->setEnabled(false);
            // Ignored width: the labels must yield rather than push the delete
            // control out of the row, which is what a natural width would do.
            when_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            preview_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            text->addWidget(when_label);
            text->addWidget(preview_label);
            row_layout->addLayout(text, 1);

            // Deleting one transcript is offered where that transcript is, so
            // it is always obvious which one would go.
            auto* remove = new QToolButton(row);
            remove->setText("\u2715");
            remove->setAutoRaise(true);
            remove->setToolTip("Delete this transcript");
            remove->setAccessibleName(QString("Delete transcript from %1").arg(QString::fromUtf8(when)));
            row_layout->addWidget(remove);

            // The path is captured by value: the row widget is destroyed and
            // rebuilt on every refresh, so it must not outlive a reference.
            const fs::path path = entry.path;
            const QString label = QString::fromUtf8(when);
            connect(remove, &QToolButton::clicked, this,
                    [this, path, label] { delete_one_transcript(path, label); });

            // Zero width lets the view use the viewport's, so the row never
            // demands more room than the list has.
            item->setSizeHint(QSize(0, row->sizeHint().height()));
            history_list_->setItemWidget(item, row);
        }
        if (previous_row >= 0 && previous_row < history_list_->count()) {
            history_list_->setCurrentRow(previous_row);
        }
    }

    void show_history_entry(int row) {
        if (row < 0 || row >= static_cast<int>(entries_.size())) {
            saved_view_->clear();
            saved_title_->setText("Select a transcript to read it");
            saved_save_->setEnabled(false);
            saved_copy_->setEnabled(false);
            return;
        }
        const TranscriptEntry& entry = entries_[static_cast<std::size_t>(row)];

        std::string text;
        std::string reason;
        if (!read_transcript(entry.path, fs::path(data_directory_.toStdString()), text, reason)) {
            QMessageBox::warning(this, "Could not open transcript",
                                 QString::fromStdString(reason));
            return;
        }

        saved_view_->setPlainText(QString::fromStdString(text));
        saved_view_->moveCursor(QTextCursor::Start);

        char when[64];
        std::tm shown = entry.stamp.when;
        std::strftime(when, sizeof(when), "%d %b %Y at %H:%M", &shown);
        const CompanionAudio audio =
            companion_audio(entry.stamp, fs::path(data_directory_.toStdString()));
        saved_title_->setText(QString("%1%2")
            .arg(QString::fromUtf8(when), audio.any() ? "" : "   (audio deleted)"));
        saved_save_->setEnabled(true);
        saved_copy_->setEnabled(true);
    }

    // Deletes a single transcript. The recording it came from is deliberately
    // left alone: File > Delete All Recordings is the way to remove audio.
    void delete_one_transcript(const fs::path& path, const QString& label) {
        const auto choice = QMessageBox::question(this, "Delete transcript",
            QString("Delete the transcript from %1?\n\nThe recording it came from is kept. "
                    "This cannot be undone.").arg(label),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice != QMessageBox::Yes) {
            return;
        }

        std::string reason;
        if (!delete_transcript(path, fs::path(data_directory_.toStdString()), reason)) {
            QMessageBox::warning(this, "Could not delete", QString::fromStdString(reason));
            return;
        }

        // Whatever was being read may be the file just removed.
        saved_view_->clear();
        saved_title_->setText("Select a transcript to read it");
        saved_save_->setEnabled(false);
        saved_copy_->setEnabled(false);
        refresh_history();
        statusBar()->showMessage("Transcript deleted", 3000);
    }

    void show_history_menu(const QPoint& at) {
        const int row = history_list_->currentRow();
        if (row < 0 || row >= static_cast<int>(entries_.size())) {
            return;
        }
        const TranscriptEntry entry = entries_[static_cast<std::size_t>(row)];
        const fs::path root(data_directory_.toStdString());
        const CompanionAudio audio = companion_audio(entry.stamp, root);

        QMenu menu(this);
        menu.addAction("&Copy", this, [this] { copy_saved(); });
        menu.addAction("&Save a Copy...", this, [this] { save_saved(); });
        menu.addSeparator();

        QStringList kept;
        if (audio.has_recording) { kept << "original"; }
        if (audio.has_cleaned) { kept << "cleaned"; }
        if (audio.has_speech) { kept << "speech only"; }
        QAction* audio_note = menu.addAction(kept.isEmpty()
            ? QString("Audio: not kept")
            : QString("Audio: %1").arg(kept.join(", ")));
        audio_note->setEnabled(false);

        menu.addSeparator();
        menu.addAction("&Delete This Transcript...", this, [this, entry] {
            char when[64];
            std::tm shown = entry.stamp.when;
            std::strftime(when, sizeof(when), "%d %b %Y, %H:%M", &shown);
            delete_one_transcript(entry.path, QString::fromUtf8(when));
        });
        menu.exec(history_list_->mapToGlobal(at));
    }

    void refresh_clock(qint64 seconds) {
        const int limit = limit_seconds_ > 0 ? limit_seconds_
                                             : duration_selector_->currentData().toInt();
        duration_label_->setText(QString("%1 / %2")
            .arg(QTime(0, 0).addSecs(static_cast<int>(seconds)).toString("mm:ss"),
                 QTime(0, 0).addSecs(limit).toString("mm:ss")));
    }

    void refresh_elapsed() {
        if (state_ != UiState::Recording) {
            return;
        }
        // Monotonic: unaffected by clock changes or midnight rollover.
        const qint64 seconds = elapsed_.elapsed() / 1000;
        refresh_clock(seconds);
        progress_->setValue(static_cast<int>(qMin<qint64>(seconds, limit_seconds_)));
    }

    // Asks the worker to enumerate capture devices before any recording starts.
    void populate_devices() {
        device_selector_->addItem("System default", -1);
        device_selector_->setEnabled(false);

        // Enumeration runs asynchronously; blocking here froze the window for
        // as long as the audio backend took to answer.
        auto* probe = new QProcess(this);
        connect(probe, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, probe](int, QProcess::ExitStatus) {
            add_enumerated_devices(QString::fromUtf8(probe->readAllStandardOutput()));
            probe->deleteLater();
        });
        connect(probe, &QProcess::errorOccurred, this, [this, probe](QProcess::ProcessError) {
            devices_ready_ = true;
            apply_state(state_);
            probe->deleteLater();
        });
        probe->start(QCoreApplication::applicationDirPath() + "/audio_to_text_cli", {"--list-devices"});
    }

    void add_enumerated_devices(const QString& output) {
        const QStringList lines = output.split('\n');
        for (const QString& line : lines) {
            if (!line.startsWith("DEVICE|")) {
                continue;
            }
            const QStringList parts = line.split('|');
            if (parts.size() < 4) {
                continue;
            }
            const int index = parts.at(1).toInt();
            const QString name = parts.mid(3).join('|').trimmed();
            const bool is_default = parts.at(2) == "default";
            device_selector_->addItem(is_default ? name + " (default)" : name, index);
        }
        devices_ready_ = true;
        apply_state(state_);
    }

    // The worker reports the validated model as MODEL|variant|language|bytes|path.
    void handle_model_line(const QString& line) {
        const QStringList parts = line.split('|');
        if (parts.size() < 5) {
            return;
        }

        const QString variant = parts.at(1);
        const QString language = parts.at(2) == "multilingual" ? "multilingual" : "English-only";
        const double megabytes = parts.at(3).toDouble() / (1024.0 * 1024.0);
        model_summary_ = QString("%1 (%2, %3 MB)")
            .arg(variant, language, QString::number(megabytes, 'f', 1));
        update_context_bar();
    }

    // The worker announces each artefact as SAVED|KIND|path.
    void handle_saved_line(const QString& line) {
        const QStringList parts = line.split('|');
        if (parts.size() < 3 || parts.at(1) != "TRANSCRIPT") {
            return;
        }

        transcript_path_ = parts.mid(2).join('|');
        storage_summary_ = transcript_path_;
        update_context_bar();
        if (stack_->currentIndex() == 1) {
            refresh_history();
        }
    }

    // Worker reports arrive as SEVERITY|CATEGORY|message.
    void handle_worker_report(const QString& line) {
        const QStringList parts = line.split('|');
        if (parts.size() < 3) {
            return;
        }

        const QString severity = parts.at(0);
        const QString category = parts.at(1);
        const QString message = parts.mid(2).join('|');

        if (severity == "WARN") {
            set_status(message);
            return;
        }

        error_shown_ = true;
        set_failed(actionable_message(category, message));
        if (process_->state() == QProcess::Running) {
            process_->write("q\n");
        }
    }

    static QString actionable_message(const QString& category, const QString& message) {
        if (category == "MICROPHONE") {
            return message + " Check that a microphone is connected and that this application may use it.";
        }
        if (category == "MODEL") {
            return message + " Download the model into models/ and select it again.";
        }
        if (category == "RECORDING") {
            return message + " Speak closer to the microphone and record again.";
        }
        if (category == "TRANSCRIPTION") {
            return message + " Try the base.en model, or record a shorter clip.";
        }
        if (category == "FILE_SAVING") {
            return message + " Check free disk space and write permissions for the working directory.";
        }
        return message;
    }

    void handle_worker_exit(int exit_code, QProcess::ExitStatus status) {
        if (closing_) {
            return;
        }
        if (cancelled_) {
            // We asked it to stop, and may have killed it. That is not a crash,
            // and the message already on screen is the accurate one.
            return;
        }
        if (error_shown_) {
            // Stay in Error: the reported cause remains on screen, and any text
            // that did arrive stays saveable.
            set_failed(status_label_->text());
            return;
        }
        if (status == QProcess::CrashExit) {
            set_failed("The audio worker stopped unexpectedly. Start recording to try again.");
            return;
        }
        if (exit_code != 0) {
            set_failed("The audio worker exited with code " + QString::number(exit_code) + ".");
            return;
        }
        set_idle(completed_ ? "Transcription complete" : "Ready");
    }

    void append_partial_text(const QString& incoming, qint64 latency_ms, double queue_seconds) {
        latency_label_->setText(QString("Partial latency: %1 ms, %2 s behind live")
            .arg(latency_ms).arg(QString::number(queue_seconds, 'f', 1)));

        partial_text::append_with_overlap(partial_words_, incoming,
            maximum_overlap_words, maximum_partial_words, partial_trimmed_);
        render_partial_text();
    }

    void render_partial_text() {
        const QString body = partial_words_.join(' ');
        transcript_->setPlainText(partial_trimmed_ ? "[earlier text trimmed] " + body : body);
        transcript_->moveCursor(QTextCursor::End);
    }

    void handle_stream_stats(const QString& line) {
        int dropped = 0;
        int rate_limited = 0;
        for (const QString& field : line.split('|')) {
            if (field.startsWith("dropped=")) {
                dropped = field.section('=', 1).toInt();
            } else if (field.startsWith("rate_limited=")) {
                rate_limited = field.section('=', 1).toInt();
            }
        }
        if (dropped > 0 || rate_limited > 0) {
            latency_label_->setText(QString("Live text skipped %1 window(s); the final "
                                            "transcription covers everything")
                .arg(dropped + rate_limited));
        }
    }

    void show_about() {
        QMessageBox::about(this, "About Audio to Text",
            QString("<b>Audio to Text %1</b><br>"
                    "Build %2, %3<br><br>"
                    "Offline speech to text. Audio never leaves this computer.<br><br>"
                    "Transcription by whisper.cpp, audio capture by miniaudio, "
                    "interface built with Qt 6.")
                .arg(AUDIO_TO_TEXT_VERSION, AUDIO_TO_TEXT_GIT_COMMIT, AUDIO_TO_TEXT_BUILD_TYPE));
    }

    void show_privacy_notice() {
        QMessageBox::information(this, "Privacy",
            "Audio is captured, analysed and transcribed entirely on this computer.\n\n"
            "Whisper runs locally against a model file on disk. No audio, transcript or "
            "metadata is uploaded, and neither this window nor the worker it runs opens a "
            "network connection.\n\n"
            "Saved to:\n" + (data_directory_.isEmpty() ? QString("(shown once a recording starts)")
                                                       : data_directory_) + "\n\n"
            "Files are created readable only by you. Nothing is written to a log file.\n\n"
            "Turn off \"Keep audio files\" to transcribe without keeping any WAV file. "
            "\"Delete recordings\" removes every stored recording and transcript.");
    }

    void delete_recordings() {
        const auto choice = QMessageBox::question(this, "Delete recordings",
            "Delete every stored recording and transcript?\n\nThis cannot be undone.",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice != QMessageBox::Yes) {
            return;
        }

        QProcess cleaner;
        cleaner.start(QCoreApplication::applicationDirPath() + "/audio_to_text_cli",
                      {"--delete-recordings"});
        if (!cleaner.waitForFinished(10000)) {
            cleaner.kill();
            cleaner.waitForFinished(1000);
            QMessageBox::warning(this, "Delete recordings", "The deletion did not finish.");
            return;
        }

        const QStringList lines = QString::fromUtf8(cleaner.readAllStandardOutput()).split('\n');
        for (const QString& line : lines) {
            if (!line.startsWith("DELETED|")) {
                continue;
            }
            const QStringList parts = line.split('|');
            if (parts.size() >= 3) {
                QMessageBox::information(this, "Delete recordings",
                    QString("Deleted %1 file(s), %2 MB.")
                        .arg(parts.at(1))
                        .arg(QString::number(parts.at(2).toDouble() / (1024.0 * 1024.0), 'f', 1)));
                transcript_path_.clear();
                refresh_history();
                return;
            }
        }
        QMessageBox::warning(this, "Delete recordings", "Nothing was reported as deleted.");
    }

    void save_transcript() {
        const QString suggested = transcript_path_.isEmpty() ? QString("transcription.txt") : transcript_path_;
        const QString path = QFileDialog::getSaveFileName(this, "Save a copy of the transcription", suggested);
        if (path.isEmpty()) {
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, "Save failed", "The transcription could not be saved.");
            return;
        }
        file.write(transcript_->toPlainText().toUtf8());
    }

    void render_final_transcription() {
        if (final_transcription_.isEmpty()) {
            return;
        }
        transcript_->setPlainText(final_transcription_);
        transcript_->moveCursor(QTextCursor::End);
    }

    void closeEvent(QCloseEvent* event) override {
        if (ui_state_is_busy(state_)) {
            const auto choice = QMessageBox::question(this, "Recording in progress",
                "A recording is still being transcribed. Closing now discards it.\n\nClose anyway?",
                QMessageBox::Close | QMessageBox::Cancel, QMessageBox::Cancel);
            if (choice != QMessageBox::Close) {
                event->ignore();
                return;
            }
        }

        closing_ = true;
        timer_->stop();
        limit_timer_->stop();
        if (process_->state() != QProcess::NotRunning) {
            process_->write("q\n");
            if (!process_->waitForFinished(3000)) {
                process_->kill();
                process_->waitForFinished(1000);
            }
        }
        QMainWindow::closeEvent(event);
    }

    void set_status(const QString& status) {
        status_label_->setText(status);
    }

    void set_idle(const QString& status) {
        timer_->stop();
        limit_timer_->stop();
        apply_state(completed_ ? UiState::Completed : UiState::Ready);
        set_status(status);
    }

    void set_failed(const QString& status) {
        timer_->stop();
        limit_timer_->stop();
        apply_state(UiState::Error);
        set_status(status);
    }

    QComboBox* model_selector_;
    QComboBox* duration_selector_;
    QComboBox* device_selector_;
    QPushButton* start_button_;
    QPushButton* stop_button_;
    QPushButton* save_button_;
    QPushButton* copy_button_;
    QLabel* status_label_;
    QLabel* duration_label_;
    QLabel* latency_label_;
    QLabel* indicator_;
    QLabel* transcript_title_;
    QListWidget* history_list_;
    QStackedWidget* stack_;
    QPushButton* back_button_;
    QPlainTextEdit* saved_view_;
    QLabel* saved_title_;
    QPushButton* saved_save_;
    QPushButton* saved_copy_;
    QAction* history_action_;
    std::vector<TranscriptEntry> entries_;
    QLabel* context_label_;
    QString model_summary_;
    QString input_summary_;
    QString storage_summary_;
    QAction* save_action_;
    QAction* copy_action_;
    QAction* delete_action_;
    QAction* record_action_;
    QAction* cancel_action_;
    QProgressBar* progress_;
    QCheckBox* keep_audio_;
    QString data_directory_;
    QPlainTextEdit* transcript_;
    QProcess* process_;
    QTimer* timer_;
    QTimer* limit_timer_;
    QTimer* processing_timer_;
    QElapsedTimer elapsed_;
    UiState state_ = UiState::Ready;
    int limit_seconds_ = 0;
    bool devices_ready_ = false;
    QString output_buffer_;
    QString final_transcription_;
    QStringList partial_words_;
    bool partial_trimmed_ = false;
    static constexpr int maximum_partial_words = 3000;
    static constexpr int maximum_overlap_words = 40;
    QString transcript_path_;
    bool completed_ = false;
    bool closing_ = false;
    bool error_shown_ = false;
    bool cancelled_ = false;
    int chunks_done_ = 0;
    int chunks_total_ = 0;
    QElapsedTimer processing_elapsed_;
};

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    AudioToTextWindow window;
    window.show();
    return application.exec();
}
