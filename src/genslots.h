// Lyra — GEN general-coverage slots (ported from old Lyra GEN1/2/3).
//
// Three "general coverage" quick-tune slots (shortwave / MW / utility),
// each behaving like a band button but for arbitrary frequencies: click
// to recall that slot's last freq + mode; while the slot is active,
// tuning around updates it (and persists — an improvement over old
// Lyra, which only saved on an explicit action).  Clicking an amateur
// band button (or another GEN) deactivates the current slot.
//
// Defaults: GEN1 = WWV 10 MHz AM, GEN2 = WWV 15 MHz AM, GEN3 = 1 MHz MW AM.

#pragma once

#include <QObject>
#include <QString>

namespace lyra::ipc { class HL2Stream; }

namespace lyra::ui {

class Prefs;

class GenSlots : public QObject {
    Q_OBJECT
    // 0 = none active; 1/2/3 = that slot owns the VFO (QML highlights it).
    Q_PROPERTY(int activeSlot READ activeSlot NOTIFY activeChanged)
public:
    GenSlots(Prefs *prefs, lyra::ipc::HL2Stream *stream,
             QObject *parent = nullptr);

    int activeSlot() const { return active_; }

    Q_INVOKABLE double  slotFreq(int n) const;     // Hz
    Q_INVOKABLE QString slotMode(int n) const;
    Q_INVOKABLE QString slotLabel(int n) const;    // operator label ("" = none)

    Q_INVOKABLE void recall(int n);                // tune to slot n + activate
    Q_INVOKABLE void deactivate();                 // a band button was clicked
    Q_INVOKABLE void resetSlot(int n);             // back to coded default
    Q_INVOKABLE void setLabel(int n, const QString &label);

signals:
    void activeChanged();
    void slotChanged(int n);

private:
    void onFreqChanged();
    void onModeChanged();
    void persist(int n) const;
    static bool valid(int n) { return n >= 1 && n <= 3; }

    struct Slot { int freq; QString mode; QString label; };
    Slot                  slots_[4];     // index 1..3
    int                   active_ = 0;
    Prefs                *prefs_  = nullptr;
    lyra::ipc::HL2Stream *stream_ = nullptr;
};

} // namespace lyra::ui
