// Lyra — QMainWindow dock shell (UI framing).
//
// The app's top-level window is a QtWidgets QMainWindow so we get the
// proven Qt docking system (movable / floatable / snappable / tabbed
// panels + saveState/restoreState layout persistence), matching the
// Python-Lyra UX.  All native C++ — Qt Widgets is a C++ module
// compiled into the binary; there is no Python and no GIL anywhere.
//
// The Vulkan scene-graph content (panadapter now; waterfall + more
// later) renders through the Qt Quick scene graph embedded INSIDE
// docks via QQuickWidget.  Control panels currently still live in the
// central QQuickWidget (Main.qml) and get peeled into their own docks
// in later steps.

#pragma once

#include <QHash>
#include <QMainWindow>
#include <QPoint>

class QAction;
class QDockWidget;
class QLabel;
class QQuickWidget;
class QTimer;
class QSystemTrayIcon;

namespace lyra::wx { class WxService; }
namespace lyra::solar { class SolarService; }
namespace lyra::profile { class ProfileManager; }
namespace lyra::ui {

class ProfileUi;
class EqModel;
class SpeechModel;
class CombinatorModel;
class PlateModel;

class Prefs;
class SettingsDialog;
class Help;
class HelpDialog;
class LogDialog;
class Bands;
class BandPlan;
class StatusBus;
class TimeSync;
class BandMemory;
class GenSlots;
class TimeStations;
class MemoryStore;
class EibiStore;
class SpotStore;
class TciServer;
class MeterModel;
class UsbBcd;
class UpdateChecker;
class WxIndicator;
class NcdxfFollow;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // The four C++ service objects are exposed to every embedded QML
    // surface as context properties (Discovery / Stream / Wdsp /
    // WdspEngine), so each panel binds whatever it needs.
    MainWindow(QObject *discovery, QObject *stream,
               QObject *wdsp, QObject *wdspEngine,
               Prefs *prefs, lyra::wx::WxService *wx,
               lyra::profile::ProfileManager *profiles,
               QWidget *parent = nullptr);

    // TX-rip Phase 1 (Q2): setTciMicSource / setTxDspWorker removed —
    // the TX DSP subsystem is being rebuilt from empty files per the
    // signed Phase 0 architectural mapping (docs/TX_ARCHITECTURAL_MAPPING.md
    // §10.3).  Re-introduced when the new TX path lands.

protected:
    void closeEvent(QCloseEvent *event) override;
    // Double-clicking a custom dock title bar toggles float/dock (the
    // standard QDockWidget gesture, lost when we set a custom title bar).
    bool eventFilter(QObject *obj, QEvent *event) override;
    // TX-0c-fsm — space-bar PTT momentary.  Press routes to
    // stream.requestMox(true), release routes to requestMox(false).
    // Suppressed when a text-entry widget has focus so typing into
    // the freq-entry overlay (or any QLineEdit/QSpinBox) doesn't key
    // the radio.  Auto-repeat is ignored (one edge per press).
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    // Build a QQuickWidget that hosts <qmlFile> from the Lyra QML
    // module, with the four service objects set as context properties
    // BEFORE the source loads.
    QQuickWidget *makeQuick(const QString &qmlFile);
    // Wrap a QML panel in a movable/floatable/closable QDockWidget,
    // register it in docks_, and dock it into <area>.  <topic> drives
    // the title-bar "?" badge (Help guide / Settings).
    QDockWidget *addQuickDock(const QString &objectName,
                              const QString &title,
                              const QString &qmlFile,
                              const QString &topic,
                              Qt::DockWidgetArea area);
    // Same as addQuickDock but hosts a plain QtWidgets widget (e.g. the
    // solar PROP strip) instead of a QML surface.
    QDockWidget *addWidgetDock(const QString &objectName,
                               const QString &title,
                               QWidget *content,
                               const QString &topic,
                               Qt::DockWidgetArea area);
    // Build a custom dock title bar: title (far left), "?" + float +
    // close (far right).  Replaces the default title bar so the help
    // badge lives in the title row, outside the panel content.
    QWidget *makeDockTitleBar(QDockWidget *dock, const QString &title,
                              const QString &topic);

