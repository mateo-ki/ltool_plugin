#include "LinuxdoWatch.h"

#include "DevEnv.h"
#include "Utils.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QDate>
#include <QTimer>

namespace {

const char kUa[] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                   "(KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36";
const char kOrigin[] = "https://linux.do/latest.json?order=created";
constexpr int kMinMinutes = 3;         // 公开站点别当 DDoS 使，下限卡死
constexpr int kMaxMinutes = 240;
constexpr int kKeepHits = 50;          // 页面上留最近这么多条
constexpr int kDirectTimeout = 6000;   // 直连这条在这台机器上是死的，别让它拖满 20 秒
constexpr int kRelayTimeout = 25000;   // 实测中转一趟 4.5~7.3 秒

QString cfgPath()
{
    return Utils::dataDir() + "/config.ini";
}

// 命中判定比的是标题和标签（比小写、全角先折半角 —— 和搜索框那一路同一个规矩）
bool contains(const QString& haystack, const QString& needle)
{
    return haystack.contains(needle, Qt::CaseInsensitive);
}

} // namespace

namespace LinuxdoWatch_ {

QVector<LinuxdoTopic> parseTopics(const QByteArray& json)
{
    QVector<LinuxdoTopic> out;
    QJsonParseError perr {};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return out;
    const QJsonArray topics = doc.object()
                                  .value(QStringLiteral("topic_list")).toObject()
                                  .value(QStringLiteral("topics")).toArray();
    for (const QJsonValue& v : topics) {
        const QJsonObject o = v.toObject();
        LinuxdoTopic t;
        t.id = qint64(o.value(QStringLiteral("id")).toDouble());
        t.title = o.value(QStringLiteral("title")).toString();
        t.categoryId = o.value(QStringLiteral("category_id")).toInt();
        t.createdAt = o.value(QStringLiteral("created_at")).toString();
        const QJsonArray tags = o.value(QStringLiteral("tags")).toArray();
        for (const QJsonValue& tv : tags)
            t.tags << tv.toString();
        if (t.id <= 0 || t.title.isEmpty())
            continue;
        t.url = QStringLiteral("https://linux.do/t/%1").arg(t.id);
        out.append(t);
    }
    return out;
}

QHash<int, QString> parseCategories(const QByteArray& json)
{
    QHash<int, QString> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject())
        return out;
    const QJsonArray cats = doc.object()
                                .value(QStringLiteral("category_list")).toObject()
                                .value(QStringLiteral("categories")).toArray();
    for (const QJsonValue& v : cats) {
        const QJsonObject o = v.toObject();
        const int id = o.value(QStringLiteral("id")).toInt();
        const QString name = o.value(QStringLiteral("name")).toString();
        const QString slug = o.value(QStringLiteral("slug")).toString();
        if (id <= 0 || name.isEmpty())
            continue;
        out.insert(id, name + QStringLiteral(" / ") + slug);
    }
    return out;
}

QVector<LinuxdoHit> matchNew(const QVector<LinuxdoTopic>& topics, const QStringList& keywords,
                             qint64 lastSeen, const QHash<int, QString>& cats)
{
    QVector<LinuxdoHit> out;
    for (const LinuxdoTopic& t : topics) {
        if (t.id <= lastSeen)
            continue;                       // 上一轮已经报过的不再报第二遍
        for (const QString& k : keywords) {
            const QString word = k.trimmed();
            if (word.isEmpty())
                continue;
            QString where;
            if (contains(t.title, word))
                where = QStringLiteral("标题");
            else {
                for (const QString& tag : t.tags)
                    if (contains(tag, word)) { where = QStringLiteral("标签"); break; }
            }
            if (where.isEmpty()) {
                const QString cat = cats.value(t.categoryId);
                if (!cat.isEmpty() && contains(cat, word))
                    where = QStringLiteral("分类「%1」").arg(cat.split(QStringLiteral(" / ")).first());
            }
            if (!where.isEmpty()) {
                out.append(LinuxdoHit { t, word, where });
                break;                      // 一条帖子只算一次，别因为撞了两个词冒两个泡
            }
        }
    }
    return out;
}

