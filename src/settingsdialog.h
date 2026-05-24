// Lyra — Settings dialog (QDialog + QTabWidget).
//
// Mirrors the Python-Lyra tabbed Settings structure.  Tabs are added
// as their features land (no inert/empty placeholder tabs).  The
// Visuals tab is the first real one: it drives the panadapter display
// options through the shared Prefs object, so edits apply live to the
// QML panadapter and persist to QSettings.

#pragma once

#include <QDialog>

class QTabWidget;

namespace lyra::ipc { class HL2Stream; class HL2Discovery; }
namespace lyra::dsp { class WdspEngine; }
namespace lyra::wx  { class WxService; }

namespace lyra::ui {

class Prefs;
class UsbBcd;
class MemoryStore;
class EibiStore;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    // <stream> drives the Hardware tab (filter board / OC + the Radio
    // connect/disconnect); <discovery> drives the Radio-discovery
    // (LAN scan + found-radios list) section there; <bcd> drives the
    // USB-BCD section.  Any may be null.
    SettingsDialog(Prefs *prefs, lyra::ipc::HL2Stream *stream,
                   lyra::ipc::HL2Discovery *discovery,
                   UsbBcd *bcd, lyra::dsp::WdspEngine *engine,
                   lyra::wx::WxService *wx, MemoryStore *memory,
                   EibiStore *eibi, QWidget *parent = nullptr);

    // Raise the tab that owns <topic> (from a panel's "?" → Settings).
    void selectTopic(const QString &topic);

private:
    QWidget *buildVisualsTab();
    QWidget *buildHardwareTab();
    QWidget *buildAudioTab();    // RX audio output device chooser
    QWidget *buildWeatherTab();  // weather-alert sources + thresholds + keys
    QWidget *buildBandsTab();    // Memory bank (+ Time Stations / SW DB later)

    Prefs                  *prefs_     = nullptr;
    lyra::ipc::HL2Stream   *stream_    = nullptr;
    lyra::ipc::HL2Discovery *discovery_ = nullptr;
    UsbBcd                 *bcd_       = nullptr;
    lyra::dsp::WdspEngine  *engine_    = nullptr;
    lyra::wx::WxService    *wx_        = nullptr;
    MemoryStore            *memory_    = nullptr;
    EibiStore              *eibi_      = nullptr;
    QTabWidget             *tabs_      = nullptr;
};

} // namespace lyra::ui
