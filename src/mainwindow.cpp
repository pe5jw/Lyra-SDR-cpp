// Lyra — QMainWindow dock shell.  See mainwindow.h.

#include "mainwindow.h"

#include "bands.h"
#include "bandplan.h"
#include "statusbus.h"
#include "timesync.h"
#include "bandmemory.h"
#include "genslots.h"
#include "time_stations.h"
#include "memorystore.h"
#include "eibistore.h"
#include "spotstore.h"
// TX-rip Phase 1 (Q2): tci_mic_source.h removed; the TciMicSource type
// is being ripped along with the rest of the TX DSP subsystem and
// rebuilt from empty files per docs/TX_ARCHITECTURAL_MAPPING.md §10.3.
#include "tci_server.h"
#include "metermodel.h"
#include "profile/ProfileManager.h"  // complete type for setContextProperty(QObject*)
#include "eqmodel.h"                  // complete type for new EqModel + context property
#include "speechmodel.h"              // complete type for new SpeechModel + context property
#include "combinatormodel.h"          // complete type for new CombinatorModel + context property
#include "platemodel.h"               // complete type for new PlateModel + context property
#include "profileui.h"                // native Save-Profile dialog (front panel)
#include "wdsp_engine.h"
#include "help.h"
#include "helpdialog.h"
#include "logdialog.h"
#include "hl2_discovery.h"
#include "hl2_stream.h"
#include "wdsp_engine.h"
#include "prefs.h"
#include "settingsdialog.h"
#include "usb_bcd.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDebug>
#include <QEvent>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QTextEdit>
#include <QMessageBox>
#include <QQmlContext>
#include <QQuickWidget>
#include <QQuickItem>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStyle>
#include <QToolBar>
#include <QStatusBar>
#include <QToolButton>
#include <QDateTime>
#include <QTimer>
#include <QDesktopServices>
#include <QPushButton>
#include <QUrl>
#include <QFileDialog>
#include <QStandardPaths>
#include <QProcess>
#include <QCoreApplication>

#include "updatechecker.h"
#include "default_layout.h"
#include "wxservice.h"
#include "wxindicator.h"
#include "solarservice.h"
#include "solarpanel.h"
#include "ncdxffollow.h"

