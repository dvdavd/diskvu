// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "nodepropertiesdialog.h"
#include "treemapwidget.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>

// ProportionBar
// Paints a filled/unfilled pill bar scaled to `ratio` in the node's color.

namespace {

class ProportionBar : public QWidget {
public:
    ProportionBar(double ratio, QColor color, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_ratio(std::clamp(ratio, 0.0, 1.0))
        , m_color(std::move(color))
    {
        setFixedHeight(10);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override { return { 200, 10 }; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QRectF r = QRectF(rect()).adjusted(0, 1.5, 0, -1.5);
        const double radius = r.height() / 2.0;

        // Track
        QPainterPath track;
        track.addRoundedRect(r, radius, radius);
        p.fillPath(track, palette().color(QPalette::Window).darker(130));

        // Filled portion — show at least 4 px so tiny values are visible
        if (m_ratio > 0.0) {
            const double filled = std::max(4.0, r.width() * m_ratio);
            QPainterPath fp;
            fp.addRoundedRect(QRectF(r.x(), r.y(), filled, r.height()), radius, radius);
            p.fillPath(fp, m_color);
        }
    }

private:
    double m_ratio;
    QColor m_color;
};

//  Helpers

static void countItems(const FileNode* node, int& files, int& dirs)
{
    for (const FileNode* child : node->children) {
        if (child->isVirtual)
            continue;
        if (child->isDirectory) {
            ++dirs;
            countItems(child, files, dirs);
        } else {
            ++files;
        }
    }
}

static QString permissionsString(QFileDevice::Permissions perms)
{
    auto ch = [](bool b, char c) { return b ? QLatin1Char(c) : QLatin1Char('-'); };
    QString s;
    s += ch(perms & QFileDevice::ReadOwner, 'r');
    s += ch(perms & QFileDevice::WriteOwner, 'w');
    s += ch(perms & QFileDevice::ExeOwner, 'x');
    s += ch(perms & QFileDevice::ReadGroup, 'r');
    s += ch(perms & QFileDevice::WriteGroup, 'w');
    s += ch(perms & QFileDevice::ExeGroup, 'x');
    s += ch(perms & QFileDevice::ReadOther, 'r');
    s += ch(perms & QFileDevice::WriteOther, 'w');
    s += ch(perms & QFileDevice::ExeOther, 'x');
    return s;
}

static int permissionsOctal(QFileDevice::Permissions perms)
{
    int v = 0;
    if (perms & QFileDevice::ReadOwner)
        v |= 0400;
    if (perms & QFileDevice::WriteOwner)
        v |= 0200;
    if (perms & QFileDevice::ExeOwner)
        v |= 0100;
    if (perms & QFileDevice::ReadGroup)
        v |= 0040;
    if (perms & QFileDevice::WriteGroup)
        v |= 0020;
    if (perms & QFileDevice::ExeGroup)
        v |= 0010;
    if (perms & QFileDevice::ReadOther)
        v |= 0004;
    if (perms & QFileDevice::WriteOther)
        v |= 0002;
    if (perms & QFileDevice::ExeOther)
        v |= 0001;
    return v;
}

static QString pctString(double ratio)
{
    if (ratio >= 0.9995)
        return QStringLiteral("100%");
    if (ratio < 0.0005)
        return QStringLiteral("<0.1%");
    return QStringLiteral("%1%").arg(ratio * 100.0, 0, 'f', 1);
}

// Returns a widget containing [bar ──────] XX.X%
static QWidget* barRow(double ratio, const QColor& color, QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 2, 0, 2);
    h->setSpacing(8);

    h->addWidget(new ProportionBar(ratio, color, w), 1);

    auto* lbl = new QLabel(pctString(ratio), w);
    lbl->setFixedWidth(44);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    h->addWidget(lbl);

    return w;
}

// ElidedLabel: re-elides middle of fullText whenever width changes.
class ElidedLabel : public QLabel {
public:
    ElidedLabel(const QString& fullText, QWidget* parent = nullptr)
        : QLabel(parent)
        , m_fullText(fullText)
    {
        setToolTip(fullText);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setTextInteractionFlags(Qt::TextSelectableByMouse);
        setMinimumWidth(0);
    }

protected:
    void resizeEvent(QResizeEvent* e) override
    {
        QLabel::resizeEvent(e);
        setText(QFontMetrics(font()).elidedText(m_fullText, Qt::ElideMiddle, width()));
    }

private:
    QString m_fullText;
};

} // namespace

// NodePropertiesDialog

NodePropertiesDialog::NodePropertiesDialog(const FileNode* node,
    const FileNode* scanRoot,
    std::shared_ptr<NodeArena> arena,
    const TreemapSettings& settings,
    QWidget* parent)
    : QDialog(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::NonModal);
    setWindowTitle(tr("Properties — %1").arg(node->name));
    setMinimumWidth(440);

    const QString path = node->computePath();
    const QFileInfo info(path);

    auto* vbox = new QVBoxLayout(this);
    vbox->setSpacing(10);

    // Header
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(10);

