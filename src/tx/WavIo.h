// Lyra-cpp — #89 Voice keyer, Stage B: WAV I/O for the clip bank.
//
// Minimal, self-contained (Qt-free, no libs) reader/writer for the clip
// format used by the voice keyer / RX recorder (design doc §6):
//   * write — 48 kHz mono, 32-bit IEEE float (the default in the Thetis
//     Recording tab); what the recorder (Stage C) produces + what we save.
//   * read  — any common WAV (PCM 8/16/24/32-bit or IEEE float32, mono or
//     stereo → mono downmix) into mono float [-1, +1], so an imported clip
//     also plays; the sample rate is returned so the bank can normalise to
//     48 kHz via resampleLinear() before handing samples to the player.
//
// Samples are float in [-1, +1] end-to-end (ClipRecorderPlayer plays a
// std::vector<float>).  Little-endian on disk (WAV) — decoded explicitly so
// it is not host-endianness dependent.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lyra::tx {

struct WavData {
    std::vector<float> mono;          // [-1, +1]
    int                sampleRate = 48000;
};

// Write mono float samples as a 32-bit IEEE-float WAV.  Returns false on any
// I/O error (path unwritable, etc.).
bool writeWavMonoFloat(const std::string &path,
                       const std::vector<float> &mono,
                       int sampleRate = 48000);

// Read a WAV into mono float.  Handles PCM 8/16/24/32-bit + IEEE float32,
// mono or stereo (averaged to mono), incl. WAVE_FORMAT_EXTENSIBLE.  On error
// returns false and sets `err`; on success fills `out`.
bool readWavMono(const std::string &path, WavData &out, std::string &err);

// Naive linear resampler (ample for voice clips) — normalise an imported
// clip to the target rate.  Returns `in` unchanged when the rates match or
// either rate is non-positive.
std::vector<float> resampleLinear(const std::vector<float> &in,
                                  int srcRate, int dstRate);

} // namespace lyra::tx
