// Lyra — DX-cluster telnet spot feeder.  See dxcluster_feeder.h.

#include "dxcluster_feeder.h"

#include "prefs.h"
#include "spotstore.h"

#include <QRegularExpression>
#include <QSettings>
#include <QTcpSocket>
#include <QTimer>

#include <cmath>

namespace lyra::ui {

namespace {
constexpr auto kKeyEnabled = "spots/telnet/enabled";
constexpr auto kKeyHost    = "spots/telnet/host";
constexpr auto kKeyPort    = "spots/telnet/port";
constexpr auto kKeyCall    = "spots/telnet/login_call";
constexpr auto kKeyCmds    = "spots/telnet/login_commands";
constexpr auto kSourceTag  = "telnet";
constexpr int  kRetryMs    = 20000;   // reconnect backoff while enabled
constexpr int  kLoginToMs  = 3000;    // send the call this long after connect if no prompt

// Known mode keywords a cluster comment may carry (so the mode filter works).
QString modeFromComment(const QString &comment) {
    static const QStringList kModes = {
        QStringLiteral("FT8"), QStringLiteral("FT4"), QStringLiteral("CW"),
        QStringLiteral("RTTY"), QStringLiteral("PSK"), QStringLiteral("JS8"),
        QStringLiteral("SSB"), QStringLiteral("USB"), QStringLiteral("LSB"),
        QStringLiteral("AM"),  QStringLiteral("FM"),  QStringLiteral("JT65"),
        QStringLiteral("JT9"), QStringLiteral("MSK144"), QStringLiteral("Q65"),
        QStringLiteral("FST4"), QStringLiteral("OLIVIA"), QStringLiteral("SSTV")};
    const auto toks = comment.toUpper().split(QRegularExpression(QStringLiteral("[^A-Z0-9]+")),
                                              Qt::SkipEmptyParts);
    for (const QString &t : toks)
        if (kModes.contains(t)) return t;
    return QString();
}
}

DxClusterFeeder::DxClusterFeeder(SpotStore *store, Prefs *prefs, QObject *parent)
    : QObject(parent), store_(store), prefs_(prefs) {
    QSettings s;
    enabled_   = s.value(QString::fromLatin1(kKeyEnabled), false).toBool();
    host_      = s.value(QString::fromLatin1(kKeyHost), QString()).toString();
    port_      = qBound(1, s.value(QString::fromLatin1(kKeyPort), 7300).toInt(), 65535);
    loginCall_ = s.value(QString::fromLatin1(kKeyCall), QString()).toString();
    loginCmds_ = s.value(QString::fromLatin1(kKeyCmds), QString()).toString();

    retry_ = new QTimer(this);
    retry_->setSingleShot(true);
    connect(retry_, &QTimer::timeout, this, &DxClusterFeeder::openConnection);
    loginTo_ = new QTimer(this);
    loginTo_->setSingleShot(true);
    connect(loginTo_, &QTimer::timeout, this, &DxClusterFeeder::sendLogin);

    if (enabled_ && !host_.trimmed().isEmpty()) openConnection();
}

QString DxClusterFeeder::effectiveCall() const {
    QString c = loginCall_.trimmed();
    if (c.isEmpty() && prefs_) c = prefs_->callsign().trimmed();
    return c.toUpper();
}

void DxClusterFeeder::openConnection() {
    if (!enabled_ || host_.trimmed().isEmpty()) return;
    closeConnection();
    buf_.clear();
    loginSent_ = false;
    cmdsSent_  = false;
    spotCount_ = 0;
    sock_ = new QTcpSocket(this);
    connect(sock_, &QTcpSocket::connected, this, &DxClusterFeeder::onConnected);
    connect(sock_, &QTcpSocket::readyRead, this, &DxClusterFeeder::onReadyRead);
    connect(sock_, &QTcpSocket::disconnected, this, &DxClusterFeeder::onClosed);
    connect(sock_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (sock_) emit statusChanged(sock_->errorString());
        onClosed();
    });
    emit statusChanged(tr("Connecting to %1…").arg(host_.trimmed()));
    sock_->connectToHost(host_.trimmed(), quint16(port_));
}

void DxClusterFeeder::closeConnection() {
    loginTo_->stop();
    if (sock_) {
        sock_->disconnect(this);   // no onClosed re-entry
        sock_->abort();
        sock_->deleteLater();
        sock_ = nullptr;
    }
}

void DxClusterFeeder::onConnected() {
    emit statusChanged(tr("Connected — logging in…"));
    // Most nodes prompt for the call; if we don't recognise a prompt, send it
    // anyway after a short delay.
    loginTo_->start(kLoginToMs);
}

void DxClusterFeeder::sendLogin() {
    if (!sock_ || loginSent_) return;
    const QString c = effectiveCall();
    if (c.isEmpty()) { emit statusChanged(tr("No login callsign set")); return; }
    sock_->write((c + QStringLiteral("\r\n")).toLatin1());
    loginSent_ = true;
    loginTo_->stop();
    emit statusChanged(tr("Logged in as %1").arg(c));
    // Send any operator login-commands a moment later (after the MOTD).
    QTimer::singleShot(1500, this, &DxClusterFeeder::sendCommands);
}

