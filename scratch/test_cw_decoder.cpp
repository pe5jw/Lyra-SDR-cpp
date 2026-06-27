// Unit test for lyra::dsp::CwDecoder (#173 CW-5a).  Qt-free, pure C++.
// Build + run:  cmake --build build --target test_cw_decoder
//               build/test_cw_decoder.exe
//
// Synthesises clean (lightly-noised) on/off-keyed CW at a known WPM/tone,
// feeds it through the decoder in blocks, and asserts the decoded text.
// Also exercises reset() and AFC lock to an off-nominal tone.

#include "dsp/CwDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using lyra::dsp::CwDecoder;

// ── #187 WAV-replay mode ────────────────────────────────────────────────────
// Read a 48k mono 16-bit PCM WAV (our LYRA_CW_WAV capture; data assumed at
// offset 44; tolerates a 0 data-size field by using the file length) and run
// the real decoder on it.  Usage: test_cw_decoder <wav> [toneHz] [wpm] [seek01]
namespace {
std::vector<float> readWav(const char* path, int& sr) {
    std::vector<float> out;
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::printf("cannot open %s\n", path); return out; }
    f.seekg(0, std::ios::end); const long fsize = static_cast<long>(f.tellg()); f.seekg(0);
    char h[44];
    if (!f.read(h, 44) || std::memcmp(h, "RIFF", 4) || std::memcmp(h + 8, "WAVE", 4)) {
        std::printf("not a RIFF/WAVE file\n"); return out;
    }
    int16_t  channels; std::memcpy(&channels, h + 22, 2);
    int32_t  rate;     std::memcpy(&rate,     h + 24, 4);
    int16_t  bits;     std::memcpy(&bits,     h + 34, 2);
    uint32_t dataSz;   std::memcpy(&dataSz,   h + 40, 4);
    sr = rate;
    const int ch = channels > 0 ? channels : 1;
    const int bytesPer = (bits > 0 ? bits : 16) / 8;
    const long avail = fsize - 44;
    long bytes = (dataSz > 0 && static_cast<long>(dataSz) <= avail) ? static_cast<long>(dataSz) : avail;
    const long nsamp = bytes / bytesPer;
    std::vector<int16_t> pcm(static_cast<size_t>(nsamp));
    f.read(reinterpret_cast<char*>(pcm.data()), nsamp * bytesPer);
    out.reserve(static_cast<size_t>(nsamp / ch));
    for (long i = 0; i < nsamp; i += ch) out.push_back(pcm[static_cast<size_t>(i)] / 32768.0f);
    return out;
}

int replayWav(int argc, char** argv) {
    int sr = 48000;
    const auto sig = readWav(argv[1], sr);
    if (sig.empty()) return 1;
    const double tone   = argc > 2 ? std::atof(argv[2]) : 700.0;
    const int    wpm    = argc > 3 ? std::atoi(argv[3]) : 18;
    const int    seek   = argc > 4 ? std::atoi(argv[4]) : 0;
    const double narrow = argc > 5 ? std::atof(argv[5]) : 0.0;   // narrow-filter BW Hz (0=off)
    const double thrPct = argc > 6 ? std::atof(argv[6]) : 0.0;   // slicer threshold % (0=default)
    const double sqlch  = argc > 7 ? std::atof(argv[7]) : 0.0;   // squelch × (0=default)
    const int    autoLv = argc > 8 ? std::atoi(argv[8]) : 0;     // auto-threshold on
    const int    dspOff = argc > 9 ? std::atoi(argv[9]) : 0;     // 1 = DSP filter OFF

    CwDecoder d;
    d.setSampleRate(sr);
    d.setToneHz(tone);
    d.setTxWpm(wpm);
    d.setAfcEnabled(true);
    d.setDspFilter(dspOff ? false : true);
    if (seek) { d.setAutoSeek(true); d.setSeekBandwidthHz(1000.0); }
    if (narrow > 0.0) { d.setNarrowDetect(true); d.setNarrowDetectBwHz(narrow); }
    if (thrPct > 0.0) d.setThreshold(thrPct / 100.0);
    if (sqlch  > 0.0) d.setSquelch(sqlch);
    if (autoLv)       d.setAutoThreshold(true);

    std::string got;
    d.onChar = [&](char c, double) { got.push_back(c); };
    for (size_t i = 0; i < sig.size(); i += 512) {
        const int n = static_cast<int>(std::min<size_t>(512, sig.size() - i));
        d.process(sig.data() + i, n);
    }
    std::printf("WAV %s  sr=%d  dur=%.1fs  tone=%.0f wpm=%d seek=%d narrowBw=%.0f\n",
                argv[1], sr, sig.size() / static_cast<double>(sr), tone, wpm, seek, narrow);
    std::printf("afcLocked=%d afcHz=%.0f rxWpm=%d\n",
                static_cast<int>(d.afcLocked()), d.afcHz(), d.rxWpm());
    std::printf("DECODE:\n%s\n", got.c_str());
    return 0;
}
} // namespace

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
    d.onChar = [&](char c, double /*conf*/) { got.push_back(c); };
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

