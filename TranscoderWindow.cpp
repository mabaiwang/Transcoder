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
#include <QTextBlock>
#include <QTextLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QDateTime>
#include <QResizeEvent>
#include <QTabWidget>
#include <cstdint>
#include <QSet>
#include <QUrlQuery>

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

// 循环左移
inline uint32_t rotl(uint32_t x, int n) {
    return ((x << n) | (x >> (32 - n)));
}

// 置换函数 P0
inline uint32_t p0(uint32_t x) {
    return x ^ rotl(x, 9) ^ rotl(x, 17);
}

// 置换函数 P1
inline uint32_t p1(uint32_t x) {
    return x ^ rotl(x, 15) ^ rotl(x, 23);
}

// 布尔函数 FFj
inline uint32_t ff(uint32_t x, uint32_t y, uint32_t z, int j) {
    if (j < 16) {
        return x ^ y ^ z;
    } else {
        return (x & y) | (x & z) | (y & z);
    }
}

// 布尔函数 GGj
inline uint32_t gg(uint32_t x, uint32_t y, uint32_t z, int j) {
    if (j < 16) {
        return x ^ y ^ z;
    } else {
        return (x & y) | (~x & z);
    }
}

// 压缩核心函数
void sm3_cf(uint32_t *v, const uint8_t *sub_block) {
    uint32_t w[68];
    uint32_t w1[64];
    
    // 1. 消息扩展
    for (int i = 0; i < 16; ++i) {
        w[i] = (sub_block[i * 4] << 24) |
               (sub_block[i * 4 + 1] << 16) |
               (sub_block[i * 4 + 2] << 8) |
               (sub_block[i * 4 + 3]);
    }
    
    for (int i = 16; i < 68; ++i) {
        w[i] = p1(w[i - 16] ^ w[i - 9] ^ rotl(w[i - 3], 15)) ^ rotl(w[i - 13], 7) ^ w[i - 6];
    }
    
    for (int i = 0; i < 64; ++i) {
        w1[i] = w[i] ^ w[i + 4];
    }
    
    // 2. 压缩
    uint32_t a = v[0];
    uint32_t b = v[1];
    uint32_t c = v[2];
    uint32_t d = v[3];
    uint32_t e = v[4];
    uint32_t f = v[5];
    uint32_t g = v[6];
    uint32_t h = v[7];
    
    for (int j = 0; j < 64; ++j) {
        uint32_t tj = (j < 16) ? 0x79CC4519 : 0x7A879D8A;
        uint32_t ss1 = rotl(rotl(a, 12) + e + rotl(tj, j % 32), 7);
        uint32_t ss2 = ss1 ^ rotl(a, 12);
        uint32_t tt1 = ff(a, b, c, j) + d + ss2 + w1[j];
        uint32_t tt2 = gg(e, f, g, j) + h + ss1 + w[j];
        
        d = c;
        c = rotl(b, 9);
        b = a;
        a = tt1;
        h = g;
        g = rotl(f, 19);
        f = e;
        e = p0(tt2);
    }
    
    v[0] ^= a;
    v[1] ^= b;
    v[2] ^= c;
    v[3] ^= d;
    v[4] ^= e;
    v[5] ^= f;
    v[6] ^= g;
    v[7] ^= h;
}

// 供外部调用的 SM3 计算
QByteArray calculateSm3(const QByteArray &data) {
    uint32_t v[8] = {
        0x7380166F, 0x4914B2B9, 0x172442D7, 0xDA8A0600,
        0xA96F30BC, 0x163138AA, 0xE38DEE4D, 0xB0FB0E4E
    };
    
    QByteArray padded = data;
    qint64 total_len = data.size();
    
    padded.append(static_cast<char>(0x80));
    
    while ((padded.size() % 64) != 56) {
        padded.append(static_cast<char>(0x00));
    }
    
    uint64_t bit_len = static_cast<uint64_t>(total_len) * 8;
    for (int i = 7; i >= 0; --i) {
        padded.append(static_cast<char>((bit_len >> (i * 8)) & 0xFF));
    }
    
    int num_blocks = padded.size() / 64;
    const uint8_t *ptr = reinterpret_cast<const uint8_t *>(padded.constData());
    for (int i = 0; i < num_blocks; ++i) {
        sm3_cf(v, ptr + i * 64);
    }
    
    QByteArray out;
    out.reserve(32);
    for (int i = 0; i < 8; ++i) {
        out.append(static_cast<char>((v[i] >> 24) & 0xFF));
        out.append(static_cast<char>((v[i] >> 16) & 0xFF));
        out.append(static_cast<char>((v[i] >> 8) & 0xFF));
        out.append(static_cast<char>(v[i] & 0xFF));
    }
    return out;
}

