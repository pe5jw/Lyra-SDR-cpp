// Lyra — Backup & Restore engine (Settings → Backup & Restore tab).
//
// A "backup" is the operator's whole configuration written to a portable
// `.lyra` file (INI-format QSettings dump).  Restore is SELECTIVE: the
// operator ticks which sections to pull back in, so a bad tweak in one
// subsystem can be reverted without clobbering unrelated settings.
//
// The section catalogue below is the single source of truth mapping the
// QSettings keyspace onto operator-facing groups.  Every key maps to exactly
// one section by key-prefix; keys matching no section fall to "advanced".
// New settings keys therefore inherit a section automatically (or land in
// "advanced"), so this stays correct as Lyra grows.
//
// Pure engine: no UI, no MainWindow dependency.  MainWindow coordinates the
// layout-flush + restart offer; SettingsDialog builds the tab.

#pragma once

#include <QDateTime>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace lyra::backup {

// One operator-facing group of settings, owned by a set of key-prefixes.
struct Section {
    QString     id;        // stable id ("tx"), used in QSettings + code
    QString     label;     // "TX audio chain"
    QString     desc;      // one-line description for the checklist
    QStringList prefixes;  // key-prefixes this section owns (startsWith match)
    bool        layout;    // true = the panel-layout section (disruptive; a
                           // restore can re-apply it live / it defaults OFF)
};

// The catalogue, in display order.  "advanced" is implicit (any key owned by
// no section) — the UI adds it as a final "Everything else" row.
const QVector<Section> &sections();

// Section id that owns <key> ("advanced" if none).  Returns "" for a
// machine-specific key (those never belong to a restorable section).
QString sectionForKey(const QString &key);

// The id used for the implicit catch-all row.
QString advancedId();

// Keys that must NEVER travel in a backup — hardware / connection identity
// and per-PC launch paths.  Filtered on BOTH export and restore.
bool isMachineSpecificKey(const QString &key);

// ── Export ───────────────────────────────────────────────────────────────
// Dump every non-machine-specific live setting to <path> as INI.  A small
// self-describing "[_backup]" header (name / created / automatic) is written
// too; it is ignored on restore.  Returns true on success.
bool exportToFile(const QString &path, const QString &name = QString(),
                  bool automatic = false);

// ── Restore ──────────────────────────────────────────────────────────────
// Write back to live QSettings only the keys whose owning section id is in
// <sectionIds> (machine-specific keys + the "[_backup]" header always
// skipped).  Returns the number of keys written; sets *touchedLayout when a
// layout-section key was among them (caller may re-apply the layout live).
int restoreFromFile(const QString &path, const QSet<QString> &sectionIds,
                    bool *touchedLayout = nullptr);

// Section ids (incl. advancedId()) that actually have ≥1 key in <path> — so
// the checklist only offers sections the file can supply.
QSet<QString> sectionsPresentInFile(const QString &path);

// ── Snapshot store ─────────────────────────────────────────────────────────
// Dated / named `.lyra` files under <AppData>/Lyra/snapshots.  Auto snapshots
// are taken every N launches (a rolling ring of the newest <keep>); manual
// snapshots are kept until the operator deletes them.
struct SnapInfo {
    QString   path;
    QString   name;       // operator-given name, or "" for an auto snapshot
    bool      automatic;  // true = launch-cadence snapshot
    QDateTime when;       // creation time
};

QString            snapshotDir();
QVector<SnapInfo>  listSnapshots();                 // newest first
// Flush is the caller's job (layout must be current); returns the file path
// (or "" on failure).  Prunes auto snapshots to <keep> afterwards.
QString            saveSnapshot(const QString &name, bool automatic, int keepAuto);
void               deleteSnapshot(const QString &path);

// Prefs keys (Settings → Backup & Restore).
inline constexpr auto kAutoEvery = "backup/autoEveryLaunches";  // 0 = off; default 10
inline constexpr auto kKeepCount = "backup/keepCount";          // default 8
inline constexpr auto kLaunchCtr = "backup/launchCounter";

// Defaults.
inline constexpr int kAutoEveryDefault = 10;
inline constexpr int kKeepCountDefault = 8;

// Called ONCE at launch: bump the launch counter and, if due per kAutoEvery,
// write an auto snapshot + prune to kKeepCount.  No-op when kAutoEvery == 0.
void autoSnapshotOnLaunch();

}  // namespace lyra::backup
