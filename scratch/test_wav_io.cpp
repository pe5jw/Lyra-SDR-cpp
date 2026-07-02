// Lyra-cpp — unit test for WavIo (#89 voice keyer, Stage B).
// float32 write/read round-trip, PCM16 stereo→mono decode, resample, errors.
//   cmake --build build --target test_wav_io  &&  build/test_wav_io.exe

#include "tx/WavIo.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace lyra::tx;

static int g_fail = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++g_fail; }            \
        else         { std::printf("  ok  : %s\n", msg); }                      \
    } while (0)

static void put32(std::vector<std::uint8_t> &v, std::uint32_t x) {
    v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
    v.push_back((x >> 16) & 0xFF); v.push_back((x >> 24) & 0xFF);
}
static void put16(std::vector<std::uint8_t> &v, std::uint16_t x) {
    v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
}
static void putStr(std::vector<std::uint8_t> &v, const char *s) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(s[i]));
}
static bool writeBytes(const std::string &p, const std::vector<std::uint8_t> &b) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

int main() {
    std::printf("== WavIo (voice-keyer clip I/O) ==\n");
    const std::string fF = "clip_wavio_f32.wav";
    const std::string fP = "clip_wavio_pcm16.wav";

    // ── 1. float32 mono round-trip ──
    {
        std::vector<float> src = { 0.0f, 0.5f, -0.5f, 0.25f, -1.0f, 0.999f };
        CHECK(writeWavMonoFloat(fF, src, 48000), "write f32 mono WAV");
        WavData d; std::string err;
        CHECK(readWavMono(fF, d, err), "read it back");
        CHECK(d.sampleRate == 48000, "sample rate = 48000");
        CHECK(d.mono.size() == src.size(), "sample count round-trips");
        bool same = d.mono.size() == src.size();
        for (std::size_t i = 0; same && i < src.size(); ++i)
            same = std::fabs(d.mono[i] - src[i]) < 1e-6f;
        CHECK(same, "float samples round-trip exactly");
    }

    // ── 2. PCM 16-bit stereo → mono downmix ──
    {
        // 3 frames: (0.5,0.5)->0.5, (0.5,-0.5)->0.0, (0.25,0.25)->0.25
        std::vector<std::int16_t> lr = { 16384, 16384,  16384, -16384,  8192, 8192 };
        std::vector<std::uint8_t> w;
        const std::uint32_t dataBytes = static_cast<std::uint32_t>(lr.size() * 2);
        putStr(w, "RIFF"); put32(w, 36 + dataBytes); putStr(w, "WAVE");
        putStr(w, "fmt "); put32(w, 16);
        put16(w, 1);            // PCM
        put16(w, 2);            // stereo
        put32(w, 48000);
        put32(w, 48000 * 4);    // byte rate (2ch*2B)
        put16(w, 4);            // block align
        put16(w, 16);           // bits
        putStr(w, "data"); put32(w, dataBytes);
        for (std::int16_t s : lr) put16(w, static_cast<std::uint16_t>(s));
        CHECK(writeBytes(fP, w), "hand-build PCM16 stereo WAV");

        WavData d; std::string err;
        CHECK(readWavMono(fP, d, err), "read PCM16 stereo");
        CHECK(d.mono.size() == 3, "3 frames after downmix");
        CHECK(std::fabs(d.mono[0] - 0.5f)  < 2e-4f, "frame0 downmix = 0.5");
        CHECK(std::fabs(d.mono[1] - 0.0f)  < 2e-4f, "frame1 downmix = 0.0");
        CHECK(std::fabs(d.mono[2] - 0.25f) < 2e-4f, "frame2 downmix = 0.25");
    }

    // ── 3. resampleLinear ──
    {
        std::vector<float> in(100);
        for (int i = 0; i < 100; ++i) in[i] = static_cast<float>(i) / 100.0f;
        auto up = resampleLinear(in, 48000, 96000);
        CHECK(up.size() > 190 && up.size() <= 200, "upsample ~2x length");
        CHECK(std::fabs(up.front() - in.front()) < 1e-6f, "upsample preserves first sample");
        auto dn = resampleLinear(in, 48000, 24000);
        CHECK(dn.size() >= 49 && dn.size() <= 51, "downsample ~0.5x length");
        auto same = resampleLinear(in, 48000, 48000);
        CHECK(same.size() == in.size(), "equal rates → unchanged");
    }

    // ── 4. error paths ──
    {
        WavData d; std::string err;
        CHECK(!readWavMono("does_not_exist_12345.wav", d, err), "missing file → false");
        std::vector<std::uint8_t> junk = { 'N','O','T','A','W','A','V','!' };
        writeBytes("clip_wavio_junk.wav", junk);
        CHECK(!readWavMono("clip_wavio_junk.wav", d, err), "non-RIFF → false");
        std::remove("clip_wavio_junk.wav");
    }

    std::remove(fF.c_str());
    std::remove(fP.c_str());
    std::printf(g_fail ? "\nRESULT: %d CHECK(S) FAILED\n" : "\nRESULT: ALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