void DxClusterFeeder::sendCommands() {
    if (!sock_ || cmdsSent_) return;
    cmdsSent_ = true;
    const QString cmds = loginCmds_.trimmed();
    if (cmds.isEmpty()) return;
    const auto lines = cmds.split(QRegularExpression(QStringLiteral("[\r\n]+")),
                                  Qt::SkipEmptyParts);
    for (const QString &ln : lines)
        sock_->write((ln.trimmed() + QStringLiteral("\r\n")).toLatin1());
}

void DxClusterFeeder::onReadyRead() {
    if (!sock_) return;
    buf_.append(sock_->readAll());

    // Login-prompt detection on the raw tail (prompts often lack a newline).
    if (!loginSent_) {
        const QString tail = QString::fromLatin1(buf_.right(80)).toLower();
        if (tail.contains(QStringLiteral("login:"))
            || tail.contains(QStringLiteral("call:"))
            || tail.contains(QStringLiteral("callsign"))
            || tail.contains(QStringLiteral("enter your call")))
            sendLogin();
    }

    // Process complete lines; keep the trailing partial in the buffer.
    int nl;
    while ((nl = buf_.indexOf('\n')) >= 0) {
        QString line = QString::fromLatin1(buf_.left(nl));
        buf_.remove(0, nl + 1);
        line.remove(QLatin1Char('\r'));
        parseLine(line);
    }
    if (buf_.size() > 8192) buf_.clear();   // runaway guard (no newlines)
}

void DxClusterFeeder::parseLine(const QString &line) {
    // "DX de W3LPL:    14025.0  K1ABC      CW  up 2       1234Z"
    QString t = line.trimmed();
    if (!t.startsWith(QStringLiteral("DX de "), Qt::CaseInsensitive)) return;
    t = t.mid(6).trimmed();
    const int colon = t.indexOf(QLatin1Char(':'));
    if (colon <= 0) return;
    const QString spotter = t.left(colon).trimmed();
    const QString rest = t.mid(colon + 1).trimmed();
    const auto toks = rest.split(QRegularExpression(QStringLiteral("\\s+")),
                                 Qt::SkipEmptyParts);
    if (toks.size() < 2) return;
    bool ok = false;
    const double khz = toks[0].toDouble(&ok);
    if (!ok || khz <= 0.0) return;
    const QString dxcall = toks[1].toUpper();
    QStringList commentToks = toks.mid(2);
    // Drop a trailing time stamp like "1234Z".
    if (!commentToks.isEmpty()) {
        const QString last = commentToks.last();
        if (last.size() >= 3 && (last.endsWith(QLatin1Char('Z')) || last.endsWith(QLatin1Char('z')))) {
            bool tOk = false;
            last.left(last.size() - 1).toInt(&tOk);
            if (tOk) commentToks.removeLast();
        }
    }
    const QString comment = commentToks.join(QLatin1Char(' '));
    const qint64 freqHz = qint64(std::llround(khz * 1000.0));
    if (freqHz <= 0) return;
    const QString mode = modeFromComment(comment);
    QString text = comment;
    if (!spotter.isEmpty()) {
        if (!text.isEmpty()) text += QLatin1String("  ");
        text += QStringLiteral("de ") + spotter;
    }
    store_->addSpot(dxcall, mode, freqHz, 0, text, QString::fromLatin1(kSourceTag));
    ++spotCount_;
    if ((spotCount_ % 10) == 1)
        emit statusChanged(tr("%1 spot(s)").arg(spotCount_));
}

void DxClusterFeeder::onClosed() {
    if (sock_) { sock_->deleteLater(); sock_ = nullptr; }
    loginTo_->stop();
    if (enabled_) {
        emit statusChanged(tr("Disconnected — retrying…"));
        retry_->start(kRetryMs);
    }
}

void DxClusterFeeder::reconnectNow() {
    if (!enabled_) return;
    retry_->stop();
    openConnection();
}

void DxClusterFeeder::setEnabled(bool on) {
    if (enabled_ == on) return;
    enabled_ = on;
    QSettings().setValue(QString::fromLatin1(kKeyEnabled), on);
    if (on) {
        openConnection();
    } else {
        retry_->stop();
        closeConnection();
        if (store_) store_->clearSource(QString::fromLatin1(kSourceTag));
        emit statusChanged(tr("Off"));
    }
}

void DxClusterFeeder::setHost(const QString &h) {
    const QString v = h.trimmed();
    if (host_ == v) return;
    host_ = v;
    QSettings().setValue(QString::fromLatin1(kKeyHost), v);
    if (enabled_) reconnectNow();
}
void DxClusterFeeder::setPort(int p) {
    p = qBound(1, p, 65535);
    if (port_ == p) return;
    port_ = p;
    QSettings().setValue(QString::fromLatin1(kKeyPort), p);
    if (enabled_) reconnectNow();
}
void DxClusterFeeder::setLoginCall(const QString &c) {
    const QString v = c.trimmed();
    if (loginCall_ == v) return;
    loginCall_ = v;
    QSettings().setValue(QString::fromLatin1(kKeyCall), v);
    if (enabled_) reconnectNow();
}
void DxClusterFeeder::setLoginCommands(const QString &cmds) {
    if (loginCmds_ == cmds) return;
    loginCmds_ = cmds;
    QSettings().setValue(QString::fromLatin1(kKeyCmds), cmds);
}

} // namespace lyra::ui