#include <QApplication>
#include <QSystemTrayIcon>

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
                       Prefs *prefs, lyra::wx::WxService *wx,
                       lyra::profile::ProfileManager *profiles,
                       QWidget *parent)
    : QMainWindow(parent),
      discovery_(discovery), stream_(stream),
      wdsp_(wdsp), wdspEngine_(wdspEngine), prefs_(prefs), wx_(wx),
      profiles_(profiles) {
    setWindowTitle(QStringLiteral(
        "Lyra — Hermes Lite 2 / 2+ — v" LYRA_VERSION " (C++23 / Qt 6)"));
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

    // Band-plan data (sub-band segments / edges / landmarks per region)
    // — exposed to QML for the panadapter's top-strip overlay.  Reads
    // the region + layer toggles from Prefs.
    bandPlan_ = new BandPlan(prefs_, this);

    // Transient status messages (out-of-band advisory, "Tuned to …").
    // QML posts via the `Status` context property; C++ posts via show().
    statusBus_ = new StatusBus(this);
    statusBar()->setStyleSheet(QStringLiteral(
        "QStatusBar{background:#0c141c;color:#cdd9e5;}"
        "QStatusBar::item{border:0;}"));
    statusBar()->setSizeGripEnabled(false);
    connect(statusBus_, &StatusBus::message, this,
            [this](const QString &t, int ms) {
                statusBar()->showMessage(t, ms);
            });

    // TX-0c-pa-debug — HL2 telemetry strip on the right of the status
    // bar.  Permanent widget so transient `showMessage` (on the left)
    // never displaces it.  T/V are RX-anytime useful; PA current is
    // the LOAD-BEARING bench observable for B-pa first-RF bench + the
    // §15.20/§15.24-C kill-test (PA bias must drop within N sec of
    // taskkill).  At-rest text uses "—" so the strip always reads as
    // intentional UI even pre-Start (telemetry getters return NaN).
    hl2TelemLabel_ = new QLabel(this);
    hl2TelemLabel_->setText(tr("HL2: T —  V —  PA —"));
    hl2TelemLabel_->setStyleSheet(QStringLiteral(
        "QLabel{color:#8fa6ba;font-family:Consolas,Menlo,monospace;"
        "padding:0 8px;}"));
    hl2TelemLabel_->setToolTip(tr(
        "HL2 telemetry: temperature, supply voltage, PA bias current.  "
        "PA turns red ≥50 mA — when red, the gateware is producing RF.  "
        "Kill-test (§15.20): taskkill /F /IM lyra.exe mid-key; PA must "
        "drop to zero within ~13 sec (gateware EP2 watchdog)."));
    statusBar()->addPermanentWidget(hl2TelemLabel_);
    if (auto *st = qobject_cast<lyra::ipc::HL2Stream *>(stream_)) {
        connect(st, &lyra::ipc::HL2Stream::statsChanged, this,
                &MainWindow::refreshHl2TelemetryStrip);
        // Initial paint (will show — until first stats tick).
        refreshHl2TelemetryStrip();
    }

    // PC clock-drift check (NCDXF beacon timing is UTC-locked).  On-demand
    // via the header clock right-click menu; result drives the ⚠ on the
    // UTC clock + an advisory dialog.
    timeSync_ = new TimeSync(this);
    connect(timeSync_, &TimeSync::result, this,
            [this](double, const QString &srv, int sev, const QString &sum) {
        driftSeverity_ = sev;
        QString body = tr("Network time (%1):\n%2").arg(srv, sum);
        if (sev == TimeSync::Warn)
            body += tr("\n\nYour clock is drifting. NCDXF beacon markers and "
                       "Follow may show the wrong station near the 10-second "
                       "slot boundaries.");
        else if (sev == TimeSync::Bad)
            body += tr("\n\nYour clock is significantly off — NCDXF beacon "
                       "tracking will identify the wrong stations. Right-click "
                       "the clock and choose \"Sync time\".");
        statusBus_->show(sum, 5000);
        QMessageBox::information(this, tr("PC clock drift"), body);
    });
    connect(timeSync_, &TimeSync::failed, this, [this](const QString &why) {
        statusBus_->show(why, 5000);
        QMessageBox::warning(this, tr("PC clock drift"), why);
    });
    connect(timeSync_, &TimeSync::resyncDone, this,
            [this](bool ok, const QString &msg) {
        statusBus_->show(msg, 5000);
        if (ok) QTimer::singleShot(1500, this,
                                   [this]() { timeSync_->checkDrift(); });
        else QMessageBox::warning(this, tr("Sync time"), msg);
    });

    // Per-band memory: restores each band's mode + panadapter/waterfall dB
    // ranges as you move across band edges (saved live while on a band).
    bandMemory_ = new BandMemory(
        prefs_, qobject_cast<lyra::ipc::HL2Stream *>(stream_), this);

    // GEN1/2/3 general-coverage slots (shortwave / MW quick-tune).
    gen_ = new GenSlots(
        prefs_, qobject_cast<lyra::ipc::HL2Stream *>(stream_), this);

    // HF time-station TIME cycle (TIME button on the GEN row).
    time_ = new TimeStations(
        prefs_, qobject_cast<lyra::ipc::HL2Stream *>(stream_), this);

    // Frequency memory bank (Mem button + Settings → Bands → Memory).
    memory_ = new MemoryStore(
        prefs_, qobject_cast<lyra::ipc::HL2Stream *>(stream_), this);

    // EiBi shortwave-broadcaster overlay (Settings → Bands → SW Database).
    eibi_ = new EibiStore(prefs_, bands_, this);

    // RX signal-strength meter (Horizon Arc / Plasma Bar panel).
    // Source = WDSP RXA_S_PK (in-passband), standard HF S-unit scale.
    meter_ = new MeterModel(qobject_cast<lyra::ipc::HL2Stream *>(stream_),
                            qobject_cast<lyra::dsp::WdspEngine *>(wdspEngine_),
                            this);

    // #50 TX parametric EQ model (drives EqPanel.qml, "Eq" context property).
    // Routes the TX mic rack through eqModel_->engine() (CMaster TX hook).
    eqModel_ = new EqModel(EqModel::Side::Tx, this);

    // #59 RX parametric EQ model (drives RxEqPanel.qml, "RxEq" context
    // property).  The RX twin of the TX EQ — same engine, shaped on the
    // post-RXA receive audio instead of the mic.  R-2 wired
    // WdspEngine::setRxEqEngine to run engine()+analyzer() at the top of
    // dispatchAudioFrame (pre-tap; auto-bypassed in DIGU/DIGL; manual ON/OFF
    // any mode).  Persists standalone (QSettings "rxeq/*"), NOT in the TX
    // Profile (#49) — it's an RX listening setting like the RX bandwidth.
    rxEqModel_ = new EqModel(EqModel::Side::Rx, this);
    if (auto *we = qobject_cast<lyra::dsp::WdspEngine *>(wdspEngine_))
        we->setRxEqEngine(rxEqModel_->engine(), rxEqModel_->analyzer());
    {
        // Restore the saved RX EQ chain (bypass + makeup + all bands).
        QSettings s;
        const QString js = s.value(QStringLiteral("rxeq/state")).toString();
        if (!js.isEmpty()) {
            const auto doc = QJsonDocument::fromJson(js.toUtf8());
            if (doc.isObject()) rxEqModel_->loadState(doc.object());
        }
        // Persist on every edit (band / bypass / makeup).  Display-only
        // analyzer options are excluded by saveState(), as on the TX side.
        auto save = [this]() {
            QSettings st;
            st.setValue(QStringLiteral("rxeq/state"),
                        QString::fromUtf8(QJsonDocument(rxEqModel_->saveState())
                                              .toJson(QJsonDocument::Compact)));
        };
        connect(rxEqModel_, &EqModel::bandsChanged, this, save);
        connect(rxEqModel_, &EqModel::bypassChanged, this, save);
        connect(rxEqModel_, &EqModel::makeupDbChanged, this, save);
    }

    // #88 speech-rack model (drives SpeechPanel.qml) — Auto-AGC + De-esser,
    // pre-EQ in the mic rack (wired via SendpTxSpeechProcessor in main.cpp).
    speechModel_ = new SpeechModel(this);

    // #51 combinator model (drives CombinatorPanel.qml) — 5-band multiband
    // comp + SBC, after the EQ in the mic rack.  Wire-INERT until Stage 3
    // routes the rack through it (SendpTxCombinatorProcessor).
    combinatorModel_ = new CombinatorModel(this);

    // #52 plate-reverb model (drives PlatePanel.qml) — last native rack
    // stage, after the Combinator.  Wire-INERT until Stage 3 registers
    // SendpTxPlateProcessor.
    plateModel_ = new PlateModel(this);

    // DX-cluster spots (pushed over TCI; drawn on the panadapter).
    spots_ = new SpotStore(prefs_, qobject_cast<lyra::ipc::HL2Stream *>(stream_),
                           qobject_cast<lyra::dsp::WdspEngine *>(wdspEngine_), this);
    // Toast when the operator's own callsign gets spotted (opt-in, with a
    // cooldown set in Settings → Network).  Tray balloon + status line.
    connect(spots_, &SpotStore::ownCallSpotted, this,
            [this](const QString &call, const QString &mode, qint64 hz,
                   const QString &text) {
        const QString mhz = QString::number(hz / 1.0e6, 'f', 3);
        const QString body = tr("%1 spotted on %2 MHz %3%4")
                                 .arg(call, mhz, mode,
                                      text.isEmpty() ? QString()
                                                     : QStringLiteral(" — ") + text);
        if (!tray_) { tray_ = new QSystemTrayIcon(windowIcon(), this); tray_->show(); }
        tray_->showMessage(tr("You've been spotted!"), body,
                           QSystemTrayIcon::Information, 8000);
        if (statusBus_) statusBus_->show(tr("★ ") + body, 8000);
        // Light the header badge (before the clocks) for a couple of minutes.
        if (spottedBadge_) {
            spottedBadge_->setText(tr("★ Spotted!"));
            spottedBadge_->setStyleSheet(
                QStringLiteral("QLabel{color:%1;font-weight:bold;}")
                    .arg(spots_ ? spots_->highlightColor()
                                : QStringLiteral("#ff4081")));
            spottedBadge_->setToolTip(body);
            if (spottedClearTimer_) spottedClearTimer_->start();
        }
    });

    // TCI server — lets loggers/cluster apps drive + read Lyra over a
    // WebSocket (Settings → Network).  START/STOP from a client route
    // through onStartStop, guarded so they only act when state differs.
    tci_ = new TciServer(prefs_, qobject_cast<lyra::ipc::HL2Stream *>(stream_),
                         qobject_cast<lyra::dsp::WdspEngine *>(wdspEngine_),
                         spots_, this);
    connect(tci_, &TciServer::startRequested, this, [this]() {
        auto *st = qobject_cast<lyra::ipc::HL2Stream *>(stream_);
        if (st && !st->isRunning()) onStartStop();
    });
    connect(tci_, &TciServer::stopRequested, this, [this]() {
        auto *st = qobject_cast<lyra::ipc::HL2Stream *>(stream_);
        if (st && st->isRunning()) onStartStop();
    });
    if (tci_->enabled()) tci_->start();

    // Solar / HF-propagation service — fetches HamQSL, derives per-band
    // ratings for the operator's day/night.  Drives the PROP dock.  It
    // re-arms itself on a Prefs location change internally.
    solar_ = new lyra::solar::SolarService(prefs_, this);

    // NCDXF beacon auto-follow engine (drives the Solar panel's Follow
    // dropdown).  Needs the stream to QSY + Prefs to set CWU.
    ncdxfFollow_ = new NcdxfFollow(
        prefs_, qobject_cast<lyra::ipc::HL2Stream *>(stream_), this);

    // USB-BCD amp band output — follows the band off the stream's freq.
    usbBcd_ = new UsbBcd(this);
    if (auto *st = qobject_cast<lyra::ipc::HL2Stream *>(stream_)) {
        connect(st, &lyra::ipc::HL2Stream::rx1FreqChanged, usbBcd_,
                [this, st]() { usbBcd_->applyForFreq(st->rx1FreqHz()); });
        usbBcd_->applyForFreq(st->rx1FreqHz());   // assert current band now

        // Band-plan in/out-of-band advisory: on a band-state transition
        // post a status message (gated like old Lyra on the edge-warning
        // layer + a real region).  The red edge lines are the visual half.
        connect(st, &lyra::ipc::HL2Stream::rx1FreqChanged, this,
                [this, st]() {
            if (!prefs_->bandPlanEdges()
                || prefs_->bandPlanRegion() == QLatin1String("NONE")) {
                lastBandState_ = -1;
                return;
            }
            const double f = double(st->rx1FreqHz());
            const QString name = bandPlan_->bandContaining(f);
            const int inb = name.isEmpty() ? 0 : 1;
            if (inb == lastBandState_) return;
            lastBandState_ = inb;
            if (inb)
                statusBus_->show(tr("In band: %1  (%2)")
                                     .arg(name, prefs_->bandPlanRegion()), 2500);
            else
                statusBus_->show(tr("⚠ Out of band — %1 MHz is outside the "
                                    "%2 amateur allocations")
                                     .arg(f / 1.0e6, 0, 'f', 3)
                                     .arg(prefs_->bandPlanRegion()), 5000);
        });
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

    // Native Save-Profile dialog helper — must exist before buildDocks()
    // (makeQuick exposes it to the ProfilePanel as the ProfileUi context
    // property).  `this` is the dialog parent.
    if (profiles_)
        profileUi_ = new ProfileUi(profiles_, prefs_, this);

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
    qw->rootContext()->setContextProperty(
        QStringLiteral("BandPlan"), bandPlan_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Status"), statusBus_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("BandMemory"), bandMemory_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Gen"), gen_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Time"), time_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Memory"), memory_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Eibi"), eibi_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Spots"), spots_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Meter"), meter_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Profiles"), profiles_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("ProfileUi"), profileUi_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Eq"), eqModel_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("RxEq"), rxEqModel_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Speech"), speechModel_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Combinator"), combinatorModel_);
    qw->rootContext()->setContextProperty(
        QStringLiteral("Plate"), plateModel_);
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
    // Collapsible panels (EqPanel/SpeechPanel) expose a `collapsed` bool.
    // Drive the host dock's height off it so collapse shrinks the WHOLE dock
    // to the title strip (SizeRootObjectToView ignores QML implicitHeight, so
    // C++ has to clamp the widget).  Stash the host widget on the root so the
    // slot can find it via sender(); panels start expanded (no-op until used).
    if (QObject *root = qw->rootObject();
        root && root->property("collapsed").isValid()) {
        root->setProperty("_lyraHostQw", QVariant::fromValue<QObject *>(qw));
        connect(root, SIGNAL(collapsedChanged()),
                this, SLOT(syncCollapsibleDock()));
    }
    return qw;
}