QString relayUrl(const QString& prefix, const QString& origin)
{
    if (prefix.isEmpty())
        return origin;
    // 前缀末尾带不带斜杠都算同一个意思，拼出来不能出现两个斜杠（也不能把 https:// 粘成 https://https://）
    QString p = prefix.trimmed();
    while (p.endsWith(QLatin1Char('/')))
        p.chop(1);
    return p + QLatin1Char('/') + origin;
}

bool shouldNotify(bool firstRun, int freshCount)
{
    return !firstRun && freshCount > 0;
}

qint64 advanceSeen(qint64 currentSeen, qint64 topId, bool haveCategories)
{
    return haveCategories ? topId : currentSeen;
}

} // namespace LinuxdoWatch_

LinuxdoWatch& LinuxdoWatch::instance()
{
    static LinuxdoWatch w;
    return w;
}

LinuxdoWatch::LinuxdoWatch(QObject* parent)
    : QObject(parent)
{
    load();
}

void LinuxdoWatch::load()
{
    QSettings st(cfgPath(), QSettings::IniFormat);
    m_enabled = st.value(QStringLiteral("linuxdo/enabled"), true).toBool();
    m_minutes = qBound(kMinMinutes, st.value(QStringLiteral("linuxdo/minutes"), 10).toInt(),
                       kMaxMinutes);
    const QString words = st.value(QStringLiteral("linuxdo/keywords")).toString();
    if (!words.trimmed().isEmpty())
        m_keywords = words.split(QLatin1Char(','), Qt::SkipEmptyParts);
    m_prefix = st.value(QStringLiteral("linuxdo/prefix"),
                        QStringLiteral("https://r.jina.ai/")).toString();
    m_lastSeen = qint64(st.value(QStringLiteral("linuxdo/lastSeen"), 0).toLongLong());
    m_lastCheck = st.value(QStringLiteral("linuxdo/lastCheck")).toString();
    const QByteArray stored = QByteArray::fromHex(
        st.value(QStringLiteral("linuxdo/hits")).toByteArray());
    const QJsonDocument doc = QJsonDocument::fromJson(stored);
    m_hits.clear();
    if (doc.isArray()) {
        for (const QJsonValue& v : doc.array()) {
            const QJsonObject o = v.toObject();
            LinuxdoHit h;
            h.topic.id = qint64(o.value(QStringLiteral("id")).toDouble());
            h.topic.title = o.value(QStringLiteral("title")).toString();
            h.topic.url = o.value(QStringLiteral("url")).toString();
            h.topic.createdAt = o.value(QStringLiteral("at")).toString();
            h.matched = o.value(QStringLiteral("matched")).toString();
            h.where = o.value(QStringLiteral("where")).toString();
            if (h.topic.id > 0 && !h.topic.title.isEmpty())
                m_hits.append(h);
        }
    }
    m_catCheck = st.value(QStringLiteral("linuxdo/catCheck")).toString();
    const QJsonDocument catDoc = QJsonDocument::fromJson(QByteArray::fromHex(
        st.value(QStringLiteral("linuxdo/categories")).toByteArray()));
    if (catDoc.isObject()) {
        const QJsonObject o = catDoc.object();
        for (auto it = o.constBegin(); it != o.constEnd(); ++it)
            m_cats.insert(it.key().toInt(), it.value().toString());
    }
    m_status = m_lastCheck.isEmpty() ? QStringLiteral("还没查过")
                                     : QStringLiteral("上次查：%1").arg(m_lastCheck);
}

