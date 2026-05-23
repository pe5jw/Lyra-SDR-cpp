// Lyra — QMainWindow dock shell.  See mainwindow.h.

#include "mainwindow.h"

#include "bands.h"
#include "help.h"
#include "helpdialog.h"
#include "hl2_discovery.h"
#include "hl2_stream.h"
#include "wdsp_engine.h"
#include "prefs.h"
#include "settingsdialog.h"
#include "usb_bcd.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QMessageBox>
#include <QQmlContext>
#include <QQuickWidget>
#include <QSettings>
#include <QStyle>
#include <QToolBar>
#include <QToolButton>
#include <QDateTime>
#include <QTimer>
#include <QDesktopServices>
#include <QPushButton>
#include <QUrl>

#include "updatechecker.h"

#include <utility>

namespace lyra::ui {

namespace {
// Standard feature set for an UNlocked dock: drag, float, close.
constexpr QDockWidget::DockWidgetFeatures kUnlockedFeatures =
    QDockWidget::DockWidgetMovable |
    QDockWidget::DockWidgetFloatable |
    QDockWidget::DockWidgetClosable;
} // namespace

MainWindow::MainWindow(QObject *discovery, QObject *stream,
                       QObject *wdsp, QObject *wdspEngine,
                       Prefs *prefs, QWidget *parent)
    : QMainWindow(parent),
      discovery_(discovery), stream_(stream),
      wdsp_(wdsp), wdspEngine_(wdspEngine), prefs_(prefs) {
    setWindowTitle(
        QStringLiteral("Lyra — Hermes Lite 2 / 2+ — v0.0.4 (C++23 / Qt 6)"));
    setObjectName(QStringLiteral("LyraMainWindow"));
    resize(1100, 760);

    // Full QDockWidget feature set: nested + tabbed docks, animated,
    // grouped-drag handle — the panel-arranging UX the operator wants.
    setDockOptions(QMainWindow::AnimatedDocks |
                   QMainWindow::AllowNestedDocks |
                   QMainWindow::AllowTabbedDocks |
                   QMainWindow::GroupedDragging);

    // Help bridge — exposed to every QML panel's "?" badge.  Created
    // before any makeQuick() so it can be set as a context property.
    help_ = new Help(this);
    connect(help_, &Help::guideRequested, this, &MainWindow::showHelp);
    connect(help_, &Help::settingsRequested,
            this, &MainWindow::openSettingsTopic);

    // Amateur band table — exposed to QML for the Band panel.
    bands_ = new Bands(this);

    // USB-BCD amp band output — follows the band off the stream's freq.
    usbBcd_ = new UsbBcd(this);
    if (auto *st = qobject_cast<lyra::ipc::HL2Stream *>(stream_)) {
        connect(st, &lyra::ipc::HL2Stream::rx1FreqChanged, usbBcd_,
                [this, st]() { usbBcd_->applyForFreq(st->rx1FreqHz()); });
        usbBcd_->applyForFreq(st->rx1FreqHz());   // assert current band now
    }
    connect(usbBcd_, &UsbBcd::statusMessage, this, [](const QString &) {});

    // No central widget content: the old Main.qml dev scaffold (Log +
    // wire-stats banner + Found-radios) is retired — discovery moved to
    // Settings → Hardware, the log lives in the launching console, and
    // the wire-stats banner is past its usefulness.  A zero-height
    // placeholder collapses the central area so the docks (panadapter on
    // top, control panels below) fill the whole window — reclaiming the
    // real estate the scaffold used to waste.
    auto *placeholder = new QWidget(this);
    placeholder->setMaximumHeight(0);
    setCentralWidget(placeholder);

    buildDocks();      // populate docks_ (so the View menu can list them)
    buildMenus();      // File / View (dock toggles + Lock) / Help
    buildToolbar();
    restoreLayout();   // geometry + dock state + lock state
}

QQuickWidget *MainWindow::makeQuick(const QString &qmlFile) {
    auto *qw = new QQuickWidget(this);
    qw->setResizeMode(QQuickWidget::SizeRootObjectToView);
    // Expose all four services to every panel; each binds what it uses.
    qw->rootContext()->setContextProperty(
        QStringLiteral("Discovery"), discovery_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Stream"), stream_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Wdsp"), wdsp_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("WdspEngine"), wdspEngine_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Prefs"), prefs_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Help"), help_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Bands"), bands_);
    qw->setSource(QUrl(QStringLiteral("qrc:/qt/qml/Lyra/src/qml/") + qmlFile));
    // Diagnostic: if a panel's QML fails to load, the QQuickWidget goes
    // blank — dump the errors so we don't have to guess.
    if (qw->status() == QQuickWidget::Error) {
        QFile f(QCoreApplication::applicationDirPath()
                + QStringLiteral("/lyra_qml_err.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << "== " << qmlFile << " ==\n";
            for (const auto &e : qw->errors()) {
                ts << e.toString() << "\n";
            }
        }
    }
    return qw;
}

QDockWidget *MainWindow::addQuickDock(const QString &objectName,
                                      const QString &title,
                                      const QString &qmlFile,
                                      const QString &topic,
                                      Qt::DockWidgetArea area) {
    auto *dock = new QDockWidget(title, this);
    dock->setObjectName(objectName);   // load-bearing for saveState/restoreState
    dock->setAllowedAreas(Qt::AllDockWidgetAreas);
    dock->setFeatures(kUnlockedFeatures);
    dock->setWidget(makeQuick(qmlFile));
    // Custom title bar: title (left) + "?" / float / close (right), so
    // the help badge sits in the title row OUTSIDE the panel content.
    dock->setTitleBarWidget(makeDockTitleBar(dock, title, topic));
    addDockWidget(area, dock);
    docks_.insert(objectName, dock);
    return dock;
}

QWidget *MainWindow::makeDockTitleBar(QDockWidget *dock,
                                      const QString &title,
                                      const QString &topic) {
    auto *bar = new QWidget(dock);
    bar->setObjectName(QStringLiteral("dockTitleBar"));
    // Restore double-click-to-float (default QDockWidget gesture, lost
    // when we replace the title bar with a custom widget).
    bar->installEventFilter(this);
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(8, 2, 4, 2);
    row->setSpacing(2);

    // Title — far left, cyan accent, matching the old-Lyra dock header.
    auto *titleLbl = new QLabel(title.toUpper(), bar);
    titleLbl->setStyleSheet(QStringLiteral(
        "QLabel{color:#00e5ff;font-weight:700;letter-spacing:1px;}"));
    row->addWidget(titleLbl);
    row->addSpacing(8);   // small gap before the "?"

    // "?" help badge — right AFTER the title text.  Click pops the Help
    // guide / Settings menu (same two-item choice as before).
    auto *helpBtn = new QToolButton(bar);
    helpBtn->setObjectName(QStringLiteral("dockHelp"));
    helpBtn->setText(QStringLiteral("?"));
    helpBtn->setCursor(Qt::PointingHandCursor);
    helpBtn->setAutoRaise(true);
    helpBtn->setToolTip(tr("Help / Settings for this panel"));
    helpBtn->setStyleSheet(QStringLiteral(
        "QToolButton{color:#00e5ff;border:1px solid #00e5ff;"
        "border-radius:8px;min-width:16px;max-width:16px;"
        "min-height:16px;max-height:16px;font-weight:700;padding:0;}"
        "QToolButton:hover{background:rgba(0,229,255,40);color:#7ff7ff;}"));
    auto *menu = new QMenu(helpBtn);
    QObject::connect(menu->addAction(tr("Help guide")), &QAction::triggered,
                     this, [this, topic]() { showHelp(topic); });
    QObject::connect(menu->addAction(tr("Settings…")), &QAction::triggered,
                     this, [this, topic]() { openSettingsTopic(topic); });
    connect(helpBtn, &QToolButton::clicked, bar, [helpBtn, menu]() {
        menu->exec(helpBtn->mapToGlobal(QPoint(0, helpBtn->height())));
    });
    row->addWidget(helpBtn);
    row->addStretch(1);   // push float/close to the far right

    // Float + close — styled like the "?" badge (cyan, visible on the
    // dark title bar; the old dark system icons were invisible).  Text
    // glyphs instead of QStyle standard icons so they actually show.
    const QString btnStyle = QStringLiteral(
        "QToolButton{color:#00e5ff;border:1px solid #00e5ff;"
        "border-radius:3px;min-width:16px;max-width:16px;"
        "min-height:16px;max-height:16px;font-weight:700;padding:0;}"
        "QToolButton:hover{background:rgba(0,229,255,40);color:#7ff7ff;}");

    auto *floatBtn = new QToolButton(bar);
    floatBtn->setObjectName(QStringLiteral("dockFloat"));
    floatBtn->setText(QStringLiteral("⧉"));   // ⧉ float/overlay glyph
    floatBtn->setCursor(Qt::PointingHandCursor);
    floatBtn->setStyleSheet(btnStyle);
    floatBtn->setToolTip(tr("Float this panel (rest it on top) / re-dock "
                            "— or double-click the title bar"));
    connect(floatBtn, &QToolButton::clicked, dock, [dock]() {
        const bool toFloat = !dock->isFloating();
        dock->setFloating(toFloat);
        if (toFloat) {           // make the popped-out window visible
            dock->raise();
            dock->activateWindow();
        }
    });
    row->addWidget(floatBtn);

    auto *closeBtn = new QToolButton(bar);
    closeBtn->setObjectName(QStringLiteral("dockClose"));
    closeBtn->setText(QStringLiteral("✕"));   // ✕ close glyph
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setStyleSheet(btnStyle);
    closeBtn->setToolTip(tr("Close this panel (View menu re-opens it)"));
    connect(closeBtn, &QToolButton::clicked, dock, &QDockWidget::close);
    row->addWidget(closeBtn);

    return bar;
}

void MainWindow::buildDocks() {
    // Panadapter — Vulkan scene-graph spectrum (top by default).
    addQuickDock(QStringLiteral("panadapter"), tr("Panadapter"),
                 QStringLiteral("PanadapterPanel.qml"),
                 QStringLiteral("panadapter"), Qt::TopDockWidgetArea);
    // Tuning + Audio — ordinary QML control panels (bottom by default).
    // Proves the dock pattern works for non-Vulkan panels too; the
    // operator can drag/float/snap/tab any of them anywhere.
    addQuickDock(QStringLiteral("tuning"), tr("Tuning"),
                 QStringLiteral("TuningPanel.qml"),
                 QStringLiteral("tuning"), Qt::BottomDockWidgetArea);
    // Mode + Filter — demod mode + RX filter bandwidth (per-mode memory).
    addQuickDock(QStringLiteral("modefilter"), tr("Mode + Filter"),
                 QStringLiteral("ModeFilterPanel.qml"),
                 QStringLiteral("modes-filters"), Qt::BottomDockWidgetArea);
    addQuickDock(QStringLiteral("audio"), tr("Audio"),
                 QStringLiteral("AudioPanel.qml"),
                 QStringLiteral("audio"), Qt::BottomDockWidgetArea);
    // Display — front-facing spectrum/waterfall controls (palette,
    // waterfall speed, smoothing, glow, grid) binding the shared Prefs.
    addQuickDock(QStringLiteral("display"), tr("Display"),
                 QStringLiteral("DisplayPanel.qml"),
                 QStringLiteral("display"), Qt::BottomDockWidgetArea);
    // Band — HF/6m amateur band switching (tunes to each band default).
    addQuickDock(QStringLiteral("band"), tr("Band"),
                 QStringLiteral("BandPanel.qml"),
                 QStringLiteral("band"), Qt::BottomDockWidgetArea);
}

void MainWindow::buildMenus() {
    // File — Settings (stub) + Exit.
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&Settings…"), QKeySequence(tr("Ctrl+,")),
                        this, &MainWindow::openSettings);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("E&xit"), QKeySequence(QKeySequence::Quit),
                        this, &QWidget::close);

    // View — one show/hide toggle per dock + Lock panels.
    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    for (auto it = docks_.cbegin(); it != docks_.cend(); ++it) {
        viewMenu->addAction(it.value()->toggleViewAction());
    }
    viewMenu->addSeparator();
    lockAction_ = viewMenu->addAction(tr("&Lock panels"));
    lockAction_->setCheckable(true);
    lockAction_->setShortcut(QKeySequence(tr("Ctrl+L")));
    connect(lockAction_, &QAction::toggled, this, [this](bool on) {
        applyPanelLock(on);
        QSettings().setValue(QStringLiteral("ui/panelsLocked"), on);
    });

    // Layout management (mirrors old Lyra): snapshot the current
    // arrangement as a personal default, jump back to it, or reset to
    // the built-in factory arrangement.  The polished N8SDR-style
    // factory default lands once the remaining panels exist.
    viewMenu->addSeparator();
    viewMenu->addAction(tr("&Save current layout as my default"),
                        this, &MainWindow::saveUserLayout);
    viewMenu->addAction(tr("&Restore my saved layout"),
                        this, &MainWindow::restoreUserLayout);
    viewMenu->addAction(tr("Reset to &default layout"),
                        this, &MainWindow::applyDefaultLayout);

    // Help — User Guide + About.
    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&User Guide…"), QKeySequence(QKeySequence::HelpContents),
                        this, [this]() { showHelp(QString()); });
    helpMenu->addAction(tr("Check for &Updates…"), this,
                        [this]() { checkForUpdates(true); });
    helpMenu->addSeparator();
    helpMenu->addAction(tr("&About Lyra…"), this, [this]() {
        const QString ver = QStringLiteral(LYRA_VERSION);
        QMessageBox::about(
            this, tr("About Lyra"),
            tr("<h2 style='margin-bottom:2px'>Lyra "
               "<span style='color:#00e5ff'>v%1</span></h2>"
               "<p style='color:#8a9aac;margin-top:0'>"
               "Hermes Lite 2 / 2+ — native C++23 / Qt 6 rebuild</p>"
               "<p>A desktop SDR transceiver for the Hermes Lite 2 / 2+, "
               "rebuilt in native C++ (Qt Quick + Vulkan/RHI) — no Python, "
               "no GIL anywhere.</p>"
               "<p style='color:#8a9aac'><i>Named for Apollo's lyre and "
               "the constellation Lyra — home of Vega. (See the User "
               "Guide for the full story.)</i></p>"
               "<p>Author: <b>Rick Langford (N8SDR)</b><br>"
               "Repository: <a href='https://github.com/N8SDR1/Lyra-SDR-cpp'>"
               "github.com/N8SDR1/Lyra-SDR-cpp</a><br>"
               "License: <b>GPL v3 or later</b></p>"
               "<p style='color:#8a9aac;font-size:11px'>"
               "DSP engine: WDSP (Warren Pratt, NR0V), GPL v3+.<br>"
               "TCI server protocol © EESDR Expert Electronics, "
               "implemented from the public TCI v1.9 / v2.0 spec."
               "</p>").arg(ver));
    });
}

