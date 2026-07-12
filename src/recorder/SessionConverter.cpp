// Lyra-cpp — Session Recorder (#201), Stage 5: offline MP4 converter impl.

#include "recorder/SessionConverter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace lyra::recorder {

SessionConverter::SessionConverter(QObject *parent) : QObject(parent) {}

SessionConverter::~SessionConverter() {
    if (proc_) {
        proc_->disconnect(this);
        if (proc_->state() != QProcess::NotRunning) {
            proc_->kill();
            proc_->waitForFinished(2000);
        }
    }
    cleanupTemps();
}

// ── ffmpeg resolution ────────────────────────────────────────────────────────
QString SessionConverter::ffmpegPath() const {
    // 1) explicit operator-configured path.
    const QString cfg =
        QSettings().value(QStringLiteral("recorder/ffmpegPath")).toString();
    if (!cfg.isEmpty() && QFileInfo(cfg).isExecutable())
        return cfg;
    // 2) alongside lyra.exe.
    const QString beside =
        QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
            QStringLiteral("ffmpeg.exe"));
#else
            QStringLiteral("ffmpeg"));
#endif
    if (QFileInfo(beside).isExecutable())
        return beside;
    // 3) on PATH.
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    return onPath;   // "" if not found
}

void SessionConverter::setFfmpegPath(const QString &path) {
    QSettings().setValue(QStringLiteral("recorder/ffmpegPath"), path);
    emit ffmpegPathChanged();
}

// ── convert ──────────────────────────────────────────────────────────────────
bool SessionConverter::convert(const QString &sessionDir) {
    if (busy_) {
        emit error(tr("A conversion is already running."));
        return false;
    }
    if (txActive_ && txActive_()) {
        emit error(tr("Can't convert while transmitting — unkey first, then "
                      "Convert."));
        return false;
    }
    const QString ff = ffmpegPath();
    if (ff.isEmpty()) {
        emit error(tr("ffmpeg was not found.  Set its path in "
                      "Settings → Recording, or put ffmpeg.exe next to Lyra."));
        return false;
    }
    QDir d(sessionDir);
    if (!d.exists()) {
        emit error(tr("Session folder not found: %1").arg(sessionDir));
        return false;
    }

    const QStringList audio =
        d.entryList({QStringLiteral("audio_*.wav")}, QDir::Files, QDir::Name);
    if (audio.isEmpty()) {
        emit error(tr("No recorded audio in this session."));
        return false;
    }
    const QStringList snaps =
        d.entryList({QStringLiteral("snap_*.png")}, QDir::Files, QDir::Name);

    // Duration (for the progress % + per-snapshot on-screen time).
    durationSec_ = 0.0;
    {
        QFile mf(d.filePath(QStringLiteral("session.json")));
        if (mf.open(QIODevice::ReadOnly)) {
            const auto doc = QJsonDocument::fromJson(mf.readAll());
            if (doc.isObject())
                durationSec_ = doc.object().value(QStringLiteral("durationSec"))
                                   .toDouble();
        }
    }

    auto fwd = [](const QString &p) {
        // concat demuxer wants forward slashes + single-quoted paths.
        QString s = QDir::fromNativeSeparators(p);
        s.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        return s;
    };
    auto writeList = [&](const QString &name, const QString &content) -> QString {
        const QString path = d.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            return QString();
        QTextStream ts(&f);
        ts << content;
        f.close();
        return path;
    };

    // Audio concat list (one entry works too — uniform path).
    QString aContent;
    for (const QString &a : audio)
        aContent += QStringLiteral("file '%1'\n")
                        .arg(fwd(QFileInfo(d.filePath(a)).absoluteFilePath()));
    const QString aList = writeList(QStringLiteral(".mp4_alist.txt"), aContent);
    if (aList.isEmpty()) {
        emit error(tr("Couldn't write the converter work file (disk full or "
                      "read-only folder?)."));
        return false;
    }
    tempFiles_.clear();
    tempFiles_ << aList;

    const bool video = !snaps.isEmpty();
    QStringList args{QStringLiteral("-y")};
    if (video) {
        const double perImg =
            durationSec_ > 0.0 ? durationSec_ / snaps.size() : 2.0;
        QString vContent;
        for (const QString &s : snaps)
            vContent += QStringLiteral("file '%1'\nduration %2\n")
                            .arg(fwd(QFileInfo(d.filePath(s)).absoluteFilePath()))
                            .arg(perImg, 0, 'f', 3);
        // concat demuxer honours the last image's duration only if it's
        // repeated once more.
        vContent += QStringLiteral("file '%1'\n")
                        .arg(fwd(QFileInfo(d.filePath(snaps.last()))
                                     .absoluteFilePath()));
        const QString vList = writeList(QStringLiteral(".mp4_vlist.txt"), vContent);
        if (vList.isEmpty()) {
            cleanupTemps();
            emit error(tr("Couldn't write the converter work file."));
            return false;
        }
        tempFiles_ << vList;

        outPath_ = d.filePath(QStringLiteral("session.mp4"));
        args << QStringLiteral("-f") << QStringLiteral("concat")
             << QStringLiteral("-safe") << QStringLiteral("0")
             << QStringLiteral("-i") << vList
             << QStringLiteral("-f") << QStringLiteral("concat")
             << QStringLiteral("-safe") << QStringLiteral("0")
             << QStringLiteral("-i") << aList
             << QStringLiteral("-map") << QStringLiteral("0:v")
             << QStringLiteral("-map") << QStringLiteral("1:a")
             // even dimensions + yuv420p so H.264 + every player is happy.
             << QStringLiteral("-vf")
             << QStringLiteral("scale=trunc(iw/2)*2:trunc(ih/2)*2,format=yuv420p")
             << QStringLiteral("-r") << QStringLiteral("30")
             << QStringLiteral("-c:v") << QStringLiteral("libx264")
             << QStringLiteral("-preset") << QStringLiteral("veryfast")
             << QStringLiteral("-crf") << QStringLiteral("23")
             << QStringLiteral("-c:a") << QStringLiteral("aac")
             << QStringLiteral("-b:a") << QStringLiteral("256k")
             << QStringLiteral("-shortest");
    } else {
        // Audio-only session → audio-only export (RECORDER_DESIGN.md §6).
        outPath_ = d.filePath(QStringLiteral("session.m4a"));
        args << QStringLiteral("-f") << QStringLiteral("concat")
             << QStringLiteral("-safe") << QStringLiteral("0")
             << QStringLiteral("-i") << aList
             << QStringLiteral("-c:a") << QStringLiteral("aac")
             << QStringLiteral("-b:a") << QStringLiteral("256k");
    }
    args << QStringLiteral("-progress") << QStringLiteral("pipe:1")
         << QStringLiteral("-nostats")
         << QStringLiteral("-loglevel") << QStringLiteral("error")
         << outPath_;

    convertingDir_ = sessionDir;
    progress_ = 0;
    errTail_.clear();
    canceled_ = false;

    proc_ = new QProcess(this);
    connect(proc_, &QProcess::readyReadStandardOutput,
            this, &SessionConverter::onProcStdout);
    connect(proc_, &QProcess::readyReadStandardError,
            this, &SessionConverter::onProcStderr);
    connect(proc_, &QProcess::started,
            this, &SessionConverter::applyBelowNormalPriority);
    connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus st) {
                onProcFinished(code, int(st));
            });

    proc_->start(ff, args);
    setBusy(true);
    if (txLockout_) txLockout_(true);
    return true;
}

