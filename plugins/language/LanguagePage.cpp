#include "LanguagePage.h"
#include "platform/Launch.h"

#include <QPointer>

#include "AppScanner.h"
#include "DevEnv.h"
#include "EnvPath.h"
#include "SilentInstallLog.h"
#include "Utils.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

struct LangMeta {
    const char* id;
    const char* name;
};

const LangMeta kLangs[] = {
    {"java", "Java"},
    {"c++", "C++ (MSYS2)"},
    {"node", "Node.js"},
    {"python", "Python"},
    {"rust", "Rust"},
};

// 语言 → 关联软件（名称用于本地应用索引匹配，url 为官网）
struct LangLink { const char* name; const char* url; };
QVector<LangLink> relatedSoftware(const QString& langId)
{
    if (langId == "java")
        return {{"IntelliJ IDEA", "https://www.jetbrains.com/idea/"},
                {"Maven", "https://maven.apache.org"},
                {"Gradle", "https://gradle.org"}};
    if (langId == "c++")
        return {{"CLion", "https://www.jetbrains.com/clion/"},
                {"CMake", "https://cmake.org"},
                {"Visual Studio", "https://visualstudio.microsoft.com"}};
    if (langId == "node")
        return {{"Visual Studio Code", "https://code.visualstudio.com"},
                {"npm", "https://www.npmjs.com"},
                {"pnpm", "https://pnpm.io"}};
    if (langId == "python")
        return {{"PyCharm", "https://www.jetbrains.com/pycharm/"},
                {"pip", "https://pip.pypa.io"},
                {"Jupyter", "https://jupyter.org"}};
    if (langId == "rust")
        return {{"RustRover", "https://www.jetbrains.com/rust/"},
                {"crates.io", "https://crates.io"},
                {"docs.rs", "https://docs.rs"}};
    return {};
}

// 语言 → 配置文件
struct LangFile { const char* label; QString path; };
QVector<LangFile> configFiles(const QString& langId)
{
    const QString home = QDir::homePath();
    if (langId == "java")
        return {{"Maven settings.xml", home + "/.m2/settings.xml"},
                {"Gradle gradle.properties", home + "/.gradle/gradle.properties"}};
    if (langId == "c++") {
        QVector<LangFile> out;
        QString msysRoot = "C:/msys64";
        for (const auto& v : DevEnv::detectVersions("c++")) {
            if (QDir::fromNativeSeparators(v.path).contains("msys64", Qt::CaseInsensitive)) {
                msysRoot = QDir::toNativeSeparators(v.path);
                break;
            }
        }
        out.append({"MSYS2 msys2.ini", msysRoot + "/msys2.ini"});
        out.append({"MSYS2 .bashrc", msysRoot + "/home/" + qEnvironmentVariable("USERNAME") + "/.bashrc"});
        return out;
    }
    if (langId == "node")
        return {{"npm .npmrc", home + "/.npmrc"},
                {"nvm settings.txt", home + "/AppData/Roaming/nvm/settings.txt"}};
    if (langId == "python")
        return {{"pip pip.ini", home + "/AppData/Roaming/pip/pip.ini"}};
    if (langId == "rust")
        return {{"cargo config.toml", home + "/.cargo/config.toml"},
                {"cargo env", home + "/.cargo/env"}};
    return {};
}

QString langName(const QString& id)
{
    for (const LangMeta& m : kLangs)
        if (id == m.id)
            return m.name;
    return id;
}

const char* kGreen = "#3fbf6f";
const char* kRed = "#d64545";
const char* kGray = "#8b93a1";
const char* kBlue = "#3f6ae0";

// 一个 QVBoxLayout 清空（连子控件一起），重建页面内容用。
// 两层都要清：只清外层的话，addLayout 进去的那排按钮会留在页面上继续显示，
// 重绘一次就漏一排"幽灵"控件（尺寸还是整页大小，看着像整块面板被复制了一份）。
// 另外必须先 hide()：deleteLater 要等事件循环，旧控件在这之前还会按上次的几何画出来。
void clearLayout(QVBoxLayout* lay)
{
    while (QLayoutItem* it = lay->takeAt(0)) {
        if (QWidget* w = it->widget()) {
            w->hide();
            w->deleteLater();
        } else if (QLayout* sub = it->layout()) {
            while (QLayoutItem* s2 = sub->takeAt(0)) {
                if (QWidget* w2 = s2->widget()) {
                    w2->hide();
                    w2->deleteLater();
                }
                delete s2;
            }
        }
        delete it;   // 子布局随这个 item 一起销毁
    }
}

// 次要说明文字：一行、超宽截断并用 tooltip 兜住全文
QLabel* mutedLabel(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName("muted");
    l->setToolTip(text);
    return l;
}

} // namespace