void LinuxdoWatch::save() const
{
    QSettings st(cfgPath(), QSettings::IniFormat);
    st.setValue(QStringLiteral("linuxdo/enabled"), m_enabled);
    st.setValue(QStringLiteral("linuxdo/minutes"), m_minutes);
    st.setValue(QStringLiteral("linuxdo/keywords"), m_keywords.join(QLatin1Char(',')));
    st.setValue(QStringLiteral("linuxdo/prefix"), m_prefix);
    st.setValue(QStringLiteral("linuxdo/lastSeen"), qlonglong(m_lastSeen));
    st.setValue(QStringLiteral("linuxdo/lastCheck"), m_lastCheck);
    st.setValue(QStringLiteral("linuxdo/catCheck"), m_catCheck);
    {
        QJsonObject o;
        for (auto it = m_cats.constBegin(); it != m_cats.constEnd(); ++it)
            o.insert(QString::number(it.key()), it.value());
        st.setValue(QStringLiteral("linuxdo/categories"),
                    QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact).toHex()));
    }
    QJsonArray arr;
    for (const LinuxdoHit& h : m_hits) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), double(h.topic.id));
        o.insert(QStringLiteral("title"), h.topic.title);
        o.insert(QStringLiteral("url"), h.topic.url);
        o.insert(QStringLiteral("at"), h.topic.createdAt);
        o.insert(QStringLiteral("matched"), h.matched);
        o.insert(QStringLiteral("where"), h.where);
        arr.append(o);
    }
    st.setValue(QStringLiteral("linuxdo/hits"),
                QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact).toHex()));
    st.sync();
}

void LinuxdoWatch::start()
{
    if (!m_timer) {
        m_timer = new QTimer(qApp);
        m_timer->setTimerType(Qt::CoarseTimer);
        connect(m_timer, &QTimer::timeout, this, [this] { checkNow(); });
    }
    arm();
    if (!m_booted) {
        // 起来之后先查一趟（15 秒后，把启动那几秒留给热键和索引）：不然装好之后最坏要等
        // 一个完整间隔才知道这条路通不通，而"到底通不通"正是这一页要说实话的事。
        // 只在第一次 start() 排这一趟 —— 改配置也会走 arm()，不该顺手触发一次取回。
        m_booted = true;
        if (m_enabled) {
            // 只点名 checkNow：分类表由它排在这趟**之后**再拿（见 onBody）——
            // 同时飞两三趟会把免费中转打到 25 秒超时（16:14 那次三条全 timeout 就是这么来的）
            QTimer::singleShot(15000, this, [this] { checkNow(); });
        }
    }
}

void LinuxdoWatch::stop()
{
    if (m_timer)
        m_timer->stop();
}

void LinuxdoWatch::arm()
{
    if (!m_timer)
        return;
    if (!m_enabled) {
        m_timer->stop();
        return;
    }
    m_timer->start(m_minutes * 60 * 1000);
}

void LinuxdoWatch::setCategories(const QHash<int, QString>& cats)
{
    m_cats = cats;
    save();
    emit changed();
}

bool LinuxdoWatch::dayChanged() const
{
    return !m_catCheck.startsWith(QDate::currentDate().toString("yyyy-MM-dd"));
}

void LinuxdoWatch::setEnabled(bool on)
{
    m_enabled = on;
    save();
    arm();
    emit changed();
}

void LinuxdoWatch::setIntervalMinutes(int minutes)
{
    m_minutes = qBound(kMinMinutes, minutes, kMaxMinutes);
    save();
    arm();
    emit changed();
}

void LinuxdoWatch::setKeywords(const QStringList& words)
{
    QStringList out;
    for (const QString& w : words) {
        const QString t = w.trimmed();
        if (!t.isEmpty() && !out.contains(t))
            out << t;
    }
    m_keywords = out;
    save();
    emit changed();
}

void LinuxdoWatch::setPrefix(const QString& prefix)
{
    m_prefix = prefix.trimmed();
    save();
    emit changed();
}

bool LinuxdoWatch::checkNow()
{
    if (m_busy) {
        m_status = QStringLiteral("上一趟还没回来（中转那边慢的时候一趟要 5~25 秒），这次跳过");
        emit changed();
        return false;
    }
    m_wantCats = m_cats.isEmpty() || dayChanged();   // 这轮结束后补拿分类表（不并发）
    m_busy = true;
    m_status = QStringLiteral("在查…");
    emit changed();
    fetchVia(0);
    return true;
}

