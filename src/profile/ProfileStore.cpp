// Lyra — ProfileStore.  See ProfileStore.h.

#include "profile/ProfileStore.h"

#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>

namespace lyra::profile {

QString ProfileStore::itemKey(const QString &name) const {
    return QStringLiteral("profiles/item/") + name;
}

QStringList ProfileStore::names() const {
    return s_->value(QStringLiteral("profiles/order")).toStringList();
}

bool ProfileStore::contains(const QString &name) const {
    return s_->contains(itemKey(name));
}

Profile ProfileStore::get(const QString &name) const {
    const QString json = s_->value(itemKey(name)).toString();
    if (json.isEmpty()) return Profile{};
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    return Profile::fromJson(name, o);
}

void ProfileStore::put(const Profile &p) {
    const QJsonDocument doc(p.toJson());
    s_->setValue(itemKey(p.name),
                 QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
    QStringList order = names();
    if (!order.contains(p.name)) {
        order.append(p.name);
        s_->setValue(QStringLiteral("profiles/order"), order);
    }
}

void ProfileStore::remove(const QString &name) {
    s_->remove(itemKey(name));
    QStringList order = names();
    if (order.removeAll(name) > 0)
        s_->setValue(QStringLiteral("profiles/order"), order);
    if (active() == name)      s_->remove(QStringLiteral("profiles/active"));
    if (defaultName() == name) s_->remove(QStringLiteral("profiles/default"));
    // Drop any mode bindings pointing at the removed profile.
    s_->beginGroup(QStringLiteral("profiles/modeBind"));
    const QStringList modes = s_->childKeys();
    for (const QString &m : modes)
        if (s_->value(m).toString() == name) s_->remove(m);
    s_->endGroup();
}

void ProfileStore::rename(const QString &oldName, const QString &newName) {
    if (oldName == newName || !contains(oldName)) return;
    Profile p = get(oldName);
    p.name = newName;
    // Insert the new name at the old slot's position to preserve order.
    QStringList order = names();
    const int idx = order.indexOf(oldName);
    s_->setValue(itemKey(newName),
                 QString::fromUtf8(QJsonDocument(p.toJson()).toJson(QJsonDocument::Compact)));
    s_->remove(itemKey(oldName));
    if (idx >= 0) { order[idx] = newName; s_->setValue(QStringLiteral("profiles/order"), order); }
    if (active() == oldName)      setActive(newName);
    if (defaultName() == oldName) setDefault(newName);
    s_->beginGroup(QStringLiteral("profiles/modeBind"));
    const QStringList modes = s_->childKeys();
    for (const QString &m : modes)
        if (s_->value(m).toString() == oldName) s_->setValue(m, newName);
    s_->endGroup();
}

QString ProfileStore::active() const {
    return s_->value(QStringLiteral("profiles/active")).toString();
}
void ProfileStore::setActive(const QString &name) {
    s_->setValue(QStringLiteral("profiles/active"), name);
}
QString ProfileStore::defaultName() const {
    return s_->value(QStringLiteral("profiles/default")).toString();
}
void ProfileStore::setDefault(const QString &name) {
    s_->setValue(QStringLiteral("profiles/default"), name);
}

QString ProfileStore::modeBinding(const QString &mode) const {
    return s_->value(QStringLiteral("profiles/modeBind/") + mode).toString();
}
void ProfileStore::setModeBinding(const QString &mode, const QString &name) {
    s_->setValue(QStringLiteral("profiles/modeBind/") + mode, name);
}
void ProfileStore::clearModeBinding(const QString &mode) {
    s_->remove(QStringLiteral("profiles/modeBind/") + mode);
}

}  // namespace lyra::profile
