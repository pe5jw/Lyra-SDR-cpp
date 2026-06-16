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

#include "dsp/ParamEq.h"

namespace lyra::ui {

class EqModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool bypass READ bypass WRITE setBypass NOTIFY bypassChanged)
    Q_PROPERTY(double makeupDb READ makeupDb WRITE setMakeupDb NOTIFY makeupDbChanged)
    Q_PROPERTY(int selectedBand READ selectedBand WRITE setSelectedBand
                   NOTIFY selectedBandChanged)
    Q_PROPERTY(int numBands READ numBands CONSTANT)

public:
    // Mirror of lyra::dsp::ParamEq::Type, in the SAME order, so QML sees
    // Eq.Peak / Eq.LowShelf / … and an int round-trips by static_cast.
    enum Type { Peak, LowShelf, HighShelf, LowPass, HighPass, BandPass, Notch };
    Q_ENUM(Type)

    explicit EqModel(QObject *parent = nullptr);

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

    // Summed response in dB at freqHz — drives the drawn curve (what you
    // see is what you hear: same coeffs the engine's process() applies).
    Q_INVOKABLE double magnitudeDb(double freqHz) const;

    // QML label helpers.
    Q_INVOKABLE QString typeName(int type) const;
    Q_INVOKABLE bool    typeUsesGain(int type) const;  // Peak + shelves only

    // The live engine — Stage 3 hands this to the TX mic rack.
    lyra::dsp::ParamEq *engine() { return &eq_; }

signals:
    void bandsChanged();        // any band edit → redraw the curve + nodes
    void bypassChanged();
    void makeupDbChanged();
    void selectedBandChanged();

private:
    bool valid(int i) const {
        return i >= 0 && i < lyra::dsp::ParamEq::kNumBands;
    }

    lyra::dsp::ParamEq eq_;
    bool   bypass_   = false;
    double makeupDb_ = 0.0;
    int    selected_ = 0;
};

}  // namespace lyra::ui