// 为什么这里不直接复用 PluginStore::fetch：那条链是"镜像→代理→直连"三级、按 github raw 的
// 形状排的，而这一路要的是"原站→中转前缀"两条、并且必须带 X-Return-Format 这个自定义头
// （中转不带它就返回一层 markdown 皮）。两边候选的排法根本不同，硬套只会互相绑住手脚。
void LinuxdoWatch::grabVia(int stage, const QString& origin,
                           std::function<void(const QByteArray&, const QString&, qint64)> onBody,
                           std::function<void(const QString&)> onFail)
{
    const QString url = stage == 0 ? origin : LinuxdoWatch_::relayUrl(m_prefix, origin);
    if (stage == 1 && m_prefix.trimmed().isEmpty()) {
        onFail(QStringLiteral("两条路都没了：直连不通，而「中转前缀」是空的（填一个才能取回）"));
        return;
    }
    auto* nam = new QNetworkAccessManager(this);
    DevEnv::applyProxy(nam);            // 留空 = 系统代理，和产品里其他地方同一条
    QNetworkRequest req { QUrl(url) };
    req.setRawHeader("User-Agent", kUa);
    req.setRawHeader("Accept", "application/json, text/plain, */*");
    if (stage == 1)
        req.setRawHeader("X-Return-Format", "text");   // 让中转原样吐字节，别包成 markdown
    req.setTransferTimeout(stage == 0 ? kDirectTimeout : kRelayTimeout);
    auto* clock = new QElapsedTimer;
    clock->start();
    QNetworkReply* reply = nam->get(req);
    QObject::connect(reply, &QNetworkReply::finished, this,
                     [this, reply, nam, clock, stage, origin, url, onBody, onFail] {
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const QString err = reply->errorString();
        const qint64 ms = clock->elapsed();
        const bool ok = reply->error() == QNetworkReply::NoError && http == 200 && body.size() >= 64;
        reply->deleteLater();
        nam->deleteLater();
        delete clock;
        if (ok) {
            onBody(body, stage == 0 ? QStringLiteral("直连") : QStringLiteral("中转"), ms);
            return;
        }
        if (stage == 0) {
            Utils::log(QStringLiteral("linuxdo: 直连不通（http=%1 %2），改走中转").arg(http).arg(err));
            grabVia(1, origin, onBody, onFail);
            return;
        }
        onFail(QStringLiteral("直连不通，中转回 %1（%2，%3ms）")
                   .arg(http ? QString::number(http) : QStringLiteral("没连上"), err.left(40)).arg(ms));
        Q_UNUSED(url);
    });
}

void LinuxdoWatch::fetchVia(int stage)
{
    grabVia(stage, QString::fromLatin1(kOrigin),
            [this](const QByteArray& body, const QString& how, qint64 ms) { onBody(body, how, ms); },
            [this](const QString& why) {
                m_busy = false;
                m_status = why;
                Utils::log(QStringLiteral("linuxdo: 取回失败 %1").arg(why));
                emit changed();
            });
}

void LinuxdoWatch::fetchCategories()
{
    grabVia(0, QStringLiteral("https://linux.do/categories.json"),
            [this](const QByteArray& body, const QString& how, qint64 ms) { onCategories(body, how, ms); },
            [this](const QString& why) {
                // 不重试轰炸：m_cats 还是空的，下一轮 checkNow 会把 m_wantCats 再立起来
                Utils::log(QStringLiteral("linuxdo: 分类表没拉到（%1）—— 这一阵只比标题和标签").arg(why));
            });
}