    void buildDocks();
    void buildMenus();
    void buildToolbar();
    // Header Start/Stop: connect to the saved radio (or scan + auto-open
    // the first found) when stopped; close the stream when running.
    void onStartStop();
    void updateConnState();   // reflect stream running state into the UI
    void updateTciStatus();   // header TCI connected/client-count indicator
    void tickClocks();        // 1 Hz: refresh the header local + UTC clocks
    void showClockMenu(const QPoint &global);   // right-click clocks → drift check
    // GitHub update notification.  manual=true (Help → Check for Updates)
    // always reports a result; manual=false (startup) is quiet unless an
    // unseen, non-skipped newer release exists (then a once-per-version
    // modal + a persistent toolbar indicator).
    void checkForUpdates(bool manual);
    void onUpdateAvailable(const QString &tag, const QString &url,
                           const QString &body);
    void onNoUpdate();
    void onUpdateCheckFailed(const QString &reason);
    void applyPanelLock(bool locked);
    void saveLayout();                 // session auto-save (on close)
    void restoreLayout();              // session auto-restore (on launch)
    // Operator-driven layout management (View menu), mirroring old Lyra:
    void saveUserLayout();             // snapshot current as "my default"
    void restoreUserLayout();          // jump back to the saved default
    void applyDefaultLayout();         // factory arrangement (built-in)
    // Export/import the full settings profile (layout + all prefs) to a
    // portable .lyra file — backup, transfer to another machine, or
    // instant recovery after layout tinkering.  Machine-specific keys
    // (graphics backend) are deliberately NOT carried.
    void exportSettings();
    void importSettings();
    void openSettings();
    // Per-panel "?" badge targets (driven by the Help bridge).
    void showHelp(const QString &topic);        // open the User Guide at topic
    void openSettingsTopic(const QString &topic);// Settings, on topic's tab

    // Collapsible-panel docks (EqPanel/SpeechPanel etc.): when the QML root's
    // `collapsed` property toggles, shrink the host dock to the title strip
    // (and release it on expand) so a collapsed panel isn't a strip floating
    // in a full-size dock.  SizeRootObjectToView ignores the QML
    // implicitHeight, so the dock size has to be driven from C++.
private slots:
    void syncCollapsibleDock();
private:
    // TX-0c-pa-debug — refresh the HL2 telemetry strip on the right
    // side of the status bar (T / V / PA current).  Driven by
    // HL2Stream::statsChanged (~5 Hz).  PA current goes red+bold when
    // ≥50 mA (the operator-visible "RF on the air" indicator for the
    // first-RF bench and beyond).
    void refreshHl2TelemetryStrip();

    QObject *discovery_  = nullptr;
    QObject *stream_     = nullptr;
    QObject *wdsp_       = nullptr;
    QObject *wdspEngine_ = nullptr;
    Prefs   *prefs_      = nullptr;
    lyra::wx::WxService *wx_ = nullptr;
    lyra::solar::SolarService *solar_ = nullptr;   // HamQSL solar/propagation
    NcdxfFollow         *ncdxfFollow_ = nullptr;   // NCDXF beacon auto-follow
    WxIndicator         *wxIndicator_ = nullptr;   // header alert badges
    QSystemTrayIcon     *tray_ = nullptr;          // lazy, for wx toasts
    // TX-0c-pa-debug — HL2 telemetry strip on the right of the status
    // bar.  Permanent widget (never displaced by transient
    // showMessage); renders "HL2: T 24.7°C  V 12.3 V  PA 0.00 A" from
    // the existing TX-0a Q_PROPERTYs.  PA current ≥50 mA → bold red
    // (operator-visible RF indicator + B-pa bench observable + the
    // §15.20 kill-test PA-bias-drop signal).
    QLabel              *hl2TelemLabel_ = nullptr;

