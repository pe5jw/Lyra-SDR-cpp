// Lyra-cpp — Session Recorder (#201) Stage 1 unit test.
//
// Drives RecorderEngine headlessly (no event loop needed — the record path
// runs on the writer thread + finalize on stop()).  Verifies:
//   * streaming WAV headers are valid (RIFF/WAVE/data sizes patched),
//   * mono record → 1-channel WAV, exact frame count,
//   * stereo (recordTx) record from a mono feed → 2-channel WAV (dup),
//   * size-based auto-split → multiple numbered files, frames sum correct,
//   * session.json manifest (audio[] + snapshot metadata) is written & parses.
//
// Qt Core only; QSettings isolated to a temp INI.  EXCLUDE_FROM_ALL.
// Build + run:  cmake --build build --target test_recorder

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <cstring>
#include <vector>

#include "recorder/RecorderEngine.h"

using namespace lyra::recorder;

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } \
} while (0)

// Minimal WAV header reader for verification.
struct WavInfo { bool ok=false; int channels=0; int rate=0; quint32 dataBytes=0; qint64 fileSize=0; };

static quint32 rd32(const unsigned char *p) {
    return quint32(p[0]) | (quint32(p[1])<<8) | (quint32(p[2])<<16) | (quint32(p[3])<<24);
}
static quint16 rd16(const unsigned char *p) { return quint16(p[0]) | (quint16(p[1])<<8); }

static WavInfo readWav(const QString &path) {
    WavInfo w;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return w;
    const QByteArray b = f.readAll();
    w.fileSize = b.size();
    if (b.size() < 44) return w;
    const auto *u = reinterpret_cast<const unsigned char*>(b.constData());
    if (std::memcmp(u, "RIFF", 4) != 0) return w;
    if (std::memcmp(u+8, "WAVE", 4) != 0) return w;
    if (std::memcmp(u+12, "fmt ", 4) != 0) return w;
    w.channels  = rd16(u+22);
    w.rate      = int(rd32(u+24));
    if (std::memcmp(u+36, "data", 4) != 0) return w;
    w.dataBytes = rd32(u+40);
    w.ok = true;
    return w;
}