LanguagePage::LanguagePage(QWidget* parent)
    : QWidget(parent)
{
    buildShell();

    m_langList->clear();
    for (const LangMeta& m : kLangs) {
        auto* it = new QListWidgetItem(m.name, m_langList);
        it->setSizeHint(QSize(150, 38));   // 行高给足，别让背景块压到下一条
        it->setData(Qt::UserRole, QString(m.id));
    }
    connect(m_langList, &QListWidget::currentRowChanged, this, &LanguagePage::selectLanguage);
    if (m_langList->count() > 0)
        m_langList->setCurrentRow(0);
}

void LanguagePage::buildShell()
{
    auto* splitter = new QHBoxLayout(this);
    splitter->setContentsMargins(0, 0, 0, 0);
    splitter->setSpacing(12);

    m_langList = new QListWidget(this);
    m_langList->setFixedWidth(150);
    m_langList->setSpacing(4);              // 与设置页菜单同理：条目之间留缝
    m_langList->setUniformItemSizes(true);

    auto* right = new QWidget(this);
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(4, 2, 4, 2);
    rl->setSpacing(8);

    // 顶部：语言名 + 一句汇总 + 重新检测
    auto* head = new QHBoxLayout;
    m_title = new QLabel(right);
    QFont f = m_title->font();
    f.setPointSize(13);
    f.setBold(true);
    m_title->setFont(f);
    m_summary = mutedLabel(QString());
    m_rescanBtn = new QPushButton("重新检测", right);
    m_rescanBtn->setToolTip("重新跑一遍各组件的检测命令（起子进程，几百毫秒）");
    head->addWidget(m_title);
    head->addSpacing(10);
    head->addWidget(m_summary, 1);
    head->addWidget(m_rescanBtn);
    rl->addLayout(head);

    // 步骤条：三步都能直接点回去看，向导不是单向门
    auto* bar = new QHBoxLayout;
    bar->setSpacing(6);
    const char* tips[3] = {
        "第一步：看看这台机器上该语言的组件装了哪些、缺哪些",
        "第二步：把缺的静默装上（可以只装一个，也可以指定 winget 版本）",
        "第三步：把装好的接进环境变量（下拉切版本，写 PATH/JAVA_HOME 走按钮）",
    };
    for (int i = 0; i < 3; ++i) {
        auto* c = new QToolButton(right);
        c->setText(QString::fromUtf8(i == 0 ? "① 检测环境" : i == 1 ? "② 安装缺失" : "③ 配好环境变量"));
        c->setCheckable(true);
        c->setAutoRaise(true);
        c->setToolTip(tips[i]);
        c->setCursor(Qt::PointingHandCursor);
        connect(c, &QToolButton::clicked, this, [this, i] { setStep(i); });
        m_chips[i] = c;
        bar->addWidget(c);
    }
    bar->addStretch(1);
    rl->addLayout(bar);

    m_steps = new QStackedWidget(right);
    for (int i = 0; i < 3; ++i) {
        auto* page = new QWidget(m_steps);
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);
        m_lays[i] = lay;
        m_steps->addWidget(page);
    }
    rl->addWidget(m_steps, 1);

    m_status = mutedLabel(QString());
    m_status->setWordWrap(true);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    rl->addWidget(m_status);

    // 底部折叠区：关联软件 / 配置文件，默认收起，不跟主区抢高度
    m_moreBtn = new QToolButton(right);
    m_moreBtn->setText("▸ 关联软件与配置文件");
    m_moreBtn->setAutoRaise(true);
    m_moreBtn->setCursor(Qt::PointingHandCursor);
    m_moreBtn->setToolTip("IDE、构建工具和本语言的配置文件清单（双击打开）");
    connect(m_moreBtn, &QToolButton::clicked, this, [this] {
        const bool show = !m_morePanel->isVisible();
        m_morePanel->setVisible(show);
        m_moreBtn->setText(show ? "▾ 关联软件与配置文件" : QStringLiteral("▸ 关联软件与配置文件"));
        if (show)
            renderMore();
    });
    rl->addWidget(m_moreBtn);
    m_morePanel = new QWidget(right);
    m_morePanel->setVisible(false);
    m_moreLay = new QVBoxLayout(m_morePanel);
    m_moreLay->setContentsMargins(0, 0, 0, 0);
    m_moreLay->setSpacing(6);
    rl->addWidget(m_morePanel);

    splitter->addWidget(m_langList);
    splitter->addWidget(right, 1);

    connect(m_rescanBtn, &QPushButton::clicked, this, &LanguagePage::rescan);
}

// ---------- 语言切换 / 检测 ----------

