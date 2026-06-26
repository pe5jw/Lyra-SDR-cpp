// Lyra — Serial PTT input.  See SerialPtt.h + com_port_design.md §2.

#include "cat/SerialPtt.h"

#include "hl2_stream.h"

#include <QSerialPort>
#include <QSettings>
#include <QTimer>

namespace lyra::cat {

SerialPtt::SerialPtt(lyra::ipc::HL2Stream *stream, QObject *parent)
    : QObject(parent), stream_(stream) {
    QSettings s;
    enabled_  = s.value(QStringLiteral("serialptt/enabled"), false).toBool();
    portName_ = s.value(QStringLiteral("serialptt/port"), QString()).toString();
    line_     = static_cast<Line>(
        s.value(QStringLiteral("serialptt/line"), int(Line::Cts)).toInt());
    invert_   = s.value(QStringLiteral("serialptt/invert"), false).toBool();
    if (enabled_ && !portName_.isEmpty())
        start();
}

SerialPtt::~SerialPtt() { stop(); }

void SerialPtt::persist() const {
    QSettings s;
    s.setValue(QStringLiteral("serialptt/enabled"), enabled_);
    s.setValue(QStringLiteral("serialptt/port"), portName_);
    s.setValue(QStringLiteral("serialptt/line"), int(line_));
    s.setValue(QStringLiteral("serialptt/invert"), invert_);
}

void SerialPtt::setPortName(const QString &p) {
    if (p == portName_) return;
    const bool wasRunning = (port_ != nullptr);
    if (wasRunning) stop();
    portName_ = p;
    persist();
    if (enabled_ && !portName_.isEmpty()) start();
}

void SerialPtt::setWatchLine(Line l) {
    if (l == line_) return;
    line_ = l;
    persist();
    if (port_) onPoll();   // re-evaluate against the new line immediately
}

void SerialPtt::setInvert(bool on) {
    if (on == invert_) return;
    invert_ = on;
    persist();
    if (port_) onPoll();
}

void SerialPtt::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    persist();
    emit enabledChanged(enabled_);
    if (enabled_) start();
    else          stop();
}

bool SerialPtt::start() {
    if (port_) return true;   // already running
    if (portName_.isEmpty()) {
        emit statusMessage(QStringLiteral("Serial PTT: no COM port selected"));
        return false;
    }
    port_ = new QSerialPort(portName_, this);
    // Baud is irrelevant for modem-line reads; set sane defaults so the
    // driver opens cleanly.
    port_->setBaudRate(QSerialPort::Baud9600);
    if (!port_->open(QIODevice::ReadWrite)) {
        emit statusMessage(QStringLiteral("Serial PTT: cannot open %1 (%2)")
                               .arg(portName_, port_->errorString()));
        delete port_;
        port_ = nullptr;
        return false;
    }
    if (!timer_) {
        timer_ = new QTimer(this);
        timer_->setInterval(kPollMs);
        connect(timer_, &QTimer::timeout, this, &SerialPtt::onPoll);
    }
    active_ = false;
    candidateCount_ = 0;
    timer_->start();
    emit statusMessage(QStringLiteral("Serial PTT watching %1 %2")
                           .arg(portName_,
                                line_ == Line::Cts ? QStringLiteral("CTS")
                                                   : QStringLiteral("DSR")));
    return true;
}

void SerialPtt::stop() {
    if (timer_) timer_->stop();
    if (active_) {   // never leave Lyra stuck in TX on teardown
        active_ = false;
        if (stream_) stream_->requestMoxFromSerialPtt(false);
        emit activeChanged(false);
    }
    if (port_) {
        port_->close();
        port_->deleteLater();
        port_ = nullptr;
    }
}

bool SerialPtt::readLine() const {
    if (!port_) return false;
    const QSerialPort::PinoutSignals sig = port_->pinoutSignals();
    const bool asserted =
        (line_ == Line::Cts) ? sig.testFlag(QSerialPort::ClearToSendSignal)
                             : sig.testFlag(QSerialPort::DataSetReadySignal);
    return invert_ ? !asserted : asserted;
}

void SerialPtt::onPoll() {
    const bool raw = readLine();
    if (raw == active_) {
        candidateCount_ = 0;   // matches the confirmed state — cancel a pending flip
        return;
    }
    // raw differs from the confirmed state — only act after it holds for
    // kDebounceSamples consecutive polls (a single-poll glitch resets it).
    if (++candidateCount_ >= kDebounceSamples) {
        active_ = raw;
        candidateCount_ = 0;
        if (stream_) stream_->requestMoxFromSerialPtt(active_);
        emit activeChanged(active_);
    }
}

} // namespace lyra::cat
