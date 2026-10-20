#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVector>
#include <functional>

// linux.do 盯帖的引擎：每隔几分钟拉一次「最新帖」，比标题和标签，命中就在托盘冒一句。
//
// 为什么引擎在这里、页面上面只管配置：这一路必须**没打开过页面也在跑**，
// 而插件的页是懒创建的（点开卡片才建），所以它靠 plugin.json 里 "Activate":true
// 挂牌，启动时由 PluginHost::startBackground() 按名字叫起来（见 PluginHost.h 那段注释）。
// 和 AdBlocker / TaskScheduler 是同一个形状：单例 + start() 幂等 + 配置落 config.ini。
//
// 取回这条路是实测出来的，不是猜的（2026-10-15）：
//  - 直连 linux.do 在这台机器上 DNS 被污染（解到 Facebook 的 IP），必然超时；
//  - 走系统代理能到 Cloudflare，但非浏览器客户端一律吃 "Just a moment" 挑战页 ——
//    带着浏览器导出的 cf_clearance + 完整 Chrome 请求头也一样（它绑 TLS 指纹），
//    所以**给 cookie 也给不出登录态**，这一路看到的是登出态的公开内容；
//  - `https://r.jina.ai/<原地址>` + `X-Return-Format: text` 经同一份代理能拿到真 JSON
//    （实测 latest 30 条 / categories 12 个分类 / 单个分类流 30 条，4.5~7.3 秒一趟）。
//  于是顺序是：先短超时试直连（哪天网络变了就自动用上的那条），再走前缀中转。
//  前缀在配置里可换 —— 谁想挂自己的中转，填进去就行，留空 = 只用直连。
//
// 发帖速率实测 0.64 帖/分钟（一页 30 条跨 47 分钟），所以 5~30 分钟一轮都漏不掉；
// 间隔下限卡 3 分钟，别拿公开 API 当 DDoS 使。

class QTimer;

struct LinuxdoTopic {
    qint64 id = 0;
    QString title;
    QStringList tags;
    QString createdAt;        // 原样留着（ISO8601 UTC），界面只贴日期那半截
    int categoryId = 0;
    QString url;              // https://linux.do/t/<id>，Discourse 自己会跳到带 slug 的正式地址
};

struct LinuxdoHit {
    LinuxdoTopic topic;
    QString matched;          // 命中的那个词（界面上要说清为什么是它）
    QString where;            // 撞在哪儿：标题 / 标签 / 分类名 —— 「福利」十有八九在分类名上
};

namespace LinuxdoWatch_ {

// 纯函数三件套 —— 判据直接打这三个，不碰网络也不碰托盘（自测一次气泡都不许弹出去）
QVector<LinuxdoTopic> parseTopics(const QByteArray& json);
// 分类 id → 「分类名 / slug」。实测过：新帖里标题含「福利」的 0 条，但在分类 36「福利羊毛」
// 里的有 4 条 —— 只比标题和标签就永远是 0 命中，所以分类名必须进比对。
QHash<int, QString> parseCategories(const QByteArray& json);
QVector<LinuxdoHit> matchNew(const QVector<LinuxdoTopic>& topics, const QStringList& keywords,
                             qint64 lastSeen, const QHash<int, QString>& cats = {});
QString relayUrl(const QString& prefix, const QString& origin);
// 要不要弹那一句：第一次跑只记下"从哪条开始"，不弹（装上就哗啦十几条没人会看）；
// 没命中也不弹。判据打这一个函数，页面和引擎共用同一条决定。
bool shouldNotify(bool firstRun, int freshCount);

// 分类表还没到手的时候，这一页先别推进 lastSeen —— 否则那 30 条永远没被分类名比过
// （「福利」这类词就只出现在分类名上）。表到手之后下一轮会把同一页再比一遍，宁可晚报不要漏。
qint64 advanceSeen(qint64 currentSeen, qint64 topId, bool haveCategories);

} // namespace LinuxdoWatch_

class LinuxdoWatch : public QObject {
    Q_OBJECT
public:
    static LinuxdoWatch& instance();

    // 幂等：start() 只建定时器 + 从配置回读（AdBlocker 同一条规矩）
    void start();
    void stop();

    bool enabled() const { return m_enabled; }
    int intervalMinutes() const { return m_minutes; }
    QStringList keywords() const { return m_keywords; }
    QString prefix() const { return m_prefix; }
    qint64 lastSeenId() const { return m_lastSeen; }
    QString lastCheckText() const { return m_lastCheck; }
    QString statusLine() const { return m_status; }
    QVector<LinuxdoHit> hits() const { return m_hits; }
    bool busy() const { return m_busy; }
    int lastTopicCount() const { return m_lastCount; }   // 上一次一趟拿到几条帖子
    QVector<LinuxdoTopic> lastTopics() const { return m_lastTopics; }   // 就是那几条（判据拿真数据用）
    QHash<int, QString> categories() const { return m_cats; }
    QString catCheckText() const { return m_catCheck; }   // 那张表是什么时候拿的
    void setCategories(const QHash<int, QString>& cats);   // 拉到新表就换（顺带落盘缓存）

    // 改配置：写盘 + 重排定时器 + 立刻生效（页面那几颗控件走的就是这一条）
    void setEnabled(bool on);
    void setIntervalMinutes(int minutes);
    void setKeywords(const QStringList& words);
    void setPrefix(const QString& prefix);

    // 查一次（异步）。返回的是"这次有没有把这一趟发出去"，结果一律走 changed()/statusLine()。
    bool checkNow();

    // 分类表（id → 名字 / slug）。公开是因为：拉不到就只能比标题和标签，
    // 而"福利"这类词十有八九只出现在分类名上 —— 页面要把这个降级状态说出来，判据也要能点名要一次。
    void fetchCategories();

signals:
    void changed();

private:
    explicit LinuxdoWatch(QObject* parent = nullptr);
    void load();
    void save() const;
    void arm();
    void fetchVia(int stage);                       // 0=直连 1=前缀中转
    // 一条通用的两级取回：先短超时试直连，不通再走前缀中转。帖子和分类表共用这一条。
    void grabVia(int stage, const QString& origin,
                 std::function<void(const QByteArray&, const QString&, qint64)> onBody,
                 std::function<void(const QString&)> onFail);

    void onBody(const QByteArray& body, const QString& how, qint64 ms);
    void onCategories(const QByteArray& body, const QString& how, qint64 ms);
    bool dayChanged() const;   // 分类表一天拿一趟就够（它变得慢）

    QTimer* m_timer = nullptr;
    bool m_enabled = true;
    int m_minutes = 10;
    QStringList m_keywords { QStringLiteral("福利"), QStringLiteral("女装"),
                             QStringLiteral("公益") };
    QString m_prefix = QStringLiteral("https://r.jina.ai/");
    qint64 m_lastSeen = 0;
    QString m_lastCheck;
    QString m_status = QStringLiteral("还没查过");
    QVector<LinuxdoHit> m_hits;
    bool m_busy = false;
    bool m_booted = false;
    bool m_wantCats = false;              // 这轮帖子跑完之后要补拿分类表
    QHash<int, QString> m_cats;
    QString m_catCheck;                     // 分类表上次是什么时候拿的
    int m_lastCount = 0;
    QVector<LinuxdoTopic> m_lastTopics;
};
