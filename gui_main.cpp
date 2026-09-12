#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QFile>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QTime>
#include <QTextCursor>
#include <QVBoxLayout>

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

        status_label_ = new QLabel("Ready", central);
        duration_label_ = new QLabel("Duration: 00:00", central);
        model_label_ = new QLabel("Model: not loaded", central);
        layout->addWidget(status_label_);
        layout->addWidget(duration_label_);
        layout->addWidget(model_label_);

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
            set_idle("Audio worker problem: " + process_->errorString());
        });
        connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                this, [this](int exit_code, QProcess::ExitStatus status) { handle_worker_exit(exit_code, status); });
        connect(timer_, &QTimer::timeout, this, [this] {
            const int seconds = started_at_.secsTo(QTime::currentTime());
            duration_label_->setText("Duration: " + QTime(0, 0).addSecs(seconds).toString("mm:ss"));
        });
        connect(limit_timer_, &QTimer::timeout, this, [this] {
            set_status("Recording limit reached; stopping recording...");
            stop_recording();
        });
    }

private:
    void start_recording() {
        if (process_->state() != QProcess::NotRunning) {
            return;
        }

        transcript_->clear();
        final_transcription_.clear();
        transcript_path_.clear();
        model_label_->setText("Model: validating...");
        completed_ = false;
        error_shown_ = false;
        const QString model = model_selector_->currentData().toString();
        const QString duration = duration_selector_->currentData().toString();
        process_->start(QCoreApplication::applicationDirPath() + "/audio_to_text_cli",
                {model, "--stream", "--threads", "4", "--duration", duration});
        if (!process_->waitForStarted(1000)) {
            set_idle("Could not start the audio worker.");
            return;
        }
        process_->write("\n");
        start_button_->setEnabled(false);
        stop_button_->setEnabled(true);
        model_selector_->setEnabled(false);
        duration_selector_->setEnabled(false);
        started_at_ = QTime::currentTime();
        timer_->start();
        limit_timer_->start(duration_selector_->currentData().toInt() * 1000);
        set_status("Recording");
    }

    void stop_recording() {
        if (process_->state() == QProcess::Running) {
            process_->write("\n");
            set_status("Finishing transcription...");
            stop_button_->setEnabled(false);
        }
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

        if (!pending_text_label_.isEmpty()) {
            if (pending_text_label_ == "partial") {
                append_partial_text(line);
            } else if (pending_text_label_ == "final") {
                final_transcription_ = line;
                render_final_transcription();
            } else {
                transcript_->setPlainText(line);
            }
            pending_text_label_.clear();
            return;
        }

        if (line.startsWith("Partial (")) {
            pending_text_label_ = "partial";
            return;
        }
        if (line == "Transcription:") {
            pending_text_label_ = "final";
            return;
        }
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
        save_button_->setEnabled(true);
        copy_button_->setEnabled(true);
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

        pending_text_label_.clear();
        error_shown_ = true;
        set_idle(actionable_message(category, message));
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
            // Keep the reported cause on screen instead of replacing it with "Ready".
            set_idle(status_label_->text());
            return;
        }
        if (status == QProcess::CrashExit) {
            set_idle("The audio worker stopped unexpectedly. Start recording to try again.");
            return;
        }
        if (exit_code != 0) {
            set_idle("The audio worker exited with code " + QString::number(exit_code) + ".");
            return;
        }
        set_idle(completed_ ? "Transcription complete" : "Ready");
    }

    void append_partial_text(const QString& incoming) {
        const QString addition = incoming.trimmed();
        if (addition.isEmpty()) {
            return;
        }

        QString current = transcript_->toPlainText().trimmed();
        if (current.isEmpty()) {
            transcript_->setPlainText(addition);
            return;
        }

        int best_length = 0;
        int best_position = 0;
        const int maximum_overlap = qMin(80, current.size());
        for (int length = maximum_overlap; length >= 4; --length) {
            const QString suffix = current.right(length);
            const int position = addition.indexOf(suffix, 0, Qt::CaseInsensitive);
            if (position >= 0 && position <= 40) {
                best_length = length;
                best_position = position;
                break;
            }
        }

        QString new_text = current;
        if (best_length > 0) {
            const QString remainder = addition.mid(best_position + best_length).trimmed();
            if (!remainder.isEmpty()) {
                new_text += current.endsWith(' ') ? remainder : " " + remainder;
            }
        } else {
            new_text += current.endsWith(' ') ? addition : " " + addition;
        }
        transcript_->setPlainText(new_text);
        transcript_->moveCursor(QTextCursor::End);
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
        closing_ = true;
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
        start_button_->setEnabled(true);
        stop_button_->setEnabled(false);
        model_selector_->setEnabled(true);
        duration_selector_->setEnabled(true);
        set_status(status);
    }

    QComboBox* model_selector_;
    QComboBox* duration_selector_;
    QPushButton* start_button_;
    QPushButton* stop_button_;
    QPushButton* save_button_;
    QPushButton* copy_button_;
    QLabel* status_label_;
    QLabel* duration_label_;
    QLabel* model_label_;
    QPlainTextEdit* transcript_;
    QProcess* process_;
    QTimer* timer_;
    QTimer* limit_timer_;
    QTime started_at_;
    QString output_buffer_;
    QString pending_text_label_;
    QString final_transcription_;
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