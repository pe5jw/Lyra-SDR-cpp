// Unit test for lyra::dsp::CwDecoder (#173 CW-5a).  Qt-free, pure C++.
// Build + run:  cmake --build build --target test_cw_decoder
//               build/test_cw_decoder.exe
//
// Synthesises clean (lightly-noised) on/off-keyed CW at a known WPM/tone,
// feeds it through the decoder in blocks, and asserts the decoded text.
// Also exercises reset() and AFC lock to an off-nominal tone.

#include "dsp/CwDecoder.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using lyra::dsp::CwDecoder;

namespace {
constexpr double kPi = 3.14159265358979323846;
int g_fail = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

const char* morseFor(char c) {
    switch (c) {
        case 'A': return ".-";   case 'B': return "-...";  case 'C': return "-.-.";
        case 'D': return "-..";  case 'E': return ".";     case 'F': return "..-.";
        case 'G': return "--.";  case 'H': return "....";  case 'I': return "..";
        case 'J': return ".---"; case 'K': return "-.-";   case 'L': return ".-..";
        case 'M': return "--";   case 'N': return "-.";    case 'O': return "---";
        case 'P': return ".--."; case 'Q': return "--.-";  case 'R': return ".-.";
        case 'S': return "...";  case 'T': return "-";     case 'U': return "..-";
        case 'V': return "...-"; case 'W': return ".--";   case 'X': return "-..-";
        case 'Y': return "-.--"; case 'Z': return "--..";  case '1': return ".----";
        case '0': return "-----";
        default:  return "";
    }
}

// Build on/off-keyed CW audio for `text` (spaces = word gaps).  Standard
// spacing: element gap 1 unit, char gap 3 units, word gap 7 units.
std::vector<float> synth(const std::string& text, double wpm, double tone,
                         double sr, double noiseStd) {
    const double ditMs   = 1200.0 / wpm;
    const int    ditN    = static_cast<int>(std::lround(ditMs / 1000.0 * sr));
    const double amp     = 0.5;
    std::vector<float> out;
    std::mt19937 rng(12345);
    std::normal_distribution<double> noise(0.0, noiseStd);
    double phase = 0.0;
    const double dphi = 2.0 * kPi * tone / sr;

    auto emit = [&](int units, bool on) {
        const int n = units * ditN;
        for (int i = 0; i < n; ++i) {
            double s = on ? amp * std::sin(phase) : 0.0;
            s += noise(rng);
            out.push_back(static_cast<float>(s));
            phase += dphi;
            if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
        }
    };

    emit(10, false);   // lead-in silence so the floor/peak settle
    for (size_t ci = 0; ci < text.size(); ++ci) {
        char c = text[ci];
        if (c == ' ') { emit(4, false); continue; }   // +4 → 7-unit word gap
        const char* code = morseFor(c);
        for (const char* p = code; *p; ++p) {
            emit(*p == '.' ? 1 : 3, true);   // dit / dah
            emit(1, false);                  // element gap
        }
        emit(2, false);   // +2 → 3-unit char gap
    }
    emit(12, false);      // trailing silence → flush last char
    return out;
}

std::string decode(CwDecoder& d, const std::vector<float>& sig, int block) {
    std::string got;
    d.onChar = [&](char c) { got.push_back(c); };
    for (size_t i = 0; i < sig.size(); i += block) {
        const int n = static_cast<int>(std::min<size_t>(block, sig.size() - i));
        d.process(sig.data() + i, n);
    }
    return got;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}
} // namespace

int main() {
    constexpr double kSr = 48000.0;

    // ── 1. clean decode at the nominal tone ──────────────────────────────
    {
        CwDecoder d;
        d.setSampleRate(kSr);
        d.setToneHz(700.0);
        d.setTxWpm(20);
        int lastWpm = 0;
        d.onWpm = [&](int w) { lastWpm = w; };

        const auto sig = synth("VVV TEST CQ", 20.0, 700.0, kSr, 0.01);
        const std::string got = decode(d, sig, 512);
        std::printf("decode(nominal) = \"%s\"  rxWpm=%d\n", got.c_str(), d.rxWpm());
        CHECK(contains(got, "TEST"));
        CHECK(contains(got, "CQ"));
        CHECK(contains(got, " "));                 // word gap emitted
        CHECK(lastWpm >= 15 && lastWpm <= 25);     // adaptive speed ≈ 20
    }

    // ── 2. reset() clears decode + re-seeds, decodes again ───────────────
    {
        CwDecoder d;
        d.setSampleRate(kSr);
        d.setToneHz(600.0);
        d.setTxWpm(25);
        // Warm-up word first ("CQ") so the floor/peak tracker has settled
        // before the asserted payload — the first character of any cold start
        // decodes during settling (true of any real decoder).
        const auto sig = synth("CQ PARIS", 25.0, 600.0, kSr, 0.01);
        std::string a = decode(d, sig, 256);
        std::printf("decode(reset-1) = \"%s\"\n", a.c_str());
        CHECK(contains(a, "PARIS"));
        d.reset();
        std::string b = decode(d, sig, 256);
        std::printf("decode(reset-2) = \"%s\"\n", b.c_str());
        CHECK(contains(b, "PARIS"));
    }

    // ── 3. AFC locks onto an off-nominal carrier ─────────────────────────
    {
        CwDecoder d;
        d.setSampleRate(kSr);
        d.setToneHz(700.0);       // configured pitch
        d.setAfcEnabled(true);
        d.setAfcRange(100);
        d.setTxWpm(18);
        bool sawLock = false; double lockHz = 0.0;
        d.onAfc = [&](bool locked, double hz) { if (locked) { sawLock = true; lockHz = hz; } };
        // Carrier 60 Hz above the configured tone — inside the ±100 Hz range.
        const auto sig = synth("CQ CQ TEST", 18.0, 760.0, kSr, 0.01);
        const std::string got = decode(d, sig, 1024);
        std::printf("decode(afc) = \"%s\"  afcLocked=%d lockHz=%.0f\n",
                    got.c_str(), int(d.afcLocked()), d.afcHz());
        CHECK(sawLock);
        CHECK(lockHz > 720.0 && lockHz < 800.0);   // pulled toward 760
        CHECK(contains(got, "TEST"));
    }

    if (g_fail == 0) std::printf("test_cw_decoder: ALL PASS\n");
    else             std::printf("test_cw_decoder: %d FAILURE(S)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
