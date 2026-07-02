// Lyra-cpp — #89 Voice keyer, Stage B (B4): the panel controller.
//
// The UI-facing controller behind VoiceKeyerPanel.qml (the floating "Voice
// Keyer" dock) + the F1..F12 voice-mode accelerators.  It owns the Stage-A
// injector (ClipRecorderPlayer) and drives it from the ClipBank (B3):
//   * playOta(id)    — load the clip's 48 kHz mono samples + play them into
//                      the mic-input funnel with MOX (transmit).  The clip
//                      runs the full TX DSP chain (PS-safe, design §5).
//   * playReview(id) — same, but no key (local monitor).
//   * stop()         — abort.
// plus clip-management glue the panel calls: importClipDialog / openClips-
// Folder / changeClipsFolderDialog (GUI-thread QFileDialog/QDesktopServices).
//
// STAGING: the injector's live producer (fillBlock pumped by the mic funnel)
// + the KeyFn/BlockedFn seams are wired in B1 (the HL2 bench step).  Until
// then `live` is false and the panel disables Transmit/Review/Record (the
// disabled-XIT / disabled-TUN precedent) — clip management (import / rename /
// gain / F-key / delete / storage folder) is fully live now.  B1 calls
// setLive(true) + wires player() into the funnel; that one change lights up
// OTA/Review/Record + the F-keys with no panel change.
//
// Global voice-keyer state (playback gain + the single Bypass-TX-DSP toggle,
// per the locked design decisions) persists to QSettings (clips/*); per-clip
// trims live on the ClipBank.

#pragma once

#include <QObject>
#include <QString>

#include <memory>

class QTimer;

namespace lyra::tx {

class ClipBank;
class ClipRecorder;
class ClipRecorderPlayer;

class VoiceKeyer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool    live       READ live       WRITE setLive  NOTIFY liveChanged)
    Q_PROPERTY(QString playingId  READ playingId  NOTIFY playingChanged)
    Q_PROPERTY(bool    reviewing  READ reviewing  NOTIFY playingChanged)
    Q_PROPERTY(double  progress   READ progress   NOTIFY progressChanged)
    Q_PROPERTY(bool    recording  READ recording  NOTIFY recordingChanged)
    Q_PROPERTY(int     recordMs   READ recordMs   NOTIFY recordMsChanged)
    Q_PROPERTY(bool    reviewReady READ reviewReady CONSTANT)
    Q_PROPERTY(double  gainDb     READ gainDb     WRITE setGainDb     NOTIFY gainDbChanged)
    Q_PROPERTY(bool    bypassDsp  READ bypassDsp  WRITE setBypassDsp  NOTIFY bypassDspChanged)
public:
    explicit VoiceKeyer(ClipBank *bank, QObject *parent = nullptr);
    ~VoiceKeyer() override;

    bool    live()      const { return live_; }
    QString playingId() const { return playingId_; }
    bool    reviewing() const { return !playingId_.isEmpty() && !ota_; }
    double  progress()  const;
    bool    recording() const { return recording_; }
    int     recordMs()  const;
    // Local (no-key) Review needs its own monitor path — it does NOT go through
    // the TX injection hook (that would feed a parked TXA + risk leaving stale
    // clip audio in the TX ring for a later manual keydown).  Lands in Stage C
    // with the RX recorder / monitor; false at B1.
    bool    reviewReady() const { return false; }
    double  gainDb()    const { return gainDb_; }
    bool    bypassDsp() const { return bypassDsp_; }

    void setGainDb(double db);
    void setBypassDsp(bool on);

    // Operate (no-op until live; the panel disables the triggers meanwhile).
    Q_INVOKABLE void playOta(const QString &id)    { play(id, /*ota=*/true); }
    Q_INVOKABLE void playReview(const QString &id) { play(id, /*ota=*/false); }
    Q_INVOKABLE void stop();

    // Record — Stage C.  Present so the panel binds them; inert until then.
    Q_INVOKABLE void startRecord(int kind);
    Q_INVOKABLE void stopRecord(const QString &label);

    // Clip-management dialogs (GUI thread; the panel calls these).
    Q_INVOKABLE void importClipDialog();
    Q_INVOKABLE void openClipsFolder() const;
    Q_INVOKABLE void changeClipsFolderDialog();

    // Voice-mode F1..F12 accelerator (MainWindow::keyPressEvent).  fn = 1..12.
    void playByFkey(int fn);

    // Operator opt-in for voice-keyer TRANSMIT (default OFF, persisted).  The
    // injector + keying seams are always wired (B1); this gate is what lets a
    // deliberate OTA / F-key actually key — the disabled-until-bench-verified
    // posture (cf. the HW-PTT-input opt-in, hl2_stream `ff5f128`).  The panel
    // disables Transmit / Review / Record while it is off.
    void setLive(bool on);

    // The injector — B1 wires player()->fillBlock() into the mic funnel + sets
    // its KeyFn/BlockedFn seams (MainWindow, once, before the Ep6 thread runs).
    ClipRecorderPlayer *player() const { return player_.get(); }
    // The recorder — MainWindow wires the Ep6 mic tap (+ the RX tap in C2) to
    // its feed*() methods, once, before the Ep6 thread runs.
    ClipRecorder *recorder() const { return recorder_.get(); }

signals:
    void liveChanged();
    void playingChanged();
    void progressChanged();
    void recordingChanged();
    void recordMsChanged();
    void gainDbChanged();
    void bypassDspChanged();

private:
    void play(const QString &id, bool ota);
    void onPollTick();
    void load();
    void save() const;

    ClipBank                            *bank_ = nullptr;
    std::unique_ptr<ClipRecorderPlayer>  player_;
    std::unique_ptr<ClipRecorder>        recorder_;
    QTimer                              *poll_ = nullptr;
    bool     live_      = false;
    bool     recording_ = false;
    bool     ota_       = false;
    QString  playingId_;
    double   gainDb_    = 0.0;    // global playback gain (dB)
    bool     bypassDsp_ = false;  // the voice keyer's single Bypass-TX-DSP toggle
};

} // namespace lyra::tx
