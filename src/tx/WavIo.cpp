// Lyra-cpp — #89 Voice keyer, Stage B: WAV I/O.  See WavIo.h.

#include "tx/WavIo.h"

#include <cstring>
#include <fstream>

namespace lyra::tx {

namespace {

std::uint16_t rd16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t rd32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::int32_t rd24s(const std::uint8_t *p) {
    std::int32_t v = static_cast<std::int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
    if (v & 0x00800000) v |= ~0x00FFFFFF;   // sign-extend
    return v;
}
bool tag(const std::uint8_t *p, const char *s) { return std::memcmp(p, s, 4) == 0; }

void wr16(std::ostream &o, std::uint16_t v) {
    char b[2] = { char(v & 0xFF), char((v >> 8) & 0xFF) };
    o.write(b, 2);
}
void wr32(std::ostream &o, std::uint32_t v) {
    char b[4] = { char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF), char((v >> 24) & 0xFF) };
    o.write(b, 4);
}

} // namespace

bool writeWavMonoFloat(const std::string &path, const std::vector<float> &mono,
                       int sampleRate) {
    if (sampleRate <= 0) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    const std::uint32_t n         = static_cast<std::uint32_t>(mono.size());
    const std::uint32_t dataBytes = n * 4u;
    const std::uint32_t rate      = static_cast<std::uint32_t>(sampleRate);

    f.write("RIFF", 4);  wr32(f, 36u + dataBytes);  f.write("WAVE", 4);
    f.write("fmt ", 4);  wr32(f, 16u);
    wr16(f, 3);          // WAVE_FORMAT_IEEE_FLOAT
    wr16(f, 1);          // mono
    wr32(f, rate);
    wr32(f, rate * 4u);  // byte rate
    wr16(f, 4);          // block align
    wr16(f, 32);         // bits
    f.write("data", 4);  wr32(f, dataBytes);
    if (n) f.write(reinterpret_cast<const char *>(mono.data()), dataBytes);  // float LE (x86)
    return static_cast<bool>(f);
}

bool readWavMono(const std::string &path, WavData &out, std::string &err) {
    out.mono.clear();
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { err = "cannot open file"; return false; }
    const std::streamoff sz = f.tellg();
    if (sz < 12) { err = "file too small"; return false; }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
    f.seekg(0);
    f.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(sz));

    if (!tag(&buf[0], "RIFF") || !tag(&buf[8], "WAVE")) { err = "not a RIFF/WAVE file"; return false; }

    std::uint16_t fmt = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
    std::size_t   dataOff = 0, dataLen = 0;
    bool haveFmt = false, haveData = false;

    std::size_t off = 12;
    while (off + 8 <= buf.size()) {
        const std::uint32_t csz = rd32(&buf[off + 4]);
        const std::size_t   body = off + 8;
        if (tag(&buf[off], "fmt ") && csz >= 16 && body + 16 <= buf.size()) {
            fmt      = rd16(&buf[body + 0]);
            channels = rd16(&buf[body + 2]);
            rate     = rd32(&buf[body + 4]);
            bits     = rd16(&buf[body + 14]);
            if (fmt == 0xFFFE && csz >= 40 && body + 26 <= buf.size())  // EXTENSIBLE → subformat code
                fmt = rd16(&buf[body + 24]);
            haveFmt = true;
        } else if (tag(&buf[off], "data")) {
            dataOff  = body;
            dataLen  = (body + csz <= buf.size()) ? csz : (buf.size() - body);
            haveData = true;
        }
        off = body + csz + (csz & 1u);   // chunks are word-aligned
    }

    if (!haveFmt || !haveData) { err = "missing fmt/data chunk"; return false; }
    if (channels < 1 || rate == 0) { err = "invalid format"; return false; }
    const int bytesPerSample = bits / 8;
    if (bytesPerSample <= 0) { err = "invalid bit depth"; return false; }
    if (!(fmt == 1 || fmt == 3)) { err = "unsupported WAV codec"; return false; }
    if (fmt == 3 && bits != 32)  { err = "only 32-bit float supported"; return false; }
    if (fmt == 1 && !(bits == 8 || bits == 16 || bits == 24 || bits == 32)) {
        err = "unsupported PCM bit depth"; return false;
    }

    const int         frameBytes = bytesPerSample * channels;
    const std::size_t frames     = frameBytes ? (dataLen / static_cast<std::size_t>(frameBytes)) : 0;
    out.sampleRate = static_cast<int>(rate);
    out.mono.reserve(frames);

    auto decode = [&](const std::uint8_t *p) -> float {
        if (fmt == 3) { float v; std::memcpy(&v, p, 4); return v; }         // float32 LE (x86)
        switch (bits) {
            case 8:  return (static_cast<float>(p[0]) - 128.0f) / 128.0f;   // unsigned
            case 16: return static_cast<float>(static_cast<std::int16_t>(rd16(p))) / 32768.0f;
            case 24: return static_cast<float>(rd24s(p)) / 8388608.0f;
            case 32: return static_cast<float>(static_cast<std::int32_t>(rd32(p))) / 2147483648.0f;
            default: return 0.0f;
        }
    };

    for (std::size_t i = 0; i < frames; ++i) {
        const std::uint8_t *fp = &buf[dataOff + i * static_cast<std::size_t>(frameBytes)];
        float acc = 0.0f;
        for (int c = 0; c < channels; ++c) acc += decode(fp + c * bytesPerSample);
        out.mono.push_back(acc / static_cast<float>(channels));   // downmix to mono
    }
    return true;
}

std::vector<float> resampleLinear(const std::vector<float> &in, int srcRate, int dstRate) {
    if (srcRate <= 0 || dstRate <= 0 || srcRate == dstRate || in.size() < 2) return in;
    const double ratio = static_cast<double>(srcRate) / static_cast<double>(dstRate);
    const std::size_t nOut = static_cast<std::size_t>(static_cast<double>(in.size()) / ratio);
    std::vector<float> out;
    out.reserve(nOut);
    for (std::size_t i = 0; i < nOut; ++i) {
        const double sp = static_cast<double>(i) * ratio;
        const std::size_t i0 = static_cast<std::size_t>(sp);
        const std::size_t i1 = (i0 + 1 < in.size()) ? i0 + 1 : i0;
        const double frac = sp - static_cast<double>(i0);
        out.push_back(static_cast<float>(in[i0] * (1.0 - frac) + in[i1] * frac));
    }
    return out;
}

} // namespace lyra::tx
