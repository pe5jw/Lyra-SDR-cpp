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
#include "wire/ILV.h"       // P0.b direct port — reference ilv.h verbatim (pcm->xmtr[].pilv)
#include "wire/CMaster.h"   // create_cmaster() / destroy_cmaster() / pcm
#include "wire/cmsetup.h"   // P0.d direct port — reference cmsetup.h verbatim
#include "wire/wdspcalls.h" // P0.a — WDSP call-table resolver (the one approved linkage seam)

// P0.d — consumer-side declarations for the PORT-exported
// ChannelMaster functions main.cpp calls.  The reference exports
// these via dllexport with NO header declaration (the C# console
// declares them consumer-side via DllImport); Lyra consumers
// re-declare them the same way (scratch/test_ilv.cpp precedent for
// the ILV PORT setters).  Definitions: wire/cmsetup.cpp
// (SetRadioStructure / set_cmdefault_rates, cmsetup.c:29-86) and
// wire/CMaster.cpp (create_xmtr / destroy_xmtr, cmaster.c:112-271).
namespace lyra::wire {
void SetRadioStructure (int cmSTREAM, int cmRCVR, int cmXMTR,
	int cmSubRCVR, int cmNspc, int* cmSPC, int* cmMAXInbound,
	int cmMAXInRate, int cmMAXAudioRate, int cmMAXTxOutRate);
void set_cmdefault_rates (int* xcm_inrates, int aud_outrate,
	int* rcvr_ch_outrates, int* xmtr_ch_outrates);
void create_xmtr();
void destroy_xmtr();
void create_rnet();   // P3 — once-per-process, AFTER create_xmtr (see QTimer block)
}  // namespace lyra::wire

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

    // P0.d — radio-structure configuration, the reference's locked
    // pre-create contract: "set radio structure, call this first"
    // (SetRadioStructure, cmsetup.c:29) then "set default sample
    // rates, call this before 'create'" (set_cmdefault_rates,
    // cmsetup.c:58).  The reference's C# console drives these via
    // DllImport before CreateRadio(); Lyra's operating-point VALUES
    // are configuration (not ported code):
    //
    //   cmSTREAM   = 2  — stream 0 = RX1 (stream exists per the
    //                     reference layout; its pump idles because
    //                     Lyra's RX dispatch is router->WdspEngine,
    //                     the operator-approved hybrid — no Inbound()
    //                     producer feeds it), stream 1 = TX.
    //   cmRCVR     = 1, cmXMTR = 1, cmSubRCVR = 1, cmNspc = 0.
    //                     Derived ids then match the live channel
    //                     layout exactly: chid(0,0)=0 (WdspEngine RX1
    //                     channel), chid(1,0)=1 (TX channel),
    //                     inid(1,0)=1, txid(1)=0.
    //   cmMAXInbound  = 256/stream (max samples per Inbound() call —
    //                     comfortably above the EP6 mic batch).
    //   cmMAXInRate   = 384000 (HL2 max IQ rate).
    //   cmMAXAudioRate / cmMAXTxOutRate = 48000 (AK4951 codec rate /
    //                     fixed HL2 TX out rate, CLAUDE.md §3.5).
    //   xcm_inrates   = 48000 per stream (TX mic-in rate; stream 0
    //                     unused by the pump), audio out 48000,
    //                     rcvr/xmtr ch_outrates 48000 -> every
    //                     derived size = getbuffsize(48000) = 64.
    {
        int cmSPC[cmMAXspc]   = {0, 0};
        int cmMAXInbound[2]   = {256, 256};
        lyra::wire::SetRadioStructure(2, 1, 1, 1, 0, cmSPC,
                                      cmMAXInbound, 384000, 48000, 48000);
        int xcm_inrates[2]    = {48000, 48000};
        int rcvr_outrates[1]  = {48000};
        int xmtr_outrates[1]  = {48000};
        lyra::wire::set_cmdefault_rates(xcm_inrates, 48000,
                                        rcvr_outrates, xmtr_outrates);
    }

    // Reference cmaster.c:273-320 — `create_cmaster()` is the central
    // process-wide CMaster lifecycle init (P0.d verbatim port; every
    // deferred subsystem is enumerated inline in src/wire/CMaster.cpp
    // with the reference text carried in place).  The router create at
    // cmaster.c:316 is inside this call; the per-stream CMB rings +
    // pump threads + in[] buffers + update[] critical sections are
    // created here per the reference per-stream loop.  create_xmtr()
    // is the one DEFERRED-CALLSITE accommodation — it runs in the
    // QTimer block below AFTER wdsp->load() + resolve_wdsp_calls()
    // (its OpenChannel/XCreateAnalyzer calls need the resolved seam).
    //
    // Must be called BEFORE HL2Stream construction (which wires the
    // EP6 thread to router_instance(0) on open) AND BEFORE the
    // `register_sink(...)` call below.  Process-lifetime; the matching
    // `destroy_cmaster()` runs in the aboutToQuit handler-4 AFTER all
    // consumers (the HL2Stream itself) have been torn down — matches
    // the reference's destroy_cmaster reverse-order discipline.
    lyra::wire::create_cmaster();

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

    // Stage 2b2-fix-v3 — Step 3d: route DDC0 baseband IQ from the
    // EP6 thread into the DSP engine.  Registers directly on
    // router_instance(0) port=0/call_idx=0/ctrl_word=0 — the
    // reference's `LoadRouterAll(prouter[0], …)` posture
    // (router.c) flattened to the single-sink case Lyra needs.
    // Signature is the reference's (n_samples, iq_pairs) order;
    // we adapt to WdspEngine::feedIq(iq, n) inside the lambda.
    // Routes BEFORE HL2Stream::open() spawns the EP6 thread, so
    // the very first datagram dispatched lands in the sink.  No
    // HL2Stream shim involved — the previous `setIqSink(…)`
    // member was a Lyra-native arg-reorder wrapper; retiring it
    // matches the reference's "register at the router directly"
    // shape (Rule 26 cleanup).
    lyra::wire::register_sink(
        lyra::wire::router_instance(0),
        /*port=*/0, /*call_idx=*/0, /*ctrl_word=*/0,
        [wdspEngine](int n, const double* iq) {
            wdspEngine->feedIq(iq, n);
        });

    // Step 5: RX audio → HL2 onboard-codec (AK4951) jack is handled
    // inside WdspEngine now — dispatchAudioFrame → OutBound(0) → the
    // verbatim EP2 writer (sendProtocol1Samples).  The old
    // setHl2AudioSink/pushAudio EP2-ring path retired in §7.

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
    //   3. stream->close() -> joins the rx-loop thread + ep6Thread.
    //   4. micSource delete -> clears ep6Thread's mic sink AFTER
    //      ep6Thread has been joined (Stage 2b2c: the wire-side
    //      sink contract is F1 set-once-before-start, so the
    //      sink-clear at dtor must happen with ep6Thread NOT
    //      running; the captured `this` is also safe — no reader
    //      can be inside the lambda once close() returns).
    lyra::dsp::Hl2Ep6MicSource *micSource = nullptr;
    // P0.d (2026-06-12) — create_xmtr()/destroy_xmtr() gate flag at
    // outer scope for the aboutToQuit handler-1.5.  The reference
    // calls create_xmtr() inside create_cmaster (cmaster.c:288);
    // Lyra-cpp's DEFERRED-CALLSITE accommodation (documented in
    // wire/CMaster.cpp's preamble) invokes it from the QTimer block
    // below AFTER wdsp->load() + resolve_wdsp_calls() succeed — the
    // verbatim body's OpenChannel/XCreateAnalyzer calls resolve
    // through the wire/wdspcalls.h seam and need the loaded DLL.
    // The flag gates the matching destroy_xmtr() (handler-1.5) so a
    // failed/never-run create never tears down through unresolved
    // seam pointers.
    //
    // The former `TxChannel` RAII carve-out is GONE: P0.d's verbatim
    // create_xmtr opens the WDSP TXA channel itself (cmaster.c:
    // 177-190 — OpenChannel(chid(inid(1,0),0)=1, 64, 4096, 48000,
    // 96000, 48000, type=1, state=0, ...)) and allocates the
    // xmtr[i].out[0..2] buffers (cmaster.c:126-127).  Reference
    // posture preserved: at create-time the TX channel is OPEN but
    // NOT STARTED (state=0; the keydown FSM keys it later via
    // SetChannelState through the seam).
    //
    // No local ILV pointer either (P0.b): the ILV lives at
    // pcm->xmtr[0].pilv, created by create_xmtr (cmaster.c:226-232)
    // and torn down by destroy_xmtr (cmaster.c:258).  SendpOutboundTx
    // (P3) wires the raw Outbound fn ptr via SetILVOutputPointer(0,
    // ...) AFTER create_xmtr, per the reference's registration
    // ordering.
    bool xmtrCreated = false;

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
                     [stream]() {
        // TX-rip Phase 1 (Q2): teardown collapses to the RX-only
        // surface (HL2Stream TX callbacks unregister + Hl2Ep6MicSource
        // delete).  TxDspWorker / TciMicSource teardown returns with
        // the rebuild per docs/TX_ARCHITECTURAL_MAPPING.md §10.3.
        //
        // Stage 2b2c: `delete micSource` moved out of this handler
        // and into a dedicated handler between stream->close()
        // (handler-2) and destroy_router (handler-4) — see below.
        // The reference-faithful ep6Thread().set_mic_sink({}) at
        // ~Hl2Ep6MicSource requires ep6Thread NOT running, so the
        // dtor must run AFTER stream->close() has joined it.
        qWarning("[shutdown] handler-1 ENTRY");
        if (stream) {
            qWarning("[shutdown] handler-1 step a: registerTxIqSource({}) - start");
            stream->registerTxIqSource({});
            qWarning("[shutdown] handler-1 step a: done");
            qWarning("[shutdown] handler-1 step b: registerTxControl({}) - start");
            stream->registerTxControl({});
            qWarning("[shutdown] handler-1 step b: done");
        }
        qWarning("[shutdown] handler-1 EXIT");
    });

    // P0.d (2026-06-12) — handler-1.5: destroy_xmtr().
    //
    // Reference cmaster.c:255-271 `destroy_xmtr` — destroys the ILV
    // (destroy_ilv), the TX analyzer (DestroyAnalyzer), closes the
    // WDSP TXA channel (CloseChannel at cmaster.c:265, NO preceding
    // SetChannelState dmode=1 per the reference design; the blocking-
    // flush stop belongs at keyup not at destroy-time) and frees the
    // xmtr[i].out[0..2] buffers — the verbatim P0.d port in
    // wire/CMaster.cpp does all four (the dexp/eer/txgain/sidetone
    // lines stay DEFERRED with their subsystems).
    //
    // Lyra ordering: between handler-1 (clears stream->registerTx{
    // IqSource,Control}({}) -- no more callbacks fire after this point)
    // and handler-2 (stream->close -- joins EP6 thread).  This window
    // is when the xmtr is safe to tear down: no producer can fire
    // SendpOutboundTx (handler-1 cleared), no consumer can pull
    // TxIqSource (handler-1 cleared).  Running BEFORE handler-4's
    // destroy_cmaster() preserves the reference's relative teardown
    // order (destroy_xmtr before the per-stream cmbuffs teardown,
    // cmaster.c:322-337) — see the DEFERRED-CALLSITE note in
    // wire/CMaster.cpp.
    //
    // Gated on xmtrCreated: if wdsp->load() failed or the operator
    // closed before the QTimer fired, create_xmtr() never ran and
    // the verbatim teardown (no null guards, reference posture) must
    // not run against unresolved seam pointers / a never-opened
    // channel.
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     [&xmtrCreated]() {
        qWarning("[shutdown] handler-1.5 ENTRY (destroy_xmtr)");
        if (xmtrCreated) {
            lyra::wire::destroy_xmtr();
            xmtrCreated = false;
        }
        qWarning("[shutdown] handler-1.5 EXIT");
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

    // P4.b TUN display-honesty — feed the panadapter the TX-analyzer
    // NCO−dial offset so its TX-state crop renders the TUN carrier at its
    // true RF (the dial, on the SSB marker) instead of NCO-relative.  The
    // TX analyzer's DC sits at the TX NCO (dial ∓ cw_pitch during TUN)
    // while the panadapter axis is centred on the RX DDS (= dial); this
    // closes that gap.  0 in RX and voice TX.  Same-thread AutoConnection.
    QObject::connect(stream, &lyra::ipc::HL2Stream::txAnalyzerOffsetChanged,
                     wdspEngine, &lyra::dsp::WdspEngine::setTxAnalyzerOffsetHz);

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

    // Stage 2b2c — Hl2Ep6MicSource teardown.  Runs AFTER handler-2
    // (stream->close has joined ep6Thread), so the dtor's
    // `ep6Thread().set_mic_sink({})` lands under the F1 set-once-
    // before-start invariant (ep6Thread is NOT running here), and
    // the lambda's captured `this` is safe (no reader thread can
    // be inside it).  Qt connects in registration order and fires
    // aboutToQuit handlers in that same order.
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     [&micSource]() {
        qWarning("[shutdown] handler-3 ENTRY (delete micSource - ~Hl2Ep6MicSource runs now)");
        delete micSource; micSource = nullptr;
        qWarning("[shutdown] handler-3 EXIT");
    });

    // Reference cmaster.c:322-337 — `destroy_cmaster()` is the central
    // process-wide CMaster lifecycle teardown.  Reverse-order match to
    // create_cmaster (which destroy_router(0,0) at line 326 is part of).
    // Phase B 2026-06-09 replaced the bare `lyra::wire::destroy_router(0,0)`
    // that previously lived here as a stand-in.  MUST run AFTER handler-2
    // above (stream->close → joins the EP6 thread → guarantees no
    // in-flight xrouter() dispatch through &router_instance(0) at the
    // moment we free the Router slot).  Qt connects in registration
    // order and fires aboutToQuit handlers in that same order, so this
    // lambda runs after handler-2 and the Stage-2b2c micSource-delete
    // handler-3.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
        qWarning("[shutdown] handler-4 ENTRY (destroy_cmaster)");
        lyra::wire::destroy_cmaster();
        qWarning("[shutdown] handler-4 EXIT");
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
                                  &micSource, &xmtrCreated]() {
        if (wdsp->load()) {
            // P0.a (2026-06-09) — resolve the ChannelMaster-port WDSP
            // call table from the now-loaded wdsp.dll.  MUST run after
            // load() and BEFORE any ported ChannelMaster code calls
            // OpenChannel/fexchange0/xresample/pscc/etc through the
            // table.  Includes the full PureSignal entry-point family
            // (operator-committed feature; all exports verified present
            // in the bundled DLL via dumpbin 2026-06-09).
            {
                const int missing = lyra::wire::resolve_wdsp_calls();
                if (missing == 0) {
                    qInfo("[wdspcalls] ChannelMaster + PureSignal call "
                          "table fully resolved from wdsp.dll");
                } else {
                    qWarning("[wdspcalls] %d UNRESOLVED entry points — "
                             "ported ChannelMaster paths touching them "
                             "would crash; see preceding log lines",
                             missing);
                }
            }

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

            // P0.d (2026-06-12) — create_xmtr(), the verbatim
            // reference body (cmaster.c:112-253).
            //
            // Reference: create_cmaster() calls create_xmtr() inline
            // (cmaster.c:288, AFTER create_rcvr).  Lyra's DEFERRED-
            // CALLSITE accommodation (see wire/CMaster.cpp preamble)
            // invokes it HERE — after resolve_wdsp_calls() so the
            // verbatim body's OpenChannel / XCreateAnalyzer calls
            // resolve through the wire/wdspcalls.h seam, and after
            // wdspEngine->openRx1() (= the reference's create_rcvr
            // equivalent in the approved hybrid) so RX init precedes
            // TX init exactly as the reference orders it.
            //
            // The verbatim body per xmtr: malloc0's xmtr[i].out[0..2]
            // (cmaster.c:126-127), OpenChannel(chid(inid(1,0),0)=1,
            // 64, 4096, 48000, 96000, 48000, type=1, state=0,
            // 0.000, 0.010, 0.000, 0.010, block=1) (cmaster.c:
            // 177-190 — state=0 == OPEN-but-NOT-STARTED, the
            // reference's create-time posture; keydown keys it
            // later), XCreateAnalyzer(inid(1,0)=1, ...) (cmaster.c:
            // 192-198 — TX analyzer disp 1; WdspEngine's panadapter
            // analyzer is disp 0, no collision), create_ilv(0, 1,
            // ch_outsize=64, 2, 3, pcm->OutboundTx) (cmaster.c:
            // 226-232).  The dexp/anti-vox/txgain/eer/sidetone lines
            // stay DEFERRED with their subsystems (reference text
            // carried in place in wire/CMaster.cpp).
            //
            // pcm->OutboundTx is still null here — P3 wires
            // SendpOutboundTx -> SetILVOutputPointer AFTER this call,
            // per the reference's netInterface registration ordering
            // (the verbatim setters carry NO null guards).  Until P3
            // + an Inbound() producer land, the TX stream stays
            // wire-quiescent: nothing releases stream 1's
            // Sem_BuffReady, so xcmaster(1) never fires.
            lyra::wire::create_xmtr();
            xmtrCreated = true;
            qInfo("[tx] P0.d direct port: create_xmtr() — WDSP TXA "
                  "channel chid(1,0)=1 opened (64, 4096, 48000, "
                  "96000, 48000, type=1, state=0), TX analyzer "
                  "disp 1 created, xmtr[0].out[0..2] allocated, "
                  "pcm->xmtr[0].pilv=%p (run=0 bypass, obid=1, "
                  "insize=64, ninputs=2, what=3)",
                  static_cast<void*>(lyra::wire::pcm->xmtr[0].pilv));

            // P3 (2026-06-12) — create_rnet() ONCE per process, HERE,
            // matching the reference's init order (create_cmaster ->
            // create_xmtr -> create_rnet -> StartAudio).  The ordering
            // is LOAD-BEARING: create_rnet's tail registration
            // SendpOutboundTx(OutBound) derefs pcm->xmtr[0].pilv with
            // NO null guard (the verbatim SetILVOutputPointer), so it
            // must follow create_xmtr above.  Previously called from
            // HL2Stream::open() on EVERY stop/start — re-allocating
            // prn each cycle (leak + wire-state reset).  open() now
            // asserts the prn-non-null contract (netInterface.c:40)
            // instead; if wdsp.dll failed to load this block never
            // runs and open() refuses with a clear qCritical (no
            // WDSP = no radio, the reference posture).
            lyra::wire::create_rnet();
            qInfo("[wire] P3: create_rnet() — prn allocated, "
                  "SendpOutboundTx(OutBound) registered "
                  "(pcm->OutboundTx -> xmtr[0].pilv)");

            // TX-1 Path A: construct micSource NOW that the WDSP DLL
            // is loaded.  Mic-source construction order RELATIVE to
            // create_xmtr: micSource AFTER the xmtr because the
            // eventual Stage 7.5 mic-consumer-lambda (which calls
            // lyra::wire::Inbound(1, n, mic_iq) -> CMB ring ->
            // cm_main pump -> xcmaster(1) -> fexchange0 + xilv via
            // pcm->xmtr[0]) requires the TX channel + ILV to be live
            // when the first Inbound fires.
            //
            // Pointers are captured by reference from main()'s
            // outer scope so the aboutToQuit lambda (connected
            // earlier) sees them populated at teardown.
            micSource = new lyra::dsp::Hl2Ep6MicSource(*stream);

            // P4.b §4 — FSM re-home: re-wire the TxControl surface to
            // the WIRE-LIVE WDSP TXA channel chid(1,0).  The TX-rip
            // Phase 1 removal left txControl_ UNREGISTERED, so
            // create_xmtr opened the TXA channel but the FSM never armed
            // it (SetChannelState) and never pushed its mode / ALC /
            // bandpass — it sat at create-time defaults, incl. the ALC
            // max-gain trap that pins the TXA output chain to silence.
            // Result: keyed TX (voice AND tune) emitted nothing on the
            // wire (operator bench 2026-06-13).  The old provider drove
            // a now-deleted TxDspWorker/TxChannel (origin/main:src/
            // main.cpp:673); re-pointed here to the wdspcalls seam on
            // chid(1,0), the wire-live owner of the TXA channel.  This
            // is the Lyra-native FSM control seam; the SetChannelState /
            // SetTXA* calls + values are reference-faithful (the
            // reference's MOX handler calls them directly — console.cs:
            // 30345 SetChannelState(id(1,0),1,0) keydown / :30357
            // (id(1,0),0,1) keyup; SetTXA* at channel setup).
            {
                const int txch = lyra::wire::chid(1, 0);   // stream 1, subrx 0 = TXA channel (=1)

                // §15.23-class TX sideband fix (2026-06-13, operator
                // 9100 bench: USB transmitted LSB).  WDSP derives the SSB
                // sideband PURELY from the SIGN of the bandpass edges:
                // TXASetupBPFilters (TXA.c:840) does
                // CalcBandpassFilter(bp0, f_low, f_high) using f_low/f_high
                // AS-IS for every SSB mode — SetTXAMode does NOT re-sign.
                // So a mode change MUST re-push the sign-coded bandpass.
                // Faithful restore of the old TxChannel::pushBandpassLocked
                // (origin/main tx_channel.cpp:82-114): sign per the SSB
                // convention (USB = +low/+high positive baseband; LSB =
                // -high/-low negative baseband — the TX mirror of RX
                // §14.2), SetTXABandpassFreqs FIRST then SetTXAMode.  My
                // earlier wire-seam port wired setMode=SetTXAMode-only +
                // setBandpass=raw, dropping both — that was the bug.
                // Shared state so setMode + setBandpass re-push coherently.
                struct TxFilter { int mode = 0; double low = 200.0; double high = 3100.0; };
                auto txf = std::make_shared<TxFilter>();
                auto pushTxFilter = [txch, txf]() {
                    double lo, hi;
                    if (txf->mode == 1) { lo =  txf->low;  hi =  txf->high; }  // USB: positive baseband
                    else                { lo = -txf->high; hi = -txf->low;  }  // LSB: negative baseband
                    lyra::wire::SetTXABandpassFreqs(txch, lo, hi);  // sign-coded edges FIRST
                    lyra::wire::SetTXAMode(txch, txf->mode);        // then mode (re-runs TXASetupBPFilters)
                };

                stream->registerTxControl(lyra::ipc::HL2Stream::TxControl{
                    .start = [txch]() {
                        lyra::wire::SetChannelState(txch, 1, 0);   // arm — non-blocking cos² up-ramp
                    },
                    .stop = [txch]() {
                        lyra::wire::SetChannelState(txch, 0, 1);   // stop — BLOCKING down-ramp flush
                    },
                    .setInjectTxIq = [](bool) {
                        // Wire-live: the cm pump feeds OutBound(1) every
                        // cycle and the EP2 writer's `!XmitBit ⇒
                        // memset(outIQbufp)` gate decides zero-vs-pass —
                        // there is no separate inject branch (that was the
                        // retired legacy EP2 packer).  Channel arm/stop +
                        // XmitBit do the gating; this is now vestigial,
                        // but the FSM still calls it so keep it a no-op.
                    },
                    .setMode = [txf, pushTxFilter](int wdspMode) {
                        // §15.23 fix: re-push the SIGN-CODED bandpass for
                        // the new sideband, not just SetTXAMode (which
                        // alone can't flip the sideband — see pushTxFilter).
                        txf->mode = (wdspMode == 1) ? 1 : 0;        // 0 = LSB, 1 = USB
                        pushTxFilter();
                    },
                    .setMicGainDb = [txch](double db) {
                        lyra::wire::SetTXAPanelGain1(txch, std::pow(10.0, db / 20.0));  // dB → linear
                    },
                    .setAlcMaxGainLinear = [txch](double lin) {
                        lyra::wire::SetTXAALCMaxGain(txch, lin);    // un-trap the ALC ceiling
                    },
                    .setAlcDecayMs = [txch](int ms) {
                        lyra::wire::SetTXAALCDecay(txch, ms);
                    },
                    .setLevelerOn = [txch](bool on, double top) {
                        lyra::wire::SetTXALevelerSt(txch, on ? 1 : 0);
                        lyra::wire::SetTXALevelerTop(txch, top);
                    },
                    .setLevelerDecayMs = [txch](int ms) {
                        lyra::wire::SetTXALevelerDecay(txch, ms);
                    },
                    .setBandpass = [txf, pushTxFilter](double lo, double hi) {
                        // Operator passes POSITIVE magnitudes; pushTxFilter
                        // signs them per the current mode (§15.23).
                        txf->low  = lo;
                        txf->high = hi;
                        pushTxFilter();
                    },
                    .setTune = [txch, txf](bool on) {
                        // P4.b TUN — WDSP TXA output-side tone generator
                        // (postgen/gen1), the Thetis TUN mechanism
                        // (console.cs:30787-30801): single tone (mode 0)
                        // at ±cw_pitch, mag MAX_TONE_MAG.  Sign per mode
                        // (USB +cw_pitch, LSB -cw_pitch).  This pairs with
                        // the ∓cw_pitch TX-DDS offset (HL2Stream::
                        // txDdsHzForTune) — they CANCEL so the on-air
                        // carrier is zero-beat at the dial (a co-channel
                        // SSB listener hears nothing).  MUST use the same
                        // kTuneCwPitchHz as the DDS offset.  The MOX
                        // keydown (TUN button) arms the TXA channel that
                        // processes this tone.
                        if (on) {
                            constexpr double kMaxToneMag = 0.99999;   // Thetis MAX_TONE_MAG
                            const double pitch = static_cast<double>(
                                lyra::ipc::HL2Stream::kTuneCwPitchHz);
                            const double freq = (txf->mode == 1) ? pitch : -pitch;  // USB +, LSB −
                            lyra::wire::SetTXAPostGenMode(txch, 0);            // 0 = single tone
                            lyra::wire::SetTXAPostGenToneFreq(txch, freq);
                            lyra::wire::SetTXAPostGenToneMag(txch, kMaxToneMag);
                            lyra::wire::SetTXAPostGenRun(txch, 1);
                            qInfo("[tx] TUN postgen: run=1 mode=0(single) freq=%.0f mag=%.5f "
                                  "(txf->mode=%d USB=1; pairs with the TX-DDS offset → dial)",
                                  freq, kMaxToneMag, txf->mode);
                        } else {
                            lyra::wire::SetTXAPostGenRun(txch, 0);
                            qInfo("[tx] TUN postgen: run=0 (stopped)");
                        }
                    },
                });
                // registerTxControl() pushes the persisted mic gain / ALC
                // max-gain / ALC decay / leveler ONCE here, so the freshly
                // armed channel is off the create-time ALC trap.
                qInfo("[tx] P4.b §4: TxControl re-homed to WDSP TXA "
                      "chid(1,0)=%d (arm/stop + mode/mic/ALC/leveler/"
                      "bandpass via the wdspcalls seam)", txch);

                // USB-stuck-LSB fix (origin/main TX-1 8a-tx-mode): TX
                // tracks the operator's RX sideband.  Push the current
                // mode now (channel sits at create-time 0=LSB otherwise)
                // + on every RX-mode change.
                auto wdspTxModeFor = [](const QString &uiMode) -> int {
                    const QString m = uiMode.toUpper();
                    if (m == QStringLiteral("LSB")  || m == QStringLiteral("CWL") ||
                        m == QStringLiteral("DIGL") || m == QStringLiteral("DRMD"))
                        return 0;   // WDSP TXA LSB
                    return 1;       // WDSP TXA USB (USB/CWU/DIGU/AM/FM/DSB/SAM/...)
                };
                QObject::connect(wdspEngine,
                                 &lyra::dsp::WdspEngine::modeChanged,
                                 stream, [stream, wdspEngine, wdspTxModeFor]() {
                    stream->setTxMode(wdspTxModeFor(wdspEngine->mode()));
                });
                stream->setTxMode(wdspTxModeFor(wdspEngine->mode()));
            }

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
