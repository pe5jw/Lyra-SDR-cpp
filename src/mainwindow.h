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

namespace lyra::ui {

class Prefs;
class SettingsDialog;
class Help;
class HelpDialog;
class Bands;
class UsbBcd;
class UpdateChecker;
class WxIndicator;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // The four C++ service objects are exposed to every embedded QML
    // surface as context properties (Discovery / Stream / Wdsp /
    // WdspEngine), so each panel binds whatever it needs.
    MainWindow(QObject *discovery, QObject *stream,
               QObject *wdsp, QObject *wdspEngine,
               Prefs *prefs, lyra::wx::WxService *wx,
               QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;
    // Double-clicking a custom dock title bar toggles float/dock (the
    // standard QDockWidget gesture, lost when we set a custom title bar).
    bool eventFilter(QObject *obj, QEvent *event) override;

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
    void tickClocks();        // 1 Hz: refresh the header local + UTC clocks
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

    QObject *discovery_  = nullptr;
    QObject *stream_     = nullptr;
    QObject *wdsp_       = nullptr;
    QObject *wdspEngine_ = nullptr;
    Prefs   *prefs_      = nullptr;
    lyra::wx::WxService *wx_ = nullptr;
    WxIndicator         *wxIndicator_ = nullptr;   // header alert badges
    QSystemTrayIcon     *tray_ = nullptr;          // lazy, for wx toasts

    QHash<QString, QDockWidget *> docks_;
    QAction                    *lockAction_ = nullptr;
    QAction                    *startStopAction_ = nullptr;   // header Start/Stop
    QLabel                     *connStatus_ = nullptr;        // header conn status
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
    Bands                      *bands_   = nullptr;   // amateur band table (QML)
    UsbBcd                     *usbBcd_  = nullptr;   // USB-BCD amp band output
};

} // namespace lyra::ui