void LanguagePage::selectLanguage(int row)
{
    if (row < 0 || row >= m_langList->count())
        return;
    m_currentLang = m_langList->item(row)->data(Qt::UserRole).toString();
    m_plugin = DevPlugin{};
    for (const DevPlugin& p : DevEnv::loadPlugins()) {
        if (p.id == m_currentLang) {
            m_plugin = p;
            break;
        }
    }
    m_hasDetect = false;
    m_rows.clear();
    m_title->setText(m_plugin.name.isEmpty() ? langName(m_currentLang) : m_plugin.name);
    if (m_plugin.items.isEmpty()) {
        m_summary->setText("没找到这个语言的插件定义（%APPDATA%/ltool/plugins 下的 json）");
        setStatus(QString());
        renderAll();
        return;
    }
    setStep(0);
    renderAll();
    rescan();   // 选语言即检测：第一步就是它，异步跑不挡界面
}

void LanguagePage::rescan()
{
    if (m_detecting || m_plugin.items.isEmpty())
        return;
    m_detecting = true;
    m_summary->setText("检测中…");
    updateHeader();
    DevPlugin copy = m_plugin;
    const QString langId = m_currentLang;
    auto future = QtConcurrent::run([copy]() mutable {
        DevEnv::detect(&copy);
        return copy;
    });
    auto* watcher = new QFutureWatcher<DevPlugin>(this);
    connect(watcher, &QFutureWatcher<DevPlugin>::finished, this,
            [this, watcher, langId] {
                m_detecting = false;
                const DevPlugin p = watcher->result();
                watcher->deleteLater();
                if (langId != m_currentLang)
                    return;
                m_plugin = p;
                m_hasDetect = true;
                renderAll();
                int ok = 0;
                for (const DevItem& it : p.items)
                    if (it.ok)
                        ++ok;
                setStatus(QString("检测完成：%1 个组件里 %2 个已装。%3")
                              .arg(p.items.size()).arg(ok)
                              .arg(ok == p.items.size()
                                       ? QStringLiteral("组件都齐了，直接看第三步的环境变量。")
                                       : QStringLiteral("点第二步把它们装上。")));
            });
    watcher->setFuture(future);
}

// ---------- 步骤切换与整体重绘 ----------

void LanguagePage::setStep(int step)
{
    step = qBound(0, step, 2);
    m_steps->setCurrentIndex(step);
    for (int i = 0; i < 3; ++i)
        m_chips[i]->setChecked(i == step);
    renderAll();
}

void LanguagePage::renderAll()
{
    renderDetect();
    renderInstall();
    renderEnv();
    updateHeader();
    if (m_morePanel->isVisible())
        renderMore();
}

void LanguagePage::updateHeader()
{
    int ok = 0;
    for (const DevItem& it : m_plugin.items)
        if (it.ok)
            ++ok;
    const int n = m_plugin.items.size();
    m_summary->setText(!n ? QStringLiteral("没有组件")
                   : m_detecting ? QStringLiteral("检测中…")
                   : m_hasDetect ? QStringLiteral("已装 %1 / %2 个组件").arg(ok).arg(n)
                                 : QStringLiteral("还没检测"));
    QStringList ids;
    missingIds(&ids);
    m_chips[1]->setText(ids.isEmpty()
                            ? QStringLiteral("② 安装缺失")
                            : QString("② 安装缺失（%1）").arg(ids.size()));
}

void LanguagePage::setStatus(const QString& text)
{
    m_status->setText(text);
}

QStringList LanguagePage::missingIds(QStringList* labelsOut) const
{
    QStringList ids, labels;
    for (const DevItem& it : m_plugin.items) {
        if (it.ok || it.wingetId.isEmpty())
            continue;
        if (ids.contains(it.wingetId))   // node 的 Node.js 和 npm 是同一个包
            continue;
        ids << it.wingetId;
        labels << it.label;
    }
    if (labelsOut)
        *labelsOut = labels;
    return ids;
}

QWidget* LanguagePage::makeRow(const QString& name, const QString& badge, const QString& badgeColor,
                               const QString& detail, QLabel** statusOut)
{
    auto* w = new QWidget(m_steps);
    w->setObjectName("row");
    // 卡片感：不然一行行文字和面板糊在一起，看不出"这是一个组件"
    w->setStyleSheet("QWidget#row{background:#1f2126;border:1px solid #2c2d34;border-radius:8px;}"
                     "QWidget#row QLabel{background:transparent;}");
    auto* lay = new QHBoxLayout(w);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(10);

    auto* nm = new QLabel(name, w);
    nm->setMinimumWidth(120);
    QFont f = nm->font();
    f.setBold(true);
    nm->setFont(f);
    auto* st = new QLabel(badge, w);
    st->setStyleSheet(QString("color:%1;").arg(badgeColor));
    st->setMinimumWidth(150);
    lay->addWidget(nm);
    lay->addWidget(st);
    if (!detail.isEmpty()) {
        auto* d = mutedLabel(detail);
        lay->addWidget(d, 1);
    } else {
        lay->addStretch(1);
    }
    if (statusOut)
        *statusOut = st;
    return w;
}