void LinuxdoWatch::onCategories(const QByteArray& body, const QString& how, qint64 ms)
{
    const QHash<int, QString> cats = LinuxdoWatch_::parseCategories(body);
    if (cats.isEmpty()) {
        Utils::log(QStringLiteral("linuxdo: 分类表回的不是 JSON（%1 %2 字节）").arg(how).arg(body.size()));
        return;
    }
    setCategories(cats);
    m_catCheck = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    save();
    Utils::log(QStringLiteral("linuxdo: 分类表 %1（%2ms）拿到 %3 个分类，分类名从此参与比对")
                   .arg(how).arg(ms).arg(cats.size()));
    if (m_status.startsWith(QStringLiteral("还没查过")) || m_status.contains(QStringLiteral("分类表")))
        m_status = QStringLiteral("分类表已拿到（%1 个分类，%2）· %3")
                       .arg(cats.size()).arg(m_catCheck, m_status);
    emit changed();
}

void LinuxdoWatch::onBody(const QByteArray& body, const QString& how, qint64 ms)
{
    const QVector<LinuxdoTopic> topics = LinuxdoWatch_::parseTopics(body);
    m_busy = false;
    m_lastCount = 0;
    m_lastTopics.clear();
    if (topics.isEmpty()) {
        // 拿到 200 但解不出 topic_list —— 十有八九是那张 Cloudflare 挑战页（它是 HTML）
        m_status = QStringLiteral("%1 这条路回的不是 JSON（多半是撞盾了），%2ms").arg(how).arg(ms);
        Utils::log(QStringLiteral("linuxdo: %1 回了 %2 字节但解不出帖子列表，前 40 字节=%3")
                       .arg(how).arg(body.size())
                       .arg(QString::fromUtf8(body.left(40).toHex())));
        emit changed();
        return;
    }
    m_lastCount = topics.size();
    m_lastTopics = topics;
    qint64 top = m_lastSeen;
    for (const LinuxdoTopic& t : topics)
        top = qMax(top, t.id);
    const bool firstRun = m_lastSeen <= 0;
    const QVector<LinuxdoHit> fresh = LinuxdoWatch_::matchNew(topics, m_keywords, m_lastSeen, m_cats);
    m_lastSeen = LinuxdoWatch_::advanceSeen(m_lastSeen, top, !m_cats.isEmpty());
    if (m_cats.isEmpty())
        m_status += QStringLiteral("（分类表还没到，这一页先不记账，下一轮拿到表再比一遍）");
    m_lastCheck = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    if (!firstRun) {
        for (const LinuxdoHit& h : fresh)
            m_hits.prepend(h);
        while (m_hits.size() > kKeepHits)
            m_hits.removeLast();
    }
    m_status = QStringLiteral("这次走的是%1（%2ms）· 拿到 %3 条 · 新命中 %4 条%5")
                   .arg(how).arg(ms).arg(topics.size()).arg(firstRun ? 0 : fresh.size())
                   .arg(firstRun ? QStringLiteral("（第一次只记下从哪条开始，不弹框）") : QString());
    save();
    if (LinuxdoWatch_::shouldNotify(firstRun, fresh.size())) {
        QStringList lines;
        for (int i = 0; i < qMin(3, fresh.size()); ++i)
            lines << QStringLiteral("· %1（%2）").arg(fresh[i].topic.title.left(34), fresh[i].where);
        Utils::trayNote(QStringLiteral("linux.do 命中 %1 条").arg(fresh.size()),
                        lines.join(QLatin1Char('\n'))
                            + (fresh.size() > 3 ? QStringLiteral("\n…还有 %1 条").arg(fresh.size() - 3)
                                                : QString())
                            + QStringLiteral("\n去「linux.do 盯帖」那一页点着看"),
                        8000);
    }
    Utils::log(QStringLiteral("linuxdo: %1 %2ms 拿到 %3 条，新命中 %4 条，lastSeen=%5，分类表 %6 个")
                   .arg(how).arg(ms).arg(topics.size()).arg(fresh.size()).arg(m_lastSeen).arg(m_cats.size()));
    if (m_wantCats) {
        m_wantCats = false;
        fetchCategories();          // 一次只飞一趟：帖子这趟回来了才轮到它
    }
    emit changed();
}
