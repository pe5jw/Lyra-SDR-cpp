// Lyra — EiBi shortwave-broadcaster overlay (ported from old Lyra's
// swdb/ package: eibi_parser + time_filter + store + overlay_gate +
// downloader).
//
// Loads the EiBi schedule (eibispace.de "sked-<season>.csv", 11-column
// ';'-delimited), keeps the entries sorted by frequency, and answers
// "what's broadcasting in this panadapter span right now?" — filtered
// by an integer minute-of-day + day-of-week on-air predicate (UTC).
// The panadapter draws the result as station ticks/labels (cyan when
// on-air, grey when off), click-to-tune in AM.
//
// The data is NOT bundled (EiBi's licence: free, attribution, no
// redistribution).  The operator downloads it from Settings -> Bands ->
// SW Database; it lands in %APPDATA%/<app>/swdb/.  Overlay is OFF by
// default and suppressed inside amateur bands unless "force all bands".

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>

class QNetworkAccessManager;
class QUrl;

namespace lyra::ui {

class Prefs;
class Bands;

class EibiStore : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY settingsChanged)
public:
    EibiStore(Prefs *prefs, Bands *bands, QObject *parent = nullptr);

    bool    loaded() const { return loaded_; }
    int     count() const { return entries_.size(); }
    QString filePath() const;          // custom override, else season file
    QString statusText() const;        // for the Settings label

    // Operator settings (persisted to QSettings "swdb/...").
    bool enabled() const        { return enabled_; }
    void setEnabled(bool on);
    bool hideOffAir() const     { return hideOffAir_; }
    void setHideOffAir(bool on);
    bool forceAllBands() const  { return forceAll_; }
    void setForceAllBands(bool on);
    int  minPower() const       { return minPower_; }
    void setMinPower(int p);

    // QML overlay query.  Returns [] when the overlay is disabled, the
    // store isn't loaded, or the VFO is inside an amateur band (and
    // "force all" is off).  Each entry: { freqHz, station, language,
    // target, onAir }.  Capped to avoid clutter at wide spans.
    Q_INVOKABLE QVariantList entriesInSpan(double centerHz, double spanHz) const;

    Q_INVOKABLE bool reload();         // re-read the CSV from disk
    Q_INVOKABLE void update();         // download the current season
    // Import a CSV the operator downloaded themselves (eibispace.de's
    // cert expires periodically, breaking the in-app download — this is
    // the reliable fallback).  Copies it into the app's swdb folder and
    // reloads.  Returns false if the file can't be read/parsed.
    Q_INVOKABLE bool importFile(const QString &srcPath);

signals:
    void changed();                    // data (re)loaded — QML re-queries
    void settingsChanged();            // a toggle changed
    void downloadFinished(bool ok, const QString &message);

private:
    struct Entry {
        int     freqKhz = 0;
        int     tStart  = 0;   // UTC minutes since midnight
        int     tStop   = 0;
        quint8  days    = 0;   // bit(dow-1); 0 = every day
        quint8  power   = 0;   // 0..3
        QString station, lang, target;
    };

    bool parseFile(const QString &path);
    void fetch(const QUrl &url, bool allowHttpFallback);   // download helper
    bool isOnAir(const Entry &e) const;   // uses current UTC
    bool gatedOff(double centerHz) const; // in ham band & !forceAll
    QString seasonFilePath() const;       // %APPDATA%/<app>/swdb/sked-<season>.csv
    static QString currentSeasonTag();    // e.g. "b25"

    Prefs                 *prefs_ = nullptr;
    Bands                 *bands_ = nullptr;
    QNetworkAccessManager *net_   = nullptr;
    QVector<Entry>         entries_;       // sorted by freqKhz
    bool                   loaded_  = false;
    QString                loadedFrom_;

    bool enabled_   = false;
    bool hideOffAir_= true;
    bool forceAll_  = false;
    int  minPower_  = 1;
};

} // namespace lyra::ui
