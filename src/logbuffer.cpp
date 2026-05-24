// Lyra — in-app diagnostic log buffer.  See logbuffer.h.

#include "logbuffer.h"

#include <QDateTime>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

namespace lyra::ui {

LogBuffer &LogBuffer::instance() {
    static LogBuffer inst;
    return inst;
}

void LogBuffer::install() {
    QMutexLocker lock(&mutex_);
    if (installed_) return;

    verbose_ = QSettings().value(QStringLiteral("debug/logging"), false).toBool();

    // Log file under the app-data dir (e.g.
    // %APPDATA%/N8SDR/Lyra-cpp/logs/lyra-log.txt).  Truncated each
    // launch so the file reflects the current session.
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/logs");
    QDir().mkpath(dir);
    filePath_ = dir + QStringLiteral("/lyra-log.txt");
    file_.setFileName(filePath_);
    if (file_.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        stream_.setDevice(&file_);
        stream_ << QStringLiteral("=== Lyra log — %1 ===\n")
                       .arg(QDateTime::currentDateTime()
                                .toString(Qt::ISODate));
        stream_.flush();
    }

    prev_ = qInstallMessageHandler(&LogBuffer::handler);
    installed_ = true;
}

void LogBuffer::setVerbose(bool on) {
    QMutexLocker lock(&mutex_);
    verbose_ = on;
}

QString LogBuffer::text() const {
    QMutexLocker lock(&mutex_);
    return lines_.join(QLatin1Char('\n'));
}

void LogBuffer::clear() {
    QMutexLocker lock(&mutex_);
    lines_.clear();
}

void LogBuffer::handler(QtMsgType type, const QMessageLogContext &ctx,
                        const QString &msg) {
    LogBuffer &self = instance();
    // Format: HH:mm:ss.zzz  LEVEL  [category]  message
    const char *lvl = "INFO ";
    switch (type) {
    case QtDebugMsg:    lvl = "DEBUG"; break;
    case QtInfoMsg:     lvl = "INFO "; break;
    case QtWarningMsg:  lvl = "WARN "; break;
    case QtCriticalMsg: lvl = "ERROR"; break;
    case QtFatalMsg:    lvl = "FATAL"; break;
    }
    QString line = QStringLiteral("%1  %2  ")
                       .arg(QDateTime::currentDateTime().toString(
                                QStringLiteral("HH:mm:ss.zzz")))
                       .arg(QString::fromLatin1(lvl));
    if (ctx.category && qstrcmp(ctx.category, "default") != 0)
        line += QStringLiteral("[%1] ").arg(QString::fromLatin1(ctx.category));
    line += msg;

    self.append(type, line);

    // Chain to the previous handler (a console-subsystem debug build
    // still prints; release GUI builds have none and this is a no-op).
    if (self.prev_) self.prev_(type, ctx, msg);
}

void LogBuffer::append(QtMsgType type, const QString &line) {
    {
        QMutexLocker lock(&mutex_);
        // Verbose OFF: drop Debug/Info — keep the file small + quiet.
        if (!verbose_ && (type == QtDebugMsg || type == QtInfoMsg))
            return;
        lines_.append(line);
        if (lines_.size() > kMaxLines)
            lines_.remove(0, lines_.size() - kMaxLines);
        if (file_.isOpen()) {
            stream_ << line << '\n';
            stream_.flush();
        }
    }
    emit lineAppended(line);
}

} // namespace lyra::ui
