// Lyra — Settings dialog (QDialog + QTabWidget).
//
// Mirrors the Python-Lyra tabbed Settings structure.  Tabs are added
// as their features land (no inert/empty placeholder tabs).  The
// Visuals tab is the first real one: it drives the panadapter display
// options through the shared Prefs object, so edits apply live to the
// QML panadapter and persist to QSettings.

#pragma once

#include <QDialog>
#include <QList>

class QTabWidget;

namespace lyra::ipc { class HL2Stream; class HL2Discovery; }
namespace lyra::dsp { class WdspEngine; }
namespace lyra::wx  { class WxService; }
namespace lyra::profile { class ProfileManager; class CompanionLauncher; }
namespace lyra::cat { class SerialPtt; class SerialCwKey; class CatServer; }
namespace lyra::tx  { class VoiceKeyer; }

namespace lyra::ui {

class Prefs;
class UsbBcd;
class MemoryStore;
class EibiStore;
class TciServer;
class SpotStore;
class SpotHoleFeeder;
class DxClusterFeeder;
class MeterModel;
class TunerMemory;

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
                   EibiStore *eibi, TciServer *tci, SpotStore *spots,
                   SpotHoleFeeder *spotHole, DxClusterFeeder *dxCluster,
                   MeterModel *meter, TunerMemory *tuner,
                   lyra::tx::VoiceKeyer *voiceKeyer,
                   lyra::profile::ProfileManager *profiles,
                   lyra::profile::CompanionLauncher *companion,
                   lyra::cat::SerialPtt *serialPtt,
                   lyra::cat::SerialCwKey *serialCwKey,
                   const QList<lyra::cat::CatServer *> &catServers,
                   QWidget *parent = nullptr);

    // Raise the tab that owns <topic> (from a panel's "?" → Settings).
    // Returns true if a tab owns this topic (and was raised); false if
    // the panel has no Settings home (caller falls back to the guide).
    bool selectTopic(const QString &topic);

private:
    QWidget *buildVisualsTab();
    QWidget *buildHardwareTab();
    QWidget *buildAudioTab();    // RX audio output device chooser
    QWidget *buildNoiseTab();    // captured-profile manager (list/rename/delete)
    QWidget *buildDspTab();      // #159 per-mode-family filter type (Linear/Low Latency)
    QWidget *buildWeatherTab();  // weather-alert sources + thresholds + keys
    QWidget *buildBandsTab();    // Memory bank (+ Time Stations / SW DB later)
    QWidget *buildNetworkTab();  // TCI server (logger / cluster integration)
    QWidget *buildCatSerialTab();// serial PTT input + Kenwood CAT (COM-port)
    QWidget *buildMeterTab();    // S-meter calibration trim
    QWidget *buildTunerTab();    // manual-ATU memory editor + match window
    QWidget *buildTxTab();       // TX-1 component 5b: TR-sequencing
    QWidget *buildVoxTab();      // #91 VOX — voice-operated TX (own tab)
    QWidget *buildPaGainTab();   // TX power model Stage 3: per-band PA Gain table
                                 // + cos² amplitude envelope (amp
                                 // hot-switch protection knobs)
    QWidget *buildCwTab();       // #105 CW mode: keyer + sidetone
                                 // (RX pitch/peaking + decoder later)
    QWidget *buildProfilesTab(); // #49 editor: profile list + Save/
                                 // Save As/Load/Rename/Delete/Set-Default
                                 // + per-mode auto-recall bindings

    Prefs                  *prefs_     = nullptr;
    lyra::ipc::HL2Stream   *stream_    = nullptr;
    lyra::ipc::HL2Discovery *discovery_ = nullptr;
    UsbBcd                 *bcd_       = nullptr;
    lyra::dsp::WdspEngine  *engine_    = nullptr;
    lyra::wx::WxService    *wx_        = nullptr;
    MemoryStore            *memory_    = nullptr;
    EibiStore              *eibi_      = nullptr;
    TciServer              *tci_       = nullptr;
    SpotStore              *spots_     = nullptr;
    SpotHoleFeeder         *spotHole_  = nullptr;
    DxClusterFeeder        *dxCluster_ = nullptr;
    MeterModel             *meter_     = nullptr;
    TunerMemory            *tuner_     = nullptr;
    lyra::tx::VoiceKeyer   *voiceKeyer_ = nullptr;   // #89 recording options
    lyra::profile::ProfileManager *profiles_ = nullptr;
    lyra::profile::CompanionLauncher *companion_ = nullptr;
    lyra::cat::SerialPtt   *serialPtt_ = nullptr;
    lyra::cat::SerialCwKey *serialCwKey_ = nullptr;   // #171
    QList<lyra::cat::CatServer *> catServers_;
    QTabWidget             *tabs_      = nullptr;
};

} // namespace lyra::ui
