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

QString DxccLookup::continentOf(const QString &iso2) {
    if (iso2.isEmpty()) return QString();
    // ISO-2 → continent.  Coarse for a few border countries (RU→EU, TR→AS),
    // which is fine for a spot region filter; sources that carry a precise
    // per-call continent (e.g. SpotHole) override this.
    static const QHash<QString, QString> kMap = [] {
        QHash<QString, QString> m;
        auto add = [&m](const QString &cont, std::initializer_list<const char *> isos) {
            for (const char *s : isos) m.insert(QString::fromLatin1(s), cont);
        };
        add(QStringLiteral("NA"), {"US","CA","MX","GT","BZ","SV","HN","NI","CR","PA",
            "CU","JM","HT","DO","BS","BB","AG","DM","GD","KN","LC","VC","TT","PR","VI",
            "AI","AW","BM","BQ","KY","CW","GL","GP","MQ","MS","SX","TC","VG","PM","MF","BL"});
        add(QStringLiteral("SA"), {"AR","BO","BR","CL","CO","EC","FK","GF","GY","PE",
            "PY","SR","UY","VE","GS"});
        add(QStringLiteral("EU"), {"AD","AL","AT","AX","BA","BE","BG","BY","CH","CZ",
            "DE","DK","EE","ES","FI","FO","FR","GG","GI","GR","HR","HU","IE","IM","IS",
            "IT","JE","LI","LT","LU","LV","MC","MD","ME","MK","MT","NL","NO","PL","PT",
            "RO","RS","RU","SE","SI","SJ","SK","SM","UA","VA","XK","GB"});
        add(QStringLiteral("AS"), {"AF","AE","AM","AZ","BD","BH","BN","BT","CN","CY",
            "GE","HK","ID","IL","IN","IQ","IR","JO","JP","KG","KH","KP","KR","KW","KZ",
            "LA","LB","LK","MM","MN","MO","MV","MY","NP","OM","PH","PK","PS","QA","SA",
            "SG","SY","TH","TJ","TL","TM","TR","TW","UZ","VN","YE"});
        add(QStringLiteral("AF"), {"AO","BF","BI","BJ","BW","CD","CF","CG","CI","CM",
            "CV","DJ","DZ","EG","EH","ER","ET","GA","GH","GM","GN","GQ","GW","KE","KM",
            "LR","LS","LY","MA","MG","ML","MR","MU","MW","MZ","NA","NE","NG","RE","RW",
            "SC","SD","SH","SL","SN","SO","SS","ST","SZ","TD","TG","TN","TZ","UG","YT",
            "ZA","ZM","ZW"});
        add(QStringLiteral("OC"), {"AU","NZ","FJ","PG","SB","VU","NC","PF","WS","TO",
            "KI","FM","MH","PW","NR","TV","CK","NU","WF","AS","GU","MP","PN","TK","NF",
            "CX","CC"});
        add(QStringLiteral("AN"), {"AQ","BV","HM","TF"});
        return m;
    }();
    return kMap.value(iso2.trimmed().toUpper());
}

} // namespace lyra::ui