void SessionConverter::cancel() {
    if (!proc_ || proc_->state() == QProcess::NotRunning) return;
    canceled_ = true;
    proc_->kill();   // onProcFinished cleans up
}

// ── process I/O ──────────────────────────────────────────────────────────────
void SessionConverter::onProcStdout() {
    if (!proc_) return;
    const QByteArray chunk = proc_->readAllStandardOutput();
    const QList<QByteArray> lines = chunk.split('\n');
    for (const QByteArray &ln : lines) {
        if (ln.startsWith("out_time_us=")) {
            bool ok = false;
            const qint64 us = ln.mid(12).trimmed().toLongLong(&ok);
            if (ok && durationSec_ > 0.0) {
                int pct = int((double(us) / 1.0e6) / durationSec_ * 100.0);
                if (pct < 0) pct = 0;
                if (pct > 99) pct = 99;   // 100 only on progress=end
                setProgress(pct);
            }
        } else if (ln.startsWith("progress=end")) {
            setProgress(100);
        }
    }
}

void SessionConverter::onProcStderr() {
    if (!proc_) return;
    errTail_ += QString::fromLocal8Bit(proc_->readAllStandardError());
    if (errTail_.size() > 4000)                 // keep only the tail
        errTail_ = errTail_.right(4000);
}

void SessionConverter::onProcFinished(int exitCode, int exitStatus) {
    const bool ok = !canceled_ && exitStatus == int(QProcess::NormalExit) &&
                    exitCode == 0 && QFileInfo(outPath_).exists();
    const QString out = outPath_;
    const QString dir = convertingDir_;
    const bool wasCanceled = canceled_;
    const QString err = errTail_.trimmed();

    cleanupTemps();
    if (proc_) { proc_->deleteLater(); proc_ = nullptr; }
    if (txLockout_) txLockout_(false);
    setBusy(false);

    if (ok) {
        setProgress(100);
        emit finished(dir, out);
    } else if (wasCanceled) {
        QFile::remove(out);   // drop the partial file
        emit error(tr("Conversion canceled."));
    } else {
        QFile::remove(out);
        emit error(err.isEmpty()
                       ? tr("Conversion failed (exit %1).").arg(exitCode)
                       : tr("Conversion failed: %1").arg(err.right(300)));
    }
}

// ── helpers ──────────────────────────────────────────────────────────────────
void SessionConverter::setBusy(bool on) {
    if (busy_ == on) return;
    busy_ = on;
    emit busyChanged(on);
}

void SessionConverter::setProgress(int pct) {
    if (progress_ == pct) return;
    progress_ = pct;
    emit progressChanged(pct);
}

void SessionConverter::cleanupTemps() {
    for (const QString &f : tempFiles_) QFile::remove(f);
    tempFiles_.clear();
}

void SessionConverter::applyBelowNormalPriority() {
#ifdef Q_OS_WIN
    if (!proc_) return;
    const qint64 pid = proc_->processId();
    if (pid <= 0) return;
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, DWORD(pid));
    if (h) {
        SetPriorityClass(h, BELOW_NORMAL_PRIORITY_CLASS);
        CloseHandle(h);
    }
#endif
}

} // namespace lyra::recorder