// ---------- 第 1 步：检测 ----------

void LanguagePage::renderDetect()
{
    auto* lay = m_lays[0];
    clearLayout(lay);
    if (m_plugin.items.isEmpty()) {
        lay->addWidget(mutedLabel("这个语言没有插件定义，无法检测。可以在 %APPDATA%/ltool/plugins 里加一个 json。"));
        lay->addStretch(1);
        return;
    }
    for (const DevItem& it : m_plugin.items) {
        QString badge, color;
        if (m_detecting) {
            badge = QStringLiteral("检测中…");
            color = kGray;
        } else if (!m_hasDetect) {
            badge = QStringLiteral("未检测");
            color = kGray;
        } else if (it.ok) {
            badge = QStringLiteral("● 已装 %1").arg(it.version);
            color = kGreen;
        } else {
            badge = QStringLiteral("○ 缺失");
            color = it.wingetId.isEmpty() ? kGray : kRed;
        }
        QString detail = it.cmd.join(QLatin1Char(' '));
        if (!it.wingetId.isEmpty())
            detail += QString("   ·   winget: %1").arg(it.wingetId);
        if (!it.note.isEmpty())
            detail += QString("   ·   %1").arg(it.note);
        QWidget* row = makeRow(it.label, badge, color, detail);
        if (!it.cmd.isEmpty()) {
            // 「验证」= 替他敲那行命令：用注册表里当前的 PATH 起新进程跑一遍，
            // 免得出"我明明装了怎么提示找不到"这种最劝退的情况
            auto* probeBtn = new QPushButton("验证", row);
            probeBtn->setCursor(Qt::PointingHandCursor);
            probeBtn->setToolTip(QString("用当前注册表里的 PATH 起一个新进程跑「%1」，"
                                         "把原样输出贴给你看（刚配好环境变量时最有用）")
                                     .arg(it.cmd.join(QLatin1Char(' '))));
            connect(probeBtn, &QPushButton::clicked, this,
                    [this, cmd = it.cmd, label = it.label] { showProbe(label, cmd); });
            static_cast<QHBoxLayout*>(row->layout())->addWidget(probeBtn);
        }
        lay->addWidget(row);
    }
    lay->addSpacing(6);
    auto* btns = new QHBoxLayout;
    auto* next = new QPushButton(m_detecting ? "检测中…" : "下一步：安装缺失 →");
    next->setObjectName("primary");
    next->setEnabled(m_hasDetect && !m_detecting);
    connect(next, &QPushButton::clicked, this, [this] {
        QStringList labels;
        const QStringList ids = missingIds(&labels);
        setStep(1);
        if (ids.isEmpty())
            setStatus("没有缺失的组件，直接看第三步的环境变量就行。");
    });
    btns->addWidget(next);
    auto* env = new QPushButton("直接看环境变量 →");
    connect(env, &QPushButton::clicked, this, [this] { setStep(2); });
    btns->addWidget(env);
    btns->addStretch(1);
    lay->addLayout(btns);
    lay->addStretch(1);
}

// ---------- 第 2 步：安装缺失 ----------