    QHash<QString, QDockWidget *> docks_;
    QAction                    *lockAction_ = nullptr;
    QAction                    *startStopAction_ = nullptr;   // header Start/Stop
    QLabel                     *connStatus_ = nullptr;        // header conn status
    QLabel                     *tciStatus_  = nullptr;        // header TCI client indicator
    QLabel                     *spottedBadge_ = nullptr;      // "you've been spotted" header badge
    QTimer                     *spottedClearTimer_ = nullptr; // auto-clears the badge
    QLabel                     *clockLocal_ = nullptr;        // header local clock
    QLabel                     *clockUtc_   = nullptr;        // header UTC/Zulu clock
    QTimer                     *clockTimer_ = nullptr;        // 1 Hz clock tick
    UpdateChecker              *updateChecker_ = nullptr;     // GitHub release check
    QAction                    *updateAction_ = nullptr;      // toolbar "update available"
    QString                     pendingUpdateUrl_;            // release page to open
    bool                        updateCheckManual_ = false;   // manual vs startup check
    QMetaObject::Connection     scanConn_;                    // one-shot scan→open
    // Drag-move state for a FLOATING dock via its custom title bar
    // (QDockWidget's built-in title drag is bypassed by the custom bar).
    QDockWidget                *floatDragDock_ = nullptr;
    QPoint                      floatDragOffset_;

    SettingsDialog             *settingsDlg_ = nullptr;
    Help                       *help_    = nullptr;   // QML→C++ help bridge
    HelpDialog                 *helpDlg_ = nullptr;   // in-app User Guide
    LogDialog                  *logDlg_  = nullptr;   // in-app diagnostic log
    Bands                      *bands_   = nullptr;   // amateur band table (QML)
    BandPlan                   *bandPlan_ = nullptr;  // band-plan overlay data (QML)
    StatusBus                  *statusBus_ = nullptr; // transient status messages (QML+C++)
    int                         lastBandState_ = -1;  // -1 unknown / 0 out-of-band / 1 in-band
    TimeSync                   *timeSync_ = nullptr;  // NTP clock-drift check
    BandMemory                 *bandMemory_ = nullptr;// per-band mode/dB-range memory
    GenSlots                   *gen_ = nullptr;       // GEN1/2/3 general-coverage slots
    TimeStations               *time_ = nullptr;      // HF time-station TIME cycle
    MemoryStore                *memory_ = nullptr;    // frequency memory bank
    EibiStore                  *eibi_  = nullptr;     // EiBi shortwave overlay
    SpotStore                  *spots_ = nullptr;     // DX-cluster spots (TCI)
    TciServer                  *tci_   = nullptr;     // TCI server (logger/cluster)
    MeterModel                 *meter_ = nullptr;     // RX S-meter (Horizon Arc / Plasma Bar)
    EqModel                    *eqModel_ = nullptr;   // #50/#59 parametric EQ (EqPanel.qml)
    SpeechModel                *speechModel_ = nullptr; // #88 speech rack (SpeechPanel.qml)
    CombinatorModel            *combinatorModel_ = nullptr; // #51 combinator (CombinatorPanel.qml)
    PlateModel                 *plateModel_ = nullptr;     // #52 plate reverb (PlatePanel.qml)
    lyra::profile::ProfileManager *profiles_ = nullptr; // TX/RX profile engine (Settings→Profiles)
    ProfileUi                  *profileUi_ = nullptr;  // native Save-Profile dialog (front panel)
    int                         driftSeverity_ = 0;   // 0 unknown/ok .. 2 warn .. 3 bad
    UsbBcd                     *usbBcd_  = nullptr;   // USB-BCD amp band output
};

} // namespace lyra::ui
