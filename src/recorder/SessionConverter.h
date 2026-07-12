// Lyra-cpp — Session Recorder (#201), Stage 5: offline MP4 converter.
//
// Turns a recorded session folder (audio_*.wav + snap_*.png + session.json)
// into a single playable session.mp4 (H.264 + AAC), or session.m4a when the
// session has no snapshots.  Runs an external ffmpeg-class encoder as a
// SEPARATE, BELOW-NORMAL-PRIORITY process (RECORDER_DESIGN.md §6) so the
// radio's realtime audio path is never starved — the "no dropouts" design.
//
// ffmpeg is SMART-RESOLVED (operator chose "no bundle"): an explicit
// configured path → ffmpeg.exe next to lyra.exe → PATH → else not-found so
// the UI can prompt (Browse / download link).
//
// TX ⇄ convert mutual lock-out is enforced by the host: setTxActiveProbe()
// refuses a start while transmitting; setTxLockout() blocks keying while
// converting.  Both are std::function hooks so this engine stays decoupled
// from HL2Stream.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QProcess;

namespace lyra::recorder {

class SessionConverter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool    busy            READ busy            NOTIFY busyChanged)
    Q_PROPERTY(int     progress        READ progress        NOTIFY progressChanged)
    Q_PROPERTY(QString convertingDir   READ convertingDir   NOTIFY busyChanged)
    Q_PROPERTY(bool    ffmpegAvailable READ ffmpegAvailable NOTIFY ffmpegPathChanged)
    Q_PROPERTY(QString ffmpegPath      READ ffmpegPath      WRITE setFfmpegPath
               NOTIFY ffmpegPathChanged)
public:
    explicit SessionConverter(QObject *parent = nullptr);
    ~SessionConverter() override;

    bool    busy() const { return busy_; }
    int     progress() const { return progress_; }          // 0..100
    QString convertingDir() const { return convertingDir_; }

    // Effective ffmpeg to use ("" = none found).  Resolution order:
    // configured path → next-to-exe → PATH.
    QString ffmpegPath() const;
    bool    ffmpegAvailable() const { return !ffmpegPath().isEmpty(); }
    // "" clears the override and reverts to auto-resolution.  Persisted.
    void    setFfmpegPath(const QString &path);

    // Host wiring (keeps the engine free of any HL2Stream dependency).
    void setTxActiveProbe(std::function<bool()> fn) { txActive_ = std::move(fn); }
    void setTxLockout(std::function<void(bool)> fn) { txLockout_ = std::move(fn); }

    // Convert <sessionDir> → <sessionDir>/session.mp4 (or .m4a if audio-only).
    // Returns false + emits error() if it can't start (busy / TX active /
    // ffmpeg missing / no audio).  One conversion at a time.
    Q_INVOKABLE bool convert(const QString &sessionDir);
    Q_INVOKABLE void cancel();

signals:
    void busyChanged(bool on);
    void progressChanged(int pct);
    void ffmpegPathChanged();
    void finished(const QString &sessionDir, const QString &outPath);
    void error(const QString &msg);

private:
    void    onProcStdout();
    void    onProcStderr();
    void    onProcFinished(int exitCode, int exitStatus);
    void    setBusy(bool on);
    void    setProgress(int pct);
    void    cleanupTemps();
    void    applyBelowNormalPriority();

    QProcess *proc_ = nullptr;
    bool      busy_ = false;
    int       progress_ = 0;
    double    durationSec_ = 0.0;
    QString   convertingDir_;
    QString   outPath_;
    QString   errTail_;
    QStringList tempFiles_;    // list files to remove when done
    bool      canceled_ = false;

    std::function<bool()>     txActive_;
    std::function<void(bool)> txLockout_;
};

} // namespace lyra::recorder