void MainWindow::openSettings() {
    if (!settingsDlg_) {
        settingsDlg_ = new SettingsDialog(
            prefs_, qobject_cast<lyra::ipc::HL2Stream *>(stream_),
            qobject_cast<lyra::ipc::HL2Discovery *>(discovery_),
            usbBcd_, qobject_cast<lyra::dsp::WdspEngine *>(wdspEngine_),
            this);
    }
    settingsDlg_->show();
    settingsDlg_->raise();
    settingsDlg_->activateWindow();
}

void MainWindow::showHelp(const QString &topic) {
    if (!helpDlg_) {
        helpDlg_ = new HelpDialog(this);
    }
    helpDlg_->showTopic(topic);
}

void MainWindow::openSettingsTopic(const QString &topic) {
    openSettings();
    settingsDlg_->selectTopic(topic);   // jump to the matching tab
}

void MainWindow::buildToolbar() {
    // Header action strip — old Lyra's top section between the menu and
    // the panels: Start/Stop first, then a connection-status readout.
    // (Time + Lightning/Wind notifications slot in here later, matching
    // old Lyra's layout.)
    QToolBar *tb = addToolBar(tr("Main"));
    tb->setObjectName(QStringLiteral("main_toolbar"));
    tb->setMovable(true);
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);

    startStopAction_ = tb->addAction(tr("▶  Start"));
    startStopAction_->setToolTip(
        tr("Connect to the radio and start streaming (and stop it)."));
    connect(startStopAction_, &QAction::triggered,
            this, &MainWindow::onStartStop);

    // Update-available indicator — hidden until a newer release is found.
    updateAction_ = tb->addAction(tr("⬆  Update"));
    updateAction_->setVisible(false);
    updateAction_->setToolTip(tr("A newer Lyra release is available — "
                                 "click to open the release page."));
    connect(updateAction_, &QAction::triggered, this, [this]() {
        if (!pendingUpdateUrl_.isEmpty())
            QDesktopServices::openUrl(QUrl(pendingUpdateUrl_));
    });

    tb->addSeparator();
    connStatus_ = new QLabel(tr("Disconnected"), tb);
    connStatus_->setContentsMargins(8, 0, 8, 0);
    tb->addWidget(connStatus_);

    // ---- Local + UTC clocks (old-Lyra header layout) ----
    // A flexible spacer pushes the clocks toward the right of the strip,
    // matching old Lyra's "menu/notifications … [Local] [UTC]" placement.
    auto *clockSpacer = new QWidget(tb);
    clockSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(clockSpacer);

    clockLocal_ = new QLabel(QStringLiteral("--:--:--"), tb);
    clockLocal_->setContentsMargins(8, 0, 4, 0);
    clockLocal_->setStyleSheet(QStringLiteral(
        "QLabel{color:#ffd54f;font-family:Consolas,monospace;"
        "font-weight:700;font-size:18px;}"));     // amber = PC local
    clockLocal_->setToolTip(tr("PC local time"));
    tb->addWidget(clockLocal_);

    clockUtc_ = new QLabel(QStringLiteral("--:--:--Z"), tb);
    clockUtc_->setContentsMargins(4, 0, 8, 0);
    clockUtc_->setStyleSheet(QStringLiteral(
        "QLabel{color:#80d8ff;font-family:Consolas,monospace;"
        "font-weight:700;font-size:18px;}"));      // cyan = UTC/Zulu
    clockUtc_->setToolTip(tr("UTC / Zulu time"));
    tb->addWidget(clockUtc_);

    clockTimer_ = new QTimer(this);
    clockTimer_->setInterval(1000);
    connect(clockTimer_, &QTimer::timeout, this, &MainWindow::tickClocks);
    clockTimer_->start();
    tickClocks();   // paint immediately, don't wait 1 s

    // Reflect the live stream state into the button + status label.
    if (auto *st = qobject_cast<lyra::ipc::HL2Stream *>(stream_)) {
        connect(st, &lyra::ipc::HL2Stream::runningChanged,
                this, &MainWindow::updateConnState);
    }
    updateConnState();

    // ---- GitHub update checker ----
    updateChecker_ = new UpdateChecker(this);
    connect(updateChecker_, &UpdateChecker::updateAvailable,
            this, &MainWindow::onUpdateAvailable);
    connect(updateChecker_, &UpdateChecker::noUpdate,
            this, &MainWindow::onNoUpdate);
    connect(updateChecker_, &UpdateChecker::checkFailed,
            this, &MainWindow::onUpdateCheckFailed);
    // Quiet startup check ~5 s after launch (operator opt-out via the
    // update_check/check_on_startup setting), matching old Lyra.
    QSettings st2;
    if (st2.value(QStringLiteral("update_check/check_on_startup"), true).toBool()) {
        QTimer::singleShot(5000, this, [this]() { checkForUpdates(false); });
    }
}

