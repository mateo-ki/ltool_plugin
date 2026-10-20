#include "SoftwarePage.h"
#include "platform/FileMeta.h"
#include "platform/Launch.h"
#include "AppScanner.h"
#include "DevEnv.h"
#include "Utils.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QComboBox>
#include <QCursor>
#include <QRegularExpression>
#include <QSettings>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <windows.h>

namespace {

// 推荐软件（keywords 用于本地应用索引匹配；wingetId 为空时"下载"回退到官网）
// 表必须按 category 连续分组书写：渲染按"分类变化"插表头，不重排
struct SoftEntry {
    const char* name;
    const char* keywords;   // 分号分隔
    const char* url;
    const char* wingetId;
    const char* category;
};

const SoftEntry kSoft[] = {
    // 开发工具
    {"VS Code", "visual studio code;vscode;code.exe", "https://code.visualstudio.com", "Microsoft.VisualStudioCode", "开发工具"},
    {"IntelliJ IDEA", "idea;intellij", "https://www.jetbrains.com/idea/", "JetBrains.IntelliJIDEA.Community", "开发工具"},
    {"CLion", "clion", "https://www.jetbrains.com/clion/", "JetBrains.CLion", "开发工具"},
    {"CMake", "cmake", "https://cmake.org", "Kitware.CMake", "开发工具"},
    {"Trae", "trae", "https://www.trae.ai", "Trae.Trae", "开发工具"},
    {"Oracle VirtualBox", "virtualbox", "https://www.virtualbox.org", "Oracle.VirtualBox", "开发工具"},
    // AI 编程
    {"CC Switch", "cc switch;ccswitch", "https://github.com/farion1231/cc-switch", "", "AI 编程"},
    {"Codex++", "codex++;codexplusplus", "https://github.com/BigPizzaV3/CodexPlusPlus", "", "AI 编程"},
    {"WorkBuddy", "workbuddy", "https://www.bing.com/search?q=WorkBuddy+%E5%AE%98%E7%BD%91", "", "AI 编程"},
    {"ZCode", "zcode", "https://z.ai", "", "AI 编程"},
    // 办公协作
    {"WPS Office", "wps", "https://www.wps.cn", "Kingsoft.WPSOffice", "办公协作"},
    {"Microsoft Office", "microsoft office;microsoft 365;winword", "https://www.microsoft.com/microsoft-365", "", "办公协作"},
    {"飞书", "飞书;feishu", "https://www.feishu.cn", "Feishu.Feishu", "办公协作"},
    {"企业微信", "企业微信;wxwork", "https://work.weixin.qq.com", "Tencent.WeCom", "办公协作"},
    {"钉钉", "钉钉;dingtalk", "https://www.dingtalk.com", "Alibaba.DingTalk", "办公协作"},
    // 社交沟通
    {"微信", "微信;weixin;wechat", "https://weixin.qq.com", "Tencent.WeChat", "社交沟通"},
    {"QQ", "qq", "https://im.qq.com", "Tencent.QQ", "社交沟通"},
    {"Telegram", "telegram", "https://telegram.org", "Telegram.TelegramDesktop", "社交沟通"},
    // 影音
    {"VLC", "vlc", "https://www.videolan.org", "VideoLAN.VLC", "影音"},
    {"LX Music", "lx music;lx-music", "https://github.com/lyswhut/lx-music-desktop", "lyswhut.lx-music-desktop", "影音"},
    // 游戏
    {"Steam", "steam", "https://store.steampowered.com", "Valve.Steam", "游戏"},
    {"WeGame", "wegame", "https://www.wegame.com.cn", "Tencent.WeGame", "游戏"},
    {"MuMu模拟器", "mumu", "https://mumu.163.com", "NetEase.MuMuPlayer", "游戏"},
    // 实用工具
    {"7-Zip", "7-zip;7z", "https://www.7-zip.org", "7zip.7zip", "实用工具"},
    {"Motrix", "motrix", "https://motrix.app", "Motrix.Motrix", "实用工具"},
    {"阿里云盘", "阿里云盘;alipan", "https://www.alipan.com", "Alibaba.aDrive", "实用工具"},
    {"夸克", "夸克;quark", "https://www.quark.cn", "", "实用工具"},
    {"UU远程", "uu远程;uuyc", "https://uuyc.163.com", "", "实用工具"},
};

constexpr int kSoftCount = int(sizeof(kSoft) / sizeof(kSoft[0]));

QIcon letterIcon(const QString& text, const QColor& color)
{
    QPixmap pm(48, 48);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(2, 2, 44, 44, 12, 12);
    p.fillPath(path, color);
    QFont f("Microsoft YaHei UI", 16);
    f.setBold(true);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(pm.rect(), Qt::AlignCenter, text.left(1));
    return QIcon(pm);
}

// 搜索引擎兜底地址：这类 url 不是软件官网，按钮不能写成"官网"
bool isSearchFallback(const QString& url)
{
    static const char* const engines[] = {"bing.com", "google.com", "baidu.com",
                                          "so.com", "sogou.com", "duckduckgo.com"};
    const QString host = QUrl(url).host().toLower();
    for (const char* e : engines)
        if (host.endsWith(QLatin1String(e)))
            return true;
    return false;
}

QString webIconPath(const QString& url)
{
    const QByteArray key = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Md5).toHex();
    return Utils::dataDir() + "/webicons/" + key + ".png";
}

