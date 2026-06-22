// Lyra — #175 TX Waterfall callsign ID: arm/cadence orchestrator.
//
// Owns the "armed → send once → repeat every N min" behaviour for the
// waterfall ID.  Arming is the header WF-ID chip (Prefs.wfIdEnabled, USB/LSB
// only).  Each burst is rendered + transmitted FLAT, reproducing exactly the
// operator-validated DIGU state, then the operator's TX state is restored on
// the un-key edge.
//
// SAFETY / no-strand contract: the transient flat switch touches ONLY live
// wire/stream state (SetTXTCIAudio / SetTxRackBypass / setTxMode / leveler) —
// never Prefs.micSource / Prefs.mode / QSettings.  So a crash mid-burst leaves
// the operator's SAVED config untouched; the restore re-derives from those
// untouched Prefs on the moxActiveChanged(false) edge (fires for ANY un-key:
// normal, safety-timeout, or external inhibit).

#pragma once

#include <QObject>
#include <QString>

class QTimer;
namespace lyra {
namespace ui  { class Prefs; }
namespace dsp { class WdspEngine; }
namespace ipc { class HL2Stream; }
}

class WaterfallIdController : public QObject {
    Q_OBJECT
public:
    WaterfallIdController(lyra::ui::Prefs *prefs,
                          lyra::ipc::HL2Stream *stream,
                          lyra::dsp::WdspEngine *engine,
                          QObject *parent = nullptr);

private:
    void onArmedChanged();          // Prefs.wfIdEnabled edge (chip)
    void onIntervalChanged();       // Prefs.wfIdIntervalMin edge
    void onCadence();               // repeat timer
    void onMoxChanged(bool on);     // restore on OUR burst's un-key edge

    bool ssbOk() const;             // operating mode is USB/LSB
    int  wdspTxModeFor(const QString &uiMode) const;  // mirror of main.cpp's map
    void fireOnce();                // flat-on → render → key → arm un-key timer
    void enterFlat();               // transient source/rack/mode/leveler switch
    void exitFlat();                // restore from the untouched operator Prefs

    lyra::ui::Prefs       *prefs_  = nullptr;
    lyra::ipc::HL2Stream  *stream_ = nullptr;
    lyra::dsp::WdspEngine *engine_ = nullptr;
    QTimer *cadence_ = nullptr;
    QTimer *unkey_   = nullptr;
    bool burstActive_  = false;     // a WF-ID burst (not an operator QSO) is live
    bool savedLeveler_ = false;     // leveler state to restore after the burst
};
