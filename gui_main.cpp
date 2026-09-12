#include <QApplication>
#include <QClipboard>
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
        layout->addWidget(status_label_);
        layout->addWidget(duration_label_);

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
        connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
            set_idle("Worker error: " + process_->errorString());
        });
        connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                this, [this](int, QProcess::ExitStatus) { set_idle("Ready"); });
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
        if (line.contains("Recording reached the") && line.contains("second limit")) {
            set_status("Recording stopped at the selected limit; processing...");
            return;
        }
        if (line.contains("Saved transcription.txt")) {
            save_button_->setEnabled(true);
            copy_button_->setEnabled(true);
            set_status("Transcription complete");
            process_->write("q\n");
            return;
        }
        if (line.contains("Could not open the microphone")) {
            set_idle("Microphone error: could not open the microphone.");
            return;
        }
        if (line.contains("Could not load Whisper model")) {
            set_idle("Model error: could not load the selected model.");
        }
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
        const QString path = QFileDialog::getSaveFileName(this, "Save transcription", "transcription.txt");
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
    QPlainTextEdit* transcript_;
    QProcess* process_;
    QTimer* timer_;
    QTimer* limit_timer_;
    QTime started_at_;
    QString output_buffer_;
    QString pending_text_label_;
    QString final_transcription_;
};

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    AudioToTextWindow window;
    window.show();
    return application.exec();
}