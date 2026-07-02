// Lyra-cpp — #89 Voice keyer, Stage B (B4): panel controller.  See VoiceKeyer.h.

#include "tx/VoiceKeyer.h"

#include "tx/ClipBank.h"
#include "tx/ClipRecorder.h"
#include "tx/ClipRecorderPlayer.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace lyra::tx {

VoiceKeyer::VoiceKeyer(ClipBank *bank, QObject *parent)
    : QObject(parent), bank_(bank),
      player_(std::make_unique<ClipRecorderPlayer>()),
      recorder_(std::make_unique<ClipRecorder>()) {
    load();
    poll_ = new QTimer(this);
    poll_->setInterval(80);   // progress / record-elapsed poll while active
    connect(poll_, &QTimer::timeout, this, &VoiceKeyer::onPollTick);
}

int VoiceKeyer::recordMs() const {
    return recorder_ ? recorder_->durationMs() : 0;
}

VoiceKeyer::~VoiceKeyer() {
    if (player_) player_->stop();
    if (recorder_ && recorder_->recording()) recorder_->stop();
}

double VoiceKeyer::progress() const {
    return player_ ? player_->progress() : 0.0;
}

void VoiceKeyer::setGainDb(double db) {
    db = std::clamp(db, -40.0, 20.0);
    if (std::abs(db - gainDb_) < 1e-9) return;
    gainDb_ = db;
    save();
    emit gainDbChanged();
}

void VoiceKeyer::setBypassDsp(bool on) {
    if (on == bypassDsp_) return;
    bypassDsp_ = on;
    save();
    emit bypassDspChanged();
}

void VoiceKeyer::setLive(bool on) {
    if (on == live_) return;
    live_ = on;
    save();
    emit liveChanged();
}

void VoiceKeyer::play(const QString &id, bool ota) {
    if (!live_ || !bank_ || !player_) return;   // gated on the operator opt-in
    if (!ota && !reviewReady()) return;         // local Review lands in Stage C
    auto samples = bank_->loadSamples(id);
    if (!samples || samples->empty()) return;

    const double clipGainDb = bank_->gainDbOf(id);
    ClipRecorderPlayer::PlayOptions opt;
    opt.ota       = ota;
    opt.bypassDsp = bypassDsp_;   // the voice keyer's single Bypass-TX-DSP toggle
    opt.gainLin   = std::pow(10.0, (gainDb_ + clipGainDb) / 20.0);

    if (!player_->play(std::move(samples), opt)) return;   // busy / key-blocked
    playingId_ = id;
    ota_       = ota;
    poll_->start();
    emit playingChanged();
    emit progressChanged();
}

void VoiceKeyer::stop() {
    if (player_) player_->stop();
    if (poll_) poll_->stop();
    if (!playingId_.isEmpty()) {
        playingId_.clear();
        emit playingChanged();
    }
    emit progressChanged();
}

void VoiceKeyer::onPollTick() {
    bool active = false;
    if (recording_) { emit recordMsChanged(); active = true; }
    if (player_ && player_->playing()) { emit progressChanged(); active = true; }
    else if (!playingId_.isEmpty()) {   // clip finished / drained on its own
        playingId_.clear();
        emit playingChanged();
        emit progressChanged();
    }
    if (!active) poll_->stop();
}

// ── Record (Stage C).  kind 0 = Voice (mic), 1 = RX (needs the RX tap, C2). ──
void VoiceKeyer::startRecord(int kind) {
    if (!recorder_ || recording_) return;
    recorder_->start(kind == 1 ? ClipRecorder::Source::Rx : ClipRecorder::Source::Mic);
    recording_ = true;
    poll_->start();
    emit recordingChanged();
    emit recordMsChanged();
}

void VoiceKeyer::stopRecord(const QString &label) {
    if (!recorder_ || !recording_) return;
    const ClipRecorder::Source src = recorder_->source();
    std::vector<float> samples = recorder_->stop();
    recording_ = false;
    if (player_ && !player_->playing()) poll_->stop();
    if (!samples.empty() && bank_) {
        const int kind = (src == ClipRecorder::Source::Rx) ? ClipBank::Rx : ClipBank::Voice;
        QString lbl = label.trimmed();
        if (lbl.isEmpty())
            lbl = tr("Message %1").arg(bank_->clips().size() + 1);
        bank_->addFromSamples(lbl, kind, samples);
    }
    emit recordingChanged();
    emit recordMsChanged();
}

// ── Clip-management dialogs (GUI thread) ──
void VoiceKeyer::importClipDialog() {
    if (!bank_) return;
    const QString path = QFileDialog::getOpenFileName(
        nullptr, tr("Import voice clip"), QString(),
        tr("Audio clips (*.wav *.WAV)"));
    if (path.isEmpty()) return;
    const QString label = QFileInfo(path).completeBaseName();
    bank_->importWav(label, ClipBank::Voice, path);   // normalised to 48k mono
}

void VoiceKeyer::openClipsFolder() const {
    if (!bank_) return;
    QDir().mkpath(bank_->clipsDir());
    QDesktopServices::openUrl(QUrl::fromLocalFile(bank_->clipsDir()));
}

void VoiceKeyer::changeClipsFolderDialog() {
    if (!bank_) return;
    const QString dir = QFileDialog::getExistingDirectory(
        nullptr, tr("Choose clips folder"), bank_->clipsDir());
    if (!dir.isEmpty()) bank_->setClipsDir(dir);
}

void VoiceKeyer::playByFkey(int fn) {
    if (!live_ || !bank_) return;
    const QString id = bank_->idForFkey(fn);
    if (!id.isEmpty()) playOta(id);
}

// ── persistence (QSettings clips/*) ──
void VoiceKeyer::load() {
    QSettings s;
    s.beginGroup(QStringLiteral("clips"));
    gainDb_    = std::clamp(s.value(QStringLiteral("playbackGainDb"), 0.0).toDouble(),
                            -40.0, 20.0);
    bypassDsp_ = s.value(QStringLiteral("bypassDsp"), false).toBool();
    live_      = s.value(QStringLiteral("txEnabled"), false).toBool();   // opt-in, default OFF
    s.endGroup();
}

void VoiceKeyer::save() const {
    QSettings s;
    s.beginGroup(QStringLiteral("clips"));
    s.setValue(QStringLiteral("playbackGainDb"), gainDb_);
    s.setValue(QStringLiteral("bypassDsp"), bypassDsp_);
    s.setValue(QStringLiteral("txEnabled"), live_);
    s.endGroup();
}

} // namespace lyra::tx
