// Lyra-cpp — wdspcalls.cpp
//
// Pointer storage + resolver for the WDSP call table.  See
// wdspcalls.h for the operator-approved seam rationale + the
// per-entry signature citations.

#include "wire/wdspcalls.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <QtGlobal>   // qWarning

namespace lyra::wire {

// ---- pointer storage (all start null; resolve_wdsp_calls fills) ----
void (*OpenChannel)(int, int, int, int, int, int, int, int,
                    double, double, double, double, int) = nullptr;
void (*CloseChannel)(int) = nullptr;
int  (*SetChannelState)(int, int, int) = nullptr;
void (*SetType)(int, int) = nullptr;
void (*SetInputBuffsize)(int, int) = nullptr;
void (*SetDSPBuffsize)(int, int) = nullptr;
void (*SetInputSamplerate)(int, int) = nullptr;
void (*SetDSPSamplerate)(int, int) = nullptr;
void (*SetOutputSamplerate)(int, int) = nullptr;
void (*fexchange0)(int, double*, double*, int*) = nullptr;
void* (*create_resample)(int, int, double*, double*, int, int,
                         double, int, double) = nullptr;
void (*destroy_resample)(void*) = nullptr;
int  (*xresample)(void*) = nullptr;
void (*XCreateAnalyzer)(int, int*, int, int, int, char*) = nullptr;
void (*DestroyAnalyzer)(int) = nullptr;
void (*Spectrum0)(int, int, int, int, double*) = nullptr;
void (*pscc)(int, int, double*, double*) = nullptr;
void (*psccF)(int, int, float*, float*, float*, float*, int, int) = nullptr;
void (*SetPSRunCal)(int, int) = nullptr;
void (*SetPSMox)(int, int) = nullptr;
void (*GetPSInfo)(int, int*) = nullptr;
void (*SetPSReset)(int, int) = nullptr;
void (*SetPSMancal)(int, int) = nullptr;
void (*SetPSAutomode)(int, int) = nullptr;
void (*SetPSTurnon)(int, int) = nullptr;
void (*SetPSControl)(int, int, int, int, int) = nullptr;
void (*SetPSLoopDelay)(int, double) = nullptr;
void (*SetPSMoxDelay)(int, double) = nullptr;
void (*SetPSHWPeak)(int, double) = nullptr;
void (*GetPSHWPeak)(int, double*) = nullptr;
void (*GetPSMaxTX)(int, double*) = nullptr;
void (*SetPSPtol)(int, double) = nullptr;
void (*GetPSDisp)(int, double*, double*, double*, double*,
                  double*, double*, double*) = nullptr;
void (*SetPSFeedbackRate)(int, int) = nullptr;
void (*SetPSPinMode)(int, int) = nullptr;
void (*SetPSMapMode)(int, int) = nullptr;
void (*SetPSStabilize)(int, int) = nullptr;
void (*SetPSIntsAndSpi)(int, int, int) = nullptr;

// X-macro over every table entry: one line per symbol; the resolver
// below expands it.  Adding a symbol = one extern in the header, one
// storage definition above, one line here.
#define LYRA_WDSP_CALL_TABLE(X) \
    X(OpenChannel)          \
    X(CloseChannel)         \
    X(SetChannelState)      \
    X(SetType)              \
    X(SetInputBuffsize)     \
    X(SetDSPBuffsize)       \
    X(SetInputSamplerate)   \
    X(SetDSPSamplerate)     \
    X(SetOutputSamplerate)  \
    X(fexchange0)           \
    X(create_resample)      \
    X(destroy_resample)     \
    X(xresample)            \
    X(XCreateAnalyzer)      \
    X(DestroyAnalyzer)      \
    X(Spectrum0)            \
    X(pscc)                 \
    X(psccF)                \
    X(SetPSRunCal)          \
    X(SetPSMox)             \
    X(GetPSInfo)            \
    X(SetPSReset)           \
    X(SetPSMancal)          \
    X(SetPSAutomode)        \
    X(SetPSTurnon)          \
    X(SetPSControl)         \
    X(SetPSLoopDelay)       \
    X(SetPSMoxDelay)        \
    X(SetPSHWPeak)          \
    X(GetPSHWPeak)          \
    X(GetPSMaxTX)           \
    X(SetPSPtol)            \
    X(GetPSDisp)            \
    X(SetPSFeedbackRate)    \
    X(SetPSPinMode)         \
    X(SetPSMapMode)         \
    X(SetPSStabilize)       \
    X(SetPSIntsAndSpi)

int resolve_wdsp_calls()
{
    HMODULE h = GetModuleHandleW(L"wdsp.dll");
    if (h == nullptr) {
        qWarning("[wdspcalls] wdsp.dll is not loaded — "
                 "resolve_wdsp_calls() called before WdspNative::load()?");
        return -1;
    }

    int missing = 0;

#define LYRA_RESOLVE(name)                                                  \
    {                                                                       \
        FARPROC p = GetProcAddress(h, #name);                               \
        if (p == nullptr) {                                                 \
            qWarning("[wdspcalls] UNRESOLVED wdsp.dll export: %s", #name);  \
            ++missing;                                                      \
        }                                                                   \
        name = reinterpret_cast<decltype(name)>(                            \
            reinterpret_cast<void*>(p));                                    \
    }

    LYRA_WDSP_CALL_TABLE(LYRA_RESOLVE)

#undef LYRA_RESOLVE

    return missing;
}

}  // namespace lyra::wire
