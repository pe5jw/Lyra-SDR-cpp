// Lyra — Kenwood CAT serial server.
//
// Emulates a Kenwood transceiver on a COM port so loggers / digital-mode
// apps (WSJT-X, VarAC, fldigi, N1MM, Log4OM, Hamlib, …) can read + set
// VFO frequency / mode and key TX over serial CAT.  Lyra is the "rig" on
// one end; the client connects to the other end of a (virtual, com0com)
// COM-port pair.
//
// Multiple independent instances run side-by-side (e.g. a logger on one
// COM port, a digital-mode app on another) — each is a CatServer with its
// own QSettings group + operator label, like Thetis's CAT1..CAT4.
//
// TS-480 (ID 020) by default, TS-2000 (ID 019) selectable — the grammar
// is identical; only the ID; reply and IF; field layout differ by model.
// The Kenwood CAT protocol (`;`-terminated ASCII; 2-letter command +
// optional payload; read = no payload, set = payload) is an open
// published standard — this is a native Qt6 implementation, modeled on
// the command coverage of openHPSDR Thetis Console/CAT (CATCommands.cs;
// study-only per docs/THETIS_DIRECT_PORT_PLAN.md) with open attribution.
//
// Mirrors TciServer (the WebSocket CAT-equivalent): same Prefs/HL2Stream/
// WdspEngine control surface, QSettings-persisted, runs on the Qt main
// thread (QSerialPort is signal/slot driven — CAT never touches the DSP/
// wire threads, it just calls the radio setters).
//
// See docs/architecture/com_port_design.md §3.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

class QSerialPort;
class QTcpServer;
class QTcpSocket;

namespace lyra::ipc { class HL2Stream; }
namespace lyra::dsp { class WdspEngine; }

namespace lyra::ui { class Prefs; }

namespace lyra::cat {

class CatServer : public QObject {
    Q_OBJECT
public:
    enum class RigModel  { Ts480 = 0, Ts2000 = 1 };
    enum class Transport { Serial = 0, Tcp = 1 };

    // settingsGroup = QSettings key prefix (e.g. "cat1") so several
    // instances persist independently.  label is the operator's free-text
    // note for what this port drives (N1MM / VarAC / FLDIGI …).
    CatServer(const QString &settingsGroup, lyra::ui::Prefs *prefs,
              lyra::ipc::HL2Stream *stream, lyra::dsp::WdspEngine *engine,
              QObject *parent = nullptr);
    ~CatServer() override;

    QString   group() const     { return group_; }
    bool      enabled() const   { return enabled_; }
    QString   label() const     { return label_; }
    Transport transport() const { return transport_; }
    QString   portName() const  { return portName_; }
    int       baud() const      { return baud_; }
    int       dataBits() const  { return dataBits_; }   // 5..8
    int       parity() const    { return parity_; }     // 0 none / 1 even / 2 odd
    int       stopBits() const  { return stopBits_; }   // 1 / 2
    QString   host() const      { return host_; }       // TCP listen address
    int       tcpPort() const   { return tcpPort_; }
    RigModel  rigModel() const  { return model_; }
    bool      running() const;

    void setLabel(const QString &s);
    void setTransport(Transport t);
    void setPortName(const QString &p);
    void setBaud(int b);
    void setDataBits(int d);
    void setParity(int p);
    void setStopBits(int s);
    void setHost(const QString &h);
    void setTcpPort(int p);
    void setRigModel(RigModel m);
    void setEnabled(bool on);   // start/stop + persist

    bool start();   // open port; false on failure
    void stop();    // close port

signals:
    void enabledChanged(bool on);
    void statusMessage(const QString &msg);
    void startRequested();   // PS1 — MainWindow drives the connect flow
    void stopRequested();    // PS0

private slots:
    void onReadyRead();           // serial transport
    void onNewConnection();       // TCP transport — accept a client
    void onRadioChanged();        // AI auto-info push (freq / mode / MOX edges)

private:
    QString    key(const char *leaf) const;      // group_ + '/' + leaf
    void       persist() const;
    void       applySerialParams();              // push baud/parity/data/stop
    bool       startSerial();
    bool       startTcp();
    void       handleTcpData(QTcpSocket *sock);  // per-client `;`-line dispatch
    void       broadcast(const QByteArray &resp);// AI → serial port or all TCP clients
    QByteArray dispatch(const QByteArray &cmd);
    QByteArray buildIf() const;
    qint64     carrierHz() const;
    char       modeToKenwood(const QString &lyraMode) const;
    QString    kenwoodToMode(int n) const;

    QString                group_;
    lyra::ui::Prefs       *prefs_  = nullptr;
    lyra::ipc::HL2Stream  *stream_ = nullptr;
    lyra::dsp::WdspEngine *engine_ = nullptr;
    QSerialPort           *port_   = nullptr;          // serial transport
    QByteArray             rxBuf_;                      // serial line buffer
    QTcpServer            *tcpServer_ = nullptr;        // TCP transport
    QHash<QTcpSocket *, QByteArray> tcpBufs_;           // per-client line buffers

    bool      enabled_   = false;
    QString   label_;
    Transport transport_ = Transport::Serial;
    QString   portName_;
    int       baud_      = 115200;
    int       dataBits_  = 8;        // 5..8
    int       parity_    = 0;        // 0 none / 1 even / 2 odd
    int       stopBits_  = 1;        // 1 / 2
    QString   host_      = QStringLiteral("127.0.0.1");
    int       tcpPort_   = 60000;
    RigModel  model_     = RigModel::Ts480;
    int       aiMode_    = 0;        // Kenwood Auto-Information level (0..4)

    static constexpr int kRxBufCap = 4096;
};

} // namespace lyra::cat
