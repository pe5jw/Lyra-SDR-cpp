// Lyra — Hermes Lite 2 / 2+ SDR transceiver (C++23 / Qt 6 rebuild).
//
// Step 1 entry point.  Opens a Qt Quick window backed by RHI
// (Vulkan/D3D12 on Windows; Metal on macOS; OpenGL fallback) and
// kicks off a C++ UDP discovery sweep on a dedicated worker
// QThread.  Discovery results stream into the window as they
// arrive.  No Python, no GIL, no in-process bottleneck on the
// wire path.

#include "hl2_discovery.h"
#include "hl2_stream.h"
#include "panadapter.h"
#include "waterfall.h"
#include "freqdisplay.h"
#include "wdsp_engine.h"
#include "mic_source.h"
#include "mainwindow.h"
// TX-rip Phase 1 (Q2): tci_mic_source.h / tx_dsp_worker.h removed —
// the TX DSP subsystem is being rebuilt from empty files per
// docs/TX_ARCHITECTURAL_MAPPING.md §10.3.  TX wiring below collapses
// to a stub until the new components land.
#include "wdsp_native.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <thread>
#ifdef _WIN32
// WIN32_LEAN_AND_MEAN prevents windows.h pulling in winsock1, which
// would clash with winsock2.h (transitively included via the existing
// networking headers later in this file).  Need just TerminateProcess
// + GetCurrentProcess for the Task #40 zombie-shutdown watchdog.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace {
// Task #40 — TX-triggered zombie shutdown watchdog.  Operator-flagged
// 2026-05-31: closing Lyra after using TX leaves the process running in
// the background (clean exit on RX-only sessions).  Until the root
// cause is identified + fixed properly, this watchdog stops the
// bleeding: a detached thread armed at aboutToQuit polls a
// "shutdown_complete" flag, and if main() hasn't returned within 10 s
// of aboutToQuit firing it calls TerminateProcess (Win32) / std::_Exit
// (fallback).  Aggressive — bypasses static destructors, leaks file
// handles + sockets back to the OS — but the OS reclaims those at
// process exit anyway, and the operator-visible symptom (process keeps
// running) is the bug we're solving.  The companion diagnostic
// [shutdown] qWarning lines flushed before this fires tell us in the
// next bench WHICH teardown step took >10 s, so the next commit can
// fix the actual wedge.  Once root cause is fixed, this watchdog stays
// as belt-and-suspenders against future regressions.
std::atomic<bool> g_shutdown_complete{false};
}  // namespace

#include <QCoreApplication>
#include <QGuiApplication>
#include <QApplication>
#include <QColor>
#include <QIcon>
#include <QPalette>
#include <QSettings>
#include <QString>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSettings>
#include <QSurfaceFormat>

#include "mainwindow.h"
#include "wxservice.h"
#include "prefs.h"
#include "logbuffer.h"
#include "theme.h"
#include <QTimer>
#include <QtQml>

#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif

