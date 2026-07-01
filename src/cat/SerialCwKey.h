// Lyra — Serial CW key input (#171).
//
// A straight key, bug, or external keyer's KEY output (e.g. a Winkeyer
// driving a serial pin) is wired to a (virtual or physical) COM port's
// CTS or DSR modem-input line.  We poll that line and key Lyra's CW on
// its edges — the host drives tx[0].cwx / cwx_ptt directly (the #105 CWX
// path), so the operator's (or the keyer's) own timing is preserved.  NO
// host iambic keyer: a single key line in, key state out.  CW mode only.
//
// This is Lyra's equivalent of the Thetis CW "Connections" COM-port option
// (Console/cwkeyer.cs reads a key on sp.CtsHolding / sp.DsrHolding and feeds
// the keyer).  On HL2 a serial key cannot reach the gateware key jack, so
// the host keys via cwx — the HL2 KEY jack stays the lower-jitter path and
// is the recommended route for a paddle (gateware iambic).
//
// Mirrors SerialPtt exactly: poll QSerialPort::pinoutSignals() on a short
// timer (Qt has no portable modem-line-change signal), default-OFF, operator
// opt-in, runs on the Qt main thread (never touches the DSP/wire threads —
// it just calls HL2Stream::requestCwKeyFromSerial).  Per-machine QSettings.
//
// See docs/architecture/external_cw_keyer_design.md + com_port_design.md §2.
#pragma once

#include <QObject>
#include <QString>

class QSerialPort;
class QTimer;

namespace lyra::ipc { class HL2Stream; }

namespace lyra::cat {

class SerialCwKey : public QObject {
    Q_OBJECT
public:
    // Which input modem line carries the key-down (asserted = key down).
    // Thetis reads dot on DSR / dash on CTS; for a single straight-key line
    // the operator picks whichever pin their cable delivers.
    enum class Line { Cts = 0, Dsr = 1 };

    explicit SerialCwKey(lyra::ipc::HL2Stream *stream, QObject *parent = nullptr);
    ~SerialCwKey() override;

    bool    enabled() const   { return enabled_; }
    QString portName() const  { return portName_; }
    Line    watchLine() const { return line_; }
    bool    invert() const    { return invert_; }
    bool    active() const    { return active_; }   // key currently down

    void setPortName(const QString &p);
    void setWatchLine(Line l);
    void setInvert(bool on);
    void setEnabled(bool on);    // start/stop + persist

    bool start();   // open port + start poll; false on failure
    void stop();    // release the key if down, close port, stop poll

signals:
    void enabledChanged(bool on);
    void activeChanged(bool on);            // key edge fed to CW
    void statusMessage(const QString &msg); // wired to the status bar

private slots:
    void onPoll();

private:
    void persist() const;
    bool readLine() const;   // asserted state of the chosen line (post-invert)

    lyra::ipc::HL2Stream *stream_ = nullptr;
    QSerialPort          *port_   = nullptr;
    QTimer               *timer_  = nullptr;

    bool    enabled_ = false;
    QString portName_;
    Line    line_    = Line::Cts;
    bool    invert_  = false;
    bool    active_  = false;   // last key state sent to CW

    // CW keying wants tighter timing than PTT: poll fast and act on the first
    // changed sample (no multi-sample debounce — a CW key edge IS the signal,
    // and host-side polling already adds jitter; the HL2 KEY jack is the route
    // for jitter-free timing).  ~2 ms granularity.
    static constexpr int kPollMs = 2;
};

} // namespace lyra::cat
