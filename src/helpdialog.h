// Lyra — in-app User Guide viewer.
//
// 2-column layout: a clickable TOC tree (QTreeWidget) on the left, the
// rendered USER_GUIDE.md (QTextBrowser) on the right, separated by a
// drag-resizable QSplitter.  Window geometry + splitter position are
// persisted across launches via QSettings (helpdialog/geometry,
// helpdialog/splitterState).  The dialog has min/max/restore buttons
// on the title bar and is fully resizable.
//
// Navigation: clicking a TOC item moves the browser's cursor to the
// document start and calls QTextBrowser::find(<heading-text>), then
// nudges the matched line to the top of the viewport.  This bypasses
// Qt's setMarkdown() → QTextDocument anchor-name path (which doesn't
// generate slug-named anchors from headings) and is the proven
// mechanism showTopic() already used.
//
// In-document body cross-reference links of the form `[label](#slug)`
// are intercepted (anchorClicked signal), routed through a
// slug → heading-text map built at load time, and dispatched to the
// same find()-based navigator.  External http/https/mailto links open
// in the system browser.

#pragma once

#include <QDialog>
#include <QHash>
#include <QString>

class QSplitter;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

namespace lyra::ui {

class HelpDialog : public QDialog {
    Q_OBJECT
public:
    explicit HelpDialog(QWidget *parent = nullptr);
    ~HelpDialog() override;

    // Show the guide scrolled to <topic>'s section (empty = top).
    // Used by per-panel "?" badges; topic strings are mapped to exact
    // heading text inside helpdialog.cpp's topicHeads().
    void showTopic(const QString &topic);

private:
    void loadGuide();
    // Parse the raw markdown source into an ordered list of
    // (level, headingText) pairs.  Hands the result to buildToc().
    void buildToc(const QString &markdownSrc);
    // Scroll the right-pane browser to the heading line whose exact
    // text matches `headingText`.  Uses find(); deferred behind a
    // 0 ms QTimer::singleShot so first-open layout has settled.
    void scrollToHeading(const QString &headingText);

    // Persist + restore the dialog's window geometry and splitter
    // position so reopening lands the operator on the size + layout
    // they last left.
    void saveLayout() const;
    void restoreLayout();

    QSplitter    *split_   = nullptr;
    QTreeWidget  *toc_     = nullptr;
    QTextBrowser *browser_ = nullptr;

    // `<a href="#slug">` body links → heading text.  Populated by
    // buildToc() so the anchorClicked handler can route to the
    // find()-based scroller without depending on QTextDocument
    // anchor support.
    QHash<QString, QString> slugToHeading_;

protected:
    // Save layout on close so the next open restores it.
    void closeEvent(QCloseEvent *e) override;
};

} // namespace lyra::ui