int main(int argc, char *argv[])
{
    // ---- Subprocess entry point (Step 3c-i) ----
    // When invoked as `lyra.exe --build-wisdom <dir>` we are a
    // SHORT-LIVED CHILD spawned by the main Lyra process to build
    // the FFTW wisdom file out-of-process.  AllocConsole inside
    // WDSPwisdom would corrupt the parent's stdout if we ran in
    // the parent process; this subprocess isolates the damage.
    //
    // Must be FIRST in main() — before QGuiApplication, before
    // any Qt RHI or networking init.  We're a console-only mode
    // here: no window, no QML, no event loop.  Just LoadLibrary,
    // call WDSPwisdom, exit.
    if (argc == 3 && std::string_view(argv[1]) == "--build-wisdom") {
        // QCoreApplication needed for QStandardPaths + QString
        // I/O.  No event loop, no GUI.
        QCoreApplication app(argc, argv);
        app.setApplicationName(QStringLiteral("Lyra-cpp"));
        app.setOrganizationName(QStringLiteral("N8SDR"));
        lyra::dsp::WdspNative builder;
        return builder.runWisdomBuilderEntryPoint(
            QString::fromLocal8Bit(argv[2]));
    }

#ifdef _WIN32
    // Explicit WSAStartup before any native socket use (HL2Stream
    // uses raw WinSock2 — see src/hl2_stream.cpp).  Qt would do it
    // lazily for QUdpSocket, but we don't want to depend on Qt's
    // init ordering for our wire path.  WSAStartup is refcounted —
    // safe to call alongside Qt's.
    WSADATA wsa;
    ::WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    // Qt RHI backend selection.  Lyra targets Vulkan as the
    // primary graphics path per FEATURES.md §0 (cross-vendor /
    // cross-platform).  setGraphicsApi()
    // MUST be called BEFORE QGuiApplication construction.  If
    // Vulkan is unavailable at runtime (no driver loader / no
    // SDK / unsupported GPU) Qt RHI transparently falls back to
    // D3D12 → OpenGL — no operator-visible breakage.
    //
    // Setting QSG_INFO=1 makes Qt log the chosen backend on
    // startup so the operator can VERIFY which RHI is live
    // (look for "Using QRhi with backend Vulkan" in stdout).
    // qputenv is portable; std::setenv would be POSIX-only.
    qputenv("QSG_INFO", "1");
    // Org/app identity set statically (before any QSettings read) so the
    // graphics-backend pref below resolves to the right HKCU location.
    // QStandardPaths leaf is "Lyra-cpp" — no dir sharing with Python Lyra
    // ("Lyra") per CLAUDE.md §15.26.
    QCoreApplication::setOrganizationName(QStringLiteral("N8SDR"));
    QCoreApplication::setApplicationName(QStringLiteral("Lyra-cpp"));
    QCoreApplication::setOrganizationDomain(
        QStringLiteral("github.com/N8SDR1/Lyra-SDR"));
    // Capture qDebug/qWarning/etc into the in-app Log viewer + a rolling
    // app-data log file.  Release builds are GUI-subsystem (no console),
    // so this is the operator's window into diagnostics.  Install early —
    // after the app-data path resolves (org/app name set above), before
    // any service object starts logging.
    lyra::ui::LogBuffer::instance().install();
    // Operator-selectable RHI backend (Settings → Visuals → Graphics
    // backend; restart-to-apply since the API is fixed at startup).
    // "auto" leaves Qt RHI to pick per platform; any explicit value still
    // falls back transparently if that backend is unavailable at runtime.
    {
        using RI = QSGRendererInterface;
        // Resolution order: LYRA_GRAPHICS env var (no-UI escape hatch for
        // testers) -> persisted Settings -> "auto" default.  "auto" leaves
        // the API unpinned so Qt RHI picks the most compatible backend per
        // machine (Direct3D 11 on Windows) and honours QSG_RHI_BACKEND.
        // This avoids Vulkan being force-pinned on GPUs/drivers where the
        // QQuickWidget swapchain fails to create OR crashes on dock
        // float/reparent (both field-reported on tester hardware).  Vulkan
        // stays fully selectable in Settings -> Visuals.
        QString be = qEnvironmentVariable("LYRA_GRAPHICS").trimmed().toLower();
        if (be.isEmpty())
            be = QSettings().value(QStringLiteral("ui/graphicsBackend"),
                                   QStringLiteral("auto")).toString().toLower();
        if      (be == "vulkan") QQuickWindow::setGraphicsApi(RI::Vulkan);
        else if (be == "opengl") QQuickWindow::setGraphicsApi(RI::OpenGL);
        else if (be == "d3d11")  QQuickWindow::setGraphicsApi(RI::Direct3D11);
        else if (be == "d3d12")  QQuickWindow::setGraphicsApi(RI::Direct3D12);
        // "auto" (default) -> leave unpinned; Qt RHI auto-selects.
    }

    // Multisample anti-aliasing.  Without this the RHI swapchain runs
    // at 1 sample/pixel — every scene-graph edge (the spectrum trace,
    // passband box, markers) renders as hard stair-stepped pixels (the
    // "etch-a-sketch" look).  4x MSAA lets the GPU smooth every polygon
    // edge essentially for free on a modern card.  This is the single
    // biggest perceived-quality win and applies to ALL geometry, not
    // just the panadapter.  Must be set on the default surface format
    // BEFORE QGuiApplication so the QML window's swapchain picks it up.
    {
        QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
        fmt.setSamples(4);
        QSurfaceFormat::setDefaultFormat(fmt);
    }

    QApplication app(argc, argv);
    // Window / taskbar icon (the embedded .exe icon comes from src/lyra.rc;
    // this sets the running window + taskbar icon from the bundled PNG).
    app.setWindowIcon(QIcon(QStringLiteral(
        ":/qt/qml/Lyra/src/assets/logo/lyra-icon-256.png")));
    // Apply old-Lyra's dark cool-CRT theme to the whole QtWidgets shell
    // (menu bar, dock title bars, Settings dialog, tabs, controls).
    app.setStyleSheet(lyra::ui::lyraStyleSheet());
    // Hyperlink colours: Qt's default Link role is a dark blue that is
    // unreadable on Lyra's near-black surfaces.  Set the cyan accent
    // app-wide so EVERY rich-text widget (the Help guide today, anything
    // with links later) renders readable links on the dark background.
    {
        QPalette pal = app.palette();
        pal.setColor(QPalette::Link, QColor(0x5e, 0xc8, 0xff));
        pal.setColor(QPalette::LinkVisited, QColor(0x8f, 0xd6, 0xff));
        app.setPalette(pal);
    }
    // (Org / app / domain identity already set statically above, before
    // the graphics-backend QSettings read.)

    // Discovery lives on the Qt main thread.  This is FINE: QUdpSocket
    // is fully async / event-loop-driven on Qt (no blocking recvfrom,
    // no GIL, no Python).  The "wire path on its OWN OS thread"
    // requirement applies to the real-time EP2 writer (380 Hz
    // cadence) and the RX loop (~5000 datagrams/s) — those land in
    // dedicated threads in later commits.  Discovery is one-shot
    // bursty UDP — it does not need its own thread, and putting it
    // on one breaks QML's main-thread Connections requirement
    // (QQmlEngine refuses cross-thread connect from QML).
    auto *discovery = new lyra::ipc::HL2Discovery(&app);

    // Step 2a: the stream object opens the EP6 RX path to a
    // selected radio on its OWN dedicated OS thread (std::jthread
    // inside HL2Stream — see hl2_stream.h for the locked
    // architecture rationale).  Exposed to QML as `Stream`.
    auto *stream = new lyra::ipc::HL2Stream(&app);

    // Step 3a: bundle the WDSP DSP engine.  Created here so the
    // QML context-property is available when Main.qml loads, but
    // load() itself is DEFERRED until AFTER engine.loadFromModule
    // — otherwise the logLine signal we emit on success/failure
    // arrives before the QML Connections are wired and the
    // operator never sees the result.  Step 3a's scope is
    // load-or-fail only — no symbols resolved yet (Step 3b), no
    // DSP yet (Step 3c+).
    auto *wdsp = new lyra::dsp::WdspNative(&app);

    // Step 3c-ii: the WDSP RX channel engine.  Owns the lifecycle of
    // a single WDSP receiver channel (OpenChannel + config + start /
    // stop + close).  Created here so the QML context-property exists
    // when Main.qml loads; openRx1() is DEFERRED until after the DLL
    // loads + wisdom is ensured (below) so the channel-open is fast.
    auto *wdspEngine = new lyra::dsp::WdspEngine(wdsp, &app);

    // Step 3d: route DDC0 baseband IQ from the stream's RX worker
    // thread into the DSP engine.  Set BEFORE any open() so the worker
    // sees the sink the moment it starts.  feedIq() runs synchronously
    // on the RX worker thread (no Qt cross-thread queueing) and guards
    // internally on the channel being open.
    stream->setIqSink([wdspEngine](const double *iq, int n) {
        wdspEngine->feedIq(iq, n);
    });

    // Step 5: route decoded RX audio the other way — engine → stream's
    // EP2 writer — so it plays out the HL2 onboard codec (AK4951) jack
    // when "HL2 audio jack" is the selected output (old Lyra's default).
    // pushAudio runs on the RX worker thread (inline after feedIq);
    // setInjectAudio toggles the EP2 L/R payload on/off.
    wdspEngine->setHl2AudioSink(
        [stream](const qint16 *lr, int n) { stream->pushAudio(lr, n); },
        [stream](bool on) { stream->setInjectAudio(on); });

    // TX-1 components 3 + 4c — Path A construction ordering
    // (matches the C reference's create_cmaster + create_xmtr
    // posture: the TXA channel is only OpenChannel'd AFTER the
    // WDSP DLL has been loaded).  The pointers are declared
    // here at outer scope (nullptr-initialised) so the
    // aboutToQuit teardown lambda — connected NOW, in
    // connection order BEFORE the stream->close() connect
    // below at line ~250 — captures them BY REFERENCE and
    // sees whichever value is live at app exit.  Actual
    // construction is deferred to the QTimer::singleShot block
    // further down (the same block that gates wdsp->ensureWisdom
    // + wdspEngine->openRx1 on wdsp->load() succeeding).  If
    // wdsp->load() fails or the user closes before the timer
    // fires, the pointers stay nullptr and `delete nullptr` is
    // a well-defined no-op — RX path is unaffected either way.
    //
    // Teardown order (aboutToQuit fires in connection order):
    //   1. stream->registerTxIqSource({}) +
    //      stream->registerTxControl({}) -> clears BOTH callback
    //      registrations FIRST.  Both callbacks capture txWorker
    //      by value (pointer); deleting txWorker before clearing
    //      them would leave dangling pointers that the EP2 writer
    //      thread (source) and the FSM (control: start/stop/inject)
    //      could call concurrently.  Both registrations take a
    //      brief mutex; after clearing, EP2 writer falls through
    //      to zero-fill on next datagram and FSM keydown/keyup
    //      hooks no-op cleanly (app is shutting down anyway).
    //   2. txWorker delete -> clears its mic consumer, joins
    //      worker thread, closes TX WDSP channel.
    //   3. micSource delete -> clears HL2Stream's forwarder
    //      while HL2Stream is still alive.
    //   4. stream->close() -> joins the rx-loop thread.
    lyra::dsp::Hl2Ep6MicSource *micSource = nullptr;
    // TX-rip Phase 1 (Q2): txWorker / tciMicSource pointer declarations
    // removed — TX DSP worker + TCI mic source are being rebuilt from
    // empty files per docs/TX_ARCHITECTURAL_MAPPING.md §10.3.

    // Task #40 — TX-triggered zombie shutdown watchdog.  REGISTERED
    // FIRST so it fires before any teardown handler and the
    // 10 s watchdog timer starts at the very beginning of teardown.
    // Also drops a "[shutdown] === aboutToQuit FIRED ===" marker into
    // lyra-log.txt so the next bench can confirm aboutToQuit even
    // ran (a separate-class bug would be aboutToQuit never firing —
    // e.g. window-close not triggering app quit).
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
        qWarning("[shutdown] === aboutToQuit FIRED — watchdog armed (10 s) ===");
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            if (g_shutdown_complete.load(std::memory_order_acquire)) return;
            qWarning("[shutdown] === WATCHDOG: teardown >10 s — TerminateProcess ===");
#ifdef _WIN32
            ::TerminateProcess(::GetCurrentProcess(), 1);
#else
            std::_Exit(1);
#endif
        }).detach();
    });

    // Note: `win` is declared further down — capture by reference so
    // the lambda sees the populated pointer at fire time (aboutToQuit
    // runs during app.exec(), well before main() returns).  Same
    // pattern as &txWorker/&micSource above.
    lyra::ui::MainWindow *winRef = nullptr;
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     [stream, &micSource]() {
        // TX-rip Phase 1 (Q2): teardown collapses to the RX-only
        // surface (HL2Stream TX callbacks unregister + Hl2Ep6MicSource
        // delete).  TxDspWorker / TciMicSource teardown returns with
        // the rebuild per docs/TX_ARCHITECTURAL_MAPPING.md §10.3.
        qWarning("[shutdown] handler-1 ENTRY");
        if (stream) {
            qWarning("[shutdown] handler-1 step a: registerTxIqSource({}) - start");
            stream->registerTxIqSource({});
            qWarning("[shutdown] handler-1 step a: done");
            qWarning("[shutdown] handler-1 step b: registerTxControl({}) - start");
            stream->registerTxControl({});
            qWarning("[shutdown] handler-1 step b: done");
        }
        qWarning("[shutdown] handler-1 step d: delete micSource - start (~Hl2Ep6MicSource runs now)");
        delete micSource; micSource = nullptr;
        qWarning("[shutdown] handler-1 step d: done");
        qWarning("[shutdown] handler-1 EXIT");
    });

    // Task #26 — auto-mute RX1 audio while the wire MOX bit is live so
    // the operator doesn't self-deafen off their own TX coupling.
    // moxActiveChanged fires on the TR-settled edges only (post-mox_delay
    // + rf_delay on keydown; post-ptt_out_delay on keyup), so the mute
    // tracks the actual on-air window, not the operator's click.  The
    // WdspEngine side honours an operator-toggleable autoMuteOnTx
    // setting (default ON; persisted), so this signal can be ignored
    // by the gain calc when ESSB self-monitoring or similar is wanted.
    // AutoConnection: signal emitted from the Qt main thread (via the
    // FSM QTimers in HL2Stream), slot also lives on the main thread.
    QObject::connect(stream, &lyra::ipc::HL2Stream::moxActiveChanged,
                     wdspEngine, &lyra::dsp::WdspEngine::setTxMuted);

    // Task #44 Phase 2 — analyzer source swap on the MOX edge.
    // Same moxActiveChanged signal as setTxMuted (above); connection
    // ORDER matters: setTxMuted MUST run before setTxOwnsAnalyzer on
    // the keydown edge so audio mutes BEFORE the analyzer flips
    // sources (operator hears silence, not a brief blip).  Qt
    // guarantees connection-order DirectConnection slot dispatch
    // for same-thread emitter+receiver — leave THIS connect AFTER
    // the setTxMuted connect above.  Reordering them is a §15.26
    // PART B integrity hazard (amendment A.7 in the reconciled
    // doc).  If a future maintainer needs to reorder for any
    // reason, route both through a single combined slot instead.
    QObject::connect(stream, &lyra::ipc::HL2Stream::moxActiveChanged,
                     wdspEngine, &lyra::dsp::WdspEngine::setTxOwnsAnalyzer);

    // Stop the RX worker on quit BEFORE the QObject children are
    // destroyed.  The IQ sink (above) calls into wdspEngine from the
    // worker thread; aboutToQuit fires while every object is still
    // alive, so closing the stream here joins the worker and
    // guarantees no feedIq() runs into a half-torn-down engine /
    // freed audio ring (see WdspEngine::stopAudio).
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     stream, [stream]() {
        qWarning("[shutdown] handler-2 ENTRY (stream->close)");
        if (stream) stream->close();
        qWarning("[shutdown] handler-2 EXIT");
    });

    // Step 5: register the panadapter widget as a QML type under its
    // own URI (keeps it separate from the qt_add_qml_module "Lyra"
    // module's auto-registration).  Main.qml: `import LyraUI`.
    qmlRegisterType<lyra::ui::Panadapter>("LyraUI", 1, 0, "Panadapter");
    qmlRegisterType<lyra::ui::Waterfall>("LyraUI", 1, 0, "Waterfall");
    qmlRegisterType<lyra::ui::FreqDisplay>("LyraUI", 1, 0, "FreqDisplay");

    // The app shell is a QtWidgets QMainWindow (dock framework +
    // menu bar + toolbar + layout persistence).  It embeds the Quick
    // scene-graph UI via QQuickWidget and exposes the four service
    // objects to that QML as context properties (Discovery / Stream /
    // Wdsp / WdspEngine).  All native C++ — no Python, no GIL.
    auto *prefs = new lyra::ui::Prefs(&app);
    // Live-apply the verbose-logging toggle (Settings → Hardware) to the
    // log buffer; the initial value was read from QSettings on install().
    lyra::ui::LogBuffer::instance().setVerbose(prefs->debugLogging());
    QObject::connect(prefs, &lyra::ui::Prefs::debugLoggingChanged, prefs,
                     [prefs]() {
                         lyra::ui::LogBuffer::instance().setVerbose(
                             prefs->debugLogging());
                     });
    // Task #44 Phase 1 — wire MOX edges into Prefs so dbMin/dbMax
    // swap to the TX persistence pair on the air, and back to the RX
    // pair on un-key.  QML's `dbMin: Prefs.dbMin` binding picks up
    // the swap via the dbMin/MaxChanged signals Prefs emits in
    // setMoxActive — no panadapter wiring needed.
    QObject::connect(stream, &lyra::ipc::HL2Stream::moxActiveChanged,
                     prefs,  &lyra::ui::Prefs::setMoxActive);
    prefs->setMoxActive(stream->moxActive());   // seed initial state

    // Task #74 / #77 / #78 — TUN separate-drive orchestrator.  When
    // the operator arms TUN and Prefs.useTuneDrive is on, stash the
    // current wire drive level and push the tune-drive % (0..100 →
    // 0..255 wire DAC); on TUN-off, restore the stashed value.  When
    // useTuneDrive is off this is a no-op — TUN keys at the
    // operator's TX Drive % (byte-identical to pre-#74 behaviour).
    //
    // #77 fix: use std::optional<int> on the heap (shared_ptr-wrapped
    // for lifetime) so we track whether a real TUN-arm edge has
    // stashed a value in this session.  HL2Stream emits a defensive
    // `tuneEnabledChanged(false)` at every stream START and STOP
    // (hl2_stream.cpp, the auto-clear at lines ~586/685) — without
    // the optional guard those spurious off-edges would call
    // `setTxDriveLevel(*savedDrive)` with the default-constructed
    // 0, blanking the operator's TX Drive on every stream restart.
    // Only an actual arm→release pair restores; spurious offs no-op.
    //
    // #78 fix: a second connection on Prefs::tuneDrivePctChanged
    // routes live slider movement to the stream during an active
    // tune (when stream->tuneEnabled() AND prefs->useTuneDrive()).
    // Critically does NOT touch `savedDrive` — the stashed pre-tune
    // value must stay frozen so the tune-release restore is correct.
    //
    // Lambdas run on the emitter thread (DirectConnection by default;
    // HL2Stream + prefs both live on the main thread) so a TUN arm's
    // drive push completes BEFORE the QML's subsequent
    // Stream.requestMox(true) — first TX frame post-MOX-rise
    // carries the tune-drive level.
    {
        auto savedDrive = std::make_shared<std::optional<int>>();
        QObject::connect(stream, &lyra::ipc::HL2Stream::tuneEnabledChanged,
                         prefs, [stream, prefs, savedDrive](bool on) {
            if (!prefs->useTuneDrive()) {
                // Toggle is off — never stash, never restore.  Also
                // clear any orphan stash so a later toggle-on +
                // arm cycle starts clean.
                savedDrive->reset();
                return;
            }
            if (on) {
                // Real TUN-arm: stash + push tune-drive.  Re-arm
                // without a release between (defensive) — keep the
                // earliest pre-tune value, not a tune-drive value.
                if (!savedDrive->has_value()) {
                    *savedDrive = stream->txDriveLevel();
                }
                const int raw = std::clamp(int(std::lround(
                    prefs->tuneDrivePct() * 255.0 / 100.0)), 0, 255);
                stream->setTxDriveLevel(raw);
            } else {
                // TUN-release.  Restore ONLY if we have a real
                // stashed value from a prior arm in this session
                // (#77 fix: the spurious off-edges at stream
                // start/stop carry no stash and must no-op).
                if (savedDrive->has_value()) {
                    stream->setTxDriveLevel(savedDrive->value());
                    savedDrive->reset();
                }
            }
        });

        // #78 — live tune-drive while TUN is active.  Drag the
        // TxPanel TUN-drive slider while keyed and the wire DAC
        // follows immediately.  Skipped when TUN is not armed or
        // the toggle is off (no spurious wire pushes; the
        // tuneDrivePct value still persists per-band via #74
        // recall for the next arm).  Does NOT mutate savedDrive
        // — restore-on-release must use the pre-tune value, not
        // the last live-tuned value.
        QObject::connect(prefs, &lyra::ui::Prefs::tuneDrivePctChanged,
                         prefs, [stream, prefs]() {
            if (!prefs->useTuneDrive()) return;
            if (!stream->tuneEnabled())  return;
            const int raw = std::clamp(int(std::lround(
                prefs->tuneDrivePct() * 255.0 / 100.0)), 0, 255);
            stream->setTxDriveLevel(raw);
        });
    }
    // Weather-alert service — polls the operator's enabled sources and
    // feeds the header badges + toasts.  Reads its location from Prefs,
    // so a station-location change re-arms it.
    auto *wx = new lyra::wx::WxService(prefs, &app);
    QObject::connect(prefs, &lyra::ui::Prefs::locationChanged,
                     wx, &lyra::wx::WxService::reloadConfig);
    auto *win = new lyra::ui::MainWindow(discovery, stream, wdsp,
                                         wdspEngine, prefs, wx);
    winRef = win;   // populate the aboutToQuit teardown handler's reference
    win->show();

    // Defer the WDSP load / wisdom / channel-open to the FIRST
    // event-loop iteration via a zero-delay single-shot.  These steps
    // emit status via logLine signals; if we run them here (before
    // app.exec()) the QML scene isn't live yet, so the lines reach the
    // console via qInfo() but NOT the in-UI Log panel (operator-
    // observed — only [disc]/[strm], which fire after exec, showed).
    // Posting to the event loop means every [wdsp] line lands in the
    // Log panel exactly like [disc]/[strm].
    QTimer::singleShot(0, &app, [wdsp, wdspEngine, stream, prefs, win,
                                  &micSource]() {
        if (wdsp->load()) {
            // Step 3c-i: ensure FFTW wisdom is loaded BEFORE the first
            // OpenChannel anywhere.  Without it, WDSP's PATIENT
            // planning runs in-process on first channel-open and
            // freezes the UI for several minutes.  ensureWisdom()
            // either imports a cached file (fast) or spawns a
            // subprocess to build one (slow but the work is in the
            // child process, so the parent doesn't run FFTW_PATIENT
            // in-process).
            wdsp->ensureWisdom();

            // Step 3c-ii: with the DLL loaded + wisdom in place, open
            // RX1 as a live WDSP channel.  Channel-lifecycle proof
            // only — no IQ flows through it yet (Step 3d wires the RX
            // worker -> fexchange0).  The engine's destructor closes
            // the channel at app exit.
            wdspEngine->openRx1();

            // TX-1 Path A: construct micSource + txWorker NOW that
            // the WDSP DLL is loaded.  TxChannel::open requires the
            // DLL; constructing earlier (e.g. right after the
            // wdspEngine setup) silently spawned an inert worker
            // and logged `[tx] open: WDSP DLL not loaded`.  This
            // ordering matches the C reference's create_cmaster
            // flow: TXA channel-open follows WDSP init.
            //
            // Pointers are captured by reference from main()'s
            // outer scope so the aboutToQuit lambda (connected
            // earlier) sees them populated at teardown.
            micSource = new lyra::dsp::Hl2Ep6MicSource(*stream);

            // TX-rip Phase 1 (Q2): TxDspWorker / TciMicSource
            // construction + the entire TxControl/TxIqSource
            // registration block + RX-mode→TX-mode propagation +
            // TX-bandpass wiring REMOVED.  The TX DSP subsystem is
            // being rebuilt from empty files per the signed Phase 0
            // mapping (docs/TX_ARCHITECTURAL_MAPPING.md §10.3).
            // HL2Stream's TX-I/Q source + TX control callbacks are
            // left unregistered — the EP2 packer zero-fills TX I/Q
            // and FSM keydown/keyup become no-ops, leaving the wire
            // RX-only.  The RX path below (filterLow wiring +
            // hwPttEnabled forwarder) is untouched.

            // Task #53 — shared filterLow also drives the RX bandpass.
            // WdspEngine.setFilterLowHz triggers an internal
            // recomputePassband + applyModeFilter (live, no channel
            // restart).  Initial push: pull the persisted Prefs value
            // ONCE so the freshly-opened RX channel uses it instead of
            // WdspEngine::filterLow_'s create-time 100 Hz default.
            QObject::connect(prefs, &lyra::ui::Prefs::filterLowChanged,
                             wdspEngine, [wdspEngine, prefs]() {
                wdspEngine->setFilterLowHz(prefs->filterLow());
            });
            wdspEngine->setFilterLowHz(prefs->filterLow());

            // Task #36 — Hardware PTT input forwarder.  Mirror the
            // operator-facing Prefs.hwPttEnabled into HL2Stream's
            // gating atomic so the EP6 RX worker sees the live
            // intent without any stream restart.  Initial push:
            // honour the persisted opt-in (default false per §10
            // Q#1 phantom-TX safety).
            QObject::connect(prefs, &lyra::ui::Prefs::hwPttEnabledChanged,
                             stream, [stream, prefs]() {
                stream->setHwPttEnabled(prefs->hwPttEnabled());
            });
            stream->setHwPttEnabled(prefs->hwPttEnabled());
        }

        // Radio memory: auto-connect to the last radio so the operator
        // doesn't have to Discover every launch.  Independent of the
        // WDSP load above — the RX/wire path works regardless.  If the
        // radio is off/unreachable the UI just shows "RX stalled".
        const QString lastIp =
            QSettings().value(QStringLiteral("radio/lastIp")).toString();
        if (!lastIp.isEmpty()) {
            stream->open(lastIp);
        }
    });

    const int rc = app.exec();

    // Task #40 — signal the watchdog that teardown completed cleanly
    // BEFORE WSACleanup or any further work that could itself hang.
    // The watchdog thread is detached + sleeping; if shutdown_complete
    // is true when it wakes, it skips TerminateProcess and the OS
    // reclaims it normally on process exit.
    qWarning("[shutdown] app.exec() returned cleanly — clearing watchdog");
    g_shutdown_complete.store(true, std::memory_order_release);

#ifdef _WIN32
    qWarning("[shutdown] WSACleanup - start");
    ::WSACleanup();
    qWarning("[shutdown] WSACleanup - done");
#endif
    qWarning("[shutdown] main() returning rc=%d", rc);
    return rc;
}
