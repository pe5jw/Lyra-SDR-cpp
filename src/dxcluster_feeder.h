// Lyra — DX-cluster telnet spot feeder.
//
// The third spot source: a raw telnet connection to an operator-chosen
// DX-cluster node (VE7CC / DXSpider / AR-Cluster …).  Logs in, then parses
// "DX de <spotter>: <freq> <dxcall> <comment> <time>" lines into the same
// source-agnostic SpotStore bus, tagged source="telnet".  Standalone — no
// SDRLogger+, no SpotHole.  Default OFF (opt-in).
//
// Native C++/Qt6 QTcpSocket; non-blocking, auto-reconnect while enabled.

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QTcpSocket;
class QTimer;

namespace lyra::ui {

class Prefs;
class SpotStore;

class DxClusterFeeder : public QObject {
    Q_OBJECT
public:
    DxClusterFeeder(SpotStore *store, Prefs *prefs, QObject *parent = nullptr);

    // --- settings (persisted "spots/telnet/...") ---
    bool    enabled() const       { return enabled_; }
    QString host() const          { return host_; }
    int     port() const          { return port_; }
    // Empty → fall back to MYCALL at connect.  A separate field (may be a
    // friend's login / a club call / a node-registered call), NOT a MYCALL alias.
    QString loginCall() const     { return loginCall_; }
    QString loginCommands() const { return loginCmds_; }
    void setEnabled(bool on);
    void setHost(const QString &h);
    void setPort(int p);
    void setLoginCall(const QString &c);
    void setLoginCommands(const QString &cmds);

    Q_INVOKABLE void reconnectNow();

signals:
    void statusChanged(const QString &msg);   // connection / spot-count status

private:
    void openConnection();
    void closeConnection();
    void onConnected();
    void onReadyRead();
    void onClosed();
    void sendLogin();
    void sendCommands();
    void parseLine(const QString &line);
    QString effectiveCall() const;

    SpotStore   *store_  = nullptr;
    Prefs       *prefs_  = nullptr;
    QTcpSocket  *sock_   = nullptr;
    QTimer      *retry_  = nullptr;   // auto-reconnect while enabled
    QTimer      *loginTo_ = nullptr;  // fallback: send call if no prompt seen
    QByteArray   buf_;
    bool         loginSent_ = false;
    bool         cmdsSent_  = false;
    int          spotCount_ = 0;

    bool    enabled_  = false;
    QString host_;
    int     port_     = 7300;
    QString loginCall_;
    QString loginCmds_;
};

} // namespace lyra::ui
