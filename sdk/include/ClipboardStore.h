#pragma once

#include <QDateTime>
#include <QIcon>
#include <QObject>
#include <QString>
#include <QVector>

// 剪贴板历史。默认关闭的持久开关（clipboard/enable）：勾上才在后台收，
// 因为"把你复制过的 every thing 存盘"是改行为的功能，不该默认打开。
// 文本直接进 clipboard.json；图片落 clipimg/<sha1>.png，json 里只存路径 + 尺寸。
class ClipboardStore : public QObject {
    Q_OBJECT
public:
    struct Item {
        QDateTime when;
        QString hash;          // 内容指纹，用来去重和给图片文件名
        bool isImage = false;
        QString text;
        QString imagePath;
        int w = 0, h = 0;
        qint64 bytes = 0;
    };

    static constexpr int kMaxItems = 200;
    static constexpr qint64 kMaxTextBytes = 256 * 1024;

    static ClipboardStore& instance();

    void start();                                   // 挂信号 + 兜底轮询（幂等）
    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);                       // 立刻持久化；关掉只停止收集，历史保留

    const QVector<Item>& items() const { return m_items; }
    void restore(int index);                        // 放回剪贴板
    void remove(int index);
    void clear();
    QIcon iconFor(const Item& it) const;
    static QString summary(const Item& it);         // 列表里那一行显示什么

private:
    explicit ClipboardStore(QObject* parent = nullptr) : QObject(parent) {}
    void load();
    void save() const;
    void capture();
    void prune();

    QVector<Item> m_items;
    bool m_enabled = false;
    bool m_started = false;
    QString m_lastHash;
    quint32 m_seq = 0;      // 剪贴板序列号：没变就一行都不做（轮询本来 2 秒一次）
};
