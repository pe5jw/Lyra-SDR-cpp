// Standalone COLA-reconstruction gate for IqStft (captured-profile slice 1).
// Build (from repo root, in a vcvars64 shell):
//   cl /std:c++20 /EHsc /O2 /I src scratch\test_iqstft.cpp src\iqstft.cpp
//   .\test_iqstft.exe
// PASS = identity-gain WOLA reconstructs the input to < 1e-9 for every
// operator-selectable FFT size.

#include "iqstft.h"

#include <cstdio>

int main() {
    const int sizes[] = {2048, 4096, 8192};
    int fails = 0;
    for (int n : sizes) {
        const double err = lyra::dsp::IqStft::selfTestMaxError(n);
        const bool ok = err < 1e-9;
        std::printf("FFT %5d : max reconstruction error = %.3e  [%s]\n",
                    n, err, ok ? "PASS" : "FAIL");
        if (!ok) ++fails;
    }
    std::printf("%s\n", fails ? "*** FAIL ***" : "ALL PASS");
    return fails ? 1 : 0;
}
