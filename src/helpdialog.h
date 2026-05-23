// Lyra — in-app User Guide viewer.
//
// Renders docs/help/USER_GUIDE.md (bundled as a Qt resource at
// :/help/USER_GUIDE.md) in a QTextBrowser.  showTopic() scrolls to the
// section that matches a panel's help topic, so the per-panel "?" badge
// lands the operator on the right place in the guide.  One persistent
// dialog is reused so reopening doesn't lose scroll position.

#pragma once

#include <QDialog>

class QTextBrowser;

namespace lyra::ui {

class HelpDialog : public QDialog {
    Q_OBJECT
public:
    explicit HelpDialog(QWidget *parent = nullptr);

    // Show the guide scrolled to <topic>'s section (empty = top).
    void showTopic(const QString &topic);

private:
    void loadGuide();

    QTextBrowser *browser_ = nullptr;
};

} // namespace lyra::ui
