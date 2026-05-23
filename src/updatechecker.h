// Lyra — GitHub release update checker.
//
// Mirrors old Lyra's update_check.py: asks the GitHub Releases API for
// the repo's releases, picks the HIGHEST parsed version (so pre-releases
// are seen, not just /latest), and compares it to the running build's
// LYRA_VERSION.  All async via QNetworkAccessManager; the UI (MainWindow)
// owns the notification UX (startup modal once-per-version + a toolbar
// indicator + a manual "Check for Updates…" dialog).
//
// Native C++ / Qt — no Python.  Repo coordinates + version come from the
// CMake-defined LYRA_REPO_OWNER / LYRA_REPO_NAME / LYRA_VERSION macros
// (single source of truth — see CMakeLists.txt).

#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;

namespace lyra::ui {

class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QObject *parent = nullptr);

    // Start an async check.  Emits exactly one of the signals below.
    void check();

    // True if remoteTag (e.g. "v0.2.0") is a newer version than
    // localVer (e.g. "0.1.0").  Tolerant of a leading 'v' and an
    // optional 4th component; unparseable input → false (no nag).
    static bool isNewer(const QString &remoteTag, const QString &localVer);

signals:
    void updateAvailable(const QString &tag, const QString &url,
                         const QString &body);
    void noUpdate();
    void checkFailed(const QString &reason);

private:
    QNetworkAccessManager *nam_ = nullptr;
};

} // namespace lyra::ui
