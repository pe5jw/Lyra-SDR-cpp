// Lyra — WDSP DLL loader implementation (Step 3a).  See wdsp_native.h
// for the locked architecture + scope.

#include "wdsp_native.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QProcess>
#include <QProgressBar>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

namespace lyra::dsp {

namespace {

// Format a Windows error code into a human-readable message via
// FormatMessageW — mirrors the helper in hl2_stream.cpp.
QString winError(DWORD code) {
    wchar_t *buf = nullptr;
    const DWORD len = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM     |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    QString descr;
    if (len && buf) {
        descr = QString::fromWCharArray(buf, len).trimmed();
        ::LocalFree(buf);
    }
    return descr.isEmpty()
        ? QStringLiteral("Win32 error %1").arg(code)
        : QStringLiteral("Win32 error %1: %2").arg(code).arg(descr);
}

} // namespace

WdspNative::WdspNative(QObject *parent) : QObject(parent) {}

WdspNative::~WdspNative() {
    unload();
}

bool WdspNative::load() {
    if (handle_ != nullptr) {
        return true;  // already loaded — idempotent
    }

    // Resolve `<exe-dir>/_native/`.  applicationDirPath() returns
    // the canonical native path on Windows so this is safe with
    // spaces / non-ASCII operator usernames.
    const QString exeDir   = QCoreApplication::applicationDirPath();
    const QString nativeDir = QDir::cleanPath(exeDir +
                              QStringLiteral("/_native"));
    const QString dllPath   = QDir::cleanPath(nativeDir +
                              QStringLiteral("/wdsp.dll"));

    if (!QFileInfo::exists(dllPath)) {
        loadError_ = QStringLiteral("wdsp.dll not found at %1")
                     .arg(dllPath);
        emitLog(QStringLiteral("[wdsp] LOAD FAILED: %1")
                .arg(loadError_));
        emit loadedChanged();
        return false;
    }

    // Add _native/ to the dynamic-link search path so wdsp.dll's
    // dependent DLLs (libfftw3-3.dll etc.) resolve from the same
    // directory.  The Python tree uses `os.add_dll_directory`;
    // C++ equivalent is `AddDllDirectory` (Win10+/Win8+) — it
    // APPENDS to the search path rather than replacing it (vs the
    // older `SetDllDirectory` which clobbers + has subtle side
    // effects).  Need LOAD_LIBRARY_SEARCH_USER_DIRS in the
    // LoadLibraryExW call for AddDllDirectory's entries to take
    // effect.  Operator is on Windows 11 so this path is supported.
    const std::wstring nativeDirW = nativeDir.toStdWString();
    DLL_DIRECTORY_COOKIE cookie =
        ::AddDllDirectory(nativeDirW.c_str());
    if (!cookie) {
        // Non-fatal — LoadLibraryExW may still find it via the
        // default search.  Log the issue and continue.
        const DWORD err = ::GetLastError();
        emit logLine(QStringLiteral(
            "[wdsp] AddDllDirectory failed (continuing): %1")
            .arg(winError(err)));
    }

    const std::wstring dllPathW = dllPath.toStdWString();
    HMODULE h = ::LoadLibraryExW(
        dllPathW.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
        LOAD_LIBRARY_SEARCH_USER_DIRS    |
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR);

    if (cookie) {
        // We don't need the cookie to persist past LoadLibraryExW
        // — wdsp.dll's deps are resolved at this point.  Remove
        // to keep the search-path list clean.
        ::RemoveDllDirectory(cookie);
    }

    if (!h) {
        const DWORD err = ::GetLastError();
        loadError_ = winError(err);
        emitLog(QStringLiteral(
            "[wdsp] LOAD FAILED: LoadLibraryExW(%1): %2")
            .arg(dllPath, loadError_));
        emit loadedChanged();
        return false;
    }

    handle_     = static_cast<void*>(h);
    loadedFrom_ = dllPath;
    loadError_.clear();
    emitLog(QStringLiteral("[wdsp] LOADED: %1").arg(dllPath));

    // Step 3b: resolve the minimum WDSP entry points via
    // GetProcAddress.  On any miss we unload + return false so the
    // operator sees an explicit symbol-resolution failure rather
    // than a deferred crash at first use in Step 3c.
    if (!resolveSymbols()) {
        ::FreeLibrary(h);
        handle_ = nullptr;
        loadedFrom_.clear();
        emit loadedChanged();
        return false;
    }

    emit loadedChanged();
    return true;
}

void WdspNative::emitLog(const QString &line) {
    // Mirror every log line to both the QML log panel (via the
    // logLine signal) AND the host's stdout via qInfo() -- the
    // operator runs lyra.exe from a console specifically to
    // capture diagnostics, so a one-stop place to read every
    // status line is operator-friendly.  Cheap (one printf-like
    // call); no production concern.
    qInfo("%s", qPrintable(line));
    emit logLine(line);
}

bool WdspNative::resolveSymbols() {
    HMODULE mod = static_cast<HMODULE>(handle_);
    if (!mod) return false;

    QStringList missing;
    int found = 0;
    int total = 0;

    // Wrapper resolves one symbol into the strongly-typed function
    // pointer, increments the running tallies, and records the
    // name on miss.  reinterpret_cast through FARPROC is the
    // standard Win32 idiom (the MS docs explicitly bless this for
    // GetProcAddress assignment to function-pointer types).
    auto resolve = [&](auto &fnPtr, const char *name) {
        ++total;
        using FnT = std::remove_reference_t<decltype(fnPtr)>;
        FARPROC p = ::GetProcAddress(mod, name);
        if (p) {
            fnPtr = reinterpret_cast<FnT>(p);
            ++found;
        } else {
            fnPtr = nullptr;
            missing << QString::fromLatin1(name);
        }
    };

    resolve(api_.OpenChannel,         "OpenChannel");
    resolve(api_.CloseChannel,        "CloseChannel");
    resolve(api_.SetChannelState,     "SetChannelState");
    resolve(api_.fexchange0,          "fexchange0");
    resolve(api_.SetRXAMode,          "SetRXAMode");
    resolve(api_.RXASetPassband,      "RXASetPassband");
    resolve(api_.SetRXAAGCMode,       "SetRXAAGCMode");
    resolve(api_.SetRXAPanelBinaural, "SetRXAPanelBinaural");
    resolve(api_.SetRXAShiftFreq,     "SetRXAShiftFreq");   // #174 CTUNE
    resolve(api_.SetRXAShiftRun,      "SetRXAShiftRun");    // #174 CTUNE
    resolve(api_.RXANBPSetShiftFrequency, "RXANBPSetShiftFrequency");  // #174 CTUNE notch track
    resolve(api_.RXASetMP,            "RXASetMP");           // #159 RX filter type
    resolve(api_.TXASetMP,            "TXASetMP");           // #159 TX filter type
    resolve(api_.WDSPwisdom,          "WDSPwisdom");
    resolve(api_.SetRXAAGCThresh,     "SetRXAAGCThresh");
    resolve(api_.SetRXAAGCSlope,      "SetRXAAGCSlope");
    resolve(api_.SetRXAPanelGain1,    "SetRXAPanelGain1");
    resolve(api_.GetRXAMeter,         "GetRXAMeter");
    resolve(api_.SetRXAEMNRRun,        "SetRXAEMNRRun");
    resolve(api_.SetRXAEMNRgainMethod, "SetRXAEMNRgainMethod");
    resolve(api_.SetRXAEMNRnpeMethod,  "SetRXAEMNRnpeMethod");
    resolve(api_.SetRXAEMNRaeRun,      "SetRXAEMNRaeRun");
    resolve(api_.SetRXAEMNRPosition,   "SetRXAEMNRPosition");
    resolve(api_.SetRXAEMNRpost2Run,   "SetRXAEMNRpost2Run");
    resolve(api_.SetRXAAGCDecay,         "SetRXAAGCDecay");
    resolve(api_.SetRXAAGCHang,          "SetRXAAGCHang");
    resolve(api_.SetRXAAGCHangThreshold, "SetRXAAGCHangThreshold");
    resolve(api_.SetRXAAGCFixed,         "SetRXAAGCFixed");
    resolve(api_.SetRXAANFRun,         "SetRXAANFRun");
    resolve(api_.SetRXAANFVals,        "SetRXAANFVals");
    resolve(api_.SetRXAANRRun,         "SetRXAANRRun");
    resolve(api_.SetRXAANRVals,        "SetRXAANRVals");
    resolve(api_.RXANBPAddNotch,       "RXANBPAddNotch");
    resolve(api_.RXANBPEditNotch,      "RXANBPEditNotch");
    resolve(api_.RXANBPDeleteNotch,    "RXANBPDeleteNotch");
    resolve(api_.RXANBPGetNumNotches,  "RXANBPGetNumNotches");
    resolve(api_.RXANBPSetNotchesRun,  "RXANBPSetNotchesRun");
    resolve(api_.SetRXASSQLRun,        "SetRXASSQLRun");
    resolve(api_.SetRXASSQLThreshold,  "SetRXASSQLThreshold");
    resolve(api_.SetRXASSQLTauMute,    "SetRXASSQLTauMute");
    resolve(api_.SetRXASSQLTauUnMute,  "SetRXASSQLTauUnMute");
    resolve(api_.SetRXAFMSQRun,        "SetRXAFMSQRun");
    resolve(api_.SetRXAFMSQThreshold,  "SetRXAFMSQThreshold");
    resolve(api_.SetRXAAMSQRun,        "SetRXAAMSQRun");
    resolve(api_.SetRXAAMSQThreshold,  "SetRXAAMSQThreshold");
    resolve(api_.SetRXAAMSQMaxTail,    "SetRXAAMSQMaxTail");
    resolve(api_.create_nobEXT,        "create_nobEXT");
    resolve(api_.destroy_nobEXT,       "destroy_nobEXT");
    resolve(api_.xnobEXT,              "xnobEXT");
    resolve(api_.SetEXTNOBRun,         "SetEXTNOBRun");
    resolve(api_.SetEXTNOBThreshold,   "SetEXTNOBThreshold");
    resolve(api_.SetRXABiQuadRun,        "SetRXABiQuadRun");
    resolve(api_.SetRXABiQuadFreq,       "SetRXABiQuadFreq");
    resolve(api_.SetRXABiQuadBandwidth,  "SetRXABiQuadBandwidth");
    resolve(api_.SetRXABiQuadGain,       "SetRXABiQuadGain");
    resolve(api_.XCreateAnalyzer,        "XCreateAnalyzer");
    resolve(api_.DestroyAnalyzer,        "DestroyAnalyzer");
    resolve(api_.SetAnalyzer,            "SetAnalyzer");
    resolve(api_.Spectrum0,              "Spectrum0");
    resolve(api_.GetPixels,              "GetPixels");
    resolve(api_.SetDisplayDetectorMode, "SetDisplayDetectorMode");
    resolve(api_.SetDisplayAverageMode,  "SetDisplayAverageMode");
    resolve(api_.SetDisplayNumAverage,   "SetDisplayNumAverage");
    resolve(api_.SetDisplayAvBackmult,   "SetDisplayAvBackmult");
    // TX-1 (design v2 §5.2): WDSP TXA surface.  See typedef block
    // header for the deliberately-omitted symbols (SetTXABandpassRun
    // §15.23 trap, SetTXAPanelSelect, SetTXAALCThresh-doesn't-exist).
    resolve(api_.SetTXAMode,           "SetTXAMode");
    resolve(api_.SetTXABandpassFreqs,  "SetTXABandpassFreqs");
    resolve(api_.SetTXAPHROTRun,       "SetTXAPHROTRun");
    resolve(api_.SetTXAPHROTCorner,    "SetTXAPHROTCorner");
    resolve(api_.SetTXAPHROTNstages,   "SetTXAPHROTNstages");
    resolve(api_.SetTXAALCAttack,      "SetTXAALCAttack");
    resolve(api_.SetTXAALCDecay,       "SetTXAALCDecay");
    resolve(api_.SetTXAALCHang,        "SetTXAALCHang");
    resolve(api_.SetTXAALCMaxGain,     "SetTXAALCMaxGain");
    resolve(api_.SetTXAALCSt,          "SetTXAALCSt");
    resolve(api_.SetTXALevelerAttack,  "SetTXALevelerAttack");
    resolve(api_.SetTXALevelerDecay,   "SetTXALevelerDecay");
    resolve(api_.SetTXALevelerHang,    "SetTXALevelerHang");
    resolve(api_.SetTXALevelerTop,     "SetTXALevelerTop");
    resolve(api_.SetTXALevelerSt,      "SetTXALevelerSt");
    resolve(api_.SetTXAPanelGain1,     "SetTXAPanelGain1");
    resolve(api_.GetTXAMeter,          "GetTXAMeter");
    resolve(api_.TXAGetaSipF1,         "TXAGetaSipF1");   // reserved
    resolve(api_.TXASetSipMode,        "TXASetSipMode");  // Task #44 Phase 2 (reference mechanism)
    resolve(api_.TXASetSipDisplay,     "TXASetSipDisplay");
    resolve(api_.SetDisplaySampleRate, "SetDisplaySampleRate");  // Task #44 Phase 2 (rate setter)
    resolve(api_.create_resampleFV,    "create_resampleFV");
    resolve(api_.xresampleFV,          "xresampleFV");
    resolve(api_.destroy_resampleFV,   "destroy_resampleFV");
    // Stage B (Thetis ChannelMaster port — AAMix) — complex-double
    // resampler.  Missing-symbol failure here is the Stage B.0 gate
    // signal: aamix per-input resampling cannot ship if the bundled
    // wdsp.dll does not export these.  (Source-verified
    // __declspec(dllexport) at wdsp/resample.h:60-70, so this should
    // resolve clean — but the linker is the bench, not the source.)
    resolve(api_.create_resample,      "create_resample");
    resolve(api_.destroy_resample,     "destroy_resample");
    resolve(api_.flush_resample,       "flush_resample");
    resolve(api_.xresample,            "xresample");

    if (!missing.isEmpty()) {
        loadError_ = QStringLiteral(
            "symbols resolved %1/%2 -- MISSING: %3")
            .arg(found).arg(total).arg(missing.join(QStringLiteral(", ")));
        emitLog(QStringLiteral("[wdsp] %1").arg(loadError_));
        return false;
    }

    emitLog(QStringLiteral("[wdsp] symbols: %1/%2 resolved")
            .arg(found).arg(total));
    return true;
}

void WdspNative::unload() {
    if (handle_ == nullptr) return;
    ::FreeLibrary(static_cast<HMODULE>(handle_));
    handle_ = nullptr;
    loadedFrom_.clear();
    loadError_.clear();
    emit loadedChanged();
}

// ---------------------------------------------------------------
// Step 3c-i: FFTW WISDOM plumbing.
// ---------------------------------------------------------------

QString WdspNative::wisdomDir() {
    // Lyra-C++-PRIVATE directory.  Qt's GenericDataLocation
    // resolves to %APPDATA%\ on Windows; we then carve out our
    // own "N8SDR/Lyra-cpp/fftw/" subdir so we share with NEITHER
    // Python Lyra (which uses .../N8SDR/Lyra/fftw/) NOR any other
    // HPSDR app.  Isolation-by-directory per CLAUDE.md §15.26
    // (the wisdom-file format isn't versioned; different WDSP
    // builds can produce mutually-incompatible cached plans, so
    // cross-app sharing is a hidden footgun we explicitly avoid).
    const QString base = QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation);
    return QDir::cleanPath(base +
        QStringLiteral("/N8SDR/Lyra-cpp/fftw"));
}

