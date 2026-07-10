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

#include <atomic>
#include <cstdio>
#include <thread>

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
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
#include <QTemporaryDir>
#include <QTextStream>
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

void WdspNative::logWisdom(const QString &line) {
    // qWarning (NOT qInfo) so the in-app LogBuffer keeps the line even
    // when the operator has NOT enabled verbose logging -- a field
    // report of "rebuilds every launch" needs these with zero setup.
    qWarning("%s", qPrintable(line));
    emit logLine(line);

    // Also mirror to a dedicated wisdom.log right next to the cache dir,
    // so the one file a user sends is self-contained.  Appended across
    // launches (each line timestamped) so the every-launch pattern is
    // visible in one file.  Best-effort: never throws, never blocks the
    // caller on failure.  Capped so it can't grow unbounded.
    const QString dir  = wisdomDir();
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(QStringLiteral("wisdom.log"));
    QFile f(path);
    const QIODevice::OpenMode mode =
        (QFileInfo(path).size() > 256 * 1024)
            ? (QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)
            : (QIODevice::WriteOnly | QIODevice::Append   | QIODevice::Text);
    if (f.open(mode)) {
        QTextStream ts(&f);
        ts << QDateTime::currentDateTime().toString(Qt::ISODate)
           << QStringLiteral("  ") << line << '\n';
    }
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

int WdspNative::runWisdomCall(const QString &callDir,
                              bool showModalImmediately) {
    // Call api_.WDSPwisdom(<callDir>/) on a BACKGROUND thread while the
    // GUI stays responsive.  WDSPwisdom imports an existing cache in
    // <100 ms (returns 0), or -- when the file is missing or REJECTED by
    // FFTW -- runs a multi-minute FFTW_PATIENT search + writes the file
    // (returns 1).  On that rebuild branch WDSP internally does
    // AllocConsole() + freopen(CONOUT$) + FreeConsole(); on our windowed
    // (WIN32_EXECUTABLE) build that would pop a visible console window
    // and leave CRT stdout dangling.  We tame it: pre-own a HIDDEN
    // console so WDSP's AllocConsole no-ops and its output goes nowhere
    // visible, then restore stdout/stderr afterward.  This is the
    // Thetis in-process model (radio.cs:135 calls WDSPwisdom directly);
    // building on a worker + a modal-if-slow keeps the UI responsive,
    // which the reference does not, and -- crucially -- there is NO
    // self-spawned child (the old `lyra.exe --build-wisdom` pattern trips
    // antivirus SONAR: an unsigned exe launching a copy of itself).
    //
    // Returns the WDSPwisdom return code (0=import, 1=rebuilt).
    QString dirArg = callDir;
    if (!dirArg.endsWith(QLatin1Char('/')) &&
        !dirArg.endsWith(QLatin1Char('\\'))) {
        dirArg += QLatin1Char('/');
    }
    const QByteArray dirBytes =
        QDir::toNativeSeparators(dirArg).toLocal8Bit();

    std::atomic<int>  rc{0};
    std::atomic<bool> done{false};
    std::thread worker([this, dirBytes, &rc, &done]() {
        // Tame WDSP's console ONLY if we don't already own one (a dev
        // console build must keep its own console).  Import-only calls
        // never touch the console, so this whole block is a no-op on the
        // fast path -- it bites only when WDSP actually rebuilds.
        const bool weOwnConsole = (::GetConsoleWindow() == nullptr);
        if (weOwnConsole && ::AllocConsole()) {
            if (HWND cw = ::GetConsoleWindow())
                ::ShowWindow(cw, SW_HIDE);
        }
        rc.store(api_.WDSPwisdom(
            const_cast<char *>(dirBytes.constData())));
        if (weOwnConsole) {
            // WDSP already called FreeConsole on its rebuild branch;
            // make sure nothing lingers and restore stdout/stderr (WDSP's
            // freopen(CONOUT$) left them on a now-dead console handle).
            // Lyra logs via Qt, not CRT stdout, so this is defensive but
            // keeps the CRT sane for anything that might touch it.
            if (::GetConsoleWindow()) ::FreeConsole();
            FILE *f = nullptr;
            ::freopen_s(&f, "NUL", "w", stdout);
            ::freopen_s(&f, "NUL", "w", stderr);
        }
        done.store(true);
    });

    // Main thread: pump events; show a modal notice ONLY if the call
    // runs longer than the threshold (a rebuild), never for a fast
    // import.  No hard timeout -- an in-process worker can't be safely
    // killed, and a genuine FFTW hang would wedge either way; the UI
    // stays responsive so the operator can always quit.
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(50);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (done.load()) loop.quit();
    });
    poll.start();

    QDialog *dlg = nullptr;
    auto showDialog = [&]() {
        if (done.load() || dlg) return;
        if (!qobject_cast<QApplication *>(QCoreApplication::instance()))
            return;
        dlg = new QDialog(nullptr, Qt::Dialog | Qt::CustomizeWindowHint |
                                       Qt::WindowTitleHint);
        dlg->setWindowTitle(
            QCoreApplication::translate("wdsp", "Lyra — one-time setup"));
        dlg->setModal(true);
        auto *v = new QVBoxLayout(dlg);
        auto *note = new QLabel(QCoreApplication::translate("wdsp",
            "<b>Optimizing FFT plans — one-time setup.</b><br><br>"
            "This runs only once and can take several minutes — up to "
            "about 10 on some PCs.  Please let it finish, and avoid "
            "launching other heavy programs while it computes.<br><br>"
            "Lyra is <b>not ready yet</b> — it opens automatically when "
            "this is done.  <b>Do not close anything.</b>"));
        note->setWordWrap(true);
        v->addWidget(note);
        auto *bar = new QProgressBar(dlg);
        bar->setRange(0, 0);   // indeterminate "busy" animation
        v->addWidget(bar);
        dlg->setMinimumWidth(460);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    };

    QTimer modalTimer;
    modalTimer.setSingleShot(true);
    QObject::connect(&modalTimer, &QTimer::timeout, &loop,
                     [&]() { showDialog(); });
    if (showModalImmediately) {
        // Known build: the caller has deferred the main window, so show
        // the notice at once -- there is no ready-looking Lyra to sit
        // behind, and no gap where the screen is blank.
        showDialog();
    } else {
        modalTimer.start(500);   // import: notice only if it unexpectedly rebuilds
    }

    loop.exec();             // responsive; quits when the worker finishes

    if (dlg) { dlg->close(); dlg->deleteLater(); }
    worker.join();
    return rc.load();
}

