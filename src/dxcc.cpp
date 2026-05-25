// Lyra — DXCC callsign → country (ISO-2) lookup.  See dxcc.h.

#include "dxcc.h"

#include <QFile>
#include <QTextStream>

namespace lyra::ui {

DxccLookup::DxccLookup() {
    QFile f(QStringLiteral(":/data/dxcc_prefix.csv"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        const int comma = line.indexOf(QLatin1Char(','));
        if (comma <= 0) continue;
        const QString key = line.left(comma);
        const QString iso = line.mid(comma + 1).trimmed();
        if (iso.isEmpty()) continue;
        if (key.startsWith(QLatin1Char('=')))
            exact_.insert(key.mid(1), iso);     // exact full-call match
        else
            prefix_.insert(key, iso);
    }
}

QString DxccLookup::isoOf(const QString &callsign) const {
    if (prefix_.isEmpty() || callsign.isEmpty()) return QString();
    QString call = callsign.trimmed().toUpper();
    // Portable calls ("W1/VK3ABC", "VK3ABC/P"): pick the part that looks
    // like the country base — most digits, then longest (old-Lyra rule).
    if (call.contains(QLatin1Char('/'))) {
        const QStringList parts = call.split(QLatin1Char('/'));
        QString best = parts.first();
        int bestScore = -1, bestLen = -1;
        for (const QString &p : parts) {
            int digits = 0;
            for (const QChar c : p) if (c.isDigit()) ++digits;
            if (digits > bestScore || (digits == bestScore && p.size() > bestLen)) {
                bestScore = digits; bestLen = p.size(); best = p;
            }
        }
        call = best;
    }
    auto itE = exact_.constFind(call);
    if (itE != exact_.constEnd()) return itE.value();
    for (int len = qMin(call.size(), 6); len >= 1; --len) {
        auto it = prefix_.constFind(call.left(len));
        if (it != prefix_.constEnd()) return it.value();
    }
    return QString();
}

QString DxccLookup::enrich(const QString &callsign) const {
    const QString iso = isoOf(callsign);
    return iso.isEmpty() ? callsign : iso + QLatin1Char(' ') + callsign;
}

} // namespace lyra::ui