// 目标 exe 的版本号：一次 PE 读实测约 10 ms，27 行串起来就是"第一次点进软件页卡住"
// （开机后深索引还在抢盘时更久）。进程内缓存，切页/重进不再重复读盘。
QHash<QString, QString>& versionCache()
{
    static QHash<QString, QString> cache;
    return cache;
}

// 本地应用索引里按关键词找第一个命中项（未命中时 lnkPath 为空）
AppEntry matchLocal(const QVector<AppEntry>& apps, const QString& keywords)
{
    const QStringList kws = keywords.split(';', Qt::SkipEmptyParts);
    for (const AppEntry& a : apps) {
        const QString hay = (a.name + " " + a.target).toLower();
        for (const QString& kw : kws)
            if (hay.contains(kw.trimmed().toLower()))
                return a;
    }
    return {};
}

} // namespace

SoftwarePage::SoftwarePage(QWidget* parent)
    : QWidget(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    QDir().mkpath(Utils::dataDir() + "/webicons");
    DevEnv::applyProxy(m_nam);   // 图标多在 GitHub/国外站点，慢的时候代理能救

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 4, 4, 4);

    auto* head = new QHBoxLayout;
    auto* tip = new QLabel("推荐软件按分类分组，右侧状态列说明能不能一键装；点「版本」看本机装了哪些版本，"
                           "点「安装/更新」用 winget 静默安装。");
    tip->setObjectName("tip");
    tip->setWordWrap(true);
    m_refreshBtn = new QPushButton("刷新状态", this);
    head->addWidget(tip, 1);
    head->addWidget(m_refreshBtn);
    lay->addLayout(head);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText("搜索软件或分类（如 java、游戏）…");
    m_search->setClearButtonEnabled(true);
    lay->addWidget(m_search);

    // 行容器 + 滚动区：行宽度随窗口自适应，不会互相挤压
    auto* rowsHost = new QWidget(this);
    rowsHost->setObjectName("rowsHost");
    m_rowsLay = new QVBoxLayout(rowsHost);
    m_rowsLay->setContentsMargins(0, 0, 0, 0);
    m_rowsLay->setSpacing(4);
    m_rowsLay->addStretch(1);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(rowsHost);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea{background:transparent;border:none;}"
                          "QWidget#rowsHost{background:transparent;}");
    lay->addWidget(scroll, 1);

    m_status = new QLabel(this);
    m_status->setObjectName("muted");
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    lay->addWidget(m_status);

    connect(m_refreshBtn, &QPushButton::clicked, this, [this] {
        versionCache().clear();   // 装完新版本点刷新，要看到新号，不能吃了缓存
        rebuild();
    });
    connect(m_search, &QLineEdit::textChanged, this, &SoftwarePage::applyFilter);
    rebuild();
}