bool WdspNative::ensureWisdom() {
    if (!isLoaded()) {
        logWisdom(QStringLiteral(
            "[wdsp] wisdom: cannot ensure — DLL not loaded"));
        return false;
    }
    if (!api_.WDSPwisdom) {
        logWisdom(QStringLiteral(
            "[wdsp] wisdom: cannot ensure — WDSPwisdom symbol "
            "not resolved"));
        return false;
    }

    const QString dir = wisdomDir();
    QDir().mkpath(dir);

    // ---- Fast path: cache exists -> import in place. ----
    // Runs through runWisdomCall (worker thread + console taming) so
    // that even if the cached file is REJECTED by FFTW and WDSP silently
    // re-plans, it neither pops a console window nor freezes the GUI
    // thread; the 'please wait' notice appears only if that rebuild
    // actually happens (a clean import is sub-100 ms).
    if (wisdomFileExists(dir)) {
        logWisdom(QStringLiteral(
            "[wdsp] wisdom: loading cached plans from %1").arg(dir));
        QElapsedTimer t; t.start();
        const int rc = runWisdomCall(dir);
        if (rc == 1) {
            // WDSPwisdom returns 1 ONLY when it rebuilt the plans --
            // i.e. the cached file was present but REJECTED by FFTW
            // (stale after a wdsp.dll/FFTW bump, wrong-CPU, or corrupt)
            // and silently re-planned IN-PROCESS.  Should be rare (the
            // atomic publish means a truncated file can't linger); log it
            // so a "rebuilds every launch" report on an existing file is
            // unambiguous rather than looking like a normal load.  WDSP
            // re-exports the file itself on this branch, so it self-heals.
            logWisdom(QStringLiteral(
                "[wdsp] wisdom: cached file at %1 was REJECTED by FFTW "
                "and re-planned in-process in %2 ms -- the cache is "
                "stale/incompatible").arg(dir).arg(t.elapsed()));
        } else {
            logWisdom(QStringLiteral(
                "[wdsp] wisdom: loaded cached plans in %1 ms (rc=%2)")
                .arg(t.elapsed()).arg(rc));
        }
        return true;
    }

    // ---- Slow path: no cache -> build in-process, then publish. ----
    logWisdom(QStringLiteral(
        "[wdsp] wisdom: building (one-time, may take several "
        "minutes; a 'please wait' notice shows and Lyra stays "
        "responsive)"));
    logWisdom(QStringLiteral(
        "[wdsp] wisdom: target dir = %1").arg(dir));

    // Build into a private temp subdir on the SAME volume as the final
    // cache, then atomically rename into place below -- so a killed or
    // blocked build can never leave a truncated wdspWisdom00 that a later
    // launch would feed to WDSP (which would silently drop into the
    // in-process FFTW_PATIENT rebuild branch and freeze the GUI).
    // "Present ⇒ valid."
    QTemporaryDir buildTmp(
        QDir(dir).filePath(QStringLiteral(".build-XXXXXX")));
    buildTmp.setAutoRemove(true);
    if (!buildTmp.isValid()) {
        logWisdom(QStringLiteral(
            "[wdsp] wisdom: BUILD FAILED — could not create temp build "
            "dir under %1: %2").arg(dir, buildTmp.errorString()));
        return false;
    }
    const QString buildDir = buildTmp.path();

    QElapsedTimer t; t.start();
    // Build the plans IN-PROCESS on a worker thread (no self-spawned
    // child -- that pattern trips antivirus SONAR: an unsigned exe
    // launching a copy of itself).  WDSPwisdom writes wdspWisdom00 into
    // buildDir AND leaves the freshly-planned wisdom live in FFTW's
    // process-global state, so once this returns the running process is
    // ready -- NO separate re-import needed (unlike the old subprocess
    // path, where the build happened in a child).  runWisdomCall keeps
    // the GUI responsive + tames WDSP's AllocConsole.  Show the modal
    // immediately: the caller (main.cpp) has deferred the main window for
    // this build, so there is nothing to sit behind and no blank gap.
    const int rc = runWisdomCall(buildDir, /*showModalImmediately=*/true);

    // Verify the built file exists + is non-empty BEFORE publishing.
    const QString builtFile =
        QDir(buildDir).filePath(QString::fromLatin1(kWisdomFilename));
    const QFileInfo builtInfo(builtFile);
    if (!builtInfo.exists() || builtInfo.size() == 0) {
        logWisdom(QStringLiteral(
            "[wdsp] wisdom: BUILD FAILED — no usable wisdom file in the "
            "temp build dir %1 after WDSPwisdom (rc=%2) -- write blocked / "
            "permissions?").arg(buildDir).arg(rc));
        return false;
    }
    // Atomic publish for the NEXT launch's fast import: rename temp ->
    // final (same volume).  MOVEFILE_WRITE_THROUGH flushes to disk before
    // returning; REPLACE_EXISTING overwrites a stale cache.  On failure,
    // log the EXACT Win32 error (e.g. 'Access is denied' = a folder
    // shield / antivirus write block).
    const QString finalFile = wisdomFilePath();
    const std::wstring srcW =
        QDir::toNativeSeparators(builtFile).toStdWString();
    const std::wstring dstW =
        QDir::toNativeSeparators(finalFile).toStdWString();
    if (!::MoveFileExW(srcW.c_str(), dstW.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD err = ::GetLastError();
        logWisdom(QStringLiteral(
            "[wdsp] wisdom: BUILD FAILED — could not publish %1 -> %2: %3")
            .arg(builtFile, finalFile, winError(err)));
        return false;
    }
    logWisdom(QStringLiteral(
        "[wdsp] wisdom: built in %1 s (rc=%2) and published %3 bytes "
        "to %4").arg(t.elapsed() / 1000).arg(rc)
        .arg(builtInfo.size()).arg(finalFile));
    return true;
}

} // namespace lyra::dsp
