// Lyra — GitHub release update checker.  See updatechecker.h.

#include "updatechecker.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

namespace lyra::ui {

namespace {
// All releases (NOT /latest) so pre-releases are visible, matching old
// Lyra.  Owner/name come from the CMake single-source-of-truth macros.
QString releasesApiUrl() {
    return QStringLiteral("https://api.github.com/repos/%1/%2/releases")
        .arg(QStringLiteral(LYRA_REPO_OWNER), QStringLiteral(LYRA_REPO_NAME));
}

// Parse "v?MAJOR.MINOR.PATCH(.MICRO)?" into a 4-tuple (micro pads to 0).
// Returns an empty list if it doesn't match.
QList<int> parseVersion(const QString &s) {
    static const QRegularExpression re(
        QStringLiteral("v?(\\d+)\\.(\\d+)\\.(\\d+)(?:\\.(\\d+))?"));
    const QRegularExpressionMatch m = re.match(s);
    if (!m.hasMatch()) return {};
    return {m.captured(1).toInt(), m.captured(2).toInt(),
            m.captured(3).toInt(),
            m.captured(4).isEmpty() ? 0 : m.captured(4).toInt()};
}
} // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {}

bool UpdateChecker::isNewer(const QString &remoteTag, const QString &localVer) {
    const QList<int> r = parseVersion(remoteTag);
    const QList<int> l = parseVersion(localVer);
    if (r.size() != 4 || l.size() != 4) return false;
    for (int i = 0; i < 4; ++i) {
        if (r[i] != l[i]) return r[i] > l[i];
    }
    return false;   // equal
}

void UpdateChecker::check() {
    QNetworkRequest req{QUrl(releasesApiUrl())};
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent",
                     QByteArrayLiteral("Lyra/") + LYRA_VERSION);
    req.setTransferTimeout(8000);   // don't hang the check on a dead link

    QNetworkReply *reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit checkFailed(reply->errorString());
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isArray()) {
            emit checkFailed(tr("Unexpected response from GitHub."));
            return;
        }
        // Pick the highest non-draft release version.
        QString bestTag, bestUrl, bestBody;
        const QString cur = QStringLiteral(LYRA_VERSION);
        for (const QJsonValue &v : doc.array()) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("draft")).toBool()) continue;
            const QString tag = o.value(QStringLiteral("tag_name")).toString();
            if (tag.isEmpty()) continue;
            // Newer than current AND newer than the best so far.
            if (isNewer(tag, cur) &&
                (bestTag.isEmpty() || isNewer(tag, bestTag))) {
                bestTag  = tag;
                bestUrl  = o.value(QStringLiteral("html_url")).toString();
                bestBody = o.value(QStringLiteral("body")).toString();
            }
        }
        if (bestTag.isEmpty()) {
            emit noUpdate();
        } else {
            if (bestUrl.isEmpty()) {
                bestUrl = QStringLiteral("https://github.com/%1/%2/releases")
                              .arg(QStringLiteral(LYRA_REPO_OWNER),
                                   QStringLiteral(LYRA_REPO_NAME));
            }
            emit updateAvailable(bestTag, bestUrl, bestBody);
        }
    });
}

} // namespace lyra::ui
