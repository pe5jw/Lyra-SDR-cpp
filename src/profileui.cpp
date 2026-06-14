// Lyra — ProfileUi.  See profileui.h.

#include "profileui.h"

#include "prefs.h"
#include "profile/ProfileManager.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QVBoxLayout>

namespace lyra::ui {

ProfileUi::ProfileUi(lyra::profile::ProfileManager *mgr, Prefs *prefs,
                     QWidget *parent)
    : QObject(parent), mgr_(mgr), prefs_(prefs), parent_(parent) {}

void ProfileUi::openSaveDialog() {
    if (!mgr_) return;
    const QString active = mgr_->activeName();

    QDialog dlg(parent_);
    dlg.setWindowTitle(tr("Save Profile"));
    auto *col = new QVBoxLayout(&dlg);

    // Overwrite vs. new.  When a profile is active, default to overwrite;
    // otherwise there's nothing to overwrite, so it's new-only.
    QRadioButton *overwriteRb = nullptr;
    QRadioButton *newRb       = nullptr;
    auto *nameEdit = new QLineEdit(&dlg);
    nameEdit->setPlaceholderText(tr("New profile name"));

    if (!active.isEmpty()) {
        overwriteRb = new QRadioButton(tr("Overwrite “%1”").arg(active), &dlg);
        newRb       = new QRadioButton(tr("Save as a new profile:"), &dlg);
        overwriteRb->setChecked(true);
        nameEdit->setEnabled(false);
        col->addWidget(overwriteRb);
        col->addWidget(newRb);
        connect(newRb, &QRadioButton::toggled, nameEdit, &QLineEdit::setEnabled);
        connect(newRb, &QRadioButton::toggled, &dlg, [nameEdit](bool on) {
            if (on) nameEdit->setFocus();
        });
    } else {
        col->addWidget(new QLabel(tr("Save as a new profile:"), &dlg));
    }
    col->addWidget(nameEdit);

    // Optional auto-recall binding — same families as Settings → Profiles.
    // Default to the current mode's family ("this profile is for the mode
    // I'm on"); "(none)" leaves bindings alone.
    col->addWidget(new QLabel(tr("Auto-recall for mode family:"), &dlg));
    auto *familyCombo = new QComboBox(&dlg);
    familyCombo->addItem(tr("(none)"));
    familyCombo->addItems(lyra::profile::ProfileManager::modeFamilies());
    if (prefs_) {
        const QString fam = lyra::profile::ProfileManager::modeFamily(prefs_->mode());
        const int fi = familyCombo->findText(fam);
        if (fi >= 0) familyCombo->setCurrentIndex(fi);
    }
    col->addWidget(familyCombo);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    col->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    const bool overwrite = overwriteRb && overwriteRb->isChecked();
    const QString name = overwrite ? active : nameEdit->text().trimmed();
    if (name.isEmpty()) return;          // nothing to save under

    mgr_->saveAs(name);                  // create-or-overwrite + make active
    // index 0 == "(none)" — only bind when a real family is picked.
    if (familyCombo->currentIndex() > 0)
        mgr_->bindMode(familyCombo->currentText(), name);
}

}  // namespace lyra::ui