namespace {

// Hard-coded filename per WDSP source (wisdom.c) — same name
// every HPSDR app produces; isolation is purely by directory.
constexpr const char *kWisdomFilename = "wdspWisdom00";

bool wisdomFileExists(const QString &dir) {
    return QFileInfo::exists(QDir(dir).filePath(
        QString::fromLatin1(kWisdomFilename)));
}

}  // namespace

QString WdspNative::wisdomFilePath() {
    return QDir(wisdomDir()).filePath(QString::fromLatin1(kWisdomFilename));
}

bool WdspNative::wisdomExists() {
    return wisdomFileExists(wisdomDir());
}

bool WdspNative::deleteWisdom() {
    const QString path = wisdomFilePath();
    if (!QFileInfo::exists(path))
        return true;                 // already gone — next launch rebuilds
    return QFile::remove(path);
}

int WdspNative::runWisdomBuilderEntryPoint(const QString &targetDir) {
    // Subprocess entry point.  Parent process spawned us with:
    //
    //   lyra.exe --build-wisdom <targetDir>
    //
    // We need wdsp.dll loaded + the WDSPwisdom symbol resolved
    // before we can call it.  Reuse our normal load() path — it
    // walks the same _native/ folder + resolves all 9 symbols.
    // On success we call WDSPwisdom(<targetDir>) which does the
    // multi-minute FFTW_PATIENT search inside WDSP, writes
    // wdspWisdom00 in the target dir, then returns.
    //
    // AllocConsole + FreeConsole inside WDSPwisdom won't bite us
    // here — we redirect stdio to nullDevice() in the parent's
    // QProcess setup, and we don't read any output from this
    // process.  Stdout corruption is irrelevant.
    if (!load()) {
        return 1;   // DLL couldn't be loaded
    }
    if (!api_.WDSPwisdom) {
        return 1;   // unexpected — load() already validated this
    }
    QDir().mkpath(targetDir);
    // WDSP appends "wdspWisdom00" to the dir string with strcat,
    // so the dir argument MUST end in a path separator.  Use
    // native separators + ANSI codepage for filesystem APIs.
    QString dirArg = targetDir;
    if (!dirArg.endsWith(QLatin1Char('/')) &&
        !dirArg.endsWith(QLatin1Char('\\'))) {
        dirArg += QLatin1Char('/');
    }
    QByteArray dirBytes =
        QDir::toNativeSeparators(dirArg).toLocal8Bit();
    api_.WDSPwisdom(dirBytes.data());
    return 0;
}