// Feed `frames` mono float frames in small blocks, giving the writer thread
// room to stay ahead of the ring (deterministic — no drops).
static void feedMono(RecorderEngine &e, int frames) {
    std::vector<float> blk(8192);
    for (size_t i = 0; i < blk.size(); ++i) blk[i] = 0.25f * float((i % 100) - 50) / 50.0f;
    int done = 0;
    while (done < frames) {
        const int n = std::min<int>(int(blk.size()), frames - done);
        e.feedAudio(blk.data(), n, 1);
        done += n;
        QThread::usleep(400);   // let the writer drain
    }
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    // Isolate QSettings to a temp INI so we don't touch the real registry.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp.path());
    QCoreApplication::setOrganizationName(QStringLiteral("LyraTest"));
    QCoreApplication::setApplicationName(QStringLiteral("recorder_test"));

    const QString recRoot = QDir(tmp.path()).filePath(QStringLiteral("rec"));

    // ── Test 1: mono, no split, exact frame count + manifest + snapshot ──
    {
        RecorderEngine e;
        RecorderConfig c = e.config();
        c.path = recRoot; c.recordTx = false; c.snapshotsOn = false;
        c.splitMinutes = 0; c.splitMB = 0; c.hardLimitMin = 0;
        c.capMB = 0; c.audioRateHz = 48000;
        e.setConfig(c);

        CHECK(e.start(14074000, QStringLiteral("USB")));
        CHECK(e.isRecording());
        const QString dir = e.sessionDir();
        CHECK(dir.contains(QStringLiteral("14074kHz_USB")));

        // A snapshot metadata entry (Stage-2 path is UI; here we exercise the
        // manifest hook directly).
        const QString snap = e.reserveSnapshotFile();
        CHECK(snap.endsWith(QStringLiteral("snap_0001.png")));
        e.noteSnapshot(snap, 14074000, QStringLiteral("USB"));

        const int frames = 100000;             // ~2.08 s @48k mono
        feedMono(e, frames);
        e.stop();
        CHECK(!e.isRecording());

        const QString wav = QDir(dir).filePath(QStringLiteral("audio_001.wav"));
        WavInfo w = readWav(wav);
        CHECK(w.ok);
        CHECK(w.channels == 1);
        CHECK(w.rate == 48000);
        CHECK(w.dataBytes == quint32(frames) * 1u * 4u);
        CHECK(w.fileSize == qint64(44) + w.dataBytes);   // header + data

        // Manifest
        QFile mf(QDir(dir).filePath(QStringLiteral("session.json")));
        CHECK(mf.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(mf.readAll()).object();
        CHECK(root["source"].toString() == QStringLiteral("rx"));
        CHECK(qRound(root["freqHz"].toDouble()) == 14074000);
        CHECK(root["mode"].toString() == QStringLiteral("USB"));
        const QJsonArray audio = root["audio"].toArray();
        CHECK(audio.size() == 1);
        CHECK(audio[0].toObject()["channels"].toInt() == 1);
        CHECK(qRound(audio[0].toObject()["frames"].toDouble()) == frames);
        const QJsonArray snaps = root["snapshots"].toArray();
        CHECK(snaps.size() == 1);
        CHECK(snaps[0].toObject()["file"].toString() == QStringLiteral("snap_0001.png"));
    }

    // ── Test 2: stereo (recordTx) from a MONO feed → 2ch WAV, frames match ──
    {
        RecorderEngine e;
        RecorderConfig c = e.config();
        c.path = recRoot; c.recordTx = true; c.snapshotsOn = false;
        c.splitMinutes = 0; c.splitMB = 0; c.hardLimitMin = 0; c.capMB = 0;
        e.setConfig(c);

        CHECK(e.start(7040000, QStringLiteral("LSB")));
        const QString dir = e.sessionDir();
        const int frames = 60000;
        feedMono(e, frames);                    // mono in → engine dups to stereo
        e.stop();

        WavInfo w = readWav(QDir(dir).filePath(QStringLiteral("audio_001.wav")));
        CHECK(w.ok);
        CHECK(w.channels == 2);
        CHECK(w.dataBytes == quint32(frames) * 2u * 4u);
    }

    // ── Test 3: size-based auto-split → 2 files, frames sum correct ──
    {
        RecorderEngine e;
        RecorderConfig c = e.config();
        c.path = recRoot; c.recordTx = false; c.snapshotsOn = false;
        c.splitMinutes = 0; c.splitMB = 1;      // 1 MB mono = 262144 frames
        c.hardLimitMin = 0; c.capMB = 0;
        e.setConfig(c);

        CHECK(e.start(21074000, QStringLiteral("USB")));
        const QString dir = e.sessionDir();
        const int frames = 300000;              // > 262144 → splits
        feedMono(e, frames);
        e.stop();

        QFile mf(QDir(dir).filePath(QStringLiteral("session.json")));
        CHECK(mf.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(mf.readAll()).object();
        const QJsonArray audio = root["audio"].toArray();
        CHECK(audio.size() == 2);
        qint64 sum = 0;
        for (const auto &v : audio) sum += qRound64(v.toObject()["frames"].toDouble());
        CHECK(sum == frames);
        // Both files present on disk.
        CHECK(QFile::exists(QDir(dir).filePath(QStringLiteral("audio_001.wav"))));
        CHECK(QFile::exists(QDir(dir).filePath(QStringLiteral("audio_002.wav"))));
    }

    if (g_fail == 0) std::printf("test_recorder: ALL PASS\n");
    else             std::printf("test_recorder: %d FAILURE(S)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