void LanguagePage::renderInstall()
{
    auto* lay = m_lays[1];
    clearLayout(lay);
    m_rows.clear();
    if (!m_hasDetect) {
        lay->addWidget(mutedLabel("先回第一步检测一遍，才知道缺哪些。"));
        lay->addStretch(1);
        return;
    }
    QStringList labels;
    const QStringList ids = missingIds(&labels);

    auto* top = new QHBoxLayout;
    auto* all = new QPushButton(ids.isEmpty() ? "没有需要安装的组件"
                                             : QString("全部静默安装（%1）").arg(ids.size()),
                                m_steps);
    all->setObjectName("primary");
    all->setEnabled(!ids.isEmpty() && m_queueIds.isEmpty());
    all->setToolTip("winget --silent 串行安装（同时只允许一个安装），装完自动配环境变量");
    connect(all, &QPushButton::clicked, this, &LanguagePage::installAllMissing);
    top->addWidget(all);
    auto* log = new QPushButton("安装日志");
    log->setToolTip("后台安装的输出与退出码都在这里，进行中的可以在里面取消");
    connect(log, &QPushButton::clicked, this, [] { SilentInstallLog::showLog(); });
    top->addWidget(log);
    top->addStretch(1);
    lay->addLayout(top);

    for (const DevItem& it : m_plugin.items) {
        if (it.ok)
            continue;
        if (it.wingetId.isEmpty()) {
            // 装不了的组件不藏起来：把"那该怎么办"写在行里，让他知道为什么这里没按钮
            lay->addWidget(makeRow(it.label, QStringLiteral("○ 无法自动安装"), kGray,
                                   it.note.isEmpty() ? QStringLiteral("没有配置 winget 包") : it.note));
            continue;
        }
        QLabel* st = nullptr;
        QWidget* row = makeRow(it.label, QStringLiteral("○ 待安装"), kRed,
                               QString("%1   →   %2").arg(it.wingetId, DevEnv::installDirFor(it.label)),
                               &st);
        auto* one = new QPushButton("安装", row);
        one->setToolTip(QString("winget install --id %1 --silent → %2")
                            .arg(it.wingetId, DevEnv::installDirFor(it.label)));
        connect(one, &QPushButton::clicked, this,
                [this, id = it.wingetId, label = it.label] { installOne(id, label, QString()); });
        static_cast<QHBoxLayout*>(row->layout())->addWidget(one);

        auto* ver = new QPushButton("装其他版本 ▾", row);
        ver->setToolTip("从 winget 的版本列表里挑一个（列表几乎不变，缓存在 versions.json，可点⟳联网刷新）");
        connect(ver, &QPushButton::clicked, this, [this, ver, id = it.wingetId, label = it.label] {
            QMenu menu(ver);
            const QStringList cached = DevEnv::cachedVersions(id);
            for (const QString& v : cached) {
                menu.addAction(v, [this, id, label, v] { installOne(id, label, v); });
            }
            if (cached.isEmpty())
                menu.addAction("（还没有版本列表，点下面联网获取）")->setEnabled(false);
            QAction* refresh = menu.addAction(QString("⟳ 联网获取版本列表%1")
                    .arg(DevEnv::versionCacheTime(id)
                             ? QString("（上次 %1）")
                                   .arg(QDateTime::fromMSecsSinceEpoch(DevEnv::versionCacheTime(id))
                                                .toString("yyyy-MM-dd"))
                             : QString()));
            connect(refresh, &QAction::triggered, this, [this, id, ver] {
                setStatus(QString("正在联网获取 %1 的版本列表…").arg(id));
                DevEnv::fetchVersionsAsync(id, [this, ver, id](QStringList versions) {
                    setStatus(versions.isEmpty()
                                  ? QString("%1 联网获取版本列表失败").arg(id)
                                  : QString("%1 版本列表已更新（%2 个版本）").arg(id).arg(versions.size()));
                    Q_UNUSED(ver);
                });
            });
            menu.exec(ver->mapToGlobal(QPoint(0, ver->height())));
        });
        static_cast<QHBoxLayout*>(row->layout())->addWidget(ver);
        m_rows.insert(it.wingetId, Row{row, st});
        lay->addWidget(row);
    }

    lay->addSpacing(6);
    auto* btns = new QHBoxLayout;
    auto* next = new QPushButton("下一步：配好环境变量 →");
    connect(next, &QPushButton::clicked, this, [this] { setStep(2); });
    btns->addWidget(next);
    btns->addStretch(1);
    lay->addLayout(btns);
    lay->addStretch(1);
}

void LanguagePage::installAllMissing()
{
    if (!m_queueIds.isEmpty()) {
        setStatus("上一批还在装（进度看「安装日志」），等它结束再点。");
        return;
    }
    QStringList labels;
    m_queueIds = missingIds(&labels);
    m_queueLabels = labels;
    m_queueLang = m_currentLang;
    m_queueDone = 0;
    if (m_queueIds.isEmpty())
        return;
    installNext();
}