void SoftwarePage::clearRows()
{
    while (m_rowsLay->count() > 1) { // 末尾是 stretch
        QLayoutItem* item = m_rowsLay->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    m_rows.clear();
    m_heads.clear();
    m_versionLabels.clear();
    m_versionTargets.clear();
}

void SoftwarePage::rebuild()
{
    QElapsedTimer all;
    all.start();
    clearRows();
    const auto apps = AppScanner::loadIndex();
    const qint64 msLoad = all.elapsed();

    // 第一趟：本地匹配 + 每个分类的"已装 n / 总 m"（表头要带统计才有意义）
    QVector<AppEntry> matches(kSoftCount);
    QVector<QString> catOrder;
    QVector<int> catTotal, catInstalled;
    for (int i = 0; i < kSoftCount; ++i) {
        const SoftEntry& s = kSoft[i];
        matches[i] = matchLocal(apps, s.keywords);
        const QString cat = QString::fromUtf8(s.category);
        int ci = catOrder.indexOf(cat);
        if (ci < 0) {
            ci = catOrder.size();
            catOrder.append(cat);
            catTotal.append(0);
            catInstalled.append(0);
        }
        ++catTotal[ci];
        if (!matches[i].lnkPath.isEmpty())
            ++catInstalled[ci];
    }

    const qint64 msMatch = all.elapsed();
    constexpr int kStatusWidth = 172;
    constexpr int kBtnsWidth = 238;   // 打开 + 版本 + 安装/更新 + 官网 的最宽情况
    QHash<QString, QWidget*> headOf;

    for (int i = 0; i < kSoftCount; ++i) {
        const SoftEntry& s = kSoft[i];
        const AppEntry& match = matches[i];
        const QString cat = QString::fromUtf8(s.category);

        // 分类表头（每个分类只出现一次，表头跟着它下面第一组行一起显示/隐藏）
        if (!headOf.contains(cat)) {
            const int ci = catOrder.indexOf(cat);
            auto* h = new QLabel(QString("%1   ·   已装 %2 / %3")
                                     .arg(cat).arg(catInstalled[ci]).arg(catTotal[ci]), this);
            h->setObjectName("catHead");
            headOf.insert(cat, h);
            m_heads.append(h);
            m_rowsLay->insertWidget(m_rowsLay->count() - 1, h);
        }
        QWidget* head = headOf.value(cat);

        auto* row = new QWidget(this);
        row->setAttribute(Qt::WA_StyledBackground);
        row->setObjectName("row");
        auto* rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(10, 5, 8, 5);
        rowLay->setSpacing(10);

        auto* icon = new QLabel(row);
        icon->setFixedSize(24, 24);
        icon->setAlignment(Qt::AlignCenter);
        icon->setAttribute(Qt::WA_TranslucentBackground);
        auto* name = new QLabel(s.name, row);
        QFont f = name->font();
        f.setBold(true);
        name->setFont(f);
        auto* status = new QLabel(row);
        status->setFixedWidth(kStatusWidth);
        status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        rowLay->addWidget(icon);
        rowLay->addWidget(name);
        rowLay->addStretch(1);
        rowLay->addWidget(status);

        const bool installed = !match.lnkPath.isEmpty();
        const bool hasWinget = QString(s.wingetId).isEmpty() == false;
        if (installed) {
            // 已安装：本地图标 + 文件版本
            QIcon ic = match.iconFile.isEmpty() ? QIcon() : QIcon(match.iconFile);
            if (!ic.isNull())
                icon->setPixmap(ic.pixmap(24, 24));
            else
                icon->setPixmap(letterIcon(s.name, QColor(46, 160, 88)).pixmap(24, 24));
            // 版本号是逐文件的 PE 读，放到后台去量（fillVersions），这里先占位
            status->setText(QStringLiteral("✔ 已安装"));
            status->setStyleSheet("color:#2ea058;background:transparent;");
            status->setToolTip(match.target);
            m_versionLabels.append(status);
            m_versionTargets.append(match.target);
        } else {
            // 未安装：站点 favicon（异步抓取并缓存），无则字母图标
            const QString cached = webIconPath(s.url);
            if (QFileInfo::exists(cached)) {
                icon->setPixmap(QPixmap(cached).scaled(24, 24, Qt::KeepAspectRatio,
                                                       Qt::SmoothTransformation));
            } else {
                icon->setPixmap(letterIcon(s.name, QColor(90, 96, 108)).pixmap(24, 24));
                requestWebIcon(icon, s.url);
            }
            // 状态列说清楚"为什么不能一键装"，别让人点了下载才发现要自己找
            status->setText(hasWinget ? QString("未安装 · 可一键装")
                                      : QString("未安装 · 需手动下载"));
            status->setStyleSheet(QString("color:%1;background:transparent;")
                                      .arg(hasWinget ? "#8b93a1" : "#6b7280"));
        }

        // 按钮区固定宽度 + 靠右：这样每行的状态列右边缘对齐，才真的像一"列"
        auto* btns = new QWidget(row);
        btns->setFixedWidth(kBtnsWidth);
        auto* blay = new QHBoxLayout(btns);
        blay->setContentsMargins(0, 0, 0, 0);
        blay->setSpacing(6);
        blay->addStretch(1);
        rowLay->addWidget(btns);

        const QString siteUrl = QString(s.url);
        const bool searchOnly = isSearchFallback(siteUrl);
        QVector<QPushButton*> rowBtns;
        if (installed) {
            auto* openBtn = new QPushButton("打开", btns);
            openBtn->setToolTip(QString("启动 %1").arg(match.name));
            openBtn->setFixedWidth(52);
            const QString lnk = match.lnkPath;
            const QString args = match.args;
            connect(openBtn, &QPushButton::clicked, this, [lnk, args] {
                // 异步：带 DDE 的 lnk 同步启动会等回调（实测卡 700ms）
                Platform::launch(lnk, args, {}, true);
            });
            rowBtns.append(openBtn);
        }
        auto* verBtn = new QPushButton("版本", btns);
        verBtn->setToolTip(installed ? "本机已装版本（可定位文件），也可从版本列表里挑一个装"
                                     : "可选装的版本列表（winget），点一条静默安装");
        verBtn->setFixedWidth(52);
        rowBtns.append(verBtn);
        // 只有真有 winget 包才给"安装/更新"：没包的话点了只能跳官网，和「官网」按钮重复
        QPushButton* dlBtn = nullptr;
        if (hasWinget) {
            dlBtn = new QPushButton(installed ? "更新" : "安装", btns);
            dlBtn->setToolTip(QString("用 winget %1 %2，静默装到 %3")
                                  .arg(installed ? "更新" : "安装", QString(s.wingetId),
                                       DevEnv::installDirFor(s.name)));
            dlBtn->setFixedWidth(60);
            dlBtn->setObjectName("primary");
            rowBtns.append(dlBtn);
        }
        auto* siteBtn = new QPushButton(searchOnly ? "搜索" : "官网", btns);
        siteBtn->setToolTip(searchOnly ? QString("未收录该软件的官网，打开搜索：%1").arg(siteUrl)
                                       : siteUrl);
        siteBtn->setFixedWidth(52);
        rowBtns.append(siteBtn);
        for (auto* b : rowBtns) {
            b->setCursor(Qt::PointingHandCursor);
            blay->addWidget(b);
        }

        connect(verBtn, &QPushButton::clicked, this,
                [this, nm = QString(s.name), kw = QString(s.keywords), url = siteUrl,
                 winget = s.wingetId] {
                    showVersions(nm, kw, url, winget);
                });
        if (dlBtn)
            connect(dlBtn, &QPushButton::clicked, this,
                    [this, name = s.name, winget = s.wingetId, url = siteUrl] {
                        download(name, winget, url);
                    });
        connect(siteBtn, &QPushButton::clicked, this,
                [this, url = siteUrl] { openOfficial(url); });

        m_rowsLay->insertWidget(m_rowsLay->count() - 1, row);
        const QString key = (QString(s.name) + ' ' + QString(s.category) + ' '
                             + QString(s.keywords)).toLower();
        m_rows.append({row, head, key, installed, hasWinget});
    }

    applyFilter();
    fillVersions();
    Utils::log(QStringLiteral("software rebuild: index=%1ms match=%2ms rows=%3ms total=%4ms")
                   .arg(msLoad).arg(msMatch - msLoad).arg(all.elapsed() - msMatch).arg(all.elapsed()));
}

// 状态列里的版本号：先画行，再在后台把 27 个 exe 的版本读回来填进去。
// 同步做的话（实测 447ms 冷读）第一次点进这一页会整窗卡住，开机后磁盘忙时更明显。
void SoftwarePage::fillVersions()
{
    QStringList todo;
    for (const QString& t : m_versionTargets)
        if (!versionCache().contains(t) && !todo.contains(t))
            todo << t;
    if (todo.isEmpty()) {
        applyCachedVersions();
        return;
    }
    auto* watcher = new QFutureWatcher<QVector<QString>>(this);
    connect(watcher, &QFutureWatcher<QVector<QString>>::finished, this, [this, watcher, todo] {
        const QVector<QString> got = watcher->result();
        for (int i = 0; i < todo.size() && i < got.size(); ++i)
            versionCache().insert(todo[i], got[i]);
        watcher->deleteLater();
        applyCachedVersions();
    });
    watcher->setFuture(QtConcurrent::run([todo] {
        QVector<QString> out;
        out.reserve(todo.size());
        for (const QString& p : todo)
            out.append(Platform::fileVersion(p));
        return out;
    }));
}

void SoftwarePage::applyCachedVersions()
{
    for (int i = 0; i < m_versionLabels.size() && i < m_versionTargets.size(); ++i) {
        QLabel* lb = m_versionLabels[i].data();
        if (!lb)
            continue;                       // 行可能已被下一次 rebuild 销毁
        const QString v = versionCache().value(m_versionTargets[i]);
        lb->setText(v.isEmpty() ? QStringLiteral("✔ 已安装")
                                : QString("✔ 已安装  v%1").arg(v));
    }
}

// 搜索只做显示/隐藏，不重建行：重建会重新抓图标、也让输入框丢焦点
void SoftwarePage::applyFilter()
{
    const QString needle = m_search->text().trimmed().toLower();
    QHash<QWidget*, int> hitsPerHead;
    int shown = 0, visInstalled = 0, visOneClick = 0, allInstalled = 0, allOneClick = 0;
    for (const Row& r : m_rows) {
        if (r.installed)
            ++allInstalled;
        else if (r.oneClick)
            ++allOneClick;
        const bool vis = needle.isEmpty() || r.key.contains(needle);
        // 用 setVisible 之外的本地计数：页面还没显示时 isVisible() 恒为 false，不能当依据
        r.widget->setVisible(vis);
        if (!vis)
            continue;
        ++shown;
        ++hitsPerHead[r.head];
        if (r.installed)
            ++visInstalled;
        else if (r.oneClick)
            ++visOneClick;
    }
    for (QWidget* h : m_heads)
        h->setVisible(hitsPerHead.value(h, 0) > 0);

    if (shown == 0) {
        m_status->setText(QString("没有匹配 “%1” 的软件").arg(m_search->text().trimmed()));
        return;
    }
    // 统计口径要分清：过滤时"已装 n 个"说的是匹配项，不是全表，否则读起来自相矛盾
    if (needle.isEmpty()) {
        m_status->setText(QString("共 %1 个推荐软件 · 本机已装 %2 个 · 未装可一键装 %3 个")
                              .arg(m_rows.size()).arg(allInstalled).arg(allOneClick));
    } else {
        m_status->setText(QString("匹配 “%1” 的 %2 个 · 已装 %3 个 · 未装可一键装 %4 个（全表 %5 个，已装 %6 个）")
                              .arg(m_search->text().trimmed()).arg(shown)
                              .arg(visInstalled).arg(visOneClick)
                              .arg(m_rows.size()).arg(allInstalled));
    }
}

void SoftwarePage::requestWebIcon(QLabel* iconLabel, const QString& url)
{
    QUrl u(url);
    const QString host = u.host();
    if (host.isEmpty())
        return;
    QNetworkRequest req(QUrl("https://" + host + "/favicon.ico"));
    req.setTransferTimeout(8000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, iconLabel, url] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;
        const QByteArray data = reply->readAll();
        QImage img;
        if (!img.loadFromData(data) || img.isNull())
            return;
        const QString path = webIconPath(url);
        img.save(path);
        iconLabel->setPixmap(QPixmap(path).scaled(24, 24, Qt::KeepAspectRatio,
                                                  Qt::SmoothTransformation));
    });
}