// ----------------------------------------------------
// Base58 编解码
// ----------------------------------------------------
QString encodeBase58(const QByteArray &data) {
    static const char al[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    if (data.isEmpty()) return {};
    int zeroCount = 0;
    while (zeroCount < data.size() && data.at(zeroCount) == 0) {
        zeroCount++;
    }
    QByteArray temp;
    temp.reserve(data.size() * 138 / 100 + 1);
    for (int i = zeroCount; i < data.size(); i++) {
        int carry = static_cast<unsigned char>(data.at(i));
        for (int j = 0; j < temp.size(); j++) {
            int val = static_cast<unsigned char>(temp.at(j)) * 256 + carry;
            temp[j] = static_cast<char>(val % 58);
            carry = val / 58;
        }
        while (carry > 0) {
            temp.append(static_cast<char>(carry % 58));
            carry /= 58;
        }
    }
    QString result;
    result.reserve(zeroCount + temp.size());
    for (int i = 0; i < zeroCount; i++) {
        result.append(QLatin1Char('1'));
    }
    for (int i = temp.size() - 1; i >= 0; i--) {
        result.append(QLatin1Char(al[static_cast<int>(temp.at(i))]));
    }
    return result;
}

QByteArray decodeBase58(const QString &text, bool *ok) {
    static const char al[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    if (text.isEmpty()) {
        if (ok) *ok = true;
        return {};
    }
    int zeroCount = 0;
    while (zeroCount < text.size() && text.at(zeroCount) == '1') {
        zeroCount++;
    }
    static int map[256];
    static bool mapInit = false;
    if (!mapInit) {
        memset(map, -1, sizeof(map));
        for (int i = 0; i < 58; i++) {
            map[static_cast<int>(al[i])] = i;
        }
        mapInit = true;
    }
    QByteArray temp;
    temp.reserve(text.size());
    for (int i = zeroCount; i < text.size(); i++) {
        int carry = map[static_cast<int>(text.at(i).toLatin1())];
        if (carry < 0) {
            if (ok) *ok = false;
            return {};
        }
        for (int j = 0; j < temp.size(); j++) {
            int val = static_cast<unsigned char>(temp.at(j)) * 58 + carry;
            temp[j] = static_cast<char>(val & 0xFF);
            carry = val >> 8;
        }
        while (carry > 0) {
            temp.append(static_cast<char>(carry & 0xFF));
            carry >>= 8;
        }
    }
    QByteArray result;
    result.reserve(zeroCount + temp.size());
    for (int i = 0; i < zeroCount; i++) {
        result.append(static_cast<char>(0x00));
    }
    for (int i = temp.size() - 1; i >= 0; i--) {
        result.append(temp.at(i));
    }
    if (ok) *ok = true;
    return result;
}

// ----------------------------------------------------
// Base32 编解码
// ----------------------------------------------------
QString encodeBase32(const QByteArray &data) {
    static const char al[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    QString result;
    int buffer = 0;
    int bitsLeft = 0;
    for (uchar byte : data) {
        buffer = (buffer << 8) | byte;
        bitsLeft += 8;
        while (bitsLeft >= 5) {
            result.append(QLatin1Char(al[(buffer >> (bitsLeft - 5)) & 0x1F]));
            bitsLeft -= 5;
        }
    }
    if (bitsLeft > 0) {
        buffer <<= (5 - bitsLeft);
        result.append(QLatin1Char(al[buffer & 0x1F]));
    }
    while ((result.size() % 8) != 0) {
        result.append(QLatin1Char('='));
    }
    return result;
}

QByteArray decodeBase32(const QString &text, bool *ok) {
    QString cleaned = text.trimmed().toUpper();
    while (cleaned.endsWith(QLatin1Char('='))) {
        cleaned.chop(1);
    }
    if (cleaned.isEmpty()) {
        if (ok) *ok = true;
        return {};
    }
    QByteArray result;
    int buffer = 0;
    int bitsLeft = 0;
    for (QChar ch : cleaned) {
        int val = -1;
        char c = ch.toLatin1();
        if (c >= 'A' && c <= 'Z') val = c - 'A';
        else if (c >= '2' && c <= '7') val = c - '2' + 26;
        if (val < 0) {
            if (ok) *ok = false;
            return {};
        }
        buffer = (buffer << 5) | val;
        bitsLeft += 5;
        if (bitsLeft >= 8) {
            result.append(static_cast<char>((buffer >> (bitsLeft - 8)) & 0xFF));
            bitsLeft -= 8;
        }
    }
    if (ok) *ok = true;
    return result;
}

// ----------------------------------------------------
// 摩斯密码双向映射
// ----------------------------------------------------
QString encodeMorse(const QString &text) {
    static QHash<QChar, QString> morseMap;
    if (morseMap.isEmpty()) {
        morseMap['A'] = ".-";    morseMap['B'] = "-...";  morseMap['C'] = "-.-.";  morseMap['D'] = "-..";
        morseMap['E'] = ".";     morseMap['F'] = "..-.";  morseMap['G'] = "--.";   morseMap['H'] = "....";
        morseMap['I'] = "..";    morseMap['J'] = ".---";  morseMap['K'] = "-.-";   morseMap['L'] = ".-..";
        morseMap['M'] = "--";    morseMap['N'] = "-.";    morseMap['O'] = "---";   morseMap['P'] = ".--.";
        morseMap['Q'] = "--.-";  morseMap['R'] = ".-.";   morseMap['S'] = "...";   morseMap['T'] = "-";
        morseMap['U'] = "..-";   morseMap['V'] = "...-";  morseMap['W'] = ".--";   morseMap['X'] = "-..-";
        morseMap['Y'] = "-.--";  morseMap['Z'] = "--..";
        morseMap['0'] = "-----"; morseMap['1'] = ".----"; morseMap['2'] = "..---"; morseMap['3'] = "...--";
        morseMap['4'] = "....-"; morseMap['5'] = "....."; morseMap['6'] = "-...."; morseMap['7'] = "--...";
        morseMap['8'] = "---.."; morseMap['9'] = "----."; morseMap[' '] = "/";
    }
    QString upper = text.trimmed().toUpper();
    QStringList result;
    for (QChar ch : upper) {
        if (morseMap.contains(ch)) {
            result << morseMap[ch];
        }
    }
    return result.join(QStringLiteral(" "));
}

QString decodeMorse(const QString &text, bool *ok) {
    static QHash<QString, QChar> morseRevMap;
    if (morseRevMap.isEmpty()) {
        static const QHash<QChar, QString> baseMap = {
            {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},
            {'E', "."},     {'F', "..-."},  {'G', "--."},   {'H', "...."},
            {'I', ".."},    {'J', ".---"},  {'K', "-.-"},   {'L', ".-.."},
            {'M', "--"},    {'N', "-."},    {'O', "---"},   {'P', ".--."},
            {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},   {'T', "-"},
            {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},
            {'Y', "-.--"},  {'Z', "--.."},
            {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
            {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
            {'8', "---.."}, {'9', "----."}, {' ', "/"}
        };
        for (auto it = baseMap.begin(); it != baseMap.end(); ++it) {
            morseRevMap[it.value()] = it.key();
        }
        morseRevMap["/"] = ' ';
    }
    const QStringList tokens = text.trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QString result;
    for (const QString &token : tokens) {
        if (morseRevMap.contains(token)) {
            result.append(morseRevMap[token]);
        } else {
            if (ok) *ok = false;
            return {};
        }
    }
    if (ok) *ok = true;
    return result;
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
    pasteButton->setCursor(Qt::PointingHandCursor);
    copySourceButton->setCursor(Qt::PointingHandCursor);
    clearButton->setCursor(Qt::PointingHandCursor);
    m_themeButton->setCursor(Qt::PointingHandCursor);

    sourceHeader->addWidget(sourceTitle);
    sourceHeader->addWidget(m_themeButton);
    sourceHeader->addWidget(pasteButton);
    sourceHeader->addWidget(copySourceButton);
    sourceHeader->addWidget(clearButton);
    sourceHeader->addStretch();

    m_sourceEdit = createTextEdit(sourceCard);
    m_sourceEdit->setObjectName(QStringLiteral("sourceEdit"));
    m_sourceEdit->setPlaceholderText(QStringLiteral("例如: 123.45"));
    m_sourceEdit->document()->setDocumentMargin(0);
    m_allEditors.append(m_sourceEdit);

    sourceLayout->addLayout(sourceHeader);
    sourceLayout->addWidget(m_sourceEdit, 1);

    struct PanelDef {
        PanelKey key;
        const char *title;
        const char *hint;
        bool reversible;
    };

    const QList<PanelDef> panels = {
        // Group 1. 常用编码类 (Encoding)
        {PanelKey::Url, "URL", "百分号编码。将非 ASCII 字符或特定符号转换为 %XX 格式，适用于网页 URL 传参 and 地址转义。", true},
        {PanelKey::Query2Json, "Query2JSON", "参数转 JSON。一键将 URL 键值对参数（a=1&b=2）转换为美化缩进的 JSON 格式，亦支持反向还原。", true},
        {PanelKey::Base64, "Base64", "标准 Base64。将字节流转换为 64 个可打印字符，解密还原时可智能兼容标准与 URL 安全格式（-_）。", true},
        {PanelKey::Base58, "Base58", "无混淆 Base58。常用于比特币地址与 IPFS 寻址，自动过滤了容易看错的 0/O/I/l/+/- 等符号并处理前导零。", true},
        {PanelKey::Base32, "Base32", "标准 Base32。按照 RFC 4648 将字符映射为 A-Z/2-7 序列，常用于 TOTP 二阶段验证秘钥生成及处理 '=' 补位。", true},
        {PanelKey::Html, "HTML Entity", "HTML 字符实体。将特殊敏感符号（如 <, >, &, \") 编码为字符实体实体引用，防止前端解析时触发 XSS 注入。", true},
        {PanelKey::Unicode, "Unicode", "Unicode 转义。转换为 \\uXXXX 形式的通用码点，还原时支持 \\uXXXX 和宽字符 \\UXXXXXXXX 的自动互转。", true},
        {PanelKey::Hex, "Hex", "十六进制。将文字以 UTF-8 编码转化为纯十六进制字节序列（0x...），常用于底层协议调试或二进制分析。", true},
        {PanelKey::Ascii, "Asc", "ASCII 十进制。将文本的 UTF-8 字节转化为以空格分隔的十进制数字序列，方便观察底层字节码值。", true},
        {PanelKey::Binary, "Binary", "二进制。将字符转换为 8 位一组的二进制 01 比特流，直观呈现数据在计算机底层的存储形态。", true},

        // Group 2. 格式美化与应用解析类 (Format & Parse)
        {PanelKey::Json, "JSON Format", "JSON 美化。自动识别校验 JSON 语法，转换为易读的多行缩进格式；还原时可自动将其压缩为紧凑的单行文本。", true},
        {PanelKey::Jwt, "JWT Decode", "JWT 解析。自动拆解 JWT 校验令牌的三段结构，解码出头部与负载 JSON，并将 exp/iat 等时间戳翻译为北京时间（只读）。", false},
        {PanelKey::Timestamp, "Timestamp", "时间戳转换。智能互转 Unix 纪元时间与北京时间；支持 10 位（秒）与 13 位（毫秒，带 .zzz 格式）的自动双向互转。", true},
        {PanelKey::TextClean, "Text Clean", "文本去重清洗。输入多行文本，一键过滤空白行、剥离首尾多余空格，并按字母排序（支持还原回原文本）。", true},

        // Group 3. 哈希指纹与国密杂凑 (Hash)
        {PanelKey::Md5_32, "MD5", "MD5 信息摘要。基于 MD5 算法生成 32 位十六进制大写特征码，常用于快速数据一致性校验或指纹提取（只读）。", false},
        {PanelKey::Sha1, "SHA1", "SHA-1 摘要。生成 160 位（40字符）的十六进制特征摘要，常用于传统的防篡改签名与校验（只读）。", false},
        {PanelKey::Sha256, "SHA256", "SHA-256 强摘要。生成 256 位（64字符）的高安全性十六进制哈希特征码，为当前主流的加密与签名算法（只读）。", false},
        {PanelKey::Sha512, "SHA512", "SHA-512 超强摘要。生成 512 位（128字符）的极高强度十六进制哈希码，提供极高防碰撞防篡改安全保障（只读）。", false},
        {PanelKey::Sm3, "SM3", "国密 SM3 杂凑。中国国家商用密码标准算法，生成 256 位（64字符）的十六进制密码学哈希，多用于国产化安全系统（只读）。", false},

        // Group 4. 进制与趣味类 (Misc & Fun)
        {PanelKey::RadixConvert, "Radix Convert", "进制转换。智能检测输入数值，将其在二进制（0b...）、八进制（0...）、十进制、十六进制（0x...）之间跨进制转换。", true},
        {PanelKey::Sql, "SQL_En", "SQL 注入逃逸编码。将字符串转为 UTF-16LE 字节十六进制（0x...），常用于绕过数据库防护层对特殊字符的检测。", true},
        {PanelKey::Morse, "Morse Code", "摩斯密码。将英文、数字及常用标点符号翻译为摩尔斯电码（.- 格式），支持输入电码反向还原文字。", true}
    };

    m_tabWidget = new QTabWidget(this);

    for (const auto &panel : panels) {
        QWidget *panelWidget = createPanel(panel.key,
                                           QString::fromUtf8(panel.title),
                                           QString::fromUtf8(panel.hint),
                                           panel.reversible);
        m_tabWidget->addTab(panelWidget, QString::fromUtf8(panel.title));
    }

    // 主垂直分割器：连接上方横向充满的源文本框和下方充满的选项卡
    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(sourceCard);
    splitter->addWidget(m_tabWidget);
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, false);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 7);
    splitter->setSizes({200, 500});

    rootLayout->addWidget(splitter, 1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("statusLabel"));
    m_statusLabel->setText(QStringLiteral("就绪"));
    rootLayout->addWidget(m_statusLabel);

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
    // 行数调节功能已被更强大的 Tab 自适应拉伸所取代

    applyTheme();
}

QWidget *TranscoderWindow::createPanel(PanelKey key, const QString &title, const QString &hint, bool reversible)
{
    auto *panelWidget = new QWidget(this);
    auto *layout = new QVBoxLayout(panelWidget);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(8);

    auto *header = new QHBoxLayout;
    
    auto *hintLabel = new QLabel(hint, panelWidget);
    hintLabel->setObjectName(QStringLiteral("hintLabel"));
    
    auto *copyButton = new QPushButton(QStringLiteral("复制"), panelWidget);
    copyButton->setCursor(Qt::PointingHandCursor);

    QPushButton *decodeButton = nullptr;
    QPushButton *pasteRestoreButton = nullptr;
    if (reversible) {
        pasteRestoreButton = new QPushButton(QStringLiteral("粘贴"), panelWidget);
        pasteRestoreButton->setCursor(Qt::PointingHandCursor);
        header->addWidget(pasteRestoreButton);
        connect(pasteRestoreButton, &QPushButton::clicked, this, [this, key]() {
            auto panel = m_panels.value(static_cast<int>(key));
            if (panel.editor) {
                panel.editor->setPlainText(QGuiApplication::clipboard()->text());
                decodeFromPanel(key);
            }
        });

        decodeButton = new QPushButton(QStringLiteral("还原"), panelWidget);
        decodeButton->setCursor(Qt::PointingHandCursor);
        header->addWidget(decodeButton);
        connect(decodeButton, &QPushButton::clicked, this, [this, key]() { decodeFromPanel(key); });
    }
    
    QPushButton *lowercaseButton = new QPushButton(QStringLiteral("小写"), panelWidget);
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

    header->addSpacing(15);
    header->addWidget(hintLabel);
    header->addStretch();

    auto *editor = createTextEdit(panelWidget);
    if (!reversible) {
        editor->setReadOnly(true);
    } else {
        editor->setPlaceholderText(QStringLiteral("可直接粘贴结果后点“还原”。"));
    }
    m_allEditors.append(editor);

    layout->addLayout(header);
    layout->addWidget(editor, 1);

    m_panels.insert(static_cast<int>(key), PanelWidgets{editor, copyButton, decodeButton, lowercaseButton, pasteRestoreButton, reversible, title});
    connect(copyButton, &QPushButton::clicked, this, [this, key]() { copyPanel(key); });
    return panelWidget;
}

void TranscoderWindow::applyTheme()
{
    m_themeButton->setText(m_darkMode ? QStringLiteral("切到浅色") : QStringLiteral("切到暗黑"));

    if (m_darkMode) {
        setStyleSheet(QStringLiteral(R"(
            QWidget { background:#121820; color:#d3dde7; font-family:"Microsoft YaHei UI"; font-size:13px; }
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
            QTabWidget::pane { border: 1px solid #2f4156; background: #1b2531; border-radius: 12px; top: -1px; }
            QTabBar::tab {
                background: #151d27; border: 1px solid #2f4156; border-bottom: none;
                border-top-left-radius: 8px; border-top-right-radius: 8px;
                padding: 8px 16px; color: #95a8bc; font-weight: bold; margin-right: 2px;
            }
            QTabBar::tab:hover { background: #202d3c; color: #d3dde7; }
            QTabBar::tab:selected { background: #1b2531; border-color: #2f4156; color: #f3f8ff; border-bottom: 2px solid #5ea4e8; }
        )"));
    } else {
        setStyleSheet(QStringLiteral(R"(
            QWidget { background:#f4f7fb; color:#1f2937; font-family:"Microsoft YaHei UI"; font-size:13px; }
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
            QTabWidget::pane { border: 1px solid #d9e3ef; background: #ffffff; border-radius: 12px; top: -1px; }
            QTabBar::tab {
                background: #e1e7f0; border: 1px solid #d9e3ef; border-bottom: none;
                border-top-left-radius: 8px; border-top-right-radius: 8px;
                padding: 8px 16px; color: #6b7280; font-weight: bold; margin-right: 2px;
            }
            QTabBar::tab:hover { background: #ebf0f7; color: #1f2937; }
            QTabBar::tab:selected { background: #ffffff; border-color: #d9e3ef; color: #102542; border-bottom: 2px solid #3e8ed0; }
        )"));
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
    };

    if (source.isEmpty()) {
        assignPanel(PanelKey::Url, {});
        assignPanel(PanelKey::Query2Json, {});
        assignPanel(PanelKey::Base64, {});
        assignPanel(PanelKey::Base58, {});
        assignPanel(PanelKey::Base32, {});
        assignPanel(PanelKey::Html, {});
        assignPanel(PanelKey::Unicode, {});
        assignPanel(PanelKey::Hex, {});
        assignPanel(PanelKey::Ascii, {});
        assignPanel(PanelKey::Binary, {});
        assignPanel(PanelKey::Json, {});
        assignPanel(PanelKey::Jwt, {});
        assignPanel(PanelKey::Timestamp, {});
        assignPanel(PanelKey::TextClean, {});
        assignPanel(PanelKey::Md5_32, {});
        assignPanel(PanelKey::Sha1, {});
        assignPanel(PanelKey::Sha256, {});
        assignPanel(PanelKey::Sha512, {});
        assignPanel(PanelKey::Sm3, {});
        assignPanel(PanelKey::RadixConvert, {});
        assignPanel(PanelKey::Sql, {});
        assignPanel(PanelKey::Morse, {});
        m_isRefreshing = false;
        return;
    }

    const QByteArray utf8 = source.toUtf8();
    const QString md5_32 = toHexHash(QCryptographicHash::hash(utf8, QCryptographicHash::Md5));

    // Group 1. 常用编码类 (Encoding)
    assignPanel(PanelKey::Url, QString::fromUtf8(QUrl::toPercentEncoding(source)));

    // Query String -> JSON
    QString q2jOut;
    QUrlQuery query(source);
    if (!query.isEmpty() || source.contains(QLatin1Char('='))) {
        QJsonObject qObj;
        auto items = query.queryItems(QUrl::FullyDecoded);
        if (items.isEmpty() && source.contains(QLatin1Char('='))) {
            const QStringList pairs = source.split(QLatin1Char('&'), Qt::SkipEmptyParts);
            for (const QString &pair : pairs) {
                int idx = pair.indexOf(QLatin1Char('='));
                if (idx != -1) {
                    qObj.insert(pair.left(idx).trimmed(), pair.mid(idx + 1).trimmed());
                } else {
                    qObj.insert(pair.trimmed(), QString());
                }
            }
        } else {
            for (const auto &item : items) {
                qObj.insert(item.first, item.second);
            }
        }
        q2jOut = QString::fromUtf8(QJsonDocument(qObj).toJson(QJsonDocument::Indented));
    } else {
        q2jOut = QStringLiteral("输入 Query 参数 (如 a=1&b=2)");
    }
    assignPanel(PanelKey::Query2Json, q2jOut);

    assignPanel(PanelKey::Base64, QString::fromLatin1(utf8.toBase64()));
    assignPanel(PanelKey::Base58, encodeBase58(utf8));
    assignPanel(PanelKey::Base32, encodeBase32(utf8));
    assignPanel(PanelKey::Html, encodeHtml(source));
    assignPanel(PanelKey::Unicode, encodeUnicode(source));
    assignPanel(PanelKey::Hex, encodeHex(source));
    assignPanel(PanelKey::Ascii, encodeAscii(source));
    assignPanel(PanelKey::Binary, encodeBinary(source));

    // Group 2. 格式美化与应用解析类 (Format & Parse)
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(utf8, &parseError);
    QString jsonOut;
    if (parseError.error == QJsonParseError::NoError) {
        jsonOut = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
    } else {
        jsonOut = QStringLiteral("非合法JSON或为空");
    }
    assignPanel(PanelKey::Json, jsonOut);

    // JWT Decode
    QString jwtOut;
    const QStringList parts = source.split(QLatin1Char('.'));
    if (parts.size() >= 2) {
        QByteArray headerBytes = QByteArray::fromBase64(normalizeBase64(parts.at(0)).toLatin1());
        QByteArray payloadBytes = QByteArray::fromBase64(normalizeBase64(parts.at(1)).toLatin1());
        QJsonParseError err;
        QJsonDocument hDoc = QJsonDocument::fromJson(headerBytes, &err);
        QString hStr = (err.error == QJsonParseError::NoError) ? QString::fromUtf8(hDoc.toJson(QJsonDocument::Indented)) : QString::fromUtf8(headerBytes);
        QJsonDocument pDoc = QJsonDocument::fromJson(payloadBytes, &err);
        QString pStr;
        if (err.error == QJsonParseError::NoError) {
            QJsonObject pObj = pDoc.object();
            auto translateTime = [&pObj](const QString &key) {
                if (pObj.contains(key)) {
                    QJsonValue val = pObj.value(key);
                    qlonglong ts = 0;
                    if (val.isDouble()) ts = static_cast<qlonglong>(val.toDouble());
                    else if (val.isString()) ts = val.toString().toLongLong();
                    if (ts > 0) {
                        QDateTime dt = QDateTime::fromSecsSinceEpoch(ts);
                        pObj.insert(key, QString("%1 (%2)").arg(ts).arg(dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
                    }
                }
            };
            translateTime(QStringLiteral("exp"));
            translateTime(QStringLiteral("iat"));
            translateTime(QStringLiteral("nbf"));
            pStr = QString::fromUtf8(QJsonDocument(pObj).toJson(QJsonDocument::Indented));
        } else {
            pStr = QString::fromUtf8(payloadBytes);
        }
        jwtOut = QString("=== Header ===\n%1\n\n=== Payload ===\n%2").arg(hStr, pStr);
    } else {
        jwtOut = QStringLiteral("请输入合法的 JWT (以 . 分割的 Base64 结构)");
    }
    assignPanel(PanelKey::Jwt, jwtOut);

    // Timestamp Convert
    bool isInt = false;
    qlonglong val = source.toLongLong(&isInt);
    QString timeOut;
    if (isInt && val > 0) {
        QDateTime dt;
        if (source.length() == 13) {
            dt = QDateTime::fromMSecsSinceEpoch(val);
            timeOut = dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
        } else {
            dt = QDateTime::fromSecsSinceEpoch(val);
            timeOut = dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        }
    } else {
        QDateTime dt = QDateTime::fromString(source, QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
        if (!dt.isValid()) {
            dt = QDateTime::fromString(source, QStringLiteral("yyyy/MM/dd HH:mm:ss.zzz"));
        }
        if (dt.isValid()) {
            timeOut = QString::number(dt.toMSecsSinceEpoch());
        } else {
            dt = QDateTime::fromString(source, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            if (!dt.isValid()) {
                dt = QDateTime::fromString(source, QStringLiteral("yyyy/MM/dd HH:mm:ss"));
            }
            if (!dt.isValid()) {
                dt = QDateTime::fromString(source, QStringLiteral("yyyy-MM-dd"));
            }
            if (dt.isValid()) {
                timeOut = QString::number(dt.toSecsSinceEpoch());
            } else {
                timeOut = QStringLiteral("输入合法的日期或时间戳");
            }
        }
    }
    assignPanel(PanelKey::Timestamp, timeOut);

    // Text Clean
    QStringList lines = source.split(QLatin1Char('\n'));
    QSet<QString> uniqueLines;
    QStringList cleanedList;
    for (auto &line : lines) {
        QString trimmed = line.trimmed();
        if (!trimmed.isEmpty() && !uniqueLines.contains(trimmed)) {
            uniqueLines.insert(trimmed);
            cleanedList.append(trimmed);
        }
    }
    cleanedList.sort(Qt::CaseInsensitive);
    assignPanel(PanelKey::TextClean, cleanedList.join(QLatin1Char('\n')));

    // Group 3. 哈希指纹与国密杂凑 (Hash)
    assignPanel(PanelKey::Md5_32, md5_32);
    assignPanel(PanelKey::Sha1, toHexHash(QCryptographicHash::hash(utf8, QCryptographicHash::Sha1)));
    assignPanel(PanelKey::Sha256, toHexHash(QCryptographicHash::hash(utf8, QCryptographicHash::Sha256)));
    assignPanel(PanelKey::Sha512, toHexHash(QCryptographicHash::hash(utf8, QCryptographicHash::Sha512)));
    assignPanel(PanelKey::Sm3, toHexHash(calculateSm3(utf8)));

    // Group 5. 进制与趣味类 (Misc & Fun)
    bool okRadix = false;
    qlonglong radixNum = 0;
    QString rTrim = source.trimmed();
    if (rTrim.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        radixNum = rTrim.mid(2).toLongLong(&okRadix, 16);
    } else if (rTrim.startsWith(QStringLiteral("0b"), Qt::CaseInsensitive)) {
        radixNum = rTrim.mid(2).toLongLong(&okRadix, 2);
    } else if (rTrim.startsWith(QStringLiteral("0"), Qt::CaseInsensitive) && rTrim.length() > 1) {
        radixNum = rTrim.mid(1).toLongLong(&okRadix, 8);
    } else {
        radixNum = rTrim.toLongLong(&okRadix, 10);
    }
    QString radixOut;
    if (okRadix) {
        radixOut = QString(
            "二进制 (BIN):  0b%1\n"
            "八进制 (OCT):  0%2\n"
            "十进制 (DEC):  %3\n"
            "十六进制 (HEX): 0x%4"
        ).arg(QString::number(radixNum, 2))
         .arg(QString::number(radixNum, 8))
         .arg(QString::number(radixNum, 10))
         .arg(QString::number(radixNum, 16).toUpper());
    } else {
        radixOut = QStringLiteral("请输入单个合法的数字（如 42 或 0x2A 或 0b1010）");
    }
    assignPanel(PanelKey::RadixConvert, radixOut);

    assignPanel(PanelKey::Sql, encodeSql(source));
    assignPanel(PanelKey::Morse, encodeMorse(source));

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
    case PanelKey::Query2Json: {
        QJsonParseError err;
        QJsonDocument jdoc = QJsonDocument::fromJson(payload.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && jdoc.isObject()) {
            QJsonObject obj = jdoc.object();
            QStringList pairs;
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                pairs << QString("%1=%2").arg(it.key()).arg(it.value().toString());
            }
            decoded = pairs.join(QLatin1Char('&'));
        } else {
            ok = false;
        }
        break;
    }
    case PanelKey::Base64: {
        const QString normalized = normalizeBase64(payload);
        ok = isBase64Payload(normalized);
        if (ok) {
            decoded = QString::fromUtf8(QByteArray::fromBase64(normalized.toLatin1()));
        }
        break;
    }
    case PanelKey::Base58: {
        QByteArray b58 = decodeBase58(payload.trimmed(), &ok);
        if (ok) {
            decoded = QString::fromUtf8(b58);
        }
        break;
    }
    case PanelKey::Base32: {
        QByteArray b32 = decodeBase32(payload, &ok);
        if (ok) {
            decoded = QString::fromUtf8(b32);
        }
        break;
    }
    case PanelKey::Html:
        decoded = decodeHtml(payload);
        break;
    case PanelKey::Unicode:
        decoded = decodeUnicode(payload, &ok);
        break;
    case PanelKey::Hex:
        decoded = decodeHex(payload, &ok);
        break;
    case PanelKey::Ascii:
        decoded = decodeAscii(payload, &ok);
        break;
    case PanelKey::Binary:
        decoded = decodeBinary(payload, &ok);
        break;
    case PanelKey::Json: {
        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8(), &error);
        if (error.error == QJsonParseError::NoError) {
            decoded = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
        } else {
            ok = false;
        }
        break;
    }
    case PanelKey::Timestamp: {
        bool isInt = false;
        qlonglong val = payload.toLongLong(&isInt);
        if (isInt && val > 0) {
            QDateTime dt;
            if (payload.length() == 13) {
                dt = QDateTime::fromMSecsSinceEpoch(val);
                decoded = dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
            } else {
                dt = QDateTime::fromSecsSinceEpoch(val);
                decoded = dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            }
        } else {
            QDateTime dt = QDateTime::fromString(payload, QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
            if (!dt.isValid()) {
                dt = QDateTime::fromString(payload, QStringLiteral("yyyy/MM/dd HH:mm:ss.zzz"));
            }
            if (dt.isValid()) {
                decoded = QString::number(dt.toMSecsSinceEpoch());
            } else {
                QDateTime dtDefault = QDateTime::fromString(payload, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
                if (!dtDefault.isValid()) {
                    dtDefault = QDateTime::fromString(payload, QStringLiteral("yyyy/MM/dd HH:mm:ss"));
                }
                if (!dtDefault.isValid()) {
                    dtDefault = QDateTime::fromString(payload, QStringLiteral("yyyy-MM-dd"));
                }
                if (dtDefault.isValid()) {
                    decoded = QString::number(dtDefault.toSecsSinceEpoch());
                } else {
                    ok = false;
                }
            }
        }
        break;
    }
    case PanelKey::TextClean: {
        decoded = payload;
        break;
    }
    case PanelKey::RadixConvert: {
        QStringList list = payload.split(QLatin1Char('\n'));
        qlonglong outVal = 0;
        bool found = false;
        for (const QString &line : list) {
            if (line.contains(QStringLiteral("DEC):"))) {
                QString part = line.mid(line.indexOf(QStringLiteral("DEC):")) + 5).trimmed();
                outVal = part.toLongLong(&found);
                break;
            }
        }
        if (found) {
            decoded = QString::number(outVal);
        } else {
            ok = false;
        }
        break;
    }
    case PanelKey::Sql:
        decoded = decodeSql(payload, &ok);
        break;
    case PanelKey::Morse: {
        decoded = decodeMorse(payload, &ok);
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

// 已移除被弃用的 adjustEditorHeight 自适应函数，所有高度拉伸交由 Qt 分割器与 QTabWidget 页面布局自动管理

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
    document.setHtml(QStringLiteral("<pre>%1</pre>").arg(text));
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

    int pos = 0;
    QString result;
    bool hasInvalid = false;
    
    while (pos < normalized.size()) {
        int uIdx = normalized.indexOf(QStringLiteral("\\u"), pos);
        if (uIdx == -1) {
            result.append(normalized.mid(pos));
            break;
        }
        
        result.append(normalized.mid(pos, uIdx - pos));
        
        int hexLen = 0;
        while (uIdx + 2 + hexLen < normalized.size()) {
            QChar ch = normalized.at(uIdx + 2 + hexLen);
            if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f')) {
                hexLen++;
            } else {
                break;
            }
        }
        
        if (hexLen < 4) {
            hasInvalid = true;
            break;
        }
        
        int consumeLen = qMin(8, hexLen);
        QString hexStr = normalized.mid(uIdx + 2, consumeLen);
        bool conversionOk = false;
        uint codePoint = hexStr.toUInt(&conversionOk, 16);
        
        if (!conversionOk || codePoint > 0x10FFFF) {
            hasInvalid = true;
            break;
        }
        
        QString decodedChar;
        if (codePoint <= 0xFFFF) {
            decodedChar = QChar(static_cast<ushort>(codePoint));
        } else {
            const char32_t scalar = static_cast<char32_t>(codePoint);
            decodedChar = QString::fromUcs4(&scalar, 1);
        }
        
        result.append(decodedChar);
        pos = uIdx + 2 + consumeLen;
    }
    
    if (hasInvalid) {
        if (ok) {
            *ok = false;
        }
        return {};
    }
    
    if (ok) {
        *ok = true;
    }
    return result;
}