void LanguagePage::installNext()
{
    if (m_queueIds.isEmpty()) {
        const int done = m_queueDone;
        m_queueDone = 0;
        m_queueLang.clear();
        setStatus(QString("一键安装结束：共处理 %1 个组件。").arg(done));
        rescan();
        setStep(2);   // 装完直接落到第三步，那里能看到/补齐环境变量
        return;
    }
    const QString id = m_queueIds.takeFirst();
    const QString label = m_queueLabels.takeFirst();
    const QString langId = m_queueLang;
    const int total = m_queueDone + m_queueIds.size() + 1;
    if (m_rows.contains(id) && m_rows[id].status)
        m_rows[id].status->setText(QStringLiteral("● 安装中…"));
    QString msg;
    const QPointer<LanguagePage> guard(this);
    if (!DevEnv::startSilentInstall(id, QString(), label, &msg,
            [this, guard, langId, label, id, total](int code, const QString& out, int jobId) {
                // 环境配置不依赖界面存活：页面关了也要照样把它接进 PATH
                const QString summary = DevEnv::postInstallEnvSetup(langId, label, code, out, jobId);
                if (!guard)
                    return;
                ++guard->m_queueDone;
                const bool ok = code == 0 || quint32(code) == 0x8A15002B;
                if (guard->m_rows.contains(id) && guard->m_rows[id].status)
                    guard->m_rows[id].status->setText(ok ? QStringLiteral("● 已装")
                                                         : QStringLiteral("○ 失败"));
                guard->setStatus(QString("%1 %2/%3：%4 → %5")
                                     .arg(langName(langId))
                                     .arg(guard->m_queueDone)
                                     .arg(total)
                                     .arg(label,
                                          ok ? (summary.isEmpty() ? QStringLiteral("已装（无需配置环境变量）")
                                                                  : summary)
                                             : QString("失败，退出码 0x%1（详情看「安装日志」）")
                                                   .arg(quint32(code), 8, 16, QLatin1Char('0'))));
                if (SilentInstallLog::wasCancelled(jobId)) {
                    guard->m_queueIds.clear();
                    guard->m_queueLabels.clear();
                    guard->setStatus(QString("%1 的安装被取消（已完成 %2 个），剩下的没有继续装")
                                         .arg(langName(langId)).arg(guard->m_queueDone));
                    guard->installNext();
                    return;
                }
                if (code == 1618 || out.contains("1618")) {
                    guard->m_queueIds.clear();
                    guard->m_queueLabels.clear();
                    guard->setStatus("检测到另一个安装正在运行（Windows Installer 同一时刻只允许一个），"
                                     "剩下的组件请等它结束后再点「全部静默安装」");
                    guard->installNext();
                    return;
                }
                guard->installNext();
            })) {
        QMessageBox::warning(this, "ltool", msg);
        m_queueIds.clear();
        m_queueLabels.clear();
        return;
    }
    setStatus(QString("正在静默安装 %1：%2 → %3")
                  .arg(langName(m_queueLang), label, DevEnv::installDirFor(label)));
}

void LanguagePage::installOne(const QString& wingetId, const QString& label, const QString& version)
{
    if (!m_queueIds.isEmpty()) {
        setStatus("上一批安装还在进行（Windows Installer 同一时刻只允许一个），等它结束再点。");
        return;
    }
    QString msg;
    const QString langId = m_currentLang;
    const QPointer<LanguagePage> guard(this);
    if (m_rows.contains(wingetId) && m_rows[wingetId].status)
        m_rows[wingetId].status->setText(QStringLiteral("● 安装中…"));
    if (!DevEnv::startSilentInstall(wingetId, version, label, &msg,
            [langId, label, guard](int code, const QString& out, int jobId) {
                const QString summary = DevEnv::postInstallEnvSetup(langId, label, code, out, jobId);
                if (!guard)
                    return;
                guard->setStatus(QString("%1 安装结束：%2")
                                     .arg(label,
                                          code == 0 || quint32(code) == 0x8A15002B
                                              ? (summary.isEmpty() ? QStringLiteral("已装（无需配环境变量）")
                                                                   : summary)
                                              : QString("失败，退出码 0x%1（详情看「安装日志」）")
                                                    .arg(quint32(code), 8, 16, QLatin1Char('0'))));
                guard->rescan();   // 装完重新检测一遍，行上的徽章和"缺几个"才跟得上
            })) {
        QMessageBox::warning(this, "ltool", msg);
        return;
    }
    setStatus(QString("正在静默安装 %1 %2 → %3（进度点「安装日志」看，那里能取消）")
                  .arg(wingetId, version.isEmpty() ? QStringLiteral("最新版") : version,
                       DevEnv::installDirFor(label)));
}

// ---------- 第 3 步：环境变量 ----------

QString LanguagePage::activeVersionPath() const
{
    QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
    return cfg.value(QString("lang/%1/activePath").arg(m_currentLang)).toString();
}

