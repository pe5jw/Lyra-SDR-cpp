// Lyra — Serial CW key input.  See SerialCwKey.h + external_cw_keyer_design.md.

#include "cat/SerialCwKey.h"

#include "hl2_stream.h"

#include <QSerialPort>
#include <QSettings>
#include <QTimer>

namespace lyra::cat {

SerialCwKey::SerialCwKey(lyra::ipc::HL2Stream *stream, QObject *parent)
    : QObject(parent), stream_(stream) {
    QSettings s;
    enabled_  = s.value(QStringLiteral("serialcwkey/enabled"), false).toBool();
    portName_ = s.value(QStringLiteral("serialcwkey/port"), QString()).toString();
    line_     = static_cast<Line>(
        s.value(QStringLiteral("serialcwkey/line"), int(Line::Cts)).toInt());
    invert_   = s.value(QStringLiteral("serialcwkey/invert"), false).toBool();
    if (enabled_ && !portName_.isEmpty())
        start();
}

SerialCwKey::~SerialCwKey() { stop(); }

void SerialCwKey::persist() const {
    QSettings s;
    s.setValue(QStringLiteral("serialcwkey/enabled"), enabled_);
    s.setValue(QStringLiteral("serialcwkey/port"), portName_);
    s.setValue(QStringLiteral("serialcwkey/line"), int(line_));
    s.setValue(QStringLiteral("serialcwkey/invert"), invert_);
}

void SerialCwKey::setPortName(const QString &p) {
    if (p == portName_) return;
    const bool wasRunning = (port_ != nullptr);
    if (wasRunning) stop();
    portName_ = p;
    persist();
    if (enabled_ && !portName_.isEmpty()) start();
}

void SerialCwKey::setWatchLine(Line l) {
    if (l == line_) return;
    line_ = l;
    persist();
    if (port_) onPoll();   // re-evaluate against the new line immediately
}

void SerialCwKey::setInvert(bool on) {
    if (on == invert_) return;
    invert_ = on;
    persist();
    if (port_) onPoll();
}

void SerialCwKey::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    persist();
    emit enabledChanged(enabled_);
    if (enabled_) start();
    else          stop();
}

bool SerialCwKey::start() {
    if (port_) return true;   // already running
    if (portName_.isEmpty()) {
        emit statusMessage(QStringLiteral("CW serial key: no COM port selected"));
        return false;
    }
    port_ = new QSerialPort(portName_, this);
    // Baud is irrelevant for modem-line reads; set a sane default so the
    // driver opens cleanly.
    port_->setBaudRate(QSerialPort::Baud9600);
    if (!port_->open(QIODevice::ReadWrite)) {
        emit statusMessage(QStringLiteral("CW serial key: cannot open %1 (%2)")
                               .arg(portName_, port_->errorString()));
        delete port_;
        port_ = nullptr;
        return false;
    }
    if (!timer_) {
        timer_ = new QTimer(this);
        timer_->setInterval(kPollMs);
        connect(timer_, &QTimer::timeout, this, &SerialCwKey::onPoll);
    }
    active_ = false;
    timer_->start();
    emit statusMessage(QStringLiteral("CW serial key watching %1 %2")
                           .arg(portName_,
                                line_ == Line::Cts ? QStringLiteral("CTS")
                                                   : QStringLiteral("DSR")));
    return true;
}

void SerialCwKey::stop() {
    if (timer_) timer_->stop();
    if (active_) {   // never leave the key stuck down on teardown
        active_ = false;
        if (stream_) stream_->requestCwKeyFromSerial(false);
        emit activeChanged(false);
    }
    if (port_) {
        port_->close();
        port_->deleteLater();
        port_ = nullptr;
    }
}

bool SerialCwKey::readLine() const {
    if (!port_) return false;
    const QSerialPort::PinoutSignals sig = port_->pinoutSignals();
    const bool asserted =
        (line_ == Line::Cts) ? sig.testFlag(QSerialPort::ClearToSendSignal)
                             : sig.testFlag(QSerialPort::DataSetReadySignal);
    return invert_ ? !asserted : asserted;
}

void SerialCwKey::onPoll() {
    const bool raw = readLine();
    if (raw == active_) return;          // no edge
    active_ = raw;                       // act on the first changed sample
    if (stream_) stream_->requestCwKeyFromSerial(active_);
    emit activeChanged(active_);
}

} // namespace lyra::cat