        auto* icon = new QLabel(this);
        const QStyle::StandardPixmap sp = node->isDirectory ? QStyle::SP_DirIcon
                                                            : QStyle::SP_FileIcon;
        icon->setPixmap(style()->standardIcon(sp).pixmap(32, 32));
        icon->setFixedSize(36, 36);
        icon->setAlignment(Qt::AlignCenter);
        row->addWidget(icon);

        auto* name = new QLabel(node->name, this);
        QFont f = name->font();
        f.setPointSize(f.pointSize() + 2);
        f.setBold(true);
        name->setFont(f);
        name->setWordWrap(true);
        name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row->addWidget(name, 1);

        vbox->addLayout(row);
    }

    // Separator hepler
    auto sep = [&] {
        auto* line = new QFrame(this);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        vbox->addWidget(line);
    };
    sep();

    // Treemap preview (directories with children only)
    const bool hasTreemap = node->isDirectory && !node->children.empty() && node->size > 0;
    if (hasTreemap) {
        auto* treemap = new TreemapWidget(this);
        treemap->setMinimumHeight(200);
        treemap->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        treemap->setWheelZoomEnabled(false);
        treemap->setAttribute(Qt::WA_TransparentForMouseEvents);
        treemap->applySettings(settings);
        treemap->setRoot(const_cast<FileNode*>(node), std::move(arena), false, false);
        vbox->addWidget(treemap, 1);
        sep();
    }

    // Form
    {
        auto* form = new QFormLayout;
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        form->setSpacing(6);
        form->setHorizontalSpacing(12);

        auto addText = [&](const QString& label, const QString& value) {
            auto* v = new QLabel(value, this);
            v->setTextInteractionFlags(Qt::TextSelectableByMouse);
            form->addRow(label, v);
        };

        // Path label with copy button
        {
            auto* row = new QWidget(this);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(4);

            auto* lbl = new ElidedLabel(path, row);
            h->addWidget(lbl, 1);

            auto* copyBtn = new QToolButton(row);
            copyBtn->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
            copyBtn->setIcon(QIcon::fromTheme(QStringLiteral("edit-copy"),
                style()->standardIcon(QStyle::SP_DialogSaveButton)));
            copyBtn->setToolTip(tr("Copy full path"));
            copyBtn->setAutoRaise(true);
            copyBtn->setFixedSize(20, 20);
            connect(copyBtn, &QToolButton::clicked, this, [path] {
                QApplication::clipboard()->setText(path);
            });
            h->addWidget(copyBtn);

            form->addRow(tr("Path"), row);
        }

        // Size
        const QString humanSize = QLocale::system().formattedDataSize(node->size);
        const QString byteCount = tr("%1 bytes").arg(QLocale::system().toString(node->size));
        addText(tr("Size"), QStringLiteral("%1  (%2)").arg(humanSize, byteCount));

        // Proportion bars — use system highlight color for consistent contrast
        const QColor barColor = palette().color(QPalette::Highlight);

        if (node->parent && node->parent->size > 0) {
            const double r = static_cast<double>(node->size) / node->parent->size;
            form->addRow(tr("Of parent"), barRow(r, barColor, this));
        }

        if (scanRoot && scanRoot != node && scanRoot->size > 0) {
            const double r = static_cast<double>(node->size) / scanRoot->size;
            form->addRow(tr("Of root"), barRow(r, barColor, this));
        }

        // Items (directories)
        if (node->isDirectory) {
            int files = 0, dirs = 0;
            countItems(node, files, dirs);
            addText(tr("Items"),
                tr("%1 files, %2 folders")
                    .arg(QLocale::system().toString(files),
                        QLocale::system().toString(dirs)));
        }

        // Type
        if (node->isVirtual) {
            addText(tr("Type"), tr("Free space"));
        } else if (node->isDirectory) {
            addText(tr("Type"), tr("Folder"));
        } else {
            const QMimeType mime = QMimeDatabase().mimeTypeForFile(
                path, QMimeDatabase::MatchExtension);
            addText(tr("Type"), mime.isValid() ? mime.comment() : tr("File"));
        }

        // Modified, permissions, owner (not meaningful for virtual nodes)
        if (!node->isVirtual && info.exists()) {
            addText(tr("Modified"),
                QLocale::system().toString(info.lastModified(), QLocale::ShortFormat));

#ifndef Q_OS_WIN
            const auto perms = info.permissions();
            addText(tr("Permissions"),
                QStringLiteral("%1  (%2)")
                    .arg(permissionsString(perms))
                    .arg(permissionsOctal(perms), 4, 8, QLatin1Char('0')));

            const QString owner = info.owner();
            const QString group = info.group();
            if (!owner.isEmpty() || !group.isEmpty()) {
                addText(tr("Owner"),
                    owner.isEmpty() ? group : group.isEmpty() ? owner
                                                              : owner + QLatin1Char(':') + group);
            }
#endif
        }

        vbox->addLayout(form);
    }

    if (!hasTreemap)
        vbox->addStretch(1);

    sep();

    {
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        vbox->addWidget(buttons);
    }

    adjustSize();
}
