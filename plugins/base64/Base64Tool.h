#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QPlainTextEdit;

// Base64 编解码工具：和 JsonTool 同一套用法，作为 DLL 插件的内嵌页出去。
// 默认"解码"模式：粘贴一串 base64 直接看结果；解不出合法文本时给十六进制而不是乱码。
class Base64Tool : public QWidget {
    Q_OBJECT
public:
    // 把一段文本接进来（搜索框/剪贴板那条「在工具里打开这段」）
    void setText(const QString& text);

    explicit Base64Tool(QWidget* parent = nullptr);

protected:
    void dropEvent(QDropEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;

private:
    void recompute();
    void setMode(bool decode);
    void applyModeUi(bool decode);   // 只同步按钮/标题，不触发重新计算
    void setStatus(const QString& text, bool ok);

    QPlainTextEdit* m_input = nullptr;
    QPlainTextEdit* m_out = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_decodeBtn = nullptr;
    QPushButton* m_encodeBtn = nullptr;
    bool m_decode = true;
};
