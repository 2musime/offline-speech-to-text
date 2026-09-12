#include "partial_text.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFile>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QTime>
#include <QTextCursor>
#include <QVBoxLayout>

// Every control's enabled state is a function of this, and of nothing else.
enum class UiState {
    Ready,
    LoadingModel,
    Recording,
    Stopping,
    Processing,
    Completed,
    Error
};

class AudioToTextWindow final : public QMainWindow {
public:
    AudioToTextWindow() {
        setWindowTitle("Audio to Text");
        resize(760, 560);

        auto* central = new QWidget(this);
        auto* layout = new QVBoxLayout(central);
        auto* controls = new QHBoxLayout();

        model_selector_ = new QComboBox(central);
        model_selector_->addItem("Accuracy: small.en", "models/ggml-small.en.bin");
        model_selector_->addItem("Speed: base.en", "models/ggml-base.en.bin");

        duration_selector_ = new QComboBox(central);
        duration_selector_->addItem("15 seconds", 15);
        duration_selector_->addItem("45 seconds", 45);
        duration_selector_->addItem("60 seconds", 60);

        start_button_ = new QPushButton("Start Recording", central);
        stop_button_ = new QPushButton("Stop Recording", central);
        stop_button_->setEnabled(false);

        controls->addWidget(new QLabel("Model:", central));
        controls->addWidget(model_selector_, 1);
        controls->addWidget(new QLabel("Limit:", central));
        controls->addWidget(duration_selector_);
        controls->addWidget(start_button_);
        controls->addWidget(stop_button_);
        layout->addLayout(controls);

        auto* input_controls = new QHBoxLayout();
        device_selector_ = new QComboBox(central);
        input_controls->addWidget(new QLabel("Microphone:", central));
        input_controls->addWidget(device_selector_, 1);
        layout->addLayout(input_controls);

        status_label_ = new QLabel("Ready", central);
        duration_label_ = new QLabel("Duration: 00:00", central);
        model_label_ = new QLabel("Model: not loaded", central);
        input_label_ = new QLabel("Input: not selected", central);
        latency_label_ = new QLabel("", central);
        progress_ = new QProgressBar(central);
        progress_->setTextVisible(false);
        progress_->setRange(0, 100);
        progress_->setValue(0);
        layout->addWidget(status_label_);
        layout->addWidget(duration_label_);
        layout->addWidget(model_label_);
        layout->addWidget(input_label_);
        layout->addWidget(latency_label_);
        layout->addWidget(progress_);

        transcript_ = new QPlainTextEdit(central);
        transcript_->setPlaceholderText("Transcription will appear here...");
        transcript_->setReadOnly(true);
        layout->addWidget(transcript_, 1);

        auto* actions = new QHBoxLayout();
        save_button_ = new QPushButton("Save", central);
        copy_button_ = new QPushButton("Copy", central);
        save_button_->setEnabled(false);
        copy_button_->setEnabled(false);
        actions->addStretch();
        actions->addWidget(save_button_);
        actions->addWidget(copy_button_);
        layout->addLayout(actions);
        setCentralWidget(central);

        process_ = new QProcess(this);
        process_->setProcessChannelMode(QProcess::MergedChannels);
        timer_ = new QTimer(this);
        timer_->setInterval(250);
        limit_timer_ = new QTimer(this);
        limit_timer_->setSingleShot(true);

        populate_devices();

        connect(start_button_, &QPushButton::clicked, this, [this] { start_recording(); });
        connect(stop_button_, &QPushButton::clicked, this, [this] { stop_recording(); });
        connect(copy_button_, &QPushButton::clicked, this, [this] {
            QApplication::clipboard()->setText(transcript_->toPlainText());
        });
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
        if (state_ != UiState::Ready && state_ != UiState::Completed && state_ != UiState::Error) {
            return;
        }
        if (process_->state() != QProcess::NotRunning) {
            return;
        }

        transcript_->clear();
        final_transcription_.clear();
        transcript_path_.clear();
        model_label_->setText("Model: validating...");
        input_label_->setText("Input: opening...");
        latency_label_->clear();
        partial_words_.clear();
        partial_trimmed_ = false;
        completed_ = false;
        error_shown_ = false;
        const QString model = model_selector_->currentData().toString();
        const QString duration = duration_selector_->currentData().toString();
        // No --threads: the worker's measured default is the single source of truth.
        QStringList arguments{model, "--stream", "--duration", duration};
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
        set_status("Recording");
        refresh_elapsed();
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
            input_label_->setText("Input: " + line.section('|', 1));
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
                apply_state(UiState::Processing);
                set_status("Transcribing...");
            }
            return;
        }

    }

    // The single place any control's enabled state is decided.
    void apply_state(UiState state) {
        state_ = state;

        const bool idle = state == UiState::Ready || state == UiState::Completed ||
            state == UiState::Error;
        start_button_->setEnabled(idle);
        stop_button_->setEnabled(state == UiState::Recording);
        // Changing the model or device mid-run would not affect the worker that
        // is already running, so the controls stay locked until it finishes.
        model_selector_->setEnabled(idle);
        duration_selector_->setEnabled(idle);
        device_selector_->setEnabled(idle && devices_ready_);

        const bool has_text = !transcript_->toPlainText().trimmed().isEmpty();
        save_button_->setEnabled(state == UiState::Completed || (state == UiState::Error && has_text));
        copy_button_->setEnabled(save_button_->isEnabled());

        switch (state) {
            case UiState::LoadingModel:
            case UiState::Stopping:
            case UiState::Processing:
                // Indeterminate: the worker gives no progress fraction here.
                progress_->setRange(0, 0);
                break;
            case UiState::Recording:
                progress_->setRange(0, limit_seconds_ > 0 ? limit_seconds_ : 100);
                break;
            default:
                progress_->setRange(0, 100);
                progress_->setValue(0);
                break;
        }
    }

    void refresh_elapsed() {
        if (state_ != UiState::Recording) {
            return;
        }
        // Monotonic: unaffected by clock changes or midnight rollover.
        const qint64 seconds = elapsed_.elapsed() / 1000;
        duration_label_->setText(QString("Duration: %1 of %2")
            .arg(QTime(0, 0).addSecs(static_cast<int>(seconds)).toString("mm:ss"),
                 QTime(0, 0).addSecs(limit_seconds_).toString("mm:ss")));
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
        model_label_->setText(QString("Model: %1, %2, %3 MB")
            .arg(variant, language, QString::number(megabytes, 'f', 1)));
    }

    // The worker announces each artefact as SAVED|KIND|path.
    void handle_saved_line(const QString& line) {
        const QStringList parts = line.split('|');
        if (parts.size() < 3 || parts.at(1) != "TRANSCRIPT") {
            return;
        }

        transcript_path_ = parts.mid(2).join('|');
        completed_ = true;
        apply_state(UiState::Completed);
        set_status("Transcription complete");
        process_->write("q\n");
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
        const bool busy = state_ == UiState::Recording || state_ == UiState::Stopping ||
            state_ == UiState::Processing || state_ == UiState::LoadingModel;
        if (busy) {
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
    QLabel* model_label_;
    QLabel* input_label_;
    QLabel* latency_label_;
    QProgressBar* progress_;
    QPlainTextEdit* transcript_;
    QProcess* process_;
    QTimer* timer_;
    QTimer* limit_timer_;
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
};

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    AudioToTextWindow window;
    window.show();
    return application.exec();
}