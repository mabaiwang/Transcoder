#include "TranscoderWindow.h"

#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStringList>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>

namespace {
class ChinesePlainTextEdit : public QPlainTextEdit
{
public:
    explicit ChinesePlainTextEdit(QWidget *parent = nullptr)
        : QPlainTextEdit(parent)
    {
    }

protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        QMenu *menu = createStandardContextMenu();
        localizeMenu(menu);
        menu->exec(event->globalPos());
        delete menu;
    }

private:
    static QString normalizeActionText(QString text)
    {
        const int tabPos = text.indexOf(QLatin1Char('\t'));
        if (tabPos >= 0) {
            text = text.left(tabPos);
        }
        return text.remove(QLatin1Char('&')).trimmed();
    }

    static void localizeMenu(QMenu *menu)
    {
        if (!menu) {
            return;
        }

        for (QAction *action : menu->actions()) {
            if (QMenu *subMenu = action->menu()) {
                localizeMenu(subMenu);
                continue;
            }

            const QString key = normalizeActionText(action->text());
            if (key == QStringLiteral("Undo")) {
                action->setText(QStringLiteral("撤销"));
            } else if (key == QStringLiteral("Redo")) {
                action->setText(QStringLiteral("重做"));
            } else if (key == QStringLiteral("Cut")) {
                action->setText(QStringLiteral("剪切"));
            } else if (key == QStringLiteral("Copy")) {
                action->setText(QStringLiteral("复制"));
            } else if (key == QStringLiteral("Paste")) {
                action->setText(QStringLiteral("粘贴"));
            } else if (key == QStringLiteral("Delete")) {
                action->setText(QStringLiteral("删除"));
            } else if (key == QStringLiteral("Select All")) {
                action->setText(QStringLiteral("全选"));
            }
        }
    }
};

QString toHexHash(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex()).toUpper();
}

QString toGroupedBytes(const QByteArray &bytes, int base, int width, const QString &separator)
{
    QStringList parts;
    parts.reserve(bytes.size());
    for (unsigned char value : bytes) {
        parts << QStringLiteral("%1").arg(value, width, base, QLatin1Char('0')).toUpper();
    }
    return parts.join(separator);
}

QString decodeCodePoints(const QList<uint> &codePoints)
{
    QString result;
    for (uint codePoint : codePoints) {
        if (codePoint > 0x10FFFF) {
            return {};
        }

        if (codePoint <= 0xFFFF) {
            result.append(QChar(static_cast<ushort>(codePoint)));
            continue;
        }

        const char32_t scalar = static_cast<char32_t>(codePoint);
        result.append(QString::fromUcs4(&scalar, 1));
    }
    return result;
}

QString normalizeBase64(QString text)
{
    text.remove(QRegularExpression(QStringLiteral("\\s+")));
    text.replace(QLatin1Char('-'), QLatin1Char('+'));
    text.replace(QLatin1Char('_'), QLatin1Char('/'));
    const int mod = text.size() % 4;
    if (mod != 0) {
        text.append(QString(4 - mod, QLatin1Char('=')));
    }
    return text;
}

QString normalizedHex(QString text)
{
    text.remove(QRegularExpression(QStringLiteral("^0x"), QRegularExpression::CaseInsensitiveOption));
    text.remove(QRegularExpression(QStringLiteral("[^0-9A-Fa-f]")));
    return text;
}

bool isBase64Payload(const QString &value)
{
    static const QRegularExpression allowed(QStringLiteral("^[A-Za-z0-9+/=]*$"));
    return allowed.match(value).hasMatch();
}

ChinesePlainTextEdit *createTextEdit(QWidget *parent)
{
    auto *editor = new ChinesePlainTextEdit(parent);
    editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    return editor;
}
}

TranscoderWindow::TranscoderWindow(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
    refreshOutputs();
}

void TranscoderWindow::buildUi()
{
    setWindowTitle(QStringLiteral("转码工具"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.ico")));
    resize(1040, 780);
    setMinimumSize(920, 650);

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(14, 14, 14, 14);
    rootLayout->setSpacing(10);

    auto *sourceCard = new QFrame(this);
    sourceCard->setObjectName(QStringLiteral("sourceCard"));
    auto *sourceLayout = new QVBoxLayout(sourceCard);
    sourceLayout->setContentsMargins(14, 10, 14, 10);
    sourceLayout->setSpacing(5);

    auto *sourceHeader = new QHBoxLayout;
    auto *sourceTitle = new QLabel(QStringLiteral("原文本"), sourceCard);
    sourceTitle->setObjectName(QStringLiteral("panelTitle"));

    auto *pasteButton = new QPushButton(QStringLiteral("粘贴"), sourceCard);
    auto *copySourceButton = new QPushButton(QStringLiteral("复制"), sourceCard);
    auto *clearButton = new QPushButton(QStringLiteral("清空"), sourceCard);
    m_themeButton = new QPushButton(QStringLiteral("暗黑/浅色"), sourceCard);
    m_editorRowsSpin = new QSpinBox(sourceCard);
    m_editorRowsSpin->setRange(1, 20);
    m_editorRowsSpin->setValue(1);
    m_editorRowsSpin->setToolTip(QStringLiteral("统一调节所有编辑框显示行数"));
    auto *rowsLabel = new QLabel(QStringLiteral("编辑框行数"), sourceCard);
    rowsLabel->setObjectName(QStringLiteral("hintLabel"));
    pasteButton->setCursor(Qt::PointingHandCursor);
    copySourceButton->setCursor(Qt::PointingHandCursor);
    clearButton->setCursor(Qt::PointingHandCursor);
    m_themeButton->setCursor(Qt::PointingHandCursor);

    sourceHeader->addWidget(sourceTitle);
    sourceHeader->addStretch();
    sourceHeader->addWidget(rowsLabel);
    sourceHeader->addWidget(m_editorRowsSpin);
    sourceHeader->addWidget(m_themeButton);
    sourceHeader->addWidget(pasteButton);
    sourceHeader->addWidget(copySourceButton);
    sourceHeader->addWidget(clearButton);

    m_sourceEdit = createTextEdit(sourceCard);
    m_sourceEdit->setObjectName(QStringLiteral("sourceEdit"));
    m_sourceEdit->setPlaceholderText(QStringLiteral("例如: 123.45"));
    m_sourceEdit->document()->setDocumentMargin(0);
    m_allEditors.append(m_sourceEdit);
    
    // 为原文本框添加textChanged信号连接，自动调整高度
    connect(m_sourceEdit, &QPlainTextEdit::textChanged, this, [this]() {
        adjustEditorHeight(m_sourceEdit);
    });

    sourceLayout->addLayout(sourceHeader);
    sourceLayout->addWidget(m_sourceEdit);

    struct PanelDef {
        PanelKey key;
        const char *title;
        const char *hint;
        bool reversible;
    };

    const QList<PanelDef> panels = {
        {PanelKey::Url, "URL格式", "百分号编码。", true},
        {PanelKey::Sql, "SQL_En", "UTF-16LE Hex（0x...）格式。", true},
        {PanelKey::Hex, "Hex", "UTF-8 字节十六进制（0x...）。", true},
        {PanelKey::Ascii, "Asc", "UTF-8 字节十进制序列。", true},
        {PanelKey::Base64, "Base64", "标准 Base64 与 URL 安全 Base64。", true},
        {PanelKey::Binary, "Binary", "UTF-8 字节二进制（8 位分组）。", true},
        {PanelKey::Unicode, "Unicode", "\\uXXXX / \\UXXXXXXXX。", true},
        {PanelKey::Html, "HTML Entity", "HTML 字符实体转义。", true},
        {PanelKey::Md5_32, "MD5_32", "32 位大写摘要。", false},
        {PanelKey::Sha1, "SHA1", "160 位摘要。", false},
        {PanelKey::Sha256, "SHA256", "256 位摘要。", false},
        {PanelKey::Sha512, "SHA512", "512 位摘要。", false}
    };

    // 创建顶部转码框区域
    auto *topResultContainer = new QWidget(this);
    auto *topGrid = new QGridLayout(topResultContainer);
    topGrid->setContentsMargins(2, 2, 2, 2);
    topGrid->setHorizontalSpacing(10);
    topGrid->setVerticalSpacing(10);

    // 创建底部转码框区域
    auto *bottomResultContainer = new QWidget(this);
    auto *bottomGrid = new QGridLayout(bottomResultContainer);
    bottomGrid->setContentsMargins(2, 2, 2, 2);
    bottomGrid->setHorizontalSpacing(10);
    bottomGrid->setVerticalSpacing(10);

    // 将转码框分为上下两部分
    const int halfPanels = panels.size() / 2;
    for (int i = 0; i < panels.size(); ++i) {
        const auto &panel = panels.at(i);
        if (i < halfPanels) {
            topGrid->addWidget(createPanel(panel.key,
                                        QString::fromUtf8(panel.title),
                                        QString::fromUtf8(panel.hint),
                                        panel.reversible),
                            i / 2,
                            i % 2);
        } else {
            bottomGrid->addWidget(createPanel(panel.key,
                                        QString::fromUtf8(panel.title),
                                        QString::fromUtf8(panel.hint),
                                        panel.reversible),
                            (i - halfPanels) / 2,
                            (i - halfPanels) % 2);
        }
    }

    auto *topResultScroll = new QScrollArea(this);
    topResultScroll->setWidgetResizable(true);
    topResultScroll->setFrameShape(QFrame::NoFrame);
    topResultScroll->setWidget(topResultContainer);

    auto *bottomResultScroll = new QScrollArea(this);
    bottomResultScroll->setWidgetResizable(true);
    bottomResultScroll->setFrameShape(QFrame::NoFrame);
    bottomResultScroll->setWidget(bottomResultContainer);

    // 创建垂直分割器，将窗口分为上、中、下三部分
    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(topResultScroll);
    splitter->addWidget(sourceCard);
    splitter->addWidget(bottomResultScroll);
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, false);
    splitter->setCollapsible(2, false);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setStretchFactor(2, 1);
    splitter->setSizes({300, 120, 300});

    rootLayout->addWidget(splitter, 1);

    connect(m_sourceEdit, &QPlainTextEdit::textChanged, this, &TranscoderWindow::refreshOutputs);
    connect(pasteButton, &QPushButton::clicked, this, [this]() {
        m_sourceEdit->setPlainText(QGuiApplication::clipboard()->text());
        setStatus(QStringLiteral("已从剪贴板填充原文本。"));
    });
    connect(copySourceButton, &QPushButton::clicked, this, [this]() {
        QGuiApplication::clipboard()->setText(m_sourceEdit->toPlainText());
        setStatus(QStringLiteral("原文本已复制。"));
    });
    connect(clearButton, &QPushButton::clicked, this, [this]() {
        m_sourceEdit->clear();
        setStatus(QStringLiteral("已清空原文本。"));
    });
    connect(m_themeButton, &QPushButton::clicked, this, &TranscoderWindow::toggleTheme);
    connect(m_editorRowsSpin, qOverload<int>(&QSpinBox::valueChanged), this, &TranscoderWindow::applyEditorRows);

    applyTheme();
    applyEditorRows(m_editorRowsSpin->value());
}

QWidget *TranscoderWindow::createPanel(PanelKey key, const QString &title, const QString &hint, bool reversible)
{
    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("panelCard"));
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(6);

    auto *header = new QHBoxLayout;
    auto *titleContainer = new QWidget(card);
    auto *titleLayout = new QHBoxLayout(titleContainer);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(8);
    
    auto *titleLabel = new QLabel(title, card);
    titleLabel->setObjectName(QStringLiteral("panelTitle"));
    
    auto *hintLabel = new QLabel(hint, card);
    hintLabel->setObjectName(QStringLiteral("hintLabel"));
    
    titleLayout->addWidget(titleLabel);
    titleLayout->addWidget(hintLabel);
    titleLayout->addStretch();
    
    auto *copyButton = new QPushButton(QStringLiteral("复制"), card);
    copyButton->setCursor(Qt::PointingHandCursor);

    header->addWidget(titleContainer);
    header->addStretch();

    QPushButton *decodeButton = nullptr;
    if (reversible) {
        decodeButton = new QPushButton(QStringLiteral("还原"), card);
        decodeButton->setCursor(Qt::PointingHandCursor);
        header->addWidget(decodeButton);
        connect(decodeButton, &QPushButton::clicked, this, [this, key]() { decodeFromPanel(key); });
    }
    
    // 添加小写按钮
    QPushButton *lowercaseButton = new QPushButton(QStringLiteral("小写"), card);
    lowercaseButton->setCursor(Qt::PointingHandCursor);
    header->addWidget(lowercaseButton);
    connect(lowercaseButton, &QPushButton::clicked, this, [this, key]() {
        auto panel = m_panels.value(static_cast<int>(key));
        if (!panel.editor) {
            return;
        }
        QSignalBlocker blocker(panel.editor);
        panel.editor->setPlainText(panel.editor->toPlainText().toLower());
    });
    
    header->addWidget(copyButton);

    auto *editor = createTextEdit(card);
    if (!reversible) {
        editor->setReadOnly(true);
    } else {
        editor->setPlaceholderText(QStringLiteral("可直接粘贴结果后点“还原”。"));
        // 为可逆编辑框添加textChanged信号连接，自动调整高度
        connect(editor, &QPlainTextEdit::textChanged, this, [this, editor]() {
            adjustEditorHeight(editor);
        });
    }
    m_allEditors.append(editor);

    layout->addLayout(header);
    layout->addWidget(editor, 1);

    m_panels.insert(static_cast<int>(key), PanelWidgets{editor, copyButton, decodeButton, lowercaseButton, reversible, title});
    connect(copyButton, &QPushButton::clicked, this, [this, key]() { copyPanel(key); });
    return card;
}

void TranscoderWindow::applyTheme()
{
    m_themeButton->setText(m_darkMode ? QStringLiteral("切到浅色") : QStringLiteral("切到暗黑"));

    if (m_darkMode) {
        setStyleSheet(QStringLiteral(R"(
            QWidget { background:#121820; color:#d3dde7; font-family:"Microsoft YaHei UI"; font-size:13px; }
            QFrame#panelCard { background:#1b2531; border:1px solid #2f4156; border-radius:14px; }
            QFrame#sourceCard { background:#1e3042; border:1px solid #4a6b8a; border-radius:14px; }
            QLabel#panelTitle { color:#f3f8ff; font-size:15px; font-weight:700; }
            QLabel#hintLabel { color:#95a8bc; font-size:12px; }
            QLabel#statusLabel { border-radius:10px; padding:8px 10px; }
            QPlainTextEdit, QListWidget, QComboBox {
                background:#101720; border:1px solid #30445c; border-radius:10px; padding:7px;
                selection-background-color:#2d537a;
            }
            QPlainTextEdit:focus { border:1px solid #5ea4e8; }
            QPlainTextEdit#sourceEdit { padding:0px; background:#152535; border:1px solid #4a6b8a; }
            QPushButton {
                background:#203346; border:1px solid #3f5f7f; border-radius:10px; padding:7px 12px; min-width:58px;
            }
            QPushButton:hover { background:#2a4560; border-color:#5b83ad; }
            QPushButton:pressed { background:#345574; }
            QSplitter::handle { background:#2e4257; height:6px; }
        )"));
    } else {
        setStyleSheet(QStringLiteral(R"(
            QWidget { background:#f4f7fb; color:#1f2937; font-family:"Microsoft YaHei UI"; font-size:13px; }
            QFrame#panelCard { background:#ffffff; border:1px solid #d9e3ef; border-radius:14px; }
            QFrame#sourceCard { background:#e6f0f8; border:1px solid #a8c7e0; border-radius:14px; }
            QLabel#panelTitle { color:#102542; font-size:15px; font-weight:700; }
            QLabel#hintLabel { color:#6b7280; font-size:12px; }
            QLabel#statusLabel { border-radius:10px; padding:8px 10px; }
            QPlainTextEdit, QListWidget, QComboBox {
                background:#fbfdff; border:1px solid #d4dde8; border-radius:10px; padding:7px;
                selection-background-color:#b9dcff;
            }
            QPlainTextEdit:focus { border:1px solid #3e8ed0; background:#ffffff; }
            QPlainTextEdit#sourceEdit { padding:0px; background:#f0f7ff; border:1px solid #a8c7e0; }
            QPushButton {
                background:#eaf2fb; border:1px solid #ccd9e7; border-radius:10px; padding:7px 12px; min-width:58px;
            }
            QPushButton:hover { background:#dcecff; border-color:#8cbce8; }
            QPushButton:pressed { background:#cde4ff; }
            QSplitter::handle { background:#d6e3f2; height:6px; }
        )"));
    }
}

int TranscoderWindow::editorHeightForRows(int rows) const
{
    const QFontMetrics fm(m_sourceEdit ? m_sourceEdit->font() : font());
    return (fm.lineSpacing() * rows) + 18;
}

void TranscoderWindow::applyEditorRows(int rows)
{
    const int commonHeight = editorHeightForRows(rows);
    const QFontMetrics fm(m_sourceEdit ? m_sourceEdit->font() : font());
    const int sourceHeight = (fm.lineSpacing() * rows) + 6;
    for (QPlainTextEdit *editor : m_allEditors) {
        if (!editor) {
            continue;
        }
        const int height = (editor == m_sourceEdit) ? sourceHeight : commonHeight;
        editor->setMinimumHeight(height);
        editor->setMaximumHeight(height);
    }
}

void TranscoderWindow::refreshOutputs()
{
    if (m_isRefreshing) {
        return;
    }

    m_isRefreshing = true;
    const QString source = m_sourceEdit->toPlainText();

    const auto assignPanel = [this](PanelKey key, const QString &value) {
        auto panel = m_panels.value(static_cast<int>(key));
        if (!panel.editor) {
            return;
        }
        QSignalBlocker blocker(panel.editor);
        panel.editor->setPlainText(value);
        adjustEditorHeight(panel.editor);
    };

    if (source.isEmpty()) {
        assignPanel(PanelKey::Url, {});
        assignPanel(PanelKey::Html, {});
        assignPanel(PanelKey::Sql, {});
        assignPanel(PanelKey::Hex, {});
        assignPanel(PanelKey::Binary, {});
        assignPanel(PanelKey::Ascii, {});
        assignPanel(PanelKey::Unicode, {});
        assignPanel(PanelKey::Base64, {});
        assignPanel(PanelKey::Md5_32, {});
        assignPanel(PanelKey::Sha1, {});
        assignPanel(PanelKey::Sha256, {});
        assignPanel(PanelKey::Sha512, {});
        m_isRefreshing = false;
        return;
    }

    const QByteArray utf8 = source.toUtf8();
    const QString md5_32 = toHexHash(QCryptographicHash::hash(utf8, QCryptographicHash::Md5));
    assignPanel(PanelKey::Url, QString::fromUtf8(QUrl::toPercentEncoding(source)));
    assignPanel(PanelKey::Html, encodeHtml(source));
    assignPanel(PanelKey::Sql, encodeSql(source));
    assignPanel(PanelKey::Hex, encodeHex(source));
    assignPanel(PanelKey::Binary, encodeBinary(source));
    assignPanel(PanelKey::Ascii, encodeAscii(source));
    assignPanel(PanelKey::Unicode, encodeUnicode(source));
    assignPanel(PanelKey::Base64, QString::fromLatin1(utf8.toBase64()));
    assignPanel(PanelKey::Md5_32, md5_32);
    assignPanel(PanelKey::Sha1, toHexHash(QCryptographicHash::hash(utf8, QCryptographicHash::Sha1)));
    assignPanel(PanelKey::Sha256, toHexHash(QCryptographicHash::hash(utf8, QCryptographicHash::Sha256)));
    assignPanel(PanelKey::Sha512, toHexHash(QCryptographicHash::hash(utf8, QCryptographicHash::Sha512)));
    m_isRefreshing = false;
}

void TranscoderWindow::decodeFromPanel(PanelKey key)
{
    const auto panel = m_panels.value(static_cast<int>(key));
    if (!panel.editor) {
        return;
    }

    const QString payload = panel.editor->toPlainText();
    QString decoded;
    bool ok = true;

    switch (key) {
    case PanelKey::Url:
        decoded = QUrl::fromPercentEncoding(payload.toUtf8());
        break;
    case PanelKey::Html:
        decoded = decodeHtml(payload);
        break;
    case PanelKey::Sql:
        decoded = decodeSql(payload, &ok);
        break;
    case PanelKey::Hex:
        decoded = decodeHex(payload, &ok);
        break;
    case PanelKey::Binary:
        decoded = decodeBinary(payload, &ok);
        break;
    case PanelKey::Ascii:
        decoded = decodeAscii(payload, &ok);
        break;
    case PanelKey::Unicode:
        decoded = decodeUnicode(payload, &ok);
        break;
    case PanelKey::Base64: {
        const QString normalized = normalizeBase64(payload);
        ok = isBase64Payload(normalized);
        if (ok) {
            decoded = QString::fromUtf8(QByteArray::fromBase64(normalized.toLatin1()));
        }
        break;
    }
    default:
        ok = false;
        break;
    }

    if (!ok) {
        setStatus(QStringLiteral("还原失败：请检查当前卡片数据格式。"), true);
        return;
    }

    m_sourceEdit->setPlainText(decoded);
    adjustEditorHeight(m_sourceEdit);
    setStatus(QStringLiteral("已从 %1 还原到原文本。").arg(panel.title));
}

void TranscoderWindow::copyPanel(PanelKey key)
{
    const auto panel = m_panels.value(static_cast<int>(key));
    if (!panel.editor) {
        return;
    }
    QGuiApplication::clipboard()->setText(panel.editor->toPlainText());
    setStatus(QStringLiteral("%1 已复制。").arg(panel.title));
}

void TranscoderWindow::toggleTheme()
{
    m_darkMode = !m_darkMode;
    applyTheme();
    setStatus(m_darkMode ? QStringLiteral("已切换到暗黑界面。") : QStringLiteral("已切换到浅色界面。"));
}

void TranscoderWindow::setStatus(const QString &message, bool isError)
{
    if (!m_statusLabel) {
        return;
    }

    m_statusLabel->setText(message);
    if (m_darkMode) {
        if (isError) {
            m_statusLabel->setStyleSheet(QStringLiteral(
                "color:#ffd6d6;background:#4b2528;border:1px solid #7d3d43;border-radius:10px;padding:8px 10px;"));
        } else {
            m_statusLabel->setStyleSheet(QStringLiteral(
                "color:#d6ebff;background:#223548;border:1px solid #35536f;border-radius:10px;padding:8px 10px;"));
        }
    } else {
        if (isError) {
            m_statusLabel->setStyleSheet(QStringLiteral(
                "color:#8c1d18;background:#fff1f1;border:1px solid #f2c9c7;border-radius:10px;padding:8px 10px;"));
        } else {
            m_statusLabel->setStyleSheet(QStringLiteral(
                "color:#245b7a;background:#eef7ff;border:1px solid #d8ebfb;border-radius:10px;padding:8px 10px;"));
        }
    }
}

void TranscoderWindow::adjustEditorHeight(QPlainTextEdit *editor)
{
    if (!editor) {
        return;
    }

    QTextDocument *document = editor->document();
    QFontMetrics fm(editor->font());
    int lineHeight = fm.lineSpacing();
    int documentHeight = document->size().height();
    int lines = qMax(1, static_cast<int>(documentHeight / lineHeight) + 1);
    
    // 确保至少有一行的高度，最多不超过20行
    lines = qMin(20, lines);
    
    int height = editorHeightForRows(lines);
    editor->setMinimumHeight(height);
    editor->setMaximumHeight(height);
}

QString TranscoderWindow::encodeHtml(const QString &text) const
{
    QString html = text;
    html.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    html.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    html.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    html.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    html.replace(QLatin1Char('\''), QStringLiteral("&#39;"));
    return html;
}

QString TranscoderWindow::decodeHtml(const QString &text) const
{
    QTextDocument document;
    document.setHtml(text);
    return document.toPlainText();
}

QString TranscoderWindow::encodeSql(const QString &text) const
{
    QByteArray utf16le;
    utf16le.reserve(text.size() * 2);
    for (QChar ch : text) {
        const ushort value = ch.unicode();
        utf16le.append(static_cast<char>(value & 0x00FF));
        utf16le.append(static_cast<char>((value >> 8) & 0x00FF));
    }
    return QStringLiteral("0x%1").arg(QString::fromLatin1(utf16le.toHex()).toUpper());
}

QString TranscoderWindow::decodeSql(const QString &text, bool *ok) const
{
    const QString normalized = normalizedHex(text);
    if (normalized.isEmpty() || (normalized.size() % 4) != 0) {
        if (ok) {
            *ok = false;
        }
        return {};
    }

    const QByteArray bytes = QByteArray::fromHex(normalized.toLatin1());
    if ((bytes.size() % 2) != 0) {
        if (ok) {
            *ok = false;
        }
        return {};
    }

    QString out;
    out.reserve(bytes.size() / 2);
    for (int i = 0; i < bytes.size(); i += 2) {
        const uchar lo = static_cast<uchar>(bytes.at(i));
        const uchar hi = static_cast<uchar>(bytes.at(i + 1));
        const ushort value = static_cast<ushort>(lo | (hi << 8));
        out.append(QChar(value));
    }

    if (ok) {
        *ok = true;
    }
    return out;
}

QString TranscoderWindow::encodeHex(const QString &text) const
{
    return QStringLiteral("0x%1").arg(QString::fromLatin1(text.toUtf8().toHex()).toUpper());
}

QString TranscoderWindow::decodeHex(const QString &text, bool *ok) const
{
    const QString normalized = normalizedHex(text);
    if (normalized.isEmpty() || (normalized.size() % 2) != 0) {
        if (ok) {
            *ok = false;
        }
        return {};
    }
    if (ok) {
        *ok = true;
    }
    return QString::fromUtf8(QByteArray::fromHex(normalized.toLatin1()));
}

QString TranscoderWindow::encodeBinary(const QString &text) const
{
    return toGroupedBytes(text.toUtf8(), 2, 8, QStringLiteral(" "));
}

QString TranscoderWindow::decodeBinary(const QString &text, bool *ok) const
{
    QString normalized = text;
    normalized.remove(QRegularExpression(QStringLiteral("[^01]")));

    if (normalized.isEmpty() || (normalized.size() % 8) != 0) {
        if (ok) {
            *ok = false;
        }
        return {};
    }

    QByteArray bytes;
    bytes.reserve(normalized.size() / 8);
    for (int i = 0; i < normalized.size(); i += 8) {
        bool chunkOk = false;
        const auto value = normalized.mid(i, 8).toUInt(&chunkOk, 2);
        if (!chunkOk) {
            if (ok) {
                *ok = false;
            }
            return {};
        }
        bytes.append(static_cast<char>(value));
    }

    if (ok) {
        *ok = true;
    }
    return QString::fromUtf8(bytes);
}

QString TranscoderWindow::encodeAscii(const QString &text) const
{
    QStringList values;
    const QByteArray bytes = text.toUtf8();
    values.reserve(bytes.size());
    for (uchar byte : bytes) {
        values << QString::number(byte);
    }
    return values.join(QStringLiteral(" "));
}

QString TranscoderWindow::decodeAscii(const QString &text, bool *ok) const
{
    const QStringList tokens = text.split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
    if (tokens.isEmpty()) {
        if (ok) {
            *ok = false;
        }
        return {};
    }

    QByteArray bytes;
    bytes.reserve(tokens.size());
    for (const QString &token : tokens) {
        bool numberOk = false;
        const int value = token.toInt(&numberOk, 10);
        if (!numberOk || value < 0 || value > 255) {
            if (ok) {
                *ok = false;
            }
            return {};
        }
        bytes.append(static_cast<char>(value));
    }

    if (ok) {
        *ok = true;
    }
    return QString::fromUtf8(bytes);
}

QString TranscoderWindow::encodeUnicode(const QString &text) const
{
    QStringList escaped;
    const auto codePoints = text.toUcs4();
    escaped.reserve(codePoints.size());
    for (uint codePoint : codePoints) {
        const int width = codePoint > 0xFFFF ? 8 : 4;
        const QString prefix = codePoint > 0xFFFF ? QStringLiteral("\\U") : QStringLiteral("\\u");
        escaped << QStringLiteral("%1%2").arg(prefix).arg(codePoint, width, 16, QLatin1Char('0')).toUpper();
    }
    return escaped.join(QStringLiteral(" "));
}

QString TranscoderWindow::decodeUnicode(const QString &text, bool *ok) const
{
    QString normalized = text;
    normalized.replace(QRegularExpression(QStringLiteral("\\\\U([0-9A-Fa-f]{8})")), QStringLiteral("\\\\u$1"));

    QList<uint> codePoints;
    int index = 0;
    while (index < normalized.size()) {
        if (normalized.at(index).isSpace()) {
            ++index;
            continue;
        }

        const QRegularExpression shortForm(QStringLiteral(R"(\\u([0-9A-Fa-f]{4,8}))"));
        const auto match = shortForm.match(normalized, index, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        if (!match.hasMatch()) {
            if (ok) {
                *ok = false;
            }
            return {};
        }

        bool numberOk = false;
        const uint value = match.captured(1).toUInt(&numberOk, 16);
        if (!numberOk) {
            if (ok) {
                *ok = false;
            }
            return {};
        }

        codePoints << value;
        index = match.capturedEnd();
    }

    const QString result = decodeCodePoints(codePoints);
    if (ok) {
        *ok = !result.isNull();
    }
    return result;
}
