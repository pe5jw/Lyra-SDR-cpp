// Lyra — in-app diagnostic log buffer.
//
// Release builds are GUI-subsystem (no console window — see CMake
// WIN32_EXECUTABLE), so qDebug/qWarning/qCritical output would
// otherwise vanish.  LogBuffer installs a Qt message handler that:
//   * keeps the most recent lines in a thread-safe ring (shown live in
//     the Help → Show Log… viewer that a non-technical operator can
//     Copy/Save and send to us),
//   * always mirrors them to a rolling log file in the app-data dir
//     (so even a crash leaves a trace), and
//   * chains to the previous handler (a debug build's console still
//     prints).
//
// The DEBUG toggle (Settings → Hardware) drives verbose(): OFF keeps
// only Warning/Critical/Fatal (a small, quiet file); ON also captures
// Debug/Info for when we need the full picture.

#pragma once

#include <QFile>
#include <QMutex>
#include <QObject>
#include <QStringList>
#include <QTextStream>

namespace lyra::ui {

class LogBuffer : public QObject {
    Q_OBJECT
public:
    static LogBuffer &instance();

    // Open the log file (truncated fresh each launch) and install the
    // Qt message handler.  Safe to call once, early in main() after the
    // org/app name are set (so the app-data path resolves).
    void install();

    // Verbose = also capture Debug/Info (not just Warning+).  Live-
    // toggleable from Settings.  Default read from QSettings on install.
    void setVerbose(bool on);
    bool verbose() const { return verbose_; }

    // Snapshot of the current buffer (newest last), newline-joined.
    QString text() const;
    void    clear();
    QString logFilePath() const { return filePath_; }

signals:
    // Emitted (queued to the receiver's thread) for each new line so the
    // open Log viewer updates live.  The message handler can fire from
    // any thread, so connect with the default AutoConnection.
    void lineAppended(const QString &line);

private:
    explicit LogBuffer(QObject *parent = nullptr) : QObject(parent) {}
    static void handler(QtMsgType type, const QMessageLogContext &ctx,
                        const QString &msg);
    void append(QtMsgType type, const QString &line);

    mutable QMutex     mutex_;
    QStringList        lines_;          // ring, capped at kMaxLines
    QFile              file_;
    QTextStream        stream_;
    QString            filePath_;
    bool               verbose_   = false;
    bool               installed_ = false;
    QtMessageHandler   prev_      = nullptr;

    static constexpr int kMaxLines = 5000;
};

} // namespace lyra::ui
