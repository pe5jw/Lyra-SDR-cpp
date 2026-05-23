// Lyra — in-app User Guide viewer.  See helpdialog.h.

#include "helpdialog.h"

#include <QFile>
#include <QFont>
#include <QHash>
#include <QPalette>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextStream>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

namespace lyra::ui {

namespace {
// Map a panel help topic -> the exact heading text in USER_GUIDE.md.
// QTextBrowser::find() locates + scrolls to the heading, which is more
// robust than relying on markdown-generated anchors.
const QHash<QString, QString> &topicHeads() {
    // Use a short, unambiguous substring of each section heading (the
    // first occurrence in the doc is the heading itself, since these
    // phrases appear there before anywhere in body text).
    static const QHash<QString, QString> m = {
        {QStringLiteral("getting-started"),
         QStringLiteral("Getting around the window")},
        {QStringLiteral("panadapter"),
         QStringLiteral("The panadapter (spectrum display)")},
        {QStringLiteral("visuals"), QStringLiteral("Settings → Visuals")},
        {QStringLiteral("tuning"), QStringLiteral("Tuning panel")},
        {QStringLiteral("audio"), QStringLiteral("Audio panel")},
        {QStringLiteral("display"), QStringLiteral("Display panel")},
        {QStringLiteral("band"), QStringLiteral("Band panel")},
    };
    return m;
}
} // namespace

HelpDialog::HelpDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Lyra — User Guide"));
    resize(760, 660);

    browser_ = new QTextBrowser(this);
    browser_->setOpenExternalLinks(true);

    // Larger base text — the default ~9 pt is cramped for a reading
    // document.  Markdown headings scale relative to this, so the whole
    // guide gets a comfortable bump.
    QFont gf = browser_->font();
    gf.setPointSize(12);
    browser_->setFont(gf);

    // Readable colours on the dark background.  Links default to a dark
    // blue (QPalette::Link) that's near-invisible on black — set them to
    // the Lyra cyan accent.  Body text light, base matches the theme.
    QPalette pal = browser_->palette();
    pal.setColor(QPalette::Base, QColor(0x0a, 0x0d, 0x12));
    pal.setColor(QPalette::Text, QColor(0xd8, 0xe4, 0xee));
    pal.setColor(QPalette::Link, QColor(0x5e, 0xc8, 0xff));        // cyan
    pal.setColor(QPalette::LinkVisited, QColor(0x8f, 0xd6, 0xff));
    browser_->setPalette(pal);
    // Belt-and-suspenders for the markdown render path (headings + any
    // CSS-styled anchors).
    browser_->document()->setDefaultStyleSheet(
        QStringLiteral("a { color: #5ec8ff; }"
                       "h1, h2, h3 { color: #00e5ff; }"));

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(browser_);

    loadGuide();
}

void HelpDialog::loadGuide() {
    QFile f(QStringLiteral(":/help/USER_GUIDE.md"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream ts(&f);
        browser_->setMarkdown(ts.readAll());
    } else {
        browser_->setPlainText(
            tr("User guide not found (:/help/USER_GUIDE.md)."));
    }
}

void HelpDialog::showTopic(const QString &topic) {
    show();
    raise();
    activateWindow();

    const QString head = topicHeads().value(topic);
    // Defer the scroll (~60 ms) so the markdown's final layout has
    // settled — on the first open an immediate scroll gets reset by a
    // late layout pass.  Each heading phrase ALSO appears in the
    // Contents list near the top, so the FIRST find() lands on that
    // link; the SECOND find() lands on the real section heading.
    QTimer::singleShot(60, this, [this, head]() {
        if (head.isEmpty())
            return;
        browser_->moveCursor(QTextCursor::Start);
        if (!browser_->find(head))         // 1st hit = Contents link
            return;
        const QTextCursor contentsHit = browser_->textCursor();
        if (!browser_->find(head))         // 2nd hit = the heading
            browser_->setTextCursor(contentsHit);  // fallback: only one
        QTextCursor c = browser_->textCursor();
        c.clearSelection();
        browser_->setTextCursor(c);
        browser_->ensureCursorVisible();
        // Nudge the section heading up to near the top of the viewport
        // (ensureCursorVisible only guarantees it's somewhere visible).
        const int top = browser_->cursorRect().top();
        auto *vb = browser_->verticalScrollBar();
        vb->setValue(vb->value() + top - 8);
    });
}

} // namespace lyra::ui
