// Lyra — parametric-EQ model (the "model" half of the EQ model/view).
//
// Owns a lyra::dsp::ParamEq engine and exposes it to QML (EqPanel.qml)
// as the `Eq` context property.  The QML view draws the summed response
// from magnitudeDb(), drags the typed band nodes, and edits the tile row;
// every edit lands on the engine and re-emits bandsChanged() so the curve
// redraws.  Stage 3 points the TX mic rack at engine() so what the
// operator dials here is what transmits.  Wire-INERT until then.
//
// QML can't bind to a C-array of band structs, so per-band state is
// reached through index-keyed Q_INVOKABLE getters/setters (0..numBands-1)
// and the view forces re-evaluation off bandsChanged() (a revision tick).

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include <atomic>
#include <vector>

#include "dsp/ParamEq.h"
#include "dsp/EqAnalyzer.h"

class QTimer;

namespace lyra::ui {

class EqModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool bypass READ bypass WRITE setBypass NOTIFY bypassChanged)
    Q_PROPERTY(double makeupDb READ makeupDb WRITE setMakeupDb NOTIFY makeupDbChanged)
    Q_PROPERTY(int selectedBand READ selectedBand WRITE setSelectedBand
                   NOTIFY selectedBandChanged)
    Q_PROPERTY(int numBands READ numBands CONSTANT)
    // Stage-2b analyzer (live mic spectrum behind the curve).
    // analyzerMode: 0=Off 1=Spectrum (line/fill) 2=RTA (band bars).
    Q_PROPERTY(int analyzerMode READ analyzerMode WRITE setAnalyzerMode NOTIFY spectrumOptsChanged)
    Q_PROPERTY(bool accumulate READ accumulate WRITE setAccumulate NOTIFY spectrumOptsChanged)
    Q_PROPERTY(bool beforeAfterMod READ beforeAfterMod WRITE setBeforeAfterMod NOTIFY spectrumOptsChanged)
    // Peak-hold hang/decay preset: 0=Fast 1=Medium 2=Slow 3=Hold(no decay)
    // 4=Live (no hold — the trace tracks the instantaneous spectrum).
    Q_PROPERTY(int peakDecayMode READ peakDecayMode WRITE setPeakDecayMode NOTIFY spectrumOptsChanged)
    Q_PROPERTY(int specBins READ specBins CONSTANT)
    Q_PROPERTY(double specNyquist READ specNyquist CONSTANT)

public:
    // Mirror of lyra::dsp::ParamEq::Type, in the SAME order, so QML sees
    // Eq.Peak / Eq.LowShelf / … and an int round-trips by static_cast.
    enum Type { Peak, LowShelf, HighShelf, LowPass, HighPass, BandPass, Notch };
    Q_ENUM(Type)

    explicit EqModel(QObject *parent = nullptr);
    ~EqModel() override;

    // C-callable bridge for the wire-layer TX pump: CMaster registers this
    // via SendpTxEqProcessor and calls it per TX block on the mic buffer
    // (interleaved {I,Q} doubles) just before fexchange0.  Runs on the
    // cm_main TX thread; routes through this process's active EQ engine
    // (published in the ctor, cleared in the dtor — atomic, lock-free read).
    // No-op when no EqModel exists or the EQ is bypassed.
    static void txProcessCb(int nSamples, double *iqPairs);

    bool   bypass() const { return bypass_; }
    double makeupDb() const { return makeupDb_; }
    int    selectedBand() const { return selected_; }
    int    numBands() const { return lyra::dsp::ParamEq::kNumBands; }

    void setBypass(bool on);
    void setMakeupDb(double db);
    void setSelectedBand(int i);

    // Per-band read (index 0..numBands-1; bandType returns a Type int).
    Q_INVOKABLE bool   bandEnabled(int i) const;
    Q_INVOKABLE int    bandType(int i) const;
    Q_INVOKABLE double bandFreq(int i) const;
    Q_INVOKABLE double bandGain(int i) const;
    Q_INVOKABLE double bandQ(int i) const;

    // Per-band edit (each clamps, applies to the engine, emits bandsChanged).
    Q_INVOKABLE void setBandEnabled(int i, bool on);
    Q_INVOKABLE void setBandType(int i, int type);
    Q_INVOKABLE void setBandFreq(int i, double hz);
    Q_INVOKABLE void setBandGain(int i, double db);
    Q_INVOKABLE void setBandQ(int i, double q);
    Q_INVOKABLE void resetBand(int i);   // restore that band's default
    Q_INVOKABLE void resetAll();         // restore every band's default (flatten)

    // Summed response in dB at freqHz — drives the drawn curve (what you
    // see is what you hear: same coeffs the engine's process() applies).
    Q_INVOKABLE double magnitudeDb(double freqHz) const;

    // QML label helpers.
    Q_INVOKABLE QString typeName(int type) const;
    Q_INVOKABLE bool    typeUsesGain(int type) const;  // Peak + shelves only

    // The live engine — Stage 3 hands this to the TX mic rack.
    lyra::dsp::ParamEq *engine() { return &eq_; }

    // ── Stage-2b analyzer (live mic spectrum behind the curve) ──────────
    int    analyzerMode()   const { return analyzerMode_; }
    bool   accumulate()     const { return accumulate_; }
    bool   beforeAfterMod() const { return beforeAfter_; }
    int    peakDecayMode()  const { return peakDecay_; }
    int    specBins()       const { return analyzer_.bins(); }
    double specNyquist()    const { return analyzer_.nyquist(); }
    void setAnalyzerMode(int mode);
    void setAccumulate(bool on);
    void setBeforeAfterMod(bool on);
    void setPeakDecayMode(int mode);
    // Latest dBFS magnitude bins (length specBins): post-EQ ("after") and
    // pre-EQ ("before").  Refreshed off the GUI poll; QML reads them in
    // onPaint after a spectrumChanged().
    Q_INVOKABLE QVariantList spectrumPost() const { return postList_; }
    Q_INVOKABLE QVariantList spectrumPre() const { return preList_; }

signals:
    void bandsChanged();        // any band edit → redraw the curve + nodes
    void bypassChanged();
    void makeupDbChanged();
    void selectedBandChanged();
    void spectrumChanged();     // new analyzer frame published (→ repaint)
    void spectrumOptsChanged(); // a Spectrum/Accumulate/Before-After toggle

private slots:
    void pollSpectrum();        // GUI-thread: drain the analyzer ~25 Hz

private:
    bool valid(int i) const {
        return i >= 0 && i < lyra::dsp::ParamEq::kNumBands;
    }

    // Active engine + analyzer for the wire-layer TX pump (txProcessCb).
    // Set in the ctor, cleared in the dtor.  Lock-free reads on the TX thread.
    static std::atomic<lyra::dsp::ParamEq *>     s_txEngine;
    static std::atomic<lyra::dsp::EqAnalyzer *>  s_txAnalyzer;

    lyra::dsp::ParamEq      eq_;
    lyra::dsp::EqAnalyzer   analyzer_;
    QTimer                 *pollTimer_ = nullptr;
    QVariantList            preList_, postList_;   // cached dBFS bins for QML
    std::vector<float>      preBins_, postBins_;   // scratch snapshot buffers
    bool   bypass_      = false;
    double makeupDb_    = 0.0;
    int    selected_    = 0;
    int    analyzerMode_ = 1;      // 0 Off / 1 Spectrum / 2 RTA
    bool   accumulate_  = false;
    bool   beforeAfter_ = false;
    int    peakDecay_   = 1;        // Medium
};

}  // namespace lyra::ui
