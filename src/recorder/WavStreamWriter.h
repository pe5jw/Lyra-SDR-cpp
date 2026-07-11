// Lyra-cpp — Session Recorder (#201), Stage 1: streaming WAV writer.
//
// A minimal, self-contained (Qt-free, no libs) *streaming* 32-bit IEEE-float
// WAV writer for the session recorder.  Unlike tx/WavIo's writeWavMonoFloat
// (which buffers the whole clip in memory and writes it at once), this writer
// appends frames incrementally to disk and patches the RIFF/data chunk sizes
// on close — so a 10-minute recording never sits in RAM.
//
//   * mono OR stereo, 32-bit IEEE float (WAVE_FORMAT_IEEE_FLOAT).
//   * samples are float in [-1, +1], interleaved by channel.
//   * little-endian on disk, written explicitly (host-endianness independent).
//
// Used ONLY by the recorder's writer thread — never on the audio thread.

#pragma once

#include <cstdint>
#include <fstream>
#include <string>

namespace lyra::recorder {

class WavStreamWriter {
public:
    WavStreamWriter() = default;
    ~WavStreamWriter();

    WavStreamWriter(const WavStreamWriter &) = delete;
    WavStreamWriter &operator=(const WavStreamWriter &) = delete;

    // Open <path> and write a placeholder header.  channels = 1 or 2.
    // Returns false if the file can't be created.
    bool open(const std::string &path, int channels, int sampleRate);

    // Append <frames> interleaved float frames (frames * channels samples).
    // Returns false on write error.  No-op if not open.
    bool write(const float *interleaved, int frames);

    // Patch the header sizes and close.  Safe to call more than once.
    // Returns false on any I/O error.
    bool close();

    bool     isOpen()       const { return file_.is_open(); }
    // Total audio bytes written so far (excludes the 44-byte header).
    uint64_t dataBytes()    const { return dataBytes_; }
    int      channels()     const { return channels_; }
    int      sampleRate()   const { return sampleRate_; }
    uint64_t frames()       const { return channels_ ? dataBytes_ / (4u * channels_) : 0; }

private:
    std::ofstream file_;
    int      channels_   = 0;
    int      sampleRate_ = 48000;
    uint64_t dataBytes_  = 0;
};

} // namespace lyra::recorder