int main(int argc, char** argv) {
    if (argc > 1) return replayWav(argc, argv);   // #187 WAV-replay mode

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

    // ── 4. #187 auto-seek grabs an off-pitch tone the narrow AFC can't reach ─
    {
        // Carrier 300 Hz above the configured 700 Hz pitch — outside the ±200 Hz
        // AFC, but inside an 800 Hz CW passband.  The meaningful metric is the
        // LOCK FREQUENCY: the narrow biased AFC stays pinned near the pitch (it
        // never reaches the true carrier), while auto-seek (unbiased, span =
        // passband) homes onto the real ~1000 Hz tone and decodes it cleanly.
        const auto sig = synth("CQ CQ TEST DE", 18.0, 1000.0, kSr, 0.02);

        CwDecoder narrow;
        narrow.setSampleRate(kSr); narrow.setToneHz(700.0);
        narrow.setAfcEnabled(true); narrow.setAfcRange(200); narrow.setTxWpm(18);
        const std::string gotN = decode(narrow, sig, 1024);
        std::printf("decode(no-seek, off-pitch) = \"%s\"  afcHz=%.0f\n",
                    gotN.c_str(), narrow.afcHz());
        CHECK(narrow.afcHz() < 920.0);         // pinned near the pitch, not the carrier

        CwDecoder seek;
        seek.setSampleRate(kSr); seek.setToneHz(700.0);
        seek.setAfcEnabled(true); seek.setTxWpm(18);
        seek.setAutoSeek(true); seek.setSeekBandwidthHz(800.0);   // ±400 Hz span
        bool sawLock = false;
        seek.onAfc = [&](bool l, double /*hz*/) { if (l) sawLock = true; };
        const std::string gotS = decode(seek, sig, 1024);
        std::printf("decode(auto-seek, off-pitch) = \"%s\"  afcHz=%.0f\n",
                    gotS.c_str(), seek.afcHz());
        CHECK(sawLock);
        CHECK(seek.afcHz() > 900.0);   // pulled the lock up toward the true 1000 Hz carrier
        // …and substantially closer to truth than the biased narrow AFC manages
        // (which structurally stays anchored near the pitch / passband edge).
        CHECK(std::fabs(seek.afcHz() - 1000.0) + 50.0 < std::fabs(narrow.afcHz() - 1000.0));
        CHECK(contains(gotS, "TEST"));
    }

    // ── 5. #187 auto-threshold decodes with no manual squelch/threshold set ──
    {
        CwDecoder d;
        d.setSampleRate(kSr); d.setToneHz(650.0); d.setTxWpm(22);
        d.setAutoThreshold(true);
        const auto sig = synth("CQ TEST PARIS", 22.0, 650.0, kSr, 0.03);   // noisier
        const std::string got = decode(d, sig, 512);
        std::printf("decode(auto-thresh) = \"%s\"\n", got.c_str());
        CHECK(contains(got, "TEST"));
        CHECK(contains(got, "PARIS"));
    }

    if (g_fail == 0) std::printf("test_cw_decoder: ALL PASS\n");
    else             std::printf("test_cw_decoder: %d FAILURE(S)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
