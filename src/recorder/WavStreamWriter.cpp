// Lyra-cpp — Session Recorder (#201), Stage 1: streaming WAV writer impl.

#include "recorder/WavStreamWriter.h"

#include <array>
#include <cstring>

namespace lyra::recorder {

namespace {

// Little-endian scalar writers (host-endianness independent).
void putU16(std::ostream &os, uint16_t v) {
    char b[2] = { char(v & 0xFF), char((v >> 8) & 0xFF) };
    os.write(b, 2);
}
void putU32(std::ostream &os, uint32_t v) {
    char b[4] = { char(v & 0xFF), char((v >> 8) & 0xFF),
                  char((v >> 16) & 0xFF), char((v >> 24) & 0xFF) };
    os.write(b, 4);
}

constexpr uint16_t kFormatFloat = 3;      // WAVE_FORMAT_IEEE_FLOAT
constexpr uint16_t kBits        = 32;

} // namespace

WavStreamWriter::~WavStreamWriter() { close(); }

bool WavStreamWriter::open(const std::string &path, int channels, int sampleRate) {
    close();
    if (channels != 1 && channels != 2) return false;
    if (sampleRate <= 0) return false;

    file_.open(path, std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) return false;

    channels_   = channels;
    sampleRate_ = sampleRate;
    dataBytes_  = 0;

    const uint32_t byteRate   = uint32_t(sampleRate) * channels * (kBits / 8);
    const uint16_t blockAlign = uint16_t(channels * (kBits / 8));

    // Header with placeholder RIFF/data sizes — patched in close().
    file_.write("RIFF", 4);
    putU32(file_, 0);                 // [4]  RIFF size — patched
    file_.write("WAVE", 4);
    file_.write("fmt ", 4);
    putU32(file_, 16);                // fmt chunk size (PCM/float basic)
    putU16(file_, kFormatFloat);
    putU16(file_, uint16_t(channels));
    putU32(file_, uint32_t(sampleRate));
    putU32(file_, byteRate);
    putU16(file_, blockAlign);
    putU16(file_, kBits);
    file_.write("data", 4);
    putU32(file_, 0);                 // [40] data size — patched
    return bool(file_);
}

bool WavStreamWriter::write(const float *interleaved, int frames) {
    if (!file_.is_open() || frames <= 0 || !interleaved) return file_.is_open();
    const size_t samples = size_t(frames) * size_t(channels_);
    // float is IEEE-754 32-bit on every platform Lyra targets; disk is LE.
    // On the (universal here) little-endian host a raw block write is exact.
    file_.write(reinterpret_cast<const char *>(interleaved),
                std::streamsize(samples * sizeof(float)));
    if (!file_) return false;
    dataBytes_ += uint64_t(samples) * sizeof(float);
    return true;
}

bool WavStreamWriter::close() {
    if (!file_.is_open()) return true;
    // Patch the two size fields now that dataBytes_ is known.
    const uint32_t dataSize = uint32_t(dataBytes_);
    const uint32_t riffSize = uint32_t(36u + dataBytes_);
    file_.seekp(4, std::ios::beg);   putU32(file_, riffSize);
    file_.seekp(40, std::ios::beg);  putU32(file_, dataSize);
    const bool ok = bool(file_);
    file_.close();
    channels_  = 0;
    dataBytes_ = 0;
    return ok;
}

} // namespace lyra::recorder
