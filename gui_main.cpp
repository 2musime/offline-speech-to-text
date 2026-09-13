#include "audio_player.h"
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
#include <QActionGroup>
#include <QShortcut>
#include <QSlider>
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
        save_button_->setShortcut(QKeySequence::Save);
        copy_button_->setShortcut(QKeySequence("Ctrl+Shift+C"));
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

        auto* library_header = new QHBoxLayout();
        auto* library_title = new QLabel("Saved transcripts", library_page);
        QFont library_font = library_title->font();
        library_font.setBold(true);
        library_title->setFont(library_font);
        library_header->addWidget(library_title);
        library_header->addStretch();
        delete_all_button_ = new QPushButton("Delete All Recordings...", library_page);
        delete_all_button_->setToolTip("Remove every stored recording and transcript");
        library_header->addWidget(delete_all_button_);
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

        // Bottom right, under the transcript it would remove, so the target is
        // never ambiguous. Ember rather than plain red: destructive enough to
        // give pause, not an error.
        delete_saved_ = new QPushButton("Delete Transcript", reader);
        delete_saved_->setEnabled(false);
        delete_saved_->setMinimumHeight(32);
        delete_saved_->setCursor(Qt::PointingHandCursor);
        delete_saved_->setToolTip("Delete the transcript shown above. The recording is kept.");
        delete_saved_->setStyleSheet(
            "QPushButton {"
            "  background-color: #c1440e;"
            "  color: #ffffff;"
            "  border: none;"
            "  border-radius: 4px;"
            "  padding: 6px 18px;"
            "}"
            "QPushButton:hover  { background-color: #a63a0c; }"
            "QPushButton:pressed { background-color: #8c3009; }"
            "QPushButton:disabled { background-color: #e0ccc4; color: #9a8d88; }");

        auto* reader_footer = new QHBoxLayout();
        reader_footer->addStretch();
        reader_footer->addWidget(delete_saved_);
        reader_layout->addLayout(reader_footer);

        library_body->addWidget(reader, 1);
        library_layout->addLayout(library_body, 1);

        // ---- Landing screen. Deliberately almost empty: it says what the
        // application is and offers the one thing a new user wants. Recording
        // happens on the recording screen, not here.
        auto* home_page = new QWidget(this);
        auto* home_layout = new QVBoxLayout(home_page);
        home_layout->addStretch();

        auto* welcome = new QLabel("Audio to Text", home_page);
        QFont welcome_font = welcome->font();
        welcome_font.setPointSize(welcome_font.pointSize() + 14);
        welcome_font.setBold(true);
        welcome->setFont(welcome_font);
        welcome->setAlignment(Qt::AlignCenter);
        home_layout->addWidget(welcome);

        auto* tagline = new QLabel(
            "Speech to text that runs entirely on this computer.\n"
            "Your audio is never uploaded.", home_page);
        tagline->setAlignment(Qt::AlignCenter);
        home_layout->addWidget(tagline);
        home_layout->addSpacing(28);

        go_record_button_ = new QPushButton("Start a Recording", home_page);
        go_record_button_->setMinimumHeight(44);
        go_record_button_->setMinimumWidth(240);
        QFont go_font = go_record_button_->font();
        go_font.setBold(true);
        go_record_button_->setFont(go_font);
        go_record_button_->setCursor(Qt::PointingHandCursor);
        auto* go_row = new QHBoxLayout();
        go_row->addStretch();
        go_row->addWidget(go_record_button_);
        go_row->addStretch();
        home_layout->addLayout(go_row);
        home_layout->addSpacing(24);

        home_summary_ = new QLabel(home_page);
        home_summary_->setAlignment(Qt::AlignCenter);
        home_summary_->setEnabled(false);
        home_layout->addWidget(home_summary_);
        home_layout->addStretch();

        // ---- Speeches: play back what was actually said. This is the raw
        // capture, not the cleaned copy or the extracted speech; those exist
        // for Whisper, and are not what someone wants to hear.
        speech_list_ = new QListWidget(this);
        speech_list_->setAlternatingRowColors(true);
        speech_list_->setMinimumWidth(300);
        speech_list_->setTextElideMode(Qt::ElideRight);
        speech_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        speech_list_->setAccessibleName("Recordings");

        auto* speech_page = new QWidget(this);
        auto* speech_layout = new QVBoxLayout(speech_page);
        speech_layout->setSpacing(12);

        auto* speech_header = new QHBoxLayout();
        auto* speech_title = new QLabel("Speeches", speech_page);
        QFont speech_font = speech_title->font();
        speech_font.setBold(true);
        speech_title->setFont(speech_font);
        speech_header->addWidget(speech_title);
        speech_header->addStretch();
        speech_layout->addLayout(speech_header);

        auto* speech_body = new QHBoxLayout();
        speech_body->setSpacing(12);
        speech_body->addWidget(speech_list_);

        auto* player_panel = new QWidget(speech_page);
        auto* player_layout = new QVBoxLayout(player_panel);
        player_layout->setContentsMargins(0, 0, 0, 0);
        player_layout->setSpacing(12);

        // Header matches the transcripts screen, so the two read the same way.
        now_playing_ = new QLabel("Select a recording to play it", player_panel);
        QFont playing_font = now_playing_->font();
        playing_font.setBold(true);
        now_playing_->setFont(playing_font);
        player_layout->addWidget(now_playing_);

        auto* transport_panel = new QGroupBox(player_panel);
        auto* transport_layout = new QVBoxLayout(transport_panel);
        transport_layout->setSpacing(10);

        play_button_ = new QPushButton("Play", transport_panel);
        play_button_->setMinimumHeight(40);
        play_button_->setMinimumWidth(130);
        QFont play_font = play_button_->font();
        play_font.setBold(true);
        play_button_->setFont(play_font);
        play_button_->setEnabled(false);
        stop_play_button_ = new QPushButton("Stop", transport_panel);
        stop_play_button_->setMinimumHeight(40);
        stop_play_button_->setEnabled(false);

        play_time_ = new QLabel("00:00 / 00:00", transport_panel);
        QFont time_font = play_time_->font();
        time_font.setPointSize(time_font.pointSize() + 3);
        play_time_->setFont(time_font);

        auto* transport = new QHBoxLayout();
        transport->addWidget(play_button_);
        transport->addWidget(stop_play_button_);
        transport->addSpacing(20);
        transport->addWidget(play_time_);
        transport->addStretch();
        transport_layout->addLayout(transport);

        position_slider_ = new QSlider(Qt::Horizontal, transport_panel);
        position_slider_->setRange(0, 0);
        position_slider_->setEnabled(false);
        transport_layout->addWidget(position_slider_);
        player_layout->addWidget(transport_panel);

        // The space under the controls was empty; this is what belongs in it.
        recording_details_ = new QLabel(player_panel);
        recording_details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        recording_details_->setWordWrap(true);
        recording_details_->setEnabled(false);
        player_layout->addWidget(recording_details_);
        player_layout->addStretch();

        delete_speech_ = new QPushButton("Delete Recording", player_panel);
        delete_speech_->setEnabled(false);
        delete_speech_->setMinimumHeight(32);
        delete_speech_->setCursor(Qt::PointingHandCursor);
        delete_speech_->setToolTip("Delete this recording and its processed copies. "
                                  "The transcript is kept.");
        delete_speech_->setStyleSheet(
            "QPushButton {"
            "  background-color: #c1440e;"
            "  color: #ffffff;"
            "  border: none;"
            "  border-radius: 4px;"
            "  padding: 6px 18px;"
            "}"
            "QPushButton:hover  { background-color: #a63a0c; }"
            "QPushButton:pressed { background-color: #8c3009; }"
            "QPushButton:disabled { background-color: #e0ccc4; color: #9a8d88; }");
        auto* speech_footer = new QHBoxLayout();
        speech_footer->addStretch();
        speech_footer->addWidget(delete_speech_);
        player_layout->addLayout(speech_footer);

        speech_body->addWidget(player_panel, 1);
        speech_layout->addLayout(speech_body, 1);

        stack_ = new QStackedWidget(this);
        stack_->addWidget(home_page);
        stack_->addWidget(central);
        stack_->addWidget(library_page);
        stack_->addWidget(speech_page);
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
        // Playback position lives on the audio thread; this is how the slider
        // and clock learn about it.
        play_timer_ = new QTimer(this);
        play_timer_->setInterval(200);

        populate_devices();
        // Resolved here rather than waiting for the worker, so saved
        // transcripts can be read before anything has been recorded.
        data_directory_ = QString::fromStdString(application_data_directory().string());
        connect(go_record_button_, &QPushButton::clicked, this, [this] { show_recorder(); });
        connect(delete_all_button_, &QPushButton::clicked, this, [this] { delete_recordings(); });
        connect(speech_list_, &QListWidget::currentRowChanged, this,
                [this](int row) { select_recording(row); });
        connect(play_button_, &QPushButton::clicked, this, [this] { toggle_playback(); });
        connect(stop_play_button_, &QPushButton::clicked, this, [this] { stop_playback(); });
        connect(delete_speech_, &QPushButton::clicked, this,
                [this] { delete_selected_recording(); });
        connect(position_slider_, &QSlider::sliderReleased, this, [this] {
            player_.seek_seconds(position_slider_->value());
            refresh_play_time();
        });
        connect(saved_copy_, &QPushButton::clicked, this, [this] { copy_saved(); });
        connect(saved_save_, &QPushButton::clicked, this, [this] { save_saved(); });
        connect(delete_saved_, &QPushButton::clicked, this, [this] { delete_selected_transcript(); });
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
        connect(play_timer_, &QTimer::timeout, this, [this] { refresh_play_time(); });
        connect(limit_timer_, &QTimer::timeout, this, [this] {
            if (state_ != UiState::Recording) {
                return;
            }
            set_status("Recording limit reached; finishing...");
            stop_recording();
        });

        apply_state(UiState::Ready);
        // Start on the landing screen, with its own status line.
        show_home();
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
    // The bar across the top is navigation. Pressing an entry goes to that
    // screen and does nothing else: pressing Recording must not find that a
    // recording has already started. Each screen owns its own actions.
    void build_menus() {
        auto* screens = new QActionGroup(this);
        screens->setExclusive(true);

        const auto add_screen = [this, screens](const QString& text, const QKeySequence& key,
                                                auto handler) {
            QAction* action = menuBar()->addAction(text);
            action->setCheckable(true);
            action->setShortcut(key);
            screens->addAction(action);
            connect(action, &QAction::triggered, this, handler);
            return action;
        };

        home_action_ = add_screen("&Home", QKeySequence("Ctrl+1"), [this] { show_home(); });
        recorder_action_ = add_screen("&Recording", QKeySequence("Ctrl+2"),
                                      [this] { show_recorder(); });
        history_action_ = add_screen("&Transcripts", QKeySequence("Ctrl+H"),
                                     [this] { show_library(); });
        speeches_action_ = add_screen("&Speeches", QKeySequence("Ctrl+P"),
                                      [this] { show_speeches(); });

        QMenu* help_menu = menuBar()->addMenu("&Help");
        help_menu->addAction("&Privacy", this, [this] { show_privacy_notice(); });
        help_menu->addAction("&About", this, [this] { show_about(); });
        help_menu->addSeparator();
        help_menu->addAction("&Quit", this, [this] { close(); })
            ->setShortcut(QKeySequence::Quit);

        // Recording keys stay available without occupying the navigation bar.
        record_action_ = new QAction(this);
        record_action_->setShortcut(QKeySequence("Ctrl+R"));
        connect(record_action_, &QAction::triggered, this, [this] {
            if (state_ == UiState::Recording) {
                stop_recording();
            } else {
                show_recorder();
                start_recording();
            }
        });
        addAction(record_action_);

        cancel_action_ = new QAction(this);
        cancel_action_->setShortcut(QKeySequence(Qt::Key_Escape));
        connect(cancel_action_, &QAction::triggered, this, [this] { cancel_work(); });
        addAction(cancel_action_);
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
        record_action_->setEnabled(controls.start || controls.stop);
        record_action_->setText(controls.stop ? "&Stop Recording" : "&Start Recording");
        cancel_action_->setEnabled(controls.cancel);

        history_list_->setEnabled(controls.history);
        // Navigating away mid-recording would hide the controls for the thing
        // still running, so the bar closes while it does.
        history_action_->setEnabled(controls.history);
        home_action_->setEnabled(!ui_state_is_busy(state));
        recorder_action_->setEnabled(!ui_state_is_busy(state));
        speeches_action_->setEnabled(controls.history);
        delete_all_button_->setEnabled(controls.delete_recordings);

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

    void show_home() {
        if (ui_state_is_busy(state_)) {
            return;
        }
        stop_playback_if_running();
        stack_->setCurrentIndex(0);
        home_action_->setChecked(true);
        context_label_->setText("Ready when you are");
        refresh_home_summary();
        apply_state(state_);
    }

    // Home is otherwise silent about what the application already holds, which
    // gives no reason to visit the other screens.
    void refresh_home_summary() {
        const fs::path root(data_directory_.toStdString());
        const std::size_t recordings = list_recordings(root).size();
        const std::size_t transcripts = list_transcripts(root).size();
        if (recordings == 0 && transcripts == 0) {
            home_summary_->setText(QString());
            return;
        }
        home_summary_->setText(QString("%1 recording%2  ·  %3 transcript%4")
            .arg(recordings).arg(recordings == 1 ? "" : "s")
            .arg(transcripts).arg(transcripts == 1 ? "" : "s"));
    }

    void show_speeches() {
        if (!ui_state_may_start(state_)) {
            return;
        }
        refresh_speeches();
        stack_->setCurrentIndex(3);
        speeches_action_->setChecked(true);
        context_label_->setText(recordings_.empty()
            ? QString("No recordings")
            : QString("%1 recording%2").arg(recordings_.size())
                  .arg(recordings_.size() == 1 ? "" : "s"));
        record_action_->setEnabled(false);
        cancel_action_->setEnabled(false);
        speech_list_->setFocus();
    }

    void refresh_speeches() {
        const int previous = speech_list_->currentRow();
        const QSignalBlocker blocker(speech_list_);
        speech_list_->clear();
        recordings_ = list_recordings(fs::path(data_directory_.toStdString()));

        if (recordings_.empty()) {
            auto* empty = new QListWidgetItem(
                "No recordings yet\n\nGo to Recording to make one", speech_list_);
            empty->setTextAlignment(Qt::AlignCenter);
            empty->setFlags(Qt::NoItemFlags);
            return;
        }

        for (const RecordingEntry& entry : recordings_) {
            // The name the file carries, and nothing else. Length and transcript
            // status belong to the recording being played, not to the list.
            char stamp[32];
            std::tm shown = entry.stamp.when;
            std::strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &shown);
            auto* item = new QListWidgetItem(
                QString("recording-%1").arg(QString::fromUtf8(stamp)), speech_list_);
            item->setTextAlignment(Qt::AlignCenter);
            item->setToolTip(QString::fromStdString(entry.path.string()));
        }
        if (previous >= 0 && previous < speech_list_->count()) {
            speech_list_->setCurrentRow(previous);
        }
    }

    void select_recording(int row) {
        player_.unload();
        play_timer_->stop();
        if (row < 0 || row >= static_cast<int>(recordings_.size())) {
            now_playing_->setText(recordings_.empty()
                ? QString("Nothing recorded yet")
                : QString("Select a recording to play it"));
            recording_details_->setText(recordings_.empty()
                ? QString("Recordings you make appear here, and can be played back.")
                : QString());
            play_button_->setEnabled(false);
            stop_play_button_->setEnabled(false);
            delete_speech_->setEnabled(false);
            position_slider_->setEnabled(false);
            position_slider_->setRange(0, 0);
            play_time_->setText("00:00 / 00:00");
            return;
        }

        const RecordingEntry& entry = recordings_[static_cast<std::size_t>(row)];
        std::string reason;
        if (!player_.load(entry.path, reason)) {
            now_playing_->setText("Could not open this recording");
            recording_details_->setText(QString::fromStdString(reason));
            play_button_->setEnabled(false);
            stop_play_button_->setEnabled(false);
            delete_speech_->setEnabled(true);
            return;
        }

        char when[64];
        std::tm shown = entry.stamp.when;
        std::strftime(when, sizeof(when), "%d %b %Y at %H:%M", &shown);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &shown);
        now_playing_->setText(QString("recording-%1").arg(QString::fromUtf8(stamp)));
        recording_details_->setText(QString(
            "%1 long, %2 MB.  %3\n"
            "This is the original recording. Whisper is given the speech "
            "extracted from it, not this.")
            .arg(QTime(0, 0).addSecs(static_cast<int>(entry.seconds)).toString("mm:ss"),
                 QString::number(entry.size_bytes / (1024.0 * 1024.0), 'f', 1),
                 entry.has_transcript ? "Transcribed." : "Not transcribed."));
        play_button_->setEnabled(true);
        play_button_->setText("Play");
        stop_play_button_->setEnabled(true);
        delete_speech_->setEnabled(true);
        position_slider_->setEnabled(true);
        position_slider_->setRange(0, static_cast<int>(player_.duration_seconds()));
        position_slider_->setValue(0);
        refresh_play_time();
    }

    void toggle_playback() {
        if (!player_.is_loaded()) {
            return;
        }
        if (player_.is_playing()) {
            player_.pause();
            play_button_->setText("Play");
            play_timer_->stop();
        } else if (player_.play()) {
            play_button_->setText("Pause");
            play_timer_->start();
        }
        refresh_play_time();
    }

    void stop_playback() {
        player_.stop();
        play_timer_->stop();
        play_button_->setText("Play");
        position_slider_->setValue(0);
        refresh_play_time();
    }

    void refresh_play_time() {
        const int position = static_cast<int>(player_.position_seconds());
        const int total = static_cast<int>(player_.duration_seconds());
        const char* format = total >= 3600 ? "hh:mm:ss" : "mm:ss";
        play_time_->setText(QString("%1 / %2")
            .arg(QTime(0, 0).addSecs(position).toString(format),
                 QTime(0, 0).addSecs(total).toString(format)));
        if (!position_slider_->isSliderDown()) {
            const QSignalBlocker blocker(position_slider_);
            position_slider_->setValue(position);
        }
        // Playback runs on its own thread; this is how the interface learns it
        // reached the end.
        if (player_.has_finished() && player_.is_playing()) {
            stop_playback();
        }
    }

    void delete_selected_recording() {
        const int row = speech_list_->currentRow();
        if (row < 0 || row >= static_cast<int>(recordings_.size())) {
            return;
        }
        const RecordingEntry entry = recordings_[static_cast<std::size_t>(row)];
        char when[64];
        std::tm shown = entry.stamp.when;
        std::strftime(when, sizeof(when), "%d %b %Y, %H:%M", &shown);

        const auto choice = QMessageBox::question(this, "Delete recording",
            QString("Delete the recording from %1?\n\nIts transcript is kept. "
                    "This cannot be undone.").arg(QString::fromUtf8(when)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice != QMessageBox::Yes) {
            return;
        }

        // Released before deleting: the file is open for playback.
        player_.unload();
        play_timer_->stop();

        std::string reason;
        if (!delete_recording(entry.path, entry.stamp,
                              fs::path(data_directory_.toStdString()), reason)) {
            QMessageBox::warning(this, "Could not delete", QString::fromStdString(reason));
            return;
        }

        refresh_speeches();
        const int remaining = static_cast<int>(recordings_.size());
        if (remaining == 0) {
            select_recording(-1);
        } else {
            const int next = qBound(0, row, remaining - 1);
            const QSignalBlocker blocker(speech_list_);
            speech_list_->setCurrentRow(next);
            select_recording(next);
        }
        statusBar()->showMessage("Recording deleted", 3000);
    }

    void show_library() {
        if (!ui_state_may_start(state_)) {
            return;
        }
        stop_playback_if_running();
        refresh_history();
        stack_->setCurrentIndex(2);
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

    void stop_playback_if_running() {
        if (player_.is_loaded()) {
            stop_playback();
            player_.unload();
        }
    }

    void show_recorder() {
        stop_playback_if_running();
        // Microphones are plugged in and unplugged while the application runs.
        // Re-reading on arrival keeps the indices honest.
        if (ui_state_may_start(state_)) {
            refresh_devices();
        }
        stack_->setCurrentIndex(1);
        recorder_action_->setChecked(true);
        update_context_bar();
        apply_state(state_);
    }

    void toggle_library() {
        if (stack_->currentIndex() == 2) {
            show_home();
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
            auto* empty = new QListWidgetItem(
                "No saved transcripts yet\n\nGo to Recording to make one", history_list_);
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
            if (preview.size() > 44) {
                preview = preview.left(44).trimmed() + "...";
            }
            auto* item = new QListWidgetItem(
                QString("%1\n%2").arg(QString::fromUtf8(when), preview), history_list_);
            item->setToolTip(QString::fromStdString(entry.path.string()));
        }
        if (previous_row >= 0 && previous_row < history_list_->count()) {
            history_list_->setCurrentRow(previous_row);
        }
    }

    void clear_reader() {
        saved_view_->clear();
        saved_title_->setText("Select a transcript to read it");
        saved_save_->setEnabled(false);
        saved_copy_->setEnabled(false);
        delete_saved_->setEnabled(false);
    }

    void show_history_entry(int row) {
        if (row < 0 || row >= static_cast<int>(entries_.size())) {
            clear_reader();
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
        delete_saved_->setEnabled(true);
    }

    // Acts on what is on screen, which is what the button sits beneath.
    void delete_selected_transcript() {
        const int row = history_list_->currentRow();
        if (row < 0 || row >= static_cast<int>(entries_.size())) {
            return;
        }
        const TranscriptEntry& entry = entries_[static_cast<std::size_t>(row)];
        char when[64];
        std::tm shown = entry.stamp.when;
        std::strftime(when, sizeof(when), "%d %b %Y, %H:%M", &shown);
        delete_one_transcript(entry.path, QString::fromUtf8(when));
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
        if (!remove_transcript(path, reason)) {
            QMessageBox::warning(this, "Could not delete", QString::fromStdString(reason));
            return;
        }
        statusBar()->showMessage("Transcript deleted", 3000);
    }

    // The deletion itself, separated from asking: this is what decides where
    // the reader lands afterwards, and it is worth being able to exercise.
    bool remove_transcript(const fs::path& path, std::string& reason) {
        if (!delete_transcript(path, fs::path(data_directory_.toStdString()), reason)) {
            return false;
        }

        // Deleting should leave you somewhere, not staring at an empty box.
        // The row that takes the deleted one's place is the natural next thing
        // to read; at the end of the list that is the one above instead.
        const int removed_row = history_list_->currentRow();
        refresh_history();

        const int remaining = static_cast<int>(entries_.size());
        if (remaining == 0) {
            clear_reader();
        } else {
            const int next = qBound(0, removed_row, remaining - 1);
            const QSignalBlocker blocker(history_list_);
            history_list_->setCurrentRow(next);
            // Called directly: the row index may be unchanged, so relying on
            // the selection signal would leave the old text on screen.
            show_history_entry(next);
        }
        return true;
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
    void refresh_devices() {
        const QVariant chosen = device_selector_->currentData();
        device_selector_->clear();
        device_selector_->addItem("System default", -1);
        populate_devices(chosen);
    }

    void populate_devices(const QVariant& restore = QVariant()) {
        if (device_selector_->count() == 0) {
            device_selector_->addItem("System default", -1);
        }
        restore_device_ = restore;
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

    // DEVICE|index|default|name|kind. The name may itself contain a separator,
    // so it is taken as everything between the known first and last fields.
    void add_enumerated_devices(const QString& output) {
        const QStringList lines = output.split('\n');
        for (const QString& line : lines) {
            if (!line.startsWith("DEVICE|")) {
                continue;
            }
            const QStringList parts = line.split('|');
            if (parts.size() < 5) {
                continue;
            }
            const int index = parts.at(1).toInt();
            const QString kind = parts.last().trimmed();
            const QString name = parts.mid(3, parts.size() - 4).join('|').trimmed();
            const bool is_default = parts.at(2) == "default";

            QString label = name;
            if (is_default) {
                label += "  (default)";
            }
            if (kind == "monitor") {
                // Saying so is the difference between a puzzling silent
                // recording and an obviously wrong choice.
                label += "  — speaker output, not a microphone";
            }
            device_selector_->addItem(label, index);
            if (kind == "monitor") {
                const int row = device_selector_->count() - 1;
                device_selector_->setItemData(row, QColor(Qt::darkGray), Qt::ForegroundRole);
                device_selector_->setItemData(
                    row, "Records what is played through the speakers, not what you say.",
                    Qt::ToolTipRole);
            }
        }
        devices_ready_ = true;
        // Keep the user's choice across a refresh when the device is still there.
        if (restore_device_.isValid()) {
            const int row = device_selector_->findData(restore_device_);
            if (row >= 0) {
                device_selector_->setCurrentIndex(row);
            }
            restore_device_ = QVariant();
        }
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
        if (stack_->currentIndex() == 2) {
            refresh_history();
        }
        if (stack_->currentIndex() == 3) {
            refresh_speeches();
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
    QPushButton* go_record_button_;
    QLabel* home_summary_;
    QPushButton* delete_all_button_;
    QPlainTextEdit* saved_view_;
    QLabel* saved_title_;
    QPushButton* saved_save_;
    QPushButton* saved_copy_;
    QPushButton* delete_saved_;
    QListWidget* speech_list_;
    QLabel* now_playing_;
    QLabel* play_time_;
    QPushButton* play_button_;
    QPushButton* stop_play_button_;
    QPushButton* delete_speech_;
    QLabel* recording_details_;
    QSlider* position_slider_;
    std::vector<RecordingEntry> recordings_;
    AudioPlayer player_;
    QAction* history_action_;
    std::vector<TranscriptEntry> entries_;
    QLabel* context_label_;
    QString model_summary_;
    QString input_summary_;
    QString storage_summary_;
    QAction* home_action_;
    QAction* speeches_action_;
    QAction* recorder_action_;
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
    QTimer* play_timer_;
    QElapsedTimer elapsed_;
    UiState state_ = UiState::Ready;
    int limit_seconds_ = 0;
    bool devices_ready_ = false;
    QVariant restore_device_;
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
