#include "SongNameView.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QLabel>
#include <QFontDatabase>
#include <cstring>

extern "C" {
#include "gcommon.h"
extern char songname[MAX_STR];
extern char authorname[MAX_STR];
extern char copyrightname[MAX_STR];
}

static void writeStr(char *dst, const QString &s) {
    QByteArray ba = s.toLocal8Bit();
    int n = qMin<int>(MAX_STR - 1, ba.size());
    std::memcpy(dst, ba.constData(), n);
    dst[n] = 0;
    for (int i = n + 1; i < MAX_STR; i++) dst[i] = 0;
}

SongNameView::SongNameView(QWidget *parent) : QWidget(parent) {
    setAccessibleName("Song name editor");
    Theme::applyDarkPalette(this);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(32, 24, 32, 24);
    layout->setSpacing(12);

    auto *header = new QLabel("Song metadata", this);
    QFont hf = header->font();
    hf.setPointSize(14);
    hf.setBold(true);
    QPalette hp = header->palette();
    hp.setColor(QPalette::WindowText, Theme::C::highlight);
    header->setPalette(hp);
    header->setFont(hf);
    layout->addWidget(header);

    auto *form = new QFormLayout();
    form->setSpacing(10);

    QFont mono = Theme::monoFont(12);

    title_ = new QLineEdit(this);
    title_->setMaxLength(MAX_STR - 1);
    title_->setFont(mono);
    author_ = new QLineEdit(this);
    author_->setMaxLength(MAX_STR - 1);
    author_->setFont(mono);
    copyright_ = new QLineEdit(this);
    copyright_->setMaxLength(MAX_STR - 1);
    copyright_->setFont(mono);

    form->addRow("Title:", title_);
    form->addRow("Author:", author_);
    form->addRow("Copyright:", copyright_);
    layout->addLayout(form);
    layout->addStretch();

    connect(title_, &QLineEdit::textEdited, this, &SongNameView::onTitleChanged);
    connect(author_, &QLineEdit::textEdited, this, &SongNameView::onAuthorChanged);
    connect(copyright_, &QLineEdit::textEdited, this, &SongNameView::onCopyrightChanged);
}

void SongNameView::refresh() {
    title_->setText(QString::fromLocal8Bit(songname));
    author_->setText(QString::fromLocal8Bit(authorname));
    copyright_->setText(QString::fromLocal8Bit(copyrightname));
}

void SongNameView::onTitleChanged(const QString &t) {
    writeStr(songname, t);
    emit edited();
}
void SongNameView::onAuthorChanged(const QString &t) {
    writeStr(authorname, t);
    emit edited();
}
void SongNameView::onCopyrightChanged(const QString &t) {
    writeStr(copyrightname, t);
    emit edited();
}