void MainWindow::checkForUpdates(bool manual) {
    if (!updateChecker_) return;
    updateCheckManual_ = manual;
    updateChecker_->check();
}

void MainWindow::onUpdateAvailable(const QString &tag, const QString &url,
                                   const QString &body) {
    pendingUpdateUrl_ = url;
    if (updateAction_) {
        updateAction_->setText(tr("⬆  %1").arg(tag));
        updateAction_->setVisible(true);
    }
    QSettings s;
    s.setValue(QStringLiteral("update_check/latest_tag"), tag);
    s.setValue(QStringLiteral("update_check/latest_url"), url);

    // Manual check → always show the result.  Startup check → modal only
    // the FIRST time per version, and never for a version the operator
    // chose to skip.
    if (!updateCheckManual_) {
        const QStringList skipped =
            s.value(QStringLiteral("update_check/skipped_versions"))
                .toStringList();
        if (skipped.contains(tag)) return;          // silenced this version
        const QStringList seen =
            s.value(QStringLiteral("update_check/modal_seen_versions"))
                .toStringList();
        if (seen.contains(tag)) return;             // already modalled; indicator only
    }

    QMessageBox box(this);
    box.setWindowTitle(tr("Lyra update available"));
    box.setTextFormat(Qt::RichText);
    box.setText(tr("<b>Lyra %1</b> is available "
                   "(you're running %2).")
                    .arg(tag, QStringLiteral(LYRA_VERSION)));
    if (!body.trimmed().isEmpty()) {
        QString notes = body.trimmed();
        if (notes.size() > 600) notes = notes.left(600) + QStringLiteral("…");
        box.setInformativeText(notes);
    }
    QPushButton *openBtn = box.addButton(tr("Open release page"),
                                         QMessageBox::AcceptRole);
    box.addButton(tr("Remind me later"), QMessageBox::RejectRole);
    QPushButton *skipBtn = box.addButton(tr("Skip this version"),
                                         QMessageBox::DestructiveRole);
    box.exec();

    if (box.clickedButton() == openBtn) {
        QDesktopServices::openUrl(QUrl(url));
    } else if (box.clickedButton() == skipBtn) {
        QStringList skipped =
            s.value(QStringLiteral("update_check/skipped_versions"))
                .toStringList();
        if (!skipped.contains(tag)) skipped << tag;
        s.setValue(QStringLiteral("update_check/skipped_versions"), skipped);
    }
    // Record that we've shown the modal for this version (so a later
    // startup check shows only the quiet toolbar indicator).
    QStringList seen =
        s.value(QStringLiteral("update_check/modal_seen_versions"))
            .toStringList();
    if (!seen.contains(tag)) seen << tag;
    s.setValue(QStringLiteral("update_check/modal_seen_versions"), seen);
}