void MainWindow::syncCollapsibleDock() {
    QObject *root = sender();
    if (!root) return;
    auto *qw = qobject_cast<QQuickWidget *>(
        root->property("_lyraHostQw").value<QObject *>());
    if (!qw) return;
    const bool collapsed = root->property("collapsed").toBool();
    if (collapsed) {
        // Remember the expanded height so we can spring back, then pin the
        // widget to the collapsed strip height → the dock shrinks to fit.
        root->setProperty("_lyraExpandH", qw->height());
        const int strip =
            qMax(28, int(root->property("implicitHeight").toReal()));
        qw->setMinimumHeight(strip);
        qw->setMaximumHeight(strip);
    } else {
        // Expand: a bare resize() on the QQuickWidget does NOT grow the host
        // dock (the dock/splitter owns the size), so the panel would stay at
        // the strip until dragged.  Force it: release the max, pin the MINIMUM
        // to the target height (the dock layout grows the dock to satisfy it),
        // then drop the floor next tick so the operator can resize freely.
        qw->setMaximumHeight(QWIDGETSIZE_MAX);
        int eh = root->property("_lyraExpandH").toInt();
        const int impl = int(root->property("implicitHeight").toReal());
        if (eh < impl) eh = impl;       // collapsed-flag now false → expanded implicit
        if (eh < 60)   eh = 200;        // sanity floor
        qw->setMinimumHeight(eh);
        QTimer::singleShot(0, qw, [qw]() { qw->setMinimumHeight(0); });
    }
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

QDockWidget *MainWindow::addWidgetDock(const QString &objectName,
                                       const QString &title,
                                       QWidget *content,
                                       const QString &topic,
                                       Qt::DockWidgetArea area) {
    auto *dock = new QDockWidget(title, this);
    dock->setObjectName(objectName);   // load-bearing for saveState/restoreState
    dock->setAllowedAreas(Qt::AllDockWidgetAreas);
    dock->setFeatures(kUnlockedFeatures);
    dock->setWidget(content);
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
    // Filters — RX/TX filter bandwidth (per-mode memory) + rate + BW lock.
    // Mode moved to the Tuning dock (under the VFO) in the VFO-cluster
    // layout; this panel is filters-only now.  Dock id stays "modefilter"
    // so saved window layouts (QSettings dock state) keep restoring it.
    addQuickDock(QStringLiteral("modefilter"), tr("Filters"),
                 QStringLiteral("ModeFilterPanel.qml"),
                 QStringLiteral("modes-filters"), Qt::BottomDockWidgetArea);
    addQuickDock(QStringLiteral("audio"), tr("Audio"),
                 QStringLiteral("AudioPanel.qml"),
                 QStringLiteral("audio"), Qt::BottomDockWidgetArea);
    // TX — front-facing TX controls (Drive %, PA Enable, MOX).  Moved
    // out of Settings (Drive + PA) and out of TuningPanel (MOX) so the
    // operator's TX surface is a single coherent dock that's docked /
    // floated / tabbed like every other panel.  Settings → Hardware →
    // Transmit keeps only the safety-timeout config (a set-once knob).
    addQuickDock(QStringLiteral("tx"), tr("TX"),
                 QStringLiteral("TxPanel.qml"),
                 QStringLiteral("tx"), Qt::BottomDockWidgetArea);
    // TX Speech (#88) — Noise Gate + Auto-AGC + De-esser, the pre-EQ mic
    // rack stage.  Its OWN dock so it moves / resizes / floats / collapses
    // like every Lyra panel; drag it onto the TX EQ dock to tab the two into
    // a "TX DSP" group, or place them side-by-side / float either.
    addQuickDock(QStringLiteral("txspeech"), tr("TX Speech"),
                 QStringLiteral("SpeechPanel.qml"),
                 QStringLiteral("txspeech"), Qt::BottomDockWidgetArea);
    // TX EQ (#50) — 10-band parametric EQ.  Own dock (see TX Speech) — comes
    // after Speech in the chain (mic → Speech → EQ → WDSP TXA).
    addQuickDock(QStringLiteral("txeq"), tr("TX EQ"),
                 QStringLiteral("EqPanel.qml"),
                 QStringLiteral("txeq"), Qt::BottomDockWidgetArea);
    // RX EQ (#59) — the RX twin of the TX EQ.  Reuses EqPanel.qml via the thin
    // RxEqPanel.qml wrapper (binds the RxEq model + the RX bandwidth as the
    // graph's band-edge source + the DIGU/DIGL auto-bypass set).  Own dock,
    // chip-summoned like the TX DSP rack (floating + hidden by default below).
    addQuickDock(QStringLiteral("rxeq"), tr("RX EQ"),
                 QStringLiteral("RxEqPanel.qml"),
                 QStringLiteral("rxeq"), Qt::BottomDockWidgetArea);
    // TX Combinator (#51) — 5-band multiband comp + SBC.  Own dock (see TX
    // Speech) — after the EQ in the chain (mic → Speech → EQ → Combinator).
    addQuickDock(QStringLiteral("txcombinator"), tr("TX Combinator"),
                 QStringLiteral("CombinatorPanel.qml"),
                 QStringLiteral("txcombinator"), Qt::BottomDockWidgetArea);
    // TX Plate (#52) — Schroeder-Moorer reverb.  Own dock — last in the
    // chain (mic → Speech → EQ → Combinator → Plate).
    addQuickDock(QStringLiteral("txplate"), tr("TX Plating"),
                 QStringLiteral("PlatePanel.qml"),
                 QStringLiteral("txplate"), Qt::BottomDockWidgetArea);
    // CW Console (#105 CW-3b) — chip-launched floating panel for keyboard
    // CW send (+ the CW-5 decoder pane).  Its OWN dock; floats by default
    // (hidden until its header chip summons it) so the front panel stays
    // clean for the majority who never keyboard-send / decode CW.
    addQuickDock(QStringLiteral("cwconsole"), tr("CW"),
                 QStringLiteral("CwConsolePanel.qml"),
                 QStringLiteral("cwconsole"), Qt::BottomDockWidgetArea);
    if (QDockWidget *d = docks_.value(QStringLiteral("cwconsole"))) {
        d->setFloating(true);
        d->hide();
    }
    // TX DSP launcher default: start each rack stage as a HIDDEN FLOATING
    // window so the front panel stays clean — the header "TX DSP" chip-strip
    // summons one (it pops as a movable/resizable float), and Save-my-layout
    // remembers what's open + where.  restoreLayout() below overrides this
    // with the operator's saved arrangement when one exists.
    for (const char *nm : {"txspeech", "txeq", "txcombinator", "txplate",
                           "rxeq"}) {
        if (QDockWidget *d = docks_.value(QString::fromLatin1(nm))) {
            d->setFloating(true);
            d->hide();
        }
    }
    // Profiles — front-facing quick recall of a saved TX/RX profile
    // (dropdown + Save + ● modified).  Full editor (create / rename /
    // delete / set-default / per-mode bind) is Settings → Profiles.
    addQuickDock(QStringLiteral("profiles"), tr("Profiles"),
                 QStringLiteral("ProfilePanel.qml"),
                 QStringLiteral("profiles"), Qt::BottomDockWidgetArea);
    // Display — front-facing spectrum/waterfall controls (palette,
    // waterfall speed, smoothing, glow, grid) binding the shared Prefs.
    addQuickDock(QStringLiteral("display"), tr("Display"),
                 QStringLiteral("DisplayPanel.qml"),
                 QStringLiteral("display"), Qt::BottomDockWidgetArea);
    // Band — HF/6m amateur band switching (tunes to each band default).
    addQuickDock(QStringLiteral("band"), tr("Band"),
                 QStringLiteral("BandPanel.qml"),
                 QStringLiteral("band"), Qt::BottomDockWidgetArea);
    // Meter — RX signal-strength S-meter (Horizon Arc / Plasma Bar).
    // Docks on the right by default (squarish); operator can drag/float it.
    addQuickDock(QStringLiteral("meter"), tr("Meter"),
                 QStringLiteral("MeterPanel.qml"),
                 QStringLiteral("meter"), Qt::RightDockWidgetArea);
    // Solar / Propagation — HamQSL SFI/A/K + 10-band day/night heat-map.
    // A plain QtWidgets strip (not QML); docks at the bottom like the
    // other control panels.
    addWidgetDock(QStringLiteral("propagation"), tr("Solar / Propagation"),
                  new SolarPanel(solar_, ncdxfFollow_, this),
                  QStringLiteral("propagation"), Qt::BottomDockWidgetArea);
}

void MainWindow::buildMenus() {
    // File — Settings (stub) + Exit.
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&Settings…"), QKeySequence(tr("Ctrl+,")),
                        this, &MainWindow::openSettings);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Export settings…"),
                        this, &MainWindow::exportSettings);
    fileMenu->addAction(tr("&Import settings…"),
                        this, &MainWindow::importSettings);
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
    helpMenu->addAction(tr("Show &Log…"), this, [this]() {
        if (!logDlg_) logDlg_ = new LogDialog(prefs_, this);
        logDlg_->show();
        logDlg_->raise();
        logDlg_->activateWindow();
    });
    helpMenu->addSeparator();
    helpMenu->addAction(tr("&About Lyra…"), this, [this]() {
        const QString ver = QStringLiteral(LYRA_VERSION);
        // Verbatim PayPal donate target from old Lyra (item_name text shows
        // on the PayPal page — keep the encoding byte-for-byte).  Kept out
        // of the tr()/.arg() body so its % escapes aren't treated as args.
        const QString payPal = QStringLiteral(
            "https://www.paypal.com/donate/?business=NP2ZQS4LR454L"
            "&no_recurring=0&item_name=Built+by+a+fellow+ham%2C+for+the+"
            "community.++Free+to+use%2C+free+to+share.+A+small+donation+"
            "keeps+the+code+flowing.+73+de+N8SDR&currency_code=USD");
        QMessageBox box(this);
        box.setWindowTitle(tr("About Lyra"));
        box.setTextFormat(Qt::RichText);
        box.setIcon(QMessageBox::Information);
        box.setText(
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
               "With <b>Brent Crier (N9BC)</b> and "
               "<b>Timmy Davis (KC8TYK)</b><br>"
               "Repository: <a href='https://github.com/N8SDR1/Lyra-SDR-cpp'>"
               "github.com/N8SDR1/Lyra-SDR-cpp</a><br>"
               "License: <b>GPL v3 or later</b></p>"
               "<p>Lyra is <b>free</b> and ad-free, built by a fellow ham "
               "for the community. If it's useful to you, consider buying "
               "the developer a coffee — 73 de N8SDR. ☕</p>"
               "<hr style='border-color:#1a2632'>"
               "<p style='color:#8a9aac;font-size:11px'>"
               "<b>Inspiration and references</b> — these projects were "
               "studied for conventions, ballistics, and protocol "
               "structure while building Lyra.  No source code was "
               "copied; each was a reference for \"how the standard "
               "idiom works.\":<br>"
               "Thetis SDR (openHPSDR) · PowerSDR · HermesLite 2 wiki "
               "and ak4951v4 gateware · pihpsdr · Quisk · linHPSDR · "
               "EESDR V3 · Behringer X-Air mixer series · SparkSDR.<br>"
               "Full credits and reference notes: see the "
               "<i>Credits and References</i> section in the User Guide.</p>"
               "<p style='color:#8a9aac;font-size:11px'>"
               "<b>Licensed components (GPL v3+)</b><br>"
               "• DSP engine: <b>WDSP</b> by Warren Pratt (NR0V).  Lyra "
               "links to the WDSP shared library and calls its public "
               "API; no source modifications.<br>"
               "• TCI server protocol — Expert Electronics public "
               "specification (EESDR).  Lyra implements a TCI server "
               "compatible with the v1.9 / v2.0 spec."
               "</p>").arg(ver));
        QPushButton *donate =
            box.addButton(tr("☕ Donate via PayPal"), QMessageBox::ActionRole);
        box.addButton(QMessageBox::Close);
        box.setDefaultButton(QMessageBox::Close);
        box.exec();
        if (box.clickedButton() == donate)
            QDesktopServices::openUrl(QUrl(payPal));
    });
}