void LanguagePage::renderEnv()
{
    auto* lay = m_lays[2];
    clearLayout(lay);
    m_versionBox = nullptr;   // 上面的控件刚被 deleteLater，别留下野指针
    m_useBtn = nullptr;
    m_envDetail = nullptr;
    if (m_plugin.items.isEmpty()) {
        lay->addWidget(mutedLabel("没有组件可配置。"));
        lay->addStretch(1);
        return;
    }
    const QVector<DevEnv::LangVersion> vers = DevEnv::detectVersions(m_currentLang);
    const QString active = activeVersionPath();

    auto* head = new QHBoxLayout;
    auto* verLabel = new QLabel("本机已装版本");
    verLabel->setMinimumWidth(96);
    head->addWidget(verLabel);
    m_versionBox = new QComboBox(m_steps);
    m_versionBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_versionBox->setToolTip("这里只切换本机已有的版本（写 PATH / JAVA_HOME / nvm use / rustup default）；"
                             "要装新版本去第二步");
    if (vers.isEmpty()) {
        m_versionBox->addItem("（本机没探测到已装版本）");
    }
    for (const DevEnv::LangVersion& v : vers) {
        const QString path = QDir::toNativeSeparators(v.path);
        m_versionBox->addItem(QString("%1  %2").arg(path.compare(active, Qt::CaseInsensitive) == 0
                                                        ? QStringLiteral("●")
                                                        : QStringLiteral("○"),
                                                    v.label),
                              path);
    }
    m_useBtn = new QPushButton("设为当前使用");
    m_useBtn->setObjectName("primary");
    m_useBtn->setEnabled(!vers.isEmpty());
    connect(m_useBtn, &QPushButton::clicked, this, &LanguagePage::applyActiveVersion);
    head->addWidget(m_versionBox, 1);
    head->addWidget(m_useBtn);
    lay->addLayout(head);

    // 当前到底接进去没有：把 JAVA_HOME / PATH 条目的实际状态摊开给他看
    QString detail;
    if (!active.isEmpty())
        detail += QString("当前使用：%1\n").arg(active);
    for (const DevEnv::LangVersion& v : vers) {
        const QString bin = QDir::toNativeSeparators(v.path) + "\\bin";
        const bool inPath = EnvPath::userPathContains(bin) || EnvPath::userPathContains(v.path);
        detail += QString("%1 %2   bin 在用户 PATH：%3\n")
                      .arg(inPath ? QStringLiteral("●") : QStringLiteral("○"),
                           QDir::toNativeSeparators(v.path),
                           inPath ? QStringLiteral("是") : QStringLiteral("否"));
    }
    if (detail.isEmpty())
        detail = QStringLiteral("没探测到本机版本，先回第二步装一个。");
    m_envDetail = new QLabel(detail);
    m_envDetail->setObjectName("muted");
    m_envDetail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(m_envDetail);

    auto* btns = new QHBoxLayout;
    auto* fix = new QPushButton("补齐环境变量");
    fix->setToolTip("不切版本，只把当前生效版本的 bin / JAVA_HOME 等重新写一遍（装完没生效时用这个）");
    connect(fix, &QPushButton::clicked, this, &LanguagePage::fixEnv);
    btns->addWidget(fix);
    auto* back = new QPushButton("← 回第二步");
    connect(back, &QPushButton::clicked, this, [this] { setStep(1); });
    btns->addWidget(back);
    btns->addStretch(1);
    lay->addLayout(btns);
    lay->addWidget(mutedLabel("改动写进注册表（HKCU\\Environment），对**新开的**终端/IDE 生效；"
                              "已经开着的窗口要重开。"));
    lay->addStretch(1);
}

// 「验证一下」的结果窗：非模态，能复制，看得见"命令到底解析到哪个 exe"。
void LanguagePage::showProbe(const QString& label, const QStringList& cmd)
{
    const DevEnv::ProbeResult r = DevEnv::probe(cmd);
    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QString("验证 %1").arg(label));
    dlg->resize(680, 300);
    auto* lay = new QVBoxLayout(dlg);
    auto* head = new QLabel(QString("<b>%1</b>   退出码 %2   %3")
                                .arg(r.program + (r.args.isEmpty()
                                                     ? QString()
                                                     : QLatin1Char(' ') + r.args.join(QLatin1Char(' '))),
                                     QString::number(r.exitCode),
                                     r.ok ? QStringLiteral("<span style='color:#3fbf6f'>成功</span>")
                                          : QStringLiteral("<span style='color:#d64545'>失败</span>")));
    head->setTextFormat(Qt::RichText);
    lay->addWidget(head);
    lay->addWidget(new QLabel(r.resolved.isEmpty()
                                  ? QStringLiteral("PATH 里没找到这个命令（所以肯定跑不通）")
                                  : QString("命令位置：%1").arg(r.resolved)));
    auto* out = new QPlainTextEdit(r.output);
    out->setReadOnly(true);
    out->setStyleSheet("font-family:Consolas,monospace;font-size:9pt;");
    lay->addWidget(out, 1);
    auto* btns = new QHBoxLayout;
    auto* copy = new QPushButton("复制输出", dlg);
    connect(copy, &QPushButton::clicked, dlg, [out] {
        QApplication::clipboard()->setText(out->toPlainText());
    });
    auto* closeBtn = new QPushButton("关闭", dlg);
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::close);
    btns->addStretch(1);
    btns->addWidget(copy);
    btns->addWidget(closeBtn);
    lay->addLayout(btns);
    dlg->show();
    if (!r.resolved.isEmpty() && r.ok)
        setStatus(QString("%1 验证通过：%2").arg(label, r.output.section(QLatin1Char('\n'), 0, 0)));
}

