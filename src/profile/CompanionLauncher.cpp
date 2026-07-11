// Lyra — CompanionLauncher.  See CompanionLauncher.h + #193.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#endif

#include "profile/CompanionLauncher.h"

#include <QFileInfo>
#include <QProcess>
#include <QSettings>

#include <algorithm>

namespace lyra::profile {

CompanionLauncher::CompanionLauncher(QObject *parent) : QObject(parent) {
    pending_.setSingleShot(true);
    connect(&pending_, &QTimer::timeout, this,
            [this]() { startApp(pendingCommand_, pendingArgs_); });
}

QString CompanionLauncher::keyOf(const QString &profile, const char *leaf) {
    return QStringLiteral("profileLaunch/%1/%2")
        .arg(profile, QLatin1String(leaf));
}

CompanionLauncher::Config CompanionLauncher::config(const QString &profile) const {
    Config c;
    if (profile.isEmpty()) return c;
    QSettings s;
    c.enabled = s.value(keyOf(profile, "enabled"), false).toBool();
    c.name    = s.value(keyOf(profile, "name"), QString()).toString();
    c.command = s.value(keyOf(profile, "command"), QString()).toString();
    c.args    = s.value(keyOf(profile, "args"), QString()).toString();
    c.delayMs = s.value(keyOf(profile, "delay_ms"), 2500).toInt();
    return c;
}

void CompanionLauncher::setConfig(const QString &profile, const Config &c) {
    if (profile.isEmpty()) return;
    QSettings s;
    s.setValue(keyOf(profile, "enabled"), c.enabled);
    s.setValue(keyOf(profile, "name"), c.name);
    s.setValue(keyOf(profile, "command"), c.command);
    s.setValue(keyOf(profile, "args"), c.args);
    s.setValue(keyOf(profile, "delay_ms"), c.delayMs);
}

void CompanionLauncher::renameBinding(const QString &oldName,
                                      const QString &newName) {
    if (oldName == newName || oldName.isEmpty() || newName.isEmpty()) return;
    const Config c = config(oldName);
    const bool hasBinding =
        c.enabled || !c.command.isEmpty() || !c.name.isEmpty();
    removeBinding(oldName);
    if (hasBinding) setConfig(newName, c);
}

void CompanionLauncher::removeBinding(const QString &profile) {
    if (profile.isEmpty()) return;
    QSettings s;
    s.remove(QStringLiteral("profileLaunch/%1").arg(profile));   // whole group
}

void CompanionLauncher::launchFor(const QString &profile) {
    const Config c = config(profile);
    if (!c.enabled || c.command.trimmed().isEmpty()) return;
    const QString shown =
        c.name.isEmpty() ? QFileInfo(c.command).fileName() : c.name;
    if (isRunning(c.command)) {
        emit statusMessage(
            tr("%1 is already running — not relaunched.").arg(shown));
        return;
    }
    // Last pick wins: a fresh selection during the delay replaces the pending
    // launch rather than spawning both.
    pendingCommand_ = c.command;
    pendingArgs_    = c.args;
    pending_.start(std::max(0, c.delayMs));
    if (c.delayMs >= 1000)
        emit statusMessage(tr("Launching %1 in %2 s…")
                               .arg(shown)
                               .arg(QString::number(c.delayMs / 1000.0, 'g', 2)));
    else
        emit statusMessage(tr("Launching %1…").arg(shown));
}

bool CompanionLauncher::testLaunch(const QString &command, const QString &args) {
    return startApp(command, args);
}

bool CompanionLauncher::launchDetached(const QString &command,
                                       const QString &args) {
    const QString cmd = command.trimmed();
    if (cmd.isEmpty()) return false;
    const QStringList argList =
        args.trimmed().isEmpty() ? QStringList() : QProcess::splitCommand(args);
    const QString workDir = QFileInfo(cmd).absolutePath();
    const QString suffix  = QFileInfo(cmd).suffix().toLower();

    if (suffix == QLatin1String("bat") || suffix == QLatin1String("cmd")) {
        // A .bat/.cmd is not an executable — run it through the shell.
        QStringList full;
        full << QStringLiteral("/c") << cmd << argList;
        return QProcess::startDetached(QStringLiteral("cmd.exe"), full, workDir);
    }
    return QProcess::startDetached(cmd, argList, workDir);
}

bool CompanionLauncher::startApp(const QString &command, const QString &args) {
    const bool ok = launchDetached(command, args);
    const QString shown = QFileInfo(command.trimmed()).fileName();
    if (ok) emit statusMessage(tr("Launched %1.").arg(shown));
    else    emit statusMessage(
        tr("Couldn't launch %1 — check the path.").arg(shown));
    return ok;
}

bool CompanionLauncher::isRunning(const QString &command) {
#ifdef _WIN32
    const QString exe = QFileInfo(command).fileName();   // e.g. "VarAC.exe"
    if (exe.isEmpty()) return false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (exe.compare(QString::fromWCharArray(pe.szExeFile),
                            Qt::CaseInsensitive) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
#else
    Q_UNUSED(command);
    return false;
#endif
}

}  // namespace lyra::profile
