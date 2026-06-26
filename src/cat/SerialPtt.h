// Lyra — Serial PTT input.
//
// A 3rd-party digital app (WSJT-X, VarAC, fldigi, …) asserts an RTS or
// DTR "PTT" line on a (virtual) COM port; on a com0com pair that line
// crosses to this end's CTS/DSR input.  We poll the chosen modem-input
// line and key Lyra (HL2Stream::requestMoxFromSerialPtt) on its edges —
// Lyra then keys the HL2/ANAN/Brick rig downstream exactly as a MOX
// button press does.  PTT *into* Lyra, not out.
//
// Reference: the line-read pattern mirrors openHPSDR Thetis
// Console/CAT/SerialPortPTT.cs (isCTS()/isDSR()).  Qt has no portable
// modem-line-change signal, so we poll QSerialPort::pinoutSignals() on
// a short timer, like Thetis's poll thread.  Default-OFF, operator
// opt-in (same posture as the EP6 HW-PTT-in forwarder).  Runs on the Qt
// main thread; no worker QThread (matches the TciServer model — this
// never touches the DSP/wire threads, it just calls requestMox*).
//
// See docs/architecture/com_port_design.md §2.
#pragma once

#include <QObject>
#include <QString>

class QSerialPort;
class QTimer;

namespace lyra::ipc { class HL2Stream; }

namespace lyra::cat {

class SerialPtt : public QObject {
    Q_OBJECT
public:
    // Which input modem line carries the app's PTT assertion.  On a
    // com0com pair the app's RTS→our CTS, DTR→our DSR (the standard
    // null-modem crossover); operator picks whichever their pairing
    // delivers.
    enum class Line { Cts = 0, Dsr = 1 };

    explicit SerialPtt(lyra::ipc::HL2Stream *stream, QObject *parent = nullptr);
    ~SerialPtt() override;

    bool    enabled() const   { return enabled_; }
    QString portName() const  { return portName_; }
    Line    watchLine() const { return line_; }
    bool    invert() const    { return invert_; }
    bool    active() const    { return active_; }   // currently keying TX

    void setPortName(const QString &p);
    void setWatchLine(Line l);
    void setInvert(bool on);
    void setEnabled(bool on);    // start/stop + persist

    bool start();   // open port + start poll; false on failure
    void stop();    // deassert MOX if keyed, close port, stop poll

signals:
    void enabledChanged(bool on);
    void activeChanged(bool on);            // edge fed to MOX
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
    bool    active_  = false;   // last confirmed (debounced) state sent to MOX
    int     candidateCount_ = 0;  // consecutive polls reading the opposite state

    // Poll the modem line at kPollMs and require kDebounceSamples consecutive
    // matching reads before flipping MOX: ~10 ms worst-case detect with finer
    // jitter than the old 15 ms poll, plus immunity to a single-sample line
    // glitch (matters only for a noisy physical PTT line — a software RTS over
    // com0com is clean either way, so this is pure upside for digital modes).
    static constexpr int kPollMs          = 5;
    static constexpr int kDebounceSamples = 2;
};

} // namespace lyra::cat
