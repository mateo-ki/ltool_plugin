#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QTreeWidget;
class QTreeWidgetItem;

// JSON 格式化工具：左边可折叠的树，右边可编辑的文本。
// 文本区是唯一事实来源：改完点「格式化」重新解析并刷新树；输入时也会防抖解析，
// 解析失败只在状态栏说一声，不会把上一次的好结果清掉。
// 作为 DLL 插件的一张内嵌页出去（工具页卡片墙 + 搜索框直达），不再有独立窗口。
class JsonTool : public QWidget {
    Q_OBJECT
public:
    // 把一段文本接进来（搜索框/剪贴板那条「在工具里打开这段」）
    void setText(const QString& text);

    explicit JsonTool(QWidget* parent = nullptr);

protected:
    void dropEvent(QDropEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;

private:
    void reparse(bool rewriteText);       // rewriteText: 把规范化后的 JSON 写回文本区
    void buildTree(const QJsonValue& root);
    void expandAll(bool on);
    void setStatus(const QString& text, bool ok);
    void loadFile(const QString& path);

    QTreeWidget* m_tree = nullptr;
    QPlainTextEdit* m_edit = nullptr;
    QLabel* m_status = nullptr;
    QCheckBox* m_wrap = nullptr;
};
