#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

class TranscoderWindow : public QWidget
{
public:
    explicit TranscoderWindow(QWidget *parent = nullptr);

private:
    enum class PanelKey {
        Url,
        Html,
        Sql,
        Hex,
        Binary,
        Ascii,
        Unicode,
        Base64,
        Md5_32,
        Md5_16,
        Sha1,
        Sha256
    };

    struct PanelWidgets {
        QPlainTextEdit *editor = nullptr;
        QPushButton *copyButton = nullptr;
        QPushButton *decodeButton = nullptr;
        bool reversible = false;
        QString title;
    };

    void buildUi();
    QWidget *createPanel(PanelKey key, const QString &title, const QString &hint, bool reversible);
    void applyTheme();
    int editorHeightForRows(int rows) const;
    void applyEditorRows(int rows);
    void refreshOutputs();
    void decodeFromPanel(PanelKey key);
    void copyPanel(PanelKey key);
    void toggleTheme();
    void setStatus(const QString &message, bool isError = false);

    QString encodeHtml(const QString &text) const;
    QString decodeHtml(const QString &text) const;
    QString encodeSql(const QString &text) const;
    QString decodeSql(const QString &text, bool *ok) const;
    QString encodeHex(const QString &text) const;
    QString decodeHex(const QString &text, bool *ok) const;
    QString encodeBinary(const QString &text) const;
    QString decodeBinary(const QString &text, bool *ok) const;
    QString encodeAscii(const QString &text) const;
    QString decodeAscii(const QString &text, bool *ok) const;
    QString encodeUnicode(const QString &text) const;
    QString decodeUnicode(const QString &text, bool *ok) const;

    QPlainTextEdit *m_sourceEdit = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_themeButton = nullptr;
    QSpinBox *m_editorRowsSpin = nullptr;
    QList<QPlainTextEdit *> m_allEditors;

    QHash<int, PanelWidgets> m_panels;
    bool m_darkMode = false;
    bool m_isRefreshing = false;
};
