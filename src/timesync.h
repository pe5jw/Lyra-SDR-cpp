// Lyra — PC clock-drift check against network time (ported from old
// Lyra's time_sync.py).
//
// The NCDXF beacon rotation is UTC-locked on 10-second slots, so a PC
// clock that's off by more than a couple seconds makes the beacon
// markers / Follow track the WRONG station.  This does an on-demand
// NTP query (pure Qt UDP — no extra deps), reports the offset + a
// severity (ok < 1 s, warn 1–3 s, bad ≥ 3 s), and can shell
// `w32tm /resync` to fix it.  Purely advisory — it does NOT silently
// rewrite the beacon math (matches old Lyra); it tells the operator.

#pragma once

#include <QHostInfo>
#include <QObject>
#include <QString>
#include <QStringList>

class QUdpSocket;
class QTimer;

namespace lyra::ui {

class TimeSync : public QObject {
    Q_OBJECT
public:
    enum Severity { Unknown = 0, Ok, Warn, Bad, Failed };

    explicit TimeSync(QObject *parent = nullptr);

    // Kick an async NTP query (tries servers in order).  Emits result()
    // on success or failed() if all servers fail/time out.
    void checkDrift();
    // Best-effort `w32tm /resync` (may need the Windows Time service /
    // elevation); emits resyncDone().
    void windowsResync();

    Severity severity() const { return severity_; }
    double   offsetSec() const { return offset_; }

signals:
    // offsetSec > 0 means the PC clock is AHEAD of true UTC.
    void result(double offsetSec, const QString &server, int severity,
                const QString &summary);
    void failed(const QString &reason);
    void resyncDone(bool ok, const QString &message);

private:
    void tryNext();
    void onResolved(const QHostInfo &info);
    void onReadyRead();
    void onTimeout();
    void finishOk(double offsetSec);
    static Severity classify(double absSec);

    QStringList     servers_;
    int             idx_ = 0;
    QUdpSocket     *sock_  = nullptr;
    QTimer         *timer_ = nullptr;
    double          t1_ = 0.0;       // local send time (Unix sec)
    bool            inFlight_ = false;
    Severity        severity_ = Unknown;
    double          offset_   = 0.0;
};

} // namespace lyra::ui