void LanguagePage::applyActiveVersion()
{
    if (!m_versionBox || m_versionBox->currentIndex() < 0)
        return;
    const QString path = m_versionBox->currentData().toString();
    if (path.isEmpty()) {
        setStatus("这个条目没有可切换的本机路径。");
        return;
    }
    QString what, err;
    const bool ok = DevEnv::applyEnvFor(m_currentLang, path, m_versionBox->currentText(), true,
                                        &what, &err);
    if (ok) {
        QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
        cfg.setValue(QString("lang/%1/activePath").arg(m_currentLang), path);
        setStatus(QString("已切换：%1").arg(what));
    } else {
        setStatus(err.isEmpty() ? QStringLiteral("切换失败") : err);
    }
    renderEnv();
}

void LanguagePage::fixEnv()
{
    const QVector<DevEnv::LangVersion> vers = DevEnv::detectVersions(m_currentLang);
    if (vers.isEmpty()) {
        setStatus("没探测到本机版本，先装一个再配环境变量。");
        return;
    }
    const QString active = activeVersionPath();
    const DevEnv::LangVersion* pick = &vers.first();
    for (const DevEnv::LangVersion& v : vers) {
        if (QDir::toNativeSeparators(v.path).compare(active, Qt::CaseInsensitive) == 0) {
            pick = &v;
            break;
        }
    }
    QString what, err;
    // activate=false：只保证"能用"，不改用户当前选中的版本
    const bool ok = DevEnv::applyEnvFor(m_currentLang, pick->path, pick->label, false, &what, &err);
    setStatus(ok ? QString("环境变量：%1（对新开的终端生效）").arg(what)
                 : QString("配置失败：%1").arg(err));
    renderEnv();
}

// ---------- 底部折叠区 ----------

void LanguagePage::renderMore()
{
    clearLayout(m_moreLay);
    const QString id = m_currentLang;

    auto* relTitle = mutedLabel("关联软件（双击启动本地应用 / 打开官网）");
    m_moreLay->addWidget(relTitle);
    auto* rel = new QListWidget(m_morePanel);
    rel->setMaximumHeight(120);
    const auto apps = AppScanner::loadIndex();
    for (const LangLink& l : relatedSoftware(id)) {
        QString status = QString("官网: %1").arg(l.url);
        for (const AppEntry& a : apps) {
            if (a.name.contains(l.name, Qt::CaseInsensitive)
                || a.target.contains(l.name, Qt::CaseInsensitive)) {
                status = QStringLiteral("已安装 · 双击启动 · ") + status;
                break;
            }
        }
        auto* it = new QListWidgetItem(QString("%1    %2").arg(l.name, status), rel);
        it->setData(Qt::UserRole, QString(l.url));
        it->setData(Qt::UserRole + 1, QString(l.name));
    }
    connect(rel, &QListWidget::itemDoubleClicked, this,
            [this, rel](QListWidgetItem*) { openRelatedSoftware(rel, rel->currentRow()); });
    m_moreLay->addWidget(rel);

    m_moreLay->addWidget(mutedLabel("配置文件（双击打开；不存在时会在资源管理器里定位到父目录）"));
    auto* files = new QListWidget(m_morePanel);
    files->setMaximumHeight(110);
    for (const LangFile& lf : configFiles(id)) {
        const bool exists = QFileInfo::exists(lf.path);
        auto* it = new QListWidgetItem(QString("%1    %2%3")
                                            .arg(lf.label, QDir::toNativeSeparators(lf.path),
                                                 exists ? QString() : QStringLiteral("   （不存在）")),
                                       files);
        it->setData(Qt::UserRole, lf.path);
    }
    connect(files, &QListWidget::itemDoubleClicked, this,
            [this, files](QListWidgetItem*) { openConfigFile(files, files->currentRow()); });
    m_moreLay->addWidget(files);
}

void LanguagePage::openConfigFile(QListWidget* list, int row)
{
    if (!list || row < 0 || row >= list->count())
        return;
    const QString path = list->item(row)->data(Qt::UserRole).toString();
    if (QFileInfo::exists(path))
        Platform::launch(path);
    else
        Platform::reveal(path);
}

void LanguagePage::openRelatedSoftware(QListWidget* list, int row)
{
    if (!list || row < 0 || row >= list->count())
        return;
    const QString url = list->item(row)->data(Qt::UserRole).toString();
    const QString name = list->item(row)->data(Qt::UserRole + 1).toString();
    for (const AppEntry& a : AppScanner::loadIndex()) {
        if (a.name.contains(name, Qt::CaseInsensitive)
            || a.target.contains(name, Qt::CaseInsensitive)) {
            Platform::launch(a.target, QString(), QString(), true);
            return;
        }
    }
    QDesktopServices::openUrl(QUrl(url));
}