bool WdspNative::ensureWisdom() {
    if (!isLoaded()) {
        emitLog(QStringLiteral(
            "[wdsp] wisdom: cannot ensure — DLL not loaded"));
        return false;
    }
    if (!api_.WDSPwisdom) {
        emitLog(QStringLiteral(
            "[wdsp] wisdom: cannot ensure — WDSPwisdom symbol "
            "not resolved"));
        return false;
    }

    const QString dir = wisdomDir();
    QDir().mkpath(dir);

    // ---- Fast path: cache exists, import in-process. ----
    // We're a console build right now (CMakeLists.txt keeps
    // WIN32_EXECUTABLE OFF for the diagnostic build), and an
    // import-only WDSPwisdom call is sub-100ms anyway, so any
    // AllocConsole stdout hijack is brief + harmless.  When we
    // flip to a --windowed binary, we'll route this through the
    // subprocess too.  For now keep it in-process for simplicity
    // + bench-visibility.
    if (wisdomFileExists(dir)) {
        emitLog(QStringLiteral(
            "[wdsp] wisdom: loading cached plans from %1").arg(dir));
        QElapsedTimer t; t.start();
        QString dirArg = dir;
        if (!dirArg.endsWith(QLatin1Char('/'))) {
            dirArg += QLatin1Char('/');
        }
        QByteArray dirBytes = QDir::toNativeSeparators(dirArg)
                              .toLocal8Bit();
        api_.WDSPwisdom(dirBytes.data());
        emitLog(QStringLiteral(
            "[wdsp] wisdom: loaded in %1 ms").arg(t.elapsed()));
        return true;
    }

    // ---- Slow path: no cache, spawn the subprocess builder. ----
    emitLog(QStringLiteral(
        "[wdsp] wisdom: building (one-time, may take several "
        "minutes; a 'please wait' notice shows and Lyra stays "
        "responsive)"));
    emitLog(QStringLiteral(
        "[wdsp] wisdom: target dir = %1").arg(dir));

    const QString exe = QCoreApplication::applicationFilePath();
    QProcess builder;
    builder.setStandardOutputFile(QProcess::nullDevice());
    builder.setStandardErrorFile(QProcess::nullDevice());
    QStringList args;
    args << QStringLiteral("--build-wisdom") << dir;

    QElapsedTimer t; t.start();
    builder.start(exe, args);
    if (!builder.waitForStarted(5000)) {
        emitLog(QStringLiteral(
            "[wdsp] wisdom: BUILD FAILED — could not spawn "
            "subprocess: %1").arg(builder.errorString()));
        return false;
    }
    // Wait for the child WITHOUT blocking the Qt main thread.  A blocking
    // waitForFinished() here freezes the whole UI ("Not Responding") for the
    // multi-minute FFTW_PATIENT run — Windows then offers to kill Lyra, and an
    // operator seeing a hung window + a stray console box tends to force-close
    // it, which aborts the build before the wisdom file is written, so it
    // rebuilds on EVERY launch.  Instead: show a modal "please wait" notice
    // and pump a local event loop (the idiomatic Qt async-QProcess pattern) so
    // the app stays responsive and the operator gets clear guidance.  30-min
    // hard cap for very slow CPUs (a stuck child still can't deadlock launch).
    constexpr int kBuildTimeoutMs = 30 * 60 * 1000;

    QDialog *dlg = nullptr;
    if (qobject_cast<QApplication *>(QCoreApplication::instance())) {
        dlg = new QDialog(nullptr, Qt::Dialog | Qt::CustomizeWindowHint |
                                       Qt::WindowTitleHint);
        dlg->setWindowTitle(
            QCoreApplication::translate("wdsp", "Lyra — one-time setup"));
        dlg->setModal(true);
        auto *v = new QVBoxLayout(dlg);
        auto *note = new QLabel(QCoreApplication::translate("wdsp",
            "<b>Optimizing FFT plans — one-time setup.</b><br><br>"
            "This runs only once and can take several minutes — up to about "
            "10 on some PCs.  Please let it finish, and avoid launching other "
            "heavy programs while it computes.<br><br>"
            "A small console window may appear; that is normal.  Lyra opens "
            "automatically when it is done — <b>do not close anything.</b>"));
        note->setWordWrap(true);
        v->addWidget(note);
        auto *bar = new QProgressBar(dlg);
        bar->setRange(0, 0);   // indeterminate "busy" animation
        v->addWidget(bar);
        dlg->setMinimumWidth(460);
        dlg->show();
    }

    QEventLoop loop;
    bool finishedOk = false;
    QObject::connect(
        &builder,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        &loop, [&](int code, QProcess::ExitStatus st) {
            finishedOk = (st == QProcess::NormalExit && code == 0);
            loop.quit();
        });
    QTimer capTimer;
    capTimer.setSingleShot(true);
    QObject::connect(&capTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    capTimer.start(kBuildTimeoutMs);
    loop.exec();   // stays responsive; quits on child-finished or the cap

    if (dlg) {
        dlg->close();
        dlg->deleteLater();
    }

    if (builder.state() != QProcess::NotRunning) {
        builder.kill();
        builder.waitForFinished(2000);
        emitLog(QStringLiteral(
            "[wdsp] wisdom: BUILD TIMEOUT after %1 min")
            .arg(kBuildTimeoutMs / 60000));
        return false;
    }
    if (!finishedOk) {
        emitLog(QStringLiteral(
            "[wdsp] wisdom: BUILD FAILED — subprocess exit %1")
            .arg(builder.exitCode()));
        return false;
    }
    if (!wisdomFileExists(dir)) {
        emitLog(QStringLiteral(
            "[wdsp] wisdom: BUILD FAILED — subprocess succeeded "
            "but no wisdom file appeared at %1").arg(dir));
        return false;
    }
    emitLog(QStringLiteral(
        "[wdsp] wisdom: built in %1 s").arg(t.elapsed() / 1000));

    // Now do the in-process import of the freshly-built cache so
    // the rest of this process sees the plans.
    QString dirArg = dir;
    if (!dirArg.endsWith(QLatin1Char('/'))) {
        dirArg += QLatin1Char('/');
    }
    QByteArray dirBytes =
        QDir::toNativeSeparators(dirArg).toLocal8Bit();
    api_.WDSPwisdom(dirBytes.data());
    emitLog(QStringLiteral("[wdsp] wisdom: loaded from %1").arg(dir));
    return true;
}

} // namespace lyra::dsp
