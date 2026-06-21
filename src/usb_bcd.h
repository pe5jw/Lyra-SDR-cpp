// Lyra — USB-BCD band-data output for external linear amplifiers.
//
// The HL2 has no native BCD band output.  The community solution (and
// old Lyra's) is an FTDI cable in synchronous bit-bang mode: one byte
// write drives 8 output pins wired to the amp's BAND DATA input.  We
// write the Yaesu BCD band code on every band change.
//
// The FTDI D2XX driver (ftd2xx.dll) is RUNTIME-loaded (LoadLibrary +
// GetProcAddress) — no build-time SDK dependency, and if the DLL/driver
// isn't present the feature simply reports "unavailable" and stays off
// (exactly old Lyra's lazy-import posture).  Mirrors how Lyra already
// runtime-loads the WDSP DLLs.
//
// ⚠ SAFETY: the wrong BCD code at high power can route TX through the
// wrong amp filter and destroy it.  Default OFF; the operator must
// verify wiring + low-power test per band before keying at full output.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace lyra::ui {

class UsbBcd : public QObject {
    Q_OBJECT
    // True if ftd2xx.dll loaded (driver present) — gates the UI.
    Q_PROPERTY(bool available     READ available     CONSTANT)
    Q_PROPERTY(bool enabled       READ enabled       WRITE setEnabled
               NOTIFY enabledChanged)
    Q_PROPERTY(QString serial     READ serial        WRITE setSerial
               NOTIFY serialChanged)
    Q_PROPERTY(bool sixtyAsForty  READ sixtyAsForty  WRITE setSixtyAsForty
               NOTIFY sixtyAsFortyChanged)
    Q_PROPERTY(bool elevenAsTen   READ elevenAsTen   WRITE setElevenAsTen
               NOTIFY elevenAsTenChanged)
    // Live BCD code currently asserted (readout).
    Q_PROPERTY(int currentBcd     READ currentBcd    NOTIFY currentBcdChanged)

public:
    explicit UsbBcd(QObject *parent = nullptr);
    ~UsbBcd() override;

    bool    available()    const { return libLoaded_; }
    bool    enabled()      const { return enabled_; }
    QString serial()       const { return serial_; }
    bool    sixtyAsForty() const { return sixtyAsForty_; }
    bool    elevenAsTen()  const { return elevenAsTen_; }
    int     currentBcd()   const { return currentBcd_; }

    // FTDI device serials currently attached (for the Settings picker).
    Q_INVOKABLE QStringList devices() const;

    void setEnabled(bool on);
    void setSerial(const QString &serial);
    void setSixtyAsForty(bool on);
    void setElevenAsTen(bool on);

public slots:
    // Recompute + assert the BCD code for the given RX frequency.  Wired
    // to the stream's rx1FreqChanged so the amp follows the band.
    void applyForFreq(quint32 hz);

signals:
    void enabledChanged(bool on);
    void serialChanged(const QString &serial);
    void sixtyAsFortyChanged(bool on);
    void elevenAsTenChanged(bool on);
    void currentBcdChanged(int code);
    void statusMessage(const QString &msg);

private:
    void openDevice();    // open serial_ in sync-bitbang mode
    void closeDevice();
    void writeByte(int value);
    void reapply();       // recompute BCD from last freq + write

    bool       libLoaded_   = false;   // ftd2xx.dll present?
    bool       enabled_     = false;
    QString    serial_;
    bool       sixtyAsForty_ = true;   // most amps share 40m for 60m
    bool       elevenAsTen_  = true;   // 11m/CB uses the 10m filter code
    int        currentBcd_  = -1;      // -1 = nothing asserted yet
    int        lastValue_   = -1;      // last byte written (dedup)
    quint32    lastFreqHz_  = 0;
    void      *handle_      = nullptr; // FT_HANDLE (opaque)
};

} // namespace lyra::ui