void MainWindow::openSettings() {
    if (!settingsDlg_) {
        settingsDlg_ = new SettingsDialog(
            prefs_, qobject_cast<lyra::ipc::HL2Stream *>(stream_),
            qobject_cast<lyra::ipc::HL2Discovery *>(discovery_),
            usbBcd_, qobject_cast<lyra::dsp::WdspEngine *>(wdspEngine_),
            wx_, memory_, eibi_, tci_, spots_, meter_, profiles_, this);
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

    // TCI client indicator — sits right after the radio connection status.
    // Hidden when the TCI server is off; amber dot = listening (no clients);
    // green dot + count = clients connected (loggers / cluster apps).
    tciStatus_ = new QLabel(tb);
    tciStatus_->setContentsMargins(4, 0, 8, 0);
    tb->addWidget(tciStatus_);
    if (tci_) {
        connect(tci_, &TciServer::runningChanged,
                this, &MainWindow::updateTciStatus);
        connect(tci_, &TciServer::clientCountChanged,
                this, [this](int) { updateTciStatus(); });
    }
    updateTciStatus();

    // ---- TX DSP launcher chip-strip ----
    // One checkable chip per native mic-rack stage; each chip IS that dock's
    // toggleViewAction, so a click summons the panel (floating, movable /
    // resizable) and a click (or closing it) tucks it away — the chip stays
    // in sync.  What's open + where rides Save-my-layout (saveState keys on
    // each dock's objectName).  #51 Combinator / #52 Plate add their names
    // here when their docks land.
    {
        tb->addSeparator();
        auto *txDspLabel = new QLabel(tr("TX DSP:"), tb);
        txDspLabel->setContentsMargins(8, 0, 4, 0);
        txDspLabel->setToolTip(tr("Mic-rack panels — click to open one as a "
                                  "movable window; layout is remembered."));
        tb->addWidget(txDspLabel);
        // Style each launcher as a proper boxed button (was flat text-only,
        // indistinguishable from a label until hover).  Checked = that
        // stage's panel is open — fill with the accent so "what's on" reads
        // at a glance.  Per-button stylesheet keeps the rest of the toolbar
        // (Start/Stop, Update, clocks) on the default look.
        static const char *kTxDspChipQss =
            "QToolButton{border:1px solid #3a4750;border-radius:4px;"
            "padding:2px 9px;margin:0 2px;color:#cfd8dc;background:#1c252b;}"
            "QToolButton:hover{border-color:#5b6b76;background:#243038;}"
            "QToolButton:checked{background:#2e7d9a;border-color:#7fd6ef;"
            "color:#ffffff;font-weight:600;}"
            "QToolButton:checked:hover{background:#3690ad;}";
        for (const char *nm : {"txspeech", "txeq", "txcombinator", "txplate"}) {
            if (QDockWidget *d = docks_.value(QString::fromLatin1(nm))) {
                QAction *act = d->toggleViewAction();
                tb->addAction(act);
                if (auto *btn = qobject_cast<QToolButton *>(
                        tb->widgetForAction(act))) {
                    btn->setObjectName(QStringLiteral("txDspChip"));
                    btn->setStyleSheet(QString::fromLatin1(kTxDspChipQss));
                }
            }
        }
        // CW console launcher (#105 CW-3b) — its own chip after the TX DSP
        // strip; summons the floating CW Console (keyboard send + the CW-5
        // decoder pane).  Same boxed style; the chip IS the dock's
        // toggleViewAction so open/close stays in sync + rides Save-layout.
        tb->addSeparator();
        if (QDockWidget *d = docks_.value(QStringLiteral("cwconsole"))) {
            QAction *act = d->toggleViewAction();
            act->setText(tr("CW"));
            tb->addAction(act);
            if (auto *btn = qobject_cast<QToolButton *>(
                    tb->widgetForAction(act))) {
                btn->setObjectName(QStringLiteral("txDspChip"));
                btn->setStyleSheet(QString::fromLatin1(kTxDspChipQss));
            }
        }
        // RX DSP launcher (#59) — the RX EQ pops as a floating window, same
        // chip idiom as the TX DSP strip (the chip IS the dock's
        // toggleViewAction, so open/close stays in sync + rides Save-layout).
        // Its own labelled group since it shapes RX audio, not the mic rack.
        tb->addSeparator();
        auto *rxDspLabel = new QLabel(tr("RX DSP:"), tb);
        rxDspLabel->setContentsMargins(8, 0, 4, 0);
        rxDspLabel->setToolTip(tr("Receive-side panels — click to open one as "
                                  "a movable window; layout is remembered."));
        tb->addWidget(rxDspLabel);
        if (QDockWidget *d = docks_.value(QStringLiteral("rxeq"))) {
            QAction *act = d->toggleViewAction();
            act->setText(tr("RX EQ"));
            tb->addAction(act);
            if (auto *btn = qobject_cast<QToolButton *>(
                    tb->widgetForAction(act))) {
                btn->setObjectName(QStringLiteral("txDspChip"));
                btn->setStyleSheet(QString::fromLatin1(kTxDspChipQss));
            }
        }
    }

    // ---- Local + UTC clocks (old-Lyra header layout) ----
    // A flexible spacer pushes the clocks toward the right of the strip,
    // matching old Lyra's "menu/notifications … [Local] [UTC]" placement.
    auto *clockSpacer = new QWidget(tb);
    clockSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(clockSpacer);

    // "You've been spotted" header badge — just BEFORE the clocks, with a
    // fixed width + right alignment so it never nudges the clocks (even if
    // a spot lands on the same tick the clocks update).  Cleared after a
    // short while by spottedClearTimer_.
    spottedBadge_ = new QLabel(tb);
    spottedBadge_->setContentsMargins(8, 0, 10, 0);
    spottedBadge_->setFixedWidth(120);
    spottedBadge_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    tb->addWidget(spottedBadge_);
    spottedClearTimer_ = new QTimer(this);
    spottedClearTimer_->setSingleShot(true);
    spottedClearTimer_->setInterval(120000);   // badge lingers ~2 min
    connect(spottedClearTimer_, &QTimer::timeout, spottedBadge_, [this]() {
        spottedBadge_->clear();
        spottedBadge_->setToolTip(QString());
    });

    clockLocal_ = new QLabel(QStringLiteral("--:--:--"), tb);
    clockLocal_->setContentsMargins(8, 0, 4, 0);
    clockLocal_->setStyleSheet(QStringLiteral(
        "QLabel{color:#ffd54f;font-family:Consolas,monospace;"
        "font-weight:700;font-size:22px;}"));     // amber = PC local
    clockLocal_->setToolTip(tr("PC local time"));
    tb->addWidget(clockLocal_);

    clockUtc_ = new QLabel(QStringLiteral("--:--:--Z"), tb);
    clockUtc_->setContentsMargins(4, 0, 8, 0);
    clockUtc_->setStyleSheet(QStringLiteral(
        "QLabel{color:#80d8ff;font-family:Consolas,monospace;"
        "font-weight:700;font-size:22px;}"));      // cyan = UTC/Zulu
    clockUtc_->setToolTip(tr("UTC / Zulu time — right-click to check PC "
                             "clock drift (matters for NCDXF beacon timing)"));
    tb->addWidget(clockUtc_);

    // Right-click either clock → NTP drift check / Windows time resync.
    for (QLabel *clk : {clockLocal_, clockUtc_}) {
        clk->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(clk, &QWidget::customContextMenuRequested, clk,
                [this, clk](const QPoint &p) {
                    showClockMenu(clk->mapToGlobal(p));
                });
    }
    clockLocal_->setToolTip(tr("PC local time — right-click to check PC "
                               "clock drift against network time"));

    // Trailing flexible spacer — balances clockSpacer on the left so the
    // clocks sit centered in the header strip rather than hard against
    // the right edge (old-Lyra placement).
    auto *clockSpacerR = new QWidget(tb);
    clockSpacerR->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(clockSpacerR);

    // Weather-alert badges — hard right of the clocks, toward the right
    // edge of the header (empty/invisible until an alert is active).
    if (wx_) {
        wxIndicator_ = new WxIndicator(wx_, tb);
        tb->addWidget(wxIndicator_);
        connect(wx_, &lyra::wx::WxService::toast, this,
                [this](const QString &title, const QString &body) {
            if (!tray_) {   // lazily create a tray icon for OS notifications
                tray_ = new QSystemTrayIcon(windowIcon(), this);
                tray_->show();
            }
            // Prefix so the OPERATOR can always tell a Lyra alert apart from
            // other weather notifiers (Ambient app, Windows widget, stale
            // Action-Center items) — and log it so the in-app Log records
            // exactly what Lyra emitted, when.
            const QString t = QStringLiteral("Lyra — ") + title;
            tray_->showMessage(t, body, QSystemTrayIcon::Warning, 8000);
            qWarning().noquote() << "[wx] DESKTOP NOTICE FIRED:" << t << "—" << body;
        });
        connect(wx_, &lyra::wx::WxService::chime, this,
                []() { QApplication::beep(); });
    }

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
    if (clockUtc_) {
        QString utc = now.toUTC().toString(QStringLiteral("HH:mm:ss")) +
                      QStringLiteral("Z");
        // ⚠ prefix when the last drift check found the clock off (warn/bad).
        if (driftSeverity_ == TimeSync::Warn || driftSeverity_ == TimeSync::Bad)
            utc.prepend(QStringLiteral("⚠ "));
        clockUtc_->setText(utc);
    }
}

void MainWindow::showClockMenu(const QPoint &global) {
    QMenu m;
    m.addAction(tr("Check clock drift now…"), this, [this]() {
        statusBus_->show(tr("Checking PC clock against network time…"), 0);
        timeSync_->checkDrift();
    });
    m.addAction(tr("Sync time (w32tm /resync)"), this, [this]() {
        statusBus_->show(tr("Requesting Windows time re-sync…"), 0);
        timeSync_->windowsResync();
    });
    m.exec(global);
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

void MainWindow::updateTciStatus() {
    if (!tciStatus_) return;
    if (!tci_ || !tci_->running()) {
        tciStatus_->clear();
        tciStatus_->setVisible(false);
        return;
    }
    const int n = tci_->clientCount();
    tciStatus_->setVisible(true);
    if (n > 0) {
        tciStatus_->setText(tr("● TCI: %1").arg(n));
        tciStatus_->setStyleSheet(QStringLiteral("color:#4caf50;font-weight:bold;"));
        tciStatus_->setToolTip(tr("%n TCI client(s) connected", "", n));
    } else {
        tciStatus_->setText(tr("● TCI"));
        tciStatus_->setStyleSheet(QStringLiteral("color:#f0c040;"));
        tciStatus_->setToolTip(tr("TCI server listening — no clients connected"));
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
            // Honour the panel lock: a locked dock carries
            // NoDockWidgetFeatures (Movable cleared), so refuse the custom
            // float-drag too — without this the lock only stopped DOCKED
            // panels (native drag) + the double-click toggle, and a
            // floating panel still dragged free.  Same feature gate the
            // double-click handler above uses.
            if (dock && dock->isFloating()
                && (dock->features() & QDockWidget::DockWidgetMovable)) {
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
        // The TX DSP rack panels (Speech / EQ / Combinator / Plating) are
        // floating tool windows summoned by the header chip-strip — they are
        // NOT part of the lockable main layout.  Locking them removed their
        // features AND froze their size, so the chips could no longer open
        // them.  Always keep them fully featured + unfrozen, regardless of
        // the lock, so the launchers keep working.
        const QString on = dock->objectName();
        if (on == QLatin1String("txspeech") || on == QLatin1String("txeq") ||
            on == QLatin1String("txcombinator") || on == QLatin1String("txplate") ||
            on == QLatin1String("rxeq")) {
            dock->setFeatures(kUnlockedFeatures);
            if (QWidget *tb = dock->titleBarWidget()) {
                if (auto *fb = tb->findChild<QToolButton *>(QStringLiteral("dockFloat")))
                    fb->setVisible(true);
                if (auto *cb = tb->findChild<QToolButton *>(QStringLiteral("dockClose")))
                    cb->setVisible(true);
            }
            if (dock->property("lyraUnlockedMin").isValid()) {
                dock->setMinimumSize(dock->property("lyraUnlockedMin").toSize());
                dock->setMaximumSize(dock->property("lyraUnlockedMax").toSize());
                dock->setProperty("lyraUnlockedMin", QVariant());
                dock->setProperty("lyraUnlockedMax", QVariant());
            }
            continue;
        }
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
        // NoDockWidgetFeatures stops move/float/close but NOT resize (the
        // dock-area separators when docked, the window frame when floating).
        // Freeze each dock's size while locked: stash its natural min/max
        // once, clamp to the current size, and restore on unlock.  (The
        // stashed properties are not persisted — restoreLayout re-locks
        // after restoreState, re-capturing the natural constraints first.)
        if (locked) {
            if (!dock->property("lyraUnlockedMin").isValid()) {
                dock->setProperty("lyraUnlockedMin", dock->minimumSize());
                dock->setProperty("lyraUnlockedMax", dock->maximumSize());
            }
            dock->setFixedSize(dock->size());
        } else if (dock->property("lyraUnlockedMin").isValid()) {
            dock->setMinimumSize(dock->property("lyraUnlockedMin").toSize());
            dock->setMaximumSize(dock->property("lyraUnlockedMax").toSize());
            dock->setProperty("lyraUnlockedMin", QVariant());
            dock->setProperty("lyraUnlockedMax", QVariant());
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
    } else {
        // First run (no saved session): come up in the curated factory
        // layout rather than the raw dock-creation order.
        restoreState(defaultWindowState());
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

namespace {
// Keys NOT carried in an exported profile — machine/hardware-specific
// choices that shouldn't follow a profile to another PC (e.g. the
// graphics backend: a Vulkan pin must not infect a machine where Vulkan
// fails — the whole reason the default is "auto").
bool isMachineSpecificKey(const QString &k) {
    // Graphics backend: a Vulkan pin must not infect a machine where
    // Vulkan fails (the whole reason the default is "auto").
    if (k == QStringLiteral("ui/graphicsBackend")) return true;
    // Radio CONNECTION IDENTITY is station-specific and must NEVER follow
    // a shared profile to another PC.  A "defaults" export that carried
    // the author's radio address would auto-connect the importer to an IP
    // that doesn't exist on their LAN on next launch -> RX stall / hang.
    //   radio/lastIp  — the startup auto-connect target (main.cpp)
    //   lastRadio/*   — the remembered radio record (ip/mac/board/...)
    // Filtered on BOTH export and import, so this also neutralises an
    // already-distributed pre-fix export when it's imported.
    if (k == QStringLiteral("radio/lastIp")) return true;
    if (k.startsWith(QStringLiteral("lastRadio/"))) return true;
    return false;
}
} // namespace

void MainWindow::exportSettings() {
    // Capture the CURRENT on-screen layout into the session keys so the
    // exported profile reflects exactly what's showing now.
    {
        QSettings s;
        s.setValue(QStringLiteral("ui/geometry"), saveGeometry());
        s.setValue(QStringLiteral("ui/windowState"), saveState());
    }
    const QString docs =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export Lyra settings"),
        docs + QStringLiteral("/lyra-profile.lyra"),
        tr("Lyra profile (*.lyra)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".lyra"), Qt::CaseInsensitive))
        path += QStringLiteral(".lyra");

    QSettings src;                                   // live scope
    QSettings dst(path, QSettings::IniFormat);
    dst.clear();
    int n = 0;
    for (const QString &k : src.allKeys()) {
        if (isMachineSpecificKey(k)) continue;
        dst.setValue(k, src.value(k));
        ++n;
    }
    dst.sync();
    if (dst.status() != QSettings::NoError) {
        QMessageBox::warning(this, tr("Export failed"),
            tr("Couldn't write the profile to:\n%1").arg(path));
        return;
    }
    QMessageBox::information(this, tr("Settings exported"),
        tr("Saved %1 settings (layout + preferences) to:\n%2")
            .arg(n).arg(path));
}

void MainWindow::importSettings() {
    const QString docs =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Lyra settings"), docs,
        tr("Lyra profile (*.lyra);;All files (*)"));
    if (path.isEmpty()) return;

    QSettings file(path, QSettings::IniFormat);
    const QStringList keys = file.allKeys();
    if (keys.isEmpty() || file.status() != QSettings::NoError) {
        QMessageBox::warning(this, tr("Import failed"),
            tr("That file doesn't look like a Lyra profile."));
        return;
    }
    {
        QSettings live;
        for (const QString &k : keys) {
            if (isMachineSpecificKey(k)) continue;   // never import hw choices
            live.setValue(k, file.value(k));
        }
        live.sync();
    }
    // Apply the imported layout live NOW so the on-screen arrangement
    // matches the profile (a later close-save then can't clobber it),
    // then offer a restart so Prefs re-read the rest cleanly at startup.
    {
        QSettings s;
        const QByteArray geo =
            s.value(QStringLiteral("ui/geometry")).toByteArray();
        const QByteArray st =
            s.value(QStringLiteral("ui/windowState")).toByteArray();
        if (!geo.isEmpty()) restoreGeometry(geo);
        if (!st.isEmpty())  restoreState(st);
        for (auto *dock : std::as_const(docks_)) dock->show();
        if (prefs_) {
            const QVariant sp =
                s.value(QStringLiteral("ui/panadapterSplit"));
            if (sp.isValid()) prefs_->setPanadapterSplit(sp);
        }
    }
    const auto btn = QMessageBox::question(this, tr("Settings imported"),
        tr("Imported %1 settings (the layout is applied now).\n\n"
           "Restart Lyra to apply the rest (visuals, etc.)?\n"
           "(Graphics backend is left as this machine's own setting.)")
            .arg(keys.size()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (btn == QMessageBox::Yes) {
        QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                QStringList());
        QCoreApplication::quit();
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
    // Prefer the curated factory layout (operator's embedded arrangement).
    // Fall back to the programmatic placement below only if it fails to
    // apply (e.g. a future build whose dock object names changed).
    if (restoreState(defaultWindowState())) {
        for (auto *dock : std::as_const(docks_)) {
            dock->show();
        }
        if (prefs_) {
            prefs_->setPanadapterSplit(QVariant());   // QML 60/40 default
        }
        return;
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

// TX-rip Phase 1 (Q2): setTciMicSource / setTxDspWorker method bodies
// removed; declarations dropped in mainwindow.h.  TX wiring rebuilt per
// docs/TX_ARCHITECTURAL_MAPPING.md §10.3.

void MainWindow::closeEvent(QCloseEvent *event) {
    saveLayout();

    // Task #47 — QML null-safety on shutdown teardown.
    //
    // Every QML panel binds against the C++ service-object context
    // properties exposed by makeQuick() (Stream, Wdsp, WdspEngine,
    // Prefs, Bands, BandPlan, Status, BandMemory, Gen, Time, Memory,
    // Eibi, Spots, Meter, Help, Discovery).  Those services are
    // parented to QCoreApplication and are destroyed by Qt's child
    // cleanup AFTER aboutToQuit returns — i.e. AFTER this MainWindow
    // is gone but possibly BEFORE every QQuickWidget has finished
    // tearing its scene-graph down on a deferred event.  Any binding
    // that re-evaluates in that window (e.g. a deleteLater on a
    // ListView delegate touching Stream.rx1FreqHz) reads a dangling
    // pointer.  No crash — Qt's qmlEngine warns ("Cannot read
    // property ... of null") and the noise drowns out real warnings.
    //
    // Fix: drop every QQuickWidget's QML scene synchronously HERE,
    // while every service is still alive and reachable.  setSource()
    // with an empty URL unloads the root component, releases all
    // bindings, and disconnects the scene graph — by the time this
    // returns no QML expression can fire against any service object,
    // regardless of teardown order downstream.
    //
    // Cheap (a handful of widgets), idempotent (a re-shown window
    // would have to call setSource() again anyway), and contained to
    // one path — the eventual feature owner (Profile Manager v0.2.x
    // dialog, etc.) gets the same protection for free since every
    // QQuickWidget is reached via findChildren<>.
    const auto qws = findChildren<QQuickWidget *>();
    for (auto *qw : qws) {
        if (qw) qw->setSource(QUrl());
    }

    QMainWindow::closeEvent(event);
}

// ----------------------------------------------------------------
// TX-0c-pa-debug — HL2 telemetry strip refresh.
//
// Reads the TX-0a Q_PROPERTYs (hl2TempC / hl2SupplyV / paCurrentA),
// formats "HL2: T 24.7°C  V 12.3 V  PA 0.00 A".  NaN getters (no
// telemetry frames yet, e.g. pre-Start) render as "—" so the strip
// is always coherent.  PA current goes BOLD RED at ≥50 mA — the
// operator's at-a-glance "RF on the air" indicator.  Threshold is
// safely above the worst-case noise on a NaN/zero idle reading and
// well below any keyed-bias level (HL2+ idle PA bias ≈ 200 mA on
// the operator's unit; full tune draws ~1.8 A).
//
// Updates fire from HL2Stream::statsChanged (~5 Hz via statsTimer_),
// the same signal that already drives the existing stats UI — no
// new timer, no new wake-ups, no impact on the EP2/wire cadence.
void MainWindow::refreshHl2TelemetryStrip() {
    if (!hl2TelemLabel_) return;
    auto *st = qobject_cast<lyra::ipc::HL2Stream *>(stream_);
    if (!st) return;

    auto fmt = [](double v, int prec, const QString &unit) -> QString {
        if (std::isnan(v)) return QStringLiteral("—");
        return QStringLiteral("%1 %2").arg(v, 0, 'f', prec).arg(unit);
    };

    const double t   = st->hl2TempC();
    const double v   = st->hl2SupplyV();
    const double pa  = st->paCurrentA();

    hl2TelemLabel_->setText(QStringLiteral("HL2: T %1  V %2  PA %3")
        .arg(fmt(t,  1, QStringLiteral("°C")))
        .arg(fmt(v,  2, QStringLiteral("V")))
        .arg(fmt(pa, 2, QStringLiteral("A"))));

    // RF-on indicator: PA current red+bold ≥50 mA.  Stays muted blue
    // when PA is idle (gateware off, MOX off, or PA-enable unchecked)
    // so the operator sees a clean transition on each keydown/keyup.
    const bool paLive = !std::isnan(pa) && pa >= 0.05;
    hl2TelemLabel_->setStyleSheet(paLive
        ? QStringLiteral("QLabel{color:#ff4040;font-family:Consolas,"
                         "Menlo,monospace;padding:0 8px;font-weight:bold;}")
        : QStringLiteral("QLabel{color:#8fa6ba;font-family:Consolas,"
                         "Menlo,monospace;padding:0 8px;}"));
}

// ----------------------------------------------------------------
// TX-0c-fsm — space-bar momentary PTT.
//
// Press space → Stream.requestMox(true); release → requestMox(false).
// Auto-repeat is ignored (one edge per actual press; held key keeps
// MOX on until release).  When a text-entry widget has focus the
// event passes through unhandled so typing space into a freq overlay
// or any QLineEdit/QSpinBox/QText* still types a space character.
//
// Limitation: QML TextField focus inside the embedded QQuickWidgets
// is NOT detected (Qt reports the QQuickWidget itself as the focus
// widget, not the inner QML item).  Mitigation today: the only QML
// text-entry surface that takes focus is the freq-entry overlay in
// TuningPanel which is hidden by default and only shown on explicit
// click — operator-controlled, not accidental.  Refine via a QML
// focus probe if a tester ever keys MOX while typing.
//
// The MOX button on TuningPanel (toggle on click) and this space-bar
// momentary path BOTH funnel through Stream.requestMox, so intent
// stays consistent: the FSM (single source of truth) resolves whether
// to keydown / keyup / collapse / cancel.

static bool isEditableFocus(QWidget *fw) {
    if (!fw) return false;
    if (qobject_cast<QLineEdit *>(fw))       return true;
    if (qobject_cast<QSpinBox *>(fw))        return true;
    if (qobject_cast<QPlainTextEdit *>(fw))  return true;
    if (qobject_cast<QTextEdit *>(fw))       return true;
    // QQuickWidget hosts QML; for TX-0c-fsm we route space through.
    // The only QML focused text-entry today is the freq overlay (see
    // commentary above); refine later if a tester reports keying MOX
    // mid-typing.
    return false;
}

void MainWindow::keyPressEvent(QKeyEvent *event) {
    // Task #157 — space-bar PTT is operator-gated (Settings → Hardware →
    // Transmit).  When disabled, fall through to default handling so the
    // operator's accidental space presses no longer key MOX.
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()
        && prefs_ && prefs_->spaceBarPttEnabled()) {
        if (!isEditableFocus(QApplication::focusWidget())) {
            if (auto *st = qobject_cast<lyra::ipc::HL2Stream *>(stream_)) {
                st->requestMox(true);
                event->accept();
                return;
            }
        }
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()
        && prefs_ && prefs_->spaceBarPttEnabled()) {
        if (!isEditableFocus(QApplication::focusWidget())) {
            if (auto *st = qobject_cast<lyra::ipc::HL2Stream *>(stream_)) {
                st->requestMox(false);
                event->accept();
                return;
            }
        }
    }
    QMainWindow::keyReleaseEvent(event);
}

} // namespace lyra::ui