void MainWindow::onNoUpdate() {
    if (updateCheckManual_) {
        QMessageBox::information(
            this, tr("Check for Updates"),
            tr("You're running the latest Lyra (%1).")
                .arg(QStringLiteral(LYRA_VERSION)));
    }
}

void MainWindow::onUpdateCheckFailed(const QString &reason) {
    if (updateCheckManual_) {
        QMessageBox::warning(
            this, tr("Check for Updates"),
            tr("Couldn't check for updates:\n%1").arg(reason));
    }
}

void MainWindow::tickClocks() {
    // Re-read the system clock each tick (never increment) so the
    // display can't drift from the OS time.
    const QDateTime now = QDateTime::currentDateTime();
    if (clockLocal_)
        clockLocal_->setText(now.toString(QStringLiteral("HH:mm:ss")));
    if (clockUtc_)
        clockUtc_->setText(now.toUTC().toString(QStringLiteral("HH:mm:ss")) +
                           QStringLiteral("Z"));
}

void MainWindow::onStartStop() {
    auto *st = qobject_cast<lyra::ipc::HL2Stream *>(stream_);
    if (!st) {
        return;
    }
    if (st->isRunning()) {
        st->close();
        return;
    }
    // Stopped → connect.  Prefer the remembered radio; if none, scan and
    // auto-open the first one found (old-Lyra "Start just connects").
    auto *disc = qobject_cast<lyra::ipc::HL2Discovery *>(discovery_);
    QString ip;
    if (disc) {
        ip = disc->savedRadio().value(QStringLiteral("ip")).toString();
    }
    if (!ip.isEmpty()) {
        if (connStatus_) connStatus_->setText(tr("Connecting to %1…").arg(ip));
        st->open(ip);
        return;
    }
    if (!disc) {
        if (connStatus_) connStatus_->setText(tr("No saved radio"));
        return;
    }
    // One-shot: open the first radio the sweep reports, then disarm.
    if (connStatus_) connStatus_->setText(tr("Scanning…"));
    QObject::disconnect(scanConn_);
    scanConn_ = connect(
        disc, &lyra::ipc::HL2Discovery::radioFound, this,
        [this, st, disc](const QString &fip, const QString &mac,
                         const QString &board, int code, int beta,
                         bool busy, int numRxs) {
            QObject::disconnect(scanConn_);
            disc->rememberRadio(fip, mac, board, code, beta, busy, numRxs);
            if (connStatus_)
                connStatus_->setText(tr("Connecting to %1…").arg(fip));
            st->open(fip);
        });
    disc->scan(1.5, 2);
}

