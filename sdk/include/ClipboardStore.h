#pragma once

#include <QDateTime>
#include <QIcon>
#include <QObject>
#include <QString>
#include <QVector>

// 剪贴板历史。默认关闭的持久开关（clipboard/enable）：勾上才在后台收，
// 因为"把你复制过的 every thing 存盘"是改行为的功能，不该默认打开。
// 文本直接进 clipboard.json；图片落 clipimg/<sha1>.png，json 里只存路径 + 尺寸。
// 留多少由两条上限决定：200 条封顶，加上 clipboard/keepDays 那个天数（默认 7 天，0=不限）。
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
    // 保留天数（clipboard/keepDays）：两条上限取交集 —— 超龄的一律清，
    // 没超龄的仍受 kMaxItems 兜底。0 = 不限天数，只按条数。
    static constexpr int kDefaultKeepDays = 7;

    static ClipboardStore& instance();

    void start();                                   // 挂信号 + 兜底轮询（幂等）
    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);                       // 立刻持久化；关掉只停止收集，历史保留

    int keepDays() const { return m_keepDays; }
    void setKeepDays(int days);                     // 立刻持久化 + 当场裁一次

    // 这一条是不是已经超出保留天数。抽成静态纯函数是为了让它能被判据直接问，
    // 不用把整份历史搬进内存、再等时钟真的走过一天。keepDays<=0 一律不算超龄。
    // 时间戳读不出来的条目按"超龄"处理：界面本来就画不出它是哪天复制的。
    static bool expired(const Item& it, int keepDays, const QDateTime& now);

    // 一张图片的 40px 缩略图落在哪儿（和原图同一个目录、同一次清理，后缀 .t40.png）。
    // 为什么要有这东西：列表里每一行的图标原来都走 `QPixmap(整图)` + Smooth 缩放 ——
    // 2026-10-19 实测他那份 51 张图（合计 51.6 百万像素）一趟要 **632ms**，而这张页
    // 每次被看见、每敲一个过滤字都要重铺一遍列表，那几百毫秒就挂在 GUI 线程上。
    // 缩略图落盘之后同款趟是 **10ms**（63×）。它跟着原图一起进 prune() 那张 keep 表，
    // 原图被撤了小的也当孤儿扫走 —— 不另开目录，免得"孤儿怎么撤"要写第二遍。
    static QString thumbPath(const QString& imagePath);

    // 孤儿图清理的那道闸门和路径归一化：都暴露出来是为了能被判据直接问（批量删文件
    // 这件事一旦写歪就是丢历史，不该只有跑起来才发现）。
    static bool sweepAllowed(int itemCount);
    static QString keepKey(const QString& path);

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
    int m_keepDays = kDefaultKeepDays;
    QDate m_sweptOn;        // 上一次按天裁剪是哪一天：轮询 2 秒一次，按天这件事一天扫一次就够
    QString m_lastHash;
    quint32 m_seq = 0;      // 剪贴板序列号：没变就一行都不做（轮询本来 2 秒一次）
};