// 每个软件装进"安装目录/软件名"自己的子目录，不把文件裸露在安装目录根下
void SoftwarePage::installVersion(const QString& name, const QString& wingetId,
                                  const QString& version)
{
    QString msg;
    if (!DevEnv::startSilentInstall(wingetId, version, name, &msg)) {
        QMessageBox::warning(this, "ltool", msg);
        return;
    }
    m_status->setText(
        QString("正在后台静默安装 %1 %2 → %3（进度看左下角的加载标志，点它打开安装日志，可随时取消）")
            .arg(name, version.isEmpty() ? "最新版" : version, DevEnv::installDirFor(name)));
}

void SoftwarePage::showVersions(const QString& name, const QString& keywords,
                                const QString& url, const QString& wingetId)
{
    const QStringList kws = keywords.split(';', Qt::SkipEmptyParts);
    const auto apps = AppScanner::loadIndex();

    QMenu menu(this);
    menu.setToolTipsVisible(true);
    // 版本列表几乎不变：有缓存直接用，只有首次或点"重新查询"才联网
    if (!wingetId.isEmpty()) {
        const QStringList cached = DevEnv::cachedVersions(wingetId);
        const qint64 fetched = DevEnv::versionCacheTime(wingetId);
        if (!cached.isEmpty()) {
            menu.addSection(QString("全部版本（winget · 缓存于 %1）· 点击静默安装")
                                .arg(QDateTime::fromMSecsSinceEpoch(fetched)
                                         .toString("yyyy-MM-dd")));
            for (const QString& v : cached)
                menu.addAction(
                    QString("安装 %1   到 %2").arg(v, DevEnv::installDirFor(name)), this,
                    [this, name, wingetId, v] { installVersion(name, wingetId, v); });
        }
        menu.addAction("⟳ 重新查询版本列表", this, [this, wingetId] {
            DevEnv::fetchVersionsAsync(wingetId, [this, wingetId](QStringList versions) {
                m_status->setText(versions.isEmpty()
                                      ? QString("%1 联网获取版本列表失败").arg(wingetId)
                                      : QString("%1 版本列表已更新（%2 个版本）")
                                            .arg(wingetId).arg(versions.size()));
            });
        });
        if (cached.isEmpty()) {
            auto* busy = menu.addAction("正在联网获取版本列表…");
            busy->setEnabled(false);
            DevEnv::fetchVersionsAsync(wingetId, [this, wingetId](QStringList versions) {
                m_status->setText(versions.isEmpty()
                                      ? QString("%1 联网获取版本列表失败").arg(wingetId)
                                      : QString("%1 版本列表已缓存（%2 个版本），再点“版本”即可选装")
                                            .arg(wingetId).arg(versions.size()));
            });
        }
    }

    // 本机已装版本：条目只写"版本 + 名称"，完整 exe 路径放 tooltip（无 winget 包的软件就只有这一段）
    struct LocalVer { QString ver; QString name; QString target; };
    QVector<LocalVer> locals;
    for (const AppEntry& a : apps) {
        const QString hay = (a.name + " " + a.target).toLower();
        bool hit = false;
        for (const QString& kw : kws)
            if (hay.contains(kw.trimmed().toLower())) {
                hit = true;
                break;
            }
        if (!hit)
            continue;
        QString ver = Platform::fileVersion(a.target);
        if (ver.isEmpty())
            ver = "未知版本";
        locals.append({ver, a.name, a.target});
    }
    bool any = !locals.isEmpty();
    if (any) {
        menu.addSection("本机已装版本");
        for (const LocalVer& lv : locals) {
            const QString target = lv.target;
            auto* act = menu.addAction(QString("v%1    %2").arg(lv.ver, lv.name));
            act->setToolTip(target);
            connect(act, &QAction::triggered, this, [target] {
                Platform::reveal(target);
            });
        }
    }
    if (any && wingetId.isEmpty()) {
        // 没有 winget 包就没有版本列表，给一条去官网挑其他版本的出口
        menu.addSeparator();
        menu.addAction(isSearchFallback(url) ? "用搜索引擎找其他版本" : "打开官网下载其他版本",
                       this, [this, url] { openOfficial(url); });
    }
    if (!any) {
        menu.addAction("未在本机找到已装版本")->setEnabled(false);
        menu.addAction(isSearchFallback(url) ? "用搜索引擎找官网下载" : "打开官网下载",
                       this, [this, url] { openOfficial(url); });
    }
    menu.exec(QCursor::pos());
}

void SoftwarePage::download(const QString& name, const QString& wingetId, const QString& url)
{
    if (wingetId.isEmpty()) {
        openOfficial(url);
        return;
    }
    installVersion(name, wingetId, QString());   // 静默装到"安装目录/软件名"
}

void SoftwarePage::openOfficial(const QString& url)
{
    Platform::launch(url);
}

void SoftwarePage::launchOrOpen(const QString& keywords, const QString& url)
{
    const QStringList kws = keywords.split(';', Qt::SkipEmptyParts);
    for (const AppEntry& a : AppScanner::loadIndex()) {
        const QString hay = (a.name + " " + a.target).toLower();
        for (const QString& kw : kws) {
            if (hay.contains(kw.trimmed().toLower())) {
                Platform::launch(a.lnkPath, {}, {}, true);   // 异步：带 DDE 的 lnk 同步会等回调
                return;
            }
        }
    }
    openOfficial(url);
}