void MainWindow::updateConnState() {
    auto *st = qobject_cast<lyra::ipc::HL2Stream *>(stream_);
    const bool running = st && st->isRunning();
    if (startStopAction_) {
        startStopAction_->setText(running ? tr("■  Stop")
                                          : tr("▶  Start"));
    }
    if (connStatus_) {
        connStatus_->setText(running && st
                                 ? tr("Connected to %1").arg(st->targetIp())
                                 : tr("Disconnected"));
    }
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event) {
    auto *w = qobject_cast<QWidget *>(obj);
    const bool isTitle =
        w && w->objectName() == QStringLiteral("dockTitleBar");
    if (!isTitle) {
        return QMainWindow::eventFilter(obj, event);
    }
    auto dockOf = [](QWidget *x) -> QDockWidget * {
        for (QWidget *p = x->parentWidget(); p; p = p->parentWidget())
            if (auto *d = qobject_cast<QDockWidget *>(p)) return d;
        return nullptr;
    };

    switch (event->type()) {
    case QEvent::MouseButtonDblClick: {
        // Double-click toggles float/dock (standard QDockWidget gesture).
        if (auto *dock = dockOf(w)) {
            if (dock->features() & QDockWidget::DockWidgetFloatable)
                dock->setFloating(!dock->isFloating());
            return true;
        }
        break;
    }
    case QEvent::MouseButtonPress: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            QDockWidget *dock = dockOf(w);
            // Only take over the drag for a FLOATING dock — then we move
            // it as a free window (no re-dock/snap).  A DOCKED panel
            // falls through to QDockWidget's own drag (move / re-dock).
            if (dock && dock->isFloating()) {
                floatDragDock_   = dock;
                floatDragOffset_ = me->globalPosition().toPoint()
                                   - dock->frameGeometry().topLeft();
                return true;
            }
        }
        break;
    }
    case QEvent::MouseMove: {
        if (floatDragDock_) {
            auto *me = static_cast<QMouseEvent *>(event);
            floatDragDock_->move(me->globalPosition().toPoint()
                                 - floatDragOffset_);
            return true;
        }
        break;
    }
    case QEvent::MouseButtonRelease: {
        if (floatDragDock_) {
            floatDragDock_ = nullptr;
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::applyPanelLock(bool locked) {
    // Locked = panels can't be dragged / floated / closed (no title-bar
    // grip).  Unlocked = full feature set.  Mirrors the Python-Lyra
    // "Lock panels" behaviour.
    const QDockWidget::DockWidgetFeatures f =
        locked ? QDockWidget::NoDockWidgetFeatures : kUnlockedFeatures;
    for (auto *dock : std::as_const(docks_)) {
        dock->setFeatures(f);
        // Our custom title bar's float/close buttons aren't governed by
        // the dock feature flags, so toggle them by hand to match the
        // locked state.  The "?" help badge stays available either way.
        if (QWidget *tb = dock->titleBarWidget()) {
            if (auto *fb = tb->findChild<QToolButton *>(
                    QStringLiteral("dockFloat")))
                fb->setVisible(!locked);
            if (auto *cb = tb->findChild<QToolButton *>(
                    QStringLiteral("dockClose")))
                cb->setVisible(!locked);
        }
    }
}

void MainWindow::saveLayout() {
    QSettings s;
    s.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    s.setValue(QStringLiteral("ui/windowState"), saveState());
}

void MainWindow::restoreLayout() {
    QSettings s;
    const QByteArray geo =
        s.value(QStringLiteral("ui/geometry")).toByteArray();
    if (!geo.isEmpty()) {
        restoreGeometry(geo);   // includes the maximized/normal flag
    } else {
        // First run (no saved geometry): open maximized / full screen —
        // the way the operator runs it (and old Lyra defaulted).  Once
        // they size/maximize and close, saveGeometry() persists it.
        setWindowState(windowState() | Qt::WindowMaximized);
    }
    const QByteArray st =
        s.value(QStringLiteral("ui/windowState")).toByteArray();
    if (!st.isEmpty()) {
        restoreState(st);
    }
    // Apply the persisted lock state (default unlocked).
    const bool locked =
        s.value(QStringLiteral("ui/panelsLocked"), false).toBool();
    if (lockAction_) {
        lockAction_->setChecked(locked);   // fires toggled -> applyPanelLock
    } else {
        applyPanelLock(locked);
    }
}

void MainWindow::saveUserLayout() {
    // Snapshot the CURRENT arrangement into dedicated "user default"
    // keys — separate from the ui/geometry+windowState session auto-save
    // so "Restore my saved layout" always returns to THIS deliberate
    // arrangement regardless of how the panels drift between sessions.
    QSettings s;
    s.setValue(QStringLiteral("ui/userGeometry"), saveGeometry());
    s.setValue(QStringLiteral("ui/userWindowState"), saveState());
    // The panadapter/waterfall divider isn't part of saveState() — it's
    // a QML SplitView inside the dock — so snapshot it alongside.
    if (prefs_) {
        s.setValue(QStringLiteral("ui/userPanadapterSplit"),
                   prefs_->panadapterSplit());
    }
    QMessageBox::information(
        this, tr("Layout saved"),
        tr("Current panel arrangement saved as your default.\n\n"
           "View → \"Restore my saved layout\" returns to this any time. "
           "\"Reset to default layout\" always goes to the built-in "
           "arrangement."));
}

void MainWindow::restoreUserLayout() {
    QSettings s;
    const QByteArray st =
        s.value(QStringLiteral("ui/userWindowState")).toByteArray();
    if (st.isEmpty()) {
        QMessageBox::information(
            this, tr("No saved layout"),
            tr("You haven't saved a layout yet.\n\nArrange the panels how "
               "you want them, then View → \"Save current layout as my "
               "default\"."));
        return;
    }
    const QByteArray geo =
        s.value(QStringLiteral("ui/userGeometry")).toByteArray();
    if (!geo.isEmpty()) {
        restoreGeometry(geo);
    }
    restoreState(st);
    for (auto *dock : std::as_const(docks_)) {
        dock->show();
    }
    // Re-apply the saved divider position (QML re-applies on the
    // panadapterSplit change).
    if (prefs_) {
        const QVariant sp =
            s.value(QStringLiteral("ui/userPanadapterSplit"));
        if (sp.isValid()) {
            prefs_->setPanadapterSplit(sp);
        }
    }
}

void MainWindow::applyDefaultLayout() {
    // Built-in (factory) arrangement: panadapter on top, the control
    // panels in a row beneath.  Re-adding a dock to an area repositions
    // it, so this is a clean reset regardless of where the operator
    // dragged things.  Mirrors buildDocks()'s placement.
    static const char *kBottom[] = {"tuning", "audio", "display", "band"};
    for (auto *dock : std::as_const(docks_)) {
        dock->setFloating(false);
        dock->show();
    }
    if (auto *pan = docks_.value(QStringLiteral("panadapter"))) {
        addDockWidget(Qt::TopDockWidgetArea, pan);
    }
    for (const char *name : kBottom) {
        if (auto *d = docks_.value(QLatin1String(name))) {
            addDockWidget(Qt::BottomDockWidgetArea, d);
        }
    }
    // Factory split (invalid QVariant → QML restores the 60/40 default).
    if (prefs_) {
        prefs_->setPanadapterSplit(QVariant());
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    saveLayout();
    QMainWindow::closeEvent(event);
}

} // namespace lyra::ui
