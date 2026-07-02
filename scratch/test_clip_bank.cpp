// Lyra-cpp — unit test for ClipBank (#89 voice keyer, Stage B / B3).
// Model + persistence: add/import/edit/remove, F-key uniqueness, sample
// round-trip, resample-on-import, reload-across-instances.
//   cmake --build build --target test_clip_bank  &&  build/test_clip_bank.exe
//
// Isolated from the real Lyra settings via QStandardPaths test mode + a
// test-specific org/app name (QSettings + AppDataLocation both redirect).

#include "tx/ClipBank.h"
#include "tx/WavIo.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QVariantMap>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace lyra::tx;

static int g_fail = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++g_fail; }            \
        else         { std::printf("  ok  : %s\n", msg); }                      \
    } while (0)

static std::vector<float> ramp(int n) {
    std::vector<float> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] = std::sin(6.2831853f * 440.0f * i / 48000.0f) * 0.5f;
    return v;
}

int main(int argc, char **argv) {
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("N8SDR-test"));
    QCoreApplication::setApplicationName(QStringLiteral("Lyra-cpp-clipbank-test"));
    { QSettings s; s.beginGroup(QStringLiteral("clips")); s.remove(QString()); }  // clean slate

    std::printf("== ClipBank (voice-keyer clip bank + persistence) ==\n");

    QString id1, id2;
    {
        ClipBank bank;
        CHECK(bank.clips().isEmpty(), "starts empty");

        // ── add from samples (100 ms @48k) ──
        id1 = bank.addFromSamples(QStringLiteral("CQ Contest"), ClipBank::Voice, ramp(4800));
        CHECK(!id1.isEmpty(), "addFromSamples returns id");
        CHECK(bank.clips().size() == 1, "one clip after add");
        CHECK(bank.contains(id1), "contains(id1)");
        {
            const QVariantMap m = bank.clips().first().toMap();
            CHECK(m.value("label").toString() == QStringLiteral("CQ Contest"), "label stored");
            CHECK(m.value("kind").toInt() == ClipBank::Voice, "kind = Voice");
            CHECK(std::abs(m.value("durationMs").toInt() - 100) <= 1, "duration ~100 ms");
            CHECK(QFile::exists(m.value("file").toString()), "WAV file on disk");
        }

        // ── loadSamples round-trip ──
        auto s = bank.loadSamples(id1);
        CHECK(s && s->size() == 4800, "loadSamples size round-trips");
        bool same = s && s->size() == 4800;
        for (std::size_t i = 0; same && i < 4800; i += 137)
            same = std::fabs((*s)[i] - ramp(4800)[i]) < 1e-4f;
        CHECK(same, "loadSamples values round-trip");

        // ── edits persist to the model ──
        bank.setGainDb(id1, 3.0);
        bank.setBypassDsp(id1, true);
        bank.setFkey(id1, 1);
        {
            const QVariantMap m = bank.clips().first().toMap();
            CHECK(std::abs(m.value("gainDb").toDouble() - 3.0) < 1e-9, "gainDb set");
            CHECK(m.value("bypassDsp").toBool(), "bypassDsp set");
            CHECK(m.value("fkey").toInt() == 1, "fkey set");
        }
        CHECK(bank.idForFkey(1) == id1, "idForFkey(1) -> id1");
        bank.setGainDb(id1, 999.0);   // over-range -> clamp
        CHECK(bank.clips().first().toMap().value("gainDb").toDouble() <= 20.0, "gainDb clamped");

        // ── F-key uniqueness: a second clip stealing F1 clears it from id1 ──
        id2 = bank.addFromSamples(QStringLiteral("73"), ClipBank::Rx, ramp(2400));
        CHECK(!id2.isEmpty() && id2 != id1, "second clip has distinct id");
        bank.setFkey(id2, 1);
        CHECK(bank.idForFkey(1) == id2, "F1 now id2");
        {
            for (const auto &v : bank.clips()) {
                if (v.toMap().value("id").toString() == id1)
                    CHECK(v.toMap().value("fkey").toInt() == 0, "id1 F-key stolen -> 0");
            }
        }

        // ── import an external WAV at 24 kHz -> normalised to 48 kHz ──
        const QString src = QDir(QDir::tempPath()).filePath(QStringLiteral("clipbank_import.wav"));
        writeWavMonoFloat(src.toStdString(), ramp(1200), 24000);   // 50 ms @24k
        const QString id3 = bank.importWav(QStringLiteral("Imported"), ClipBank::Voice, src);
        CHECK(!id3.isEmpty(), "importWav returns id");
        {
            for (const auto &v : bank.clips()) {
                const QVariantMap m = v.toMap();
                if (m.value("id").toString() == id3)
                    CHECK(std::abs(m.value("durationMs").toInt() - 50) <= 1, "import duration ~50 ms (rate-normalised)");
            }
        }
        auto si = bank.loadSamples(id3);
        CHECK(si && std::abs(static_cast<int>(si->size()) - 2400) <= 2, "imported clip resampled to ~48k length");
        QFile::remove(src);

        CHECK(bank.clips().size() == 3, "three clips before reload");
    }

    // ── persistence across instances ──
    {
        ClipBank bank;   // fresh instance -> load() from QSettings
        CHECK(bank.clips().size() == 3, "clips reload from QSettings");
        CHECK(bank.contains(id1) && bank.contains(id2), "ids survive reload");
        CHECK(bank.idForFkey(1) == id2, "F-key mapping survives reload");

        // new id must not collide with a reloaded one (nextSeq persisted)
        const QString id4 = bank.addFromSamples(QStringLiteral("new"), ClipBank::Voice, ramp(480));
        CHECK(!id4.isEmpty() && id4 != id1 && id4 != id2, "post-reload id is unique (nextSeq persisted)");

        // remove deletes the file + row
        const QString file1 = [&]{
            for (const auto &v : bank.clips())
                if (v.toMap().value("id").toString() == id1) return v.toMap().value("file").toString();
            return QString();
        }();
        CHECK(QFile::exists(file1), "id1 file exists pre-remove");
        bank.remove(id1);
        CHECK(!bank.contains(id1), "id1 gone after remove");
        CHECK(!QFile::exists(file1), "id1 WAV deleted from disk");
    }

    // best-effort cleanup of the test clips dir
    { ClipBank bank; QDir(bank.clipsDir()).removeRecursively(); }

    std::printf(g_fail ? "\nRESULT: %d CHECK(S) FAILED\n" : "\nRESULT: ALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
