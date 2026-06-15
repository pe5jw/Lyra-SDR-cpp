// #158 DL-0 smoke — prove the vendored PortAudio 19.7.0 static lib
// builds, links, and its header resolves, and that the runtime can
// Pa_Initialize / enumerate / Pa_Terminate on this machine.  No Lyra
// code, no Qt — just the PortAudio surface the IVAC device layer (DL-1
// CallbackIVAC / StartAudioIVAC) will use.  Built on demand:
//   cmake --build build --target test_portaudio
#include <portaudio.h>
#include <cstdio>

int main() {
    std::printf("PortAudio version: %s\n", Pa_GetVersionText());

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::printf("FAIL: Pa_Initialize -> %s\n", Pa_GetErrorText(err));
        return 1;
    }

    const int nDev = Pa_GetDeviceCount();
    const int nApi = Pa_GetHostApiCount();
    std::printf("Pa_Initialize OK: %d host APIs, %d devices\n", nApi, nDev);

    for (int i = 0; i < nApi; ++i) {
        const PaHostApiInfo *ha = Pa_GetHostApiInfo(i);
        if (ha)
            std::printf("  host API %d: %-16s (%d devices)\n",
                        i, ha->name, ha->deviceCount);
    }

    Pa_Terminate();
    std::printf("DL-0 smoke OK\n");
    return 0;
}
