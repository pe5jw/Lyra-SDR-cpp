// Lyra — PC clock-drift check against network time.  See timesync.h.

#include "timesync.h"

#include <QDateTime>
#include <QHostAddress>
#include <QProcess>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>

namespace lyra::ui {

namespace {
constexpr quint64 kNtpUnixEpochDelta = 2208988800ULL;  // 1900→1970 seconds
double nowUnix() {
    return double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}
// Read a 64-bit NTP timestamp (seconds.fraction since 1900) at offset,
// return Unix seconds as a double.
double readNtpTs(const QByteArray &b, int off) {
    if (b.size() < off + 8) return 0.0;
    const auto *p = reinterpret_cast<const uchar *>(b.constData()) + off;
    const quint32 sec  = qFromBigEndian<quint32>(p);
    const quint32 frac = qFromBigEndian<quint32>(p + 4);
    return double(sec) - double(kNtpUnixEpochDelta) + double(frac) / 4294967296.0;
}
} // namespace

TimeSync::TimeSync(QObject *parent) : QObject(parent) {
    servers_ = {QStringLiteral("time.cloudflare.com"),
                QStringLiteral("pool.ntp.org"),
                QStringLiteral("time.google.com"),
                QStringLiteral("time.windows.com")};
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(3000);
    connect(timer_, &QTimer::timeout, this, &TimeSync::onTimeout);
}

TimeSync::Severity TimeSync::classify(double absSec) {
    if (absSec < 1.0) return Ok;
    if (absSec < 3.0) return Warn;
    return Bad;
}

void TimeSync::checkDrift() {
    if (inFlight_) return;
    inFlight_ = true;
    idx_ = 0;
    tryNext();
}

void TimeSync::tryNext() {
    if (idx_ >= servers_.size()) {
        inFlight_ = false;
        emit failed(tr("No network time server could be reached."));
        return;
    }
    QHostInfo::lookupHost(servers_[idx_], this,
                          [this](const QHostInfo &i) { onResolved(i); });
}

void TimeSync::onResolved(const QHostInfo &info) {
    QHostAddress addr;
    for (const QHostAddress &a : info.addresses()) {
        if (a.protocol() == QAbstractSocket::IPv4Protocol) { addr = a; break; }
    }
    if (addr.isNull()) {           // DNS failed → next server
        ++idx_;
        tryNext();
        return;
    }
    if (!sock_) {
        sock_ = new QUdpSocket(this);
        connect(sock_, &QUdpSocket::readyRead, this, &TimeSync::onReadyRead);
    }
    // NTPv4 client request: 48 bytes, first byte LI=0 VN=3 Mode=3 (0x1B).
    QByteArray pkt(48, '\0');
    pkt[0] = char(0x1B);
    t1_ = nowUnix();
    sock_->writeDatagram(pkt, addr, 123);
    timer_->start();
}

void TimeSync::onReadyRead() {
    if (!sock_->hasPendingDatagrams()) return;
    QByteArray buf(int(sock_->pendingDatagramSize()), '\0');
    sock_->readDatagram(buf.data(), buf.size());
    timer_->stop();
    if (buf.size() < 48) { ++idx_; tryNext(); return; }
    const double t4 = nowUnix();
    const double t2 = readNtpTs(buf, 32);   // server receive timestamp
    const double t3 = readNtpTs(buf, 40);   // server transmit timestamp
    // Standard NTP offset.
    const double offset = ((t2 - t1_) + (t3 - t4)) / 2.0;
    finishOk(offset);
}

void TimeSync::finishOk(double offsetSec) {
    inFlight_ = false;
    offset_ = offsetSec;
    severity_ = classify(qAbs(offsetSec));
    const QString sign = offsetSec >= 0 ? QStringLiteral("+") : QStringLiteral("-");
    const double a = qAbs(offsetSec);
    QString summary = QStringLiteral("%1%2 sec").arg(sign).arg(a, 0, 'f', 2);
    if (severity_ == Ok)
        summary += tr(" (clock OK)");
    else
        summary += offsetSec >= 0
            ? tr(" (clock %1 sec ahead of network time)").arg(a, 0, 'f', 1)
            : tr(" (clock %1 sec behind network time)").arg(a, 0, 'f', 1);
    emit result(offsetSec, servers_.value(idx_), int(severity_), summary);
}

void TimeSync::onTimeout() {
    if (sock_) sock_->abort();
    ++idx_;
    tryNext();
}

void TimeSync::windowsResync() {
    auto *proc = new QProcess(this);
    connect(proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc](int code, QProcess::ExitStatus) {
        const QString out = QString::fromLocal8Bit(
            proc->readAllStandardOutput() + proc->readAllStandardError());
        proc->deleteLater();
        if (code == 0)
            emit resyncDone(true, tr("Windows time re-synchronised."));
        else
            emit resyncDone(false, out.trimmed().isEmpty()
                ? tr("w32tm /resync failed (you may need to run as "
                     "administrator, or start the Windows Time service).")
                : out.trimmed());
    });
    proc->start(QStringLiteral("w32tm"), {QStringLiteral("/resync")});
    if (!proc->waitForStarted(2000)) {
        proc->deleteLater();
        emit resyncDone(false, tr("Could not run w32tm."));
    }
}

} // namespace lyra::ui
