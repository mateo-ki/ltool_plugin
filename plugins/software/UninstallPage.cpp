#include "UninstallPage.h"
#include "platform/Launch.h"
#include "platform/Paths.h"

#include "Firewall.h"
#include "InfoRow.h"
#include "Utils.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QIcon>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

#include <windows.h>

// 行内四个按钮的宽度。表头要靠这几个数算出"操作"列的起点，两边必须一致。
constexpr int kBtnW = 52;        // 卸载 / 位置
constexpr int kFwW = 68;         // 防火墙 / 已允许 / 未允许
constexpr int kMoreW = 64;       // 更多 ▾
constexpr int kRowGap = 8;       // 行内控件间距
constexpr int kActionsW = kBtnW + kFwW + kBtnW + kMoreW + 3 * kRowGap;

namespace {

QString rString(HKEY k, const wchar_t* name)
{
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
        return {};
    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return {};
    QByteArray buf(int(size), 0);
    if (RegQueryValueExW(k, name, nullptr, &type, (BYTE*)buf.data(), &size) != ERROR_SUCCESS)
        return {};
    QString s = QString::fromWCharArray((const wchar_t*)buf.data(), int(size) / 2);
    while (s.endsWith(QChar(0)))
        s.chop(1);
    return s.trimmed();
}

DWORD rDword(HKEY k, const wchar_t* name)
{
    DWORD v = 0, sz = sizeof(v), t = 0;
    if (RegQueryValueExW(k, name, nullptr, &t, (BYTE*)&v, &sz) != ERROR_SUCCESS)
        return 0;
    return t == REG_DWORD ? v : 0;
}

// 三处 Uninstall 根：64 位系统视图、32 位兼容视图、当前用户
const struct { HKEY root; const wchar_t* sub; const char* bitness; } kUninstallRoots[] = {
    { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", "64 位" },
    { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall", "32 位" },
    { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", "当前用户" },
};

// InstallDate 是 "20240815" 这种 8 位数字（个别软件会带后缀），能认就认出来
QString prettyDate(const QString& raw)
{
    const QString s = raw.trimmed();
    if (s.size() >= 8) {
        bool digits = true;
        for (int i = 0; i < 8; ++i)
            digits = digits && s[i].isDigit();
        if (digits) {
            const QDate d = QDate::fromString(s.left(8), "yyyyMMdd");
            if (d.isValid())
                return d.toString("yyyy-MM-dd");
        }
    }
    // 有的软件写的是 "Sun Oct 12 19:25:22 2025" 这种 ctime 格式，直接吐出来很难看
    const QDateTime dt = QDateTime::fromString(s, Qt::TextDate);
    if (dt.isValid())
        return dt.toString("yyyy-MM-dd");
    return s;
}

QString humanSize(qint64 kb)
{
    if (kb <= 0)
        return QStringLiteral("未提供");
    const double mb = kb / 1024.0;
    if (mb >= 1024.0)
        return QString("%1 GB").arg(mb / 1024.0, 0, 'f', 2);
    return QString("%1 MB").arg(mb, 0, 'f', 0);
}

// 把一条命令行拆成"程序 + 参数"，注意注册表里既有加引号的也有没加的
void splitCommand(const QString& cmd, QString* exe, QString* params)
{
    *exe = Utils::exeFromCommand(cmd);
    if (cmd.startsWith(QLatin1Char('"'))) {
        const int close = cmd.indexOf(QLatin1Char('"'), 1);
        *params = close > 0 ? cmd.mid(close + 1).trimmed() : QString();
    } else {
        *params = cmd.size() > exe->size() ? cmd.mid(exe->size()).trimmed() : QString();
    }
}

// DisplayIcon 常见形态：C:\dir\a.exe 或 "C:\dir\a.exe",0 —— 清成能用的 exe 路径
QString cleanIconPath(const QString& raw)
{
    QString p = Platform::expandEnv(raw.trimmed());
    p.remove(QLatin1Char('"'));
    const int comma = p.lastIndexOf(QLatin1Char(','));
    if (comma > 2) {
        bool ok = false;
        const int idx = p.mid(comma + 1).toInt(&ok);
        if (ok && idx >= 0 && idx < 128 && p.left(comma).endsWith(QLatin1String(".exe"), Qt::CaseInsensitive))
            p = p.left(comma);
    }
    return QDir::toNativeSeparators(p);
}

// 名字比对用的键：去掉空格和符号，只留字母数字
QString nameKey(const QString& s)
{
    QString k;
    for (const QChar c : s)
        if (c.isLetterOrNumber())
            k += c;
    return k.toLower();
}

// 注册表没写卸载命令、或写的命令指向的文件已经没了时，去安装目录和几个常见子目录里
// 找软件自带的卸载器：Inno / NSIS 那一套经常把 unins000.exe 放在 bin 下而不是根目录。
// 只认名字像卸载器的，别把主程序当成卸载器递上去。
QString findUninstaller(const QString& installLocation, const QString& wantBase)
{
    if (installLocation.isEmpty() || !QFileInfo(installLocation).isDir())
        return {};
    static const char* const kSubs[] = { "", "bin", "Bin", "sbin", "app", "application", "Application",
                                         "tools", "uninstall", "Uninstall" };
    QString best;
    int bestScore = 0;
    for (const char* sub : kSubs) {
        const QString s = QString::fromUtf8(sub);
        const QString dir = s.isEmpty() ? installLocation : QDir(installLocation).absoluteFilePath(s);
        if (!QFileInfo(dir).isDir())
            continue;
        QDirIterator it(dir, {QStringLiteral("*.exe")}, QDir::Files);
        while (it.hasNext()) {
            const QFileInfo fi(it.next());
            const QString base = fi.completeBaseName();
            int score = 0;
            if (!wantBase.isEmpty() && base.compare(wantBase, Qt::CaseInsensitive) == 0)
                score = 40;                        // 注册表那条命令的同名文件，最可信
            else if (base.startsWith(QLatin1String("unins"), Qt::CaseInsensitive))
                score = 30;                        // Inno Setup
            else if (base.contains(QLatin1String("uninstall"), Qt::CaseInsensitive))
                score = 20;                        // NSIS / 各家自研
            else if (base.startsWith(QLatin1String("remove"), Qt::CaseInsensitive)
                     || base.startsWith(QLatin1String("unattend"), Qt::CaseInsensitive))
                score = 10;                        // 少见的命名
            if (score > bestScore) {
                bestScore = score;
                best = fi.absoluteFilePath();      // 同分时先扫到的（根目录）优先
            }
        }
    }
    return best;
}

// 静默参数：只给认得出格式的安装器加，认不出来就别乱猜
QString silentFlagFor(const QString& exe)
{
    const QString base = QFileInfo(exe).completeBaseName().toLower();
    if (base.startsWith(QStringLiteral("unins")))
        return QStringLiteral("/SILENT /NORESTART /SUPPRESSMSGBOXES");   // Inno Setup
    if (base.contains(QStringLiteral("uninstall")))
        return QStringLiteral("/S");                                     // NSIS
    return QString();
}

} // namespace

// 猜"这个软件本体的 exe"：防火墙规则要针对它，而注册表里给的往往是卸载器。
// 顺序：DisplayIcon → 安装目录里名字最像/最大的 exe → 卸载器同目录里的 exe
QString UninstallPage::guessMainExe(const Entry& e, bool* sure)
{
    *sure = false;
    if (!e.displayIcon.isEmpty()) {
        const QString p = cleanIconPath(e.displayIcon);
        if (p.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive) && QFileInfo::exists(p)) {
            *sure = true;
            return p;
        }
    }
    const QString want = nameKey(e.name);
    auto scanDir = [&](const QString& dir) -> QString {
        if (dir.isEmpty() || !QFileInfo(dir).isDir())
            return {};
        QString best;
        qint64 bestScore = -1;
        QDirIterator it(dir, {QStringLiteral("*.exe")}, QDir::Files);
        while (it.hasNext()) {
            const QFileInfo fi(it.next());
            const QString key = nameKey(fi.completeBaseName());
            const bool likeName = !want.isEmpty() && (key.startsWith(want) || want.startsWith(key));
            const qint64 score = (likeName ? qint64(1e12) : qint64(0)) + fi.size();
            if (score > bestScore) {
                bestScore = score;
                best = fi.absoluteFilePath();
            }
        }
        return bestScore > 0 ? best : QString();
    };
    // 只翻安装目录本身 + 几个常见子目录：递归整棵 Office/ASUS 那种大树会把扫描拖到几秒，
    // 而这页是点一下就要出结果的
    if (QFileInfo(e.installLocation).isDir()) {
        QString found = scanDir(e.installLocation);
        for (const char* sub : {"bin", "Bin", "app", "application", "Application", "programs", "x64", "win32"}) {
            if (!found.isEmpty())
                break;
            found = scanDir(QDir(e.installLocation).absoluteFilePath(QString::fromUtf8(sub)));
        }
        if (!found.isEmpty()) {
            *sure = true;
            return found;
        }
    }
    // 最后才退到"卸载器同目录"：这里挑出来的往往就是 uninstall.exe 本身
    if (!e.uninstallString.isEmpty())
        return scanDir(QFileInfo(cleanIconPath(Utils::exeFromCommand(e.uninstallString))).absolutePath());
    return {};
}

UninstallPage::UninstallPage(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 8, 8);
    lay->setSpacing(10);

    auto* top = new QHBoxLayout;
    top->setSpacing(8);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("筛选名称或发布者…"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedWidth(240);
    auto* rescan = new QPushButton(QStringLiteral("重新扫描"), this);
    auto* sysToggle = new QPushButton(QStringLiteral("显示系统组件"), this);
    sysToggle->setCheckable(true);
    sysToggle->setToolTip(QStringLiteral("默认不显示 Windows 更新、运行库这类 SystemComponent 条目"));
    top->addStretch(1);
    top->addStretch(1);
    top->addWidget(m_search);
    top->addWidget(sysToggle);
    top->addWidget(rescan);
    lay->addLayout(top);

    m_list = new QListWidget(this);
    m_list->setUniformItemSizes(true);
    m_list->setIconSize(QSize(20, 20));
    // 动作全在行上，不再需要"选中一行"这件事
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 滚动条常驻：不然表头和列的宽度会随页数忽左忽右对不齐
    m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    lay->addWidget(m_list, 1);

    // 表头：列宽和 makeRow 一致；左右还要按列表的边框/滚动条宽度补一下，
    // 因为行是画在 viewport 里的，比外面这条表头窄一截
    auto* head = new QHBoxLayout;
    const int sbw = m_list->verticalScrollBar()->sizeHint().width();
    const int fw = m_list->frameWidth();
    head->setContentsMargins(6 + fw, 0, 6 + fw + sbw, 0);
    head->setSpacing(8);
    const auto headCol = [this](const QString& text, int w, Qt::Alignment al) {
        auto* l = new QLabel(text, this);
        l->setObjectName(QStringLiteral("colHead"));
        if (w > 0)
            l->setFixedWidth(w);
        l->setAlignment(al | Qt::AlignVCenter);
        return l;
    };
    head->addWidget(headCol(QStringLiteral("软件"), 0, Qt::AlignLeft), 1);
    head->addWidget(headCol(QStringLiteral("安装日期"), 82, Qt::AlignLeft));
    head->addWidget(headCol(QStringLiteral("占用"), 66, Qt::AlignRight));
    head->addWidget(headCol(QStringLiteral("位数"), 56, Qt::AlignLeft));
    head->addWidget(headCol(QStringLiteral("操作"), kActionsW, Qt::AlignLeft));
    // 得先有 m_list 才能量到滚动条宽度，所以插回列表上面（addLayout 会掉到底部去）
    lay->insertLayout(1, head);

    // 底栏：左边一句实话，右边分页
    auto* bar = new QHBoxLayout;
    bar->setSpacing(8);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("muted"));
    m_status->setWordWrap(true);
    m_prev = new QPushButton(QStringLiteral("← 上一页"), this);
    m_pageInfo = new QLabel(this);
    m_pageInfo->setObjectName(QStringLiteral("muted"));
    m_next = new QPushButton(QStringLiteral("下一页 →"), this);
    for (QPushButton* b : { m_prev, m_next }) {
        b->setObjectName(QStringLiteral("rowBtn"));
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
    }
    bar->addWidget(m_status, 1);
    bar->addWidget(m_prev);
    bar->addWidget(m_pageInfo);
    bar->addWidget(m_next);
    lay->addLayout(bar);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(180);
    connect(m_debounce, &QTimer::timeout, this, &UninstallPage::applyFilter);
    // 搜索框：连打时只在停手后重排一次（每页只建 20 行，但仍然不值得每键都建）
    connect(m_search, &QLineEdit::textChanged, this, [this] { m_debounce->start(); });
    connect(rescan, &QPushButton::clicked, this, &UninstallPage::reload);
    connect(sysToggle, &QPushButton::toggled, this, [this, sysToggle] {
        m_showSystem = sysToggle->isChecked();
        sysToggle->setText(m_showSystem ? QStringLiteral("隐藏系统组件")
                                        : QStringLiteral("显示系统组件"));
        reload();
    });
    connect(m_prev, &QPushButton::clicked, this, [this] { showPage(m_page - 1); });
    connect(m_next, &QPushButton::clicked, this, [this] { showPage(m_page + 1); });
}


void UninstallPage::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (m_loaded)
        return;
    m_loaded = true;
    reload();
}

void UninstallPage::reload()
{
    QElapsedTimer all;
    all.start();
    int nFallback = 0;
    m_entries.clear();
    m_fwStale = true;
    QFileIconProvider icons;          // 图标走 shell，一次扫描里复用同一个 provider
    for (const auto& root : kUninstallRoots) {
        HKEY base = nullptr;
        if (RegOpenKeyExW(root.root, root.sub, 0, KEY_READ, &base) != ERROR_SUCCESS)
            continue;
        DWORD count = 0, maxName = 0;
        RegQueryInfoKeyW(base, nullptr, nullptr, nullptr, &count, &maxName, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr);
        std::wstring nm(size_t(maxName) + 2, 0);
        for (DWORD i = 0; i < count; ++i) {
            DWORD len = DWORD(nm.size());
            if (RegEnumKeyExW(base, i, nm.data(), &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                continue;
            const QString keyName = QString::fromWCharArray(nm.data(), int(len));
            HKEY k = nullptr;
            if (RegOpenKeyExW(base, (LPCWSTR)keyName.utf16(), 0, KEY_READ, &k) != ERROR_SUCCESS)
                continue;
            Entry e;
            e.name = rString(k, L"DisplayName");
            e.system = rDword(k, L"SystemComponent") == 1;
            const DWORD hidden = rDword(k, L"Hidden");
            e.version = rString(k, L"DisplayVersion");
            e.publisher = rString(k, L"Publisher");
            e.installDate = prettyDate(rString(k, L"InstallDate"));
            e.installLocation = rString(k, L"InstallLocation");
            e.displayIcon = rString(k, L"DisplayIcon");
            e.uninstallString = rString(k, L"UninstallString");
            e.quietString = rString(k, L"QuietUninstallString");
            e.sizeKB = qint64(rDword(k, L"EstimatedSize"));
            e.registryPath = QString(root.root == HKEY_CURRENT_USER ? "HKEY_CURRENT_USER\\" : "HKEY_LOCAL_MACHINE\\")
                             + QString::fromWCharArray(root.sub) + "\\" + keyName;
            e.bitness = root.bitness;
            RegCloseKey(k);

            // 勾了"显示系统组件"是把它们一起列出来，不是只列它们
            if (e.name.isEmpty() || hidden != 0 || (!m_showSystem && e.system))
                continue;
            // MSI 装的 UninstallString 是 "MsiExec.exe /I{GUID}"，那是"修改安装"，
            // 卸载必须换成 /x；产品码优先取键名（MSI 的键名就是 {GUID}）
            if (keyName.startsWith(QLatin1Char('{'))) {
                e.productCode = keyName;
            } else if (e.uninstallString.contains(QLatin1String("msiexec"), Qt::CaseInsensitive)) {
                const int a = e.uninstallString.indexOf(QLatin1Char('{'));
                const int b = e.uninstallString.indexOf(QLatin1Char('}'));
                if (a >= 0 && b > a)
                    e.productCode = e.uninstallString.mid(a, b - a + 1);
            }
            const QString cmd = e.uninstallString.isEmpty() ? e.quietString : e.uninstallString;
            QString regExe;
            if (e.productCode.isEmpty() && cmd.isEmpty()) {
                e.runnable = false;
            } else if (!e.productCode.isEmpty()) {
                e.runnable = true;                     // msiexec 总在
            } else {
                QString params;
                splitCommand(cmd, &regExe, &params);
                e.runnable = !Utils::fileTargetMissing(regExe);
            }
            if (!e.runnable) {
                // 注册表那条走不通，再去安装目录本身和 bin 这类常见子目录里找卸载器
                e.fallbackUninstaller = findUninstaller(e.installLocation,
                                                        QFileInfo(regExe).completeBaseName());
                if (!e.fallbackUninstaller.isEmpty()) {
                    e.runnable = true;
                    ++nFallback;
                }
            }
            // 主程序 + 图标：只给留下来的条目算，别为被过滤掉的项白扫目录
            e.mainExe = guessMainExe(e, &e.mainExeSure);
            if (!e.mainExe.isEmpty())
                e.icon = icons.icon(QFileInfo(e.mainExe));
            else if (QFileInfo(e.installLocation).isDir())
                e.icon = icons.icon(QFileIconProvider::Folder);
            m_entries.append(e);
        }
        RegCloseKey(base);
    }
    std::sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
        if (a.installDate != b.installDate)
            return a.installDate > b.installDate;          // 新装的排前面
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    applyFilter();
    Utils::log(QString("uninstall reload: entries=%1 scan=%2ms foundUninstaller=%3").arg(m_entries.size()).arg(all.elapsed()).arg(nFallback));
}

// 过滤只重算"哪些条目算进这一页"，建行控件交给 rebuildList 分批做。
void UninstallPage::applyFilter()
{
    const QString want = m_search->text().trimmed();
    m_shown.clear();
    m_shown.reserve(m_entries.size());
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry& e = m_entries[i];
        if (!want.isEmpty() && !e.name.contains(want, Qt::CaseInsensitive)
            && !e.publisher.contains(want, Qt::CaseInsensitive))
            continue;
        m_shown.append(i);
    }
    const int pages = qMax(1, (int(m_shown.size()) + kPageSize - 1) / kPageSize);
    m_page = qBound(0, m_page, pages - 1);
    rebuildList();
}

// 一页只有 20 行，一次建完（实测 make≈45-80ms）比切片更利落：切片每跳一次事件循环
// 要多花 ~8ms 的布局/绘制，还会看到行一条条冒出来，反而像卡。
void UninstallPage::rebuildList()
{
    QElapsedTimer prep;
    prep.start();
    m_list->clear();
    m_msClear = prep.elapsed();
    m_fwBtns.clear();
    const int from = qMin(m_page * kPageSize, m_shown.size());
    const int to = qMin(from + kPageSize, m_shown.size());
    prep.restart();
    if (m_fwStale) {          // 翻页不必重读防火墙全表：只有重新扫描 / 写过规则之后才要
        refreshFwMap();
        m_fwStale = false;
    }
    m_msFw = prep.elapsed();
    updatePager();
    m_pageClock.restart();
    m_msMake = m_msAttach = 0;
    for (int i = from; i < to; ++i) {
        QElapsedTimer lap;
        lap.start();
        auto* it = new QListWidgetItem();
        it->setSizeHint(QSize(0, 54));
        auto* row = makeRow(m_shown[i]);
        m_msMake += lap.restart();
        m_list->addItem(it);
        m_list->setItemWidget(it, row);
        m_msAttach += lap.elapsed();
    }
    updateFwButtons();
    Utils::log(QStringLiteral("uninstall page %1: rows=%2 clear=%3 fw=%4 make=%5 attach=%6 总=%7ms")
                   .arg(m_page + 1).arg(to - from)
                   .arg(m_msClear).arg(m_msFw).arg(m_msMake).arg(m_msAttach)
                   .arg(m_pageClock.elapsed()));
}

void UninstallPage::showPage(int page)
{
    const int pages = qMax(1, (int(m_shown.size()) + kPageSize - 1) / kPageSize);
    m_page = qBound(0, page, pages - 1);
    rebuildList();
    m_list->scrollToTop();
}

void UninstallPage::updatePager()
{
    const int total = m_shown.size();
    const int pages = qMax(1, (total + kPageSize - 1) / kPageSize);
    const int from = total == 0 ? 0 : m_page * kPageSize + 1;
    const int to = qMin(total, (m_page + 1) * kPageSize);
    m_pageInfo->setText(QStringLiteral("第 %1 / %2 页 · 本显示 %3-%4")
                            .arg(m_page + 1).arg(pages).arg(from).arg(to));
    m_prev->setEnabled(m_page > 0);
    m_next->setEnabled(m_page + 1 < pages);
    m_status->setText(m_entries.isEmpty()
                          ? QStringLiteral("没读到任何卸载记录（注册表 Uninstall 三处都是空的？）")
                          : QStringLiteral("共 %1 个程序 · 筛选后 %2 个 · 卸载前会先把要执行的命令给你看")
                                .arg(m_entries.size()).arg(total));
}

// 一行：图标 + 名称/发布者 + 日期 · 占用 · 位数 +「卸载 / 防火墙 / 位置 / 更多」
QWidget* UninstallPage::makeRow(int entryIdx)
{
    Entry& e = m_entries[entryIdx];          // 非 const：图标位图解出来要缓存回条目
    auto* row = new QWidget(m_list);

    QStringList tip;
    tip << QStringLiteral("安装位置：%1").arg(
             e.installLocation.isEmpty() ? QStringLiteral("注册表没写") : e.installLocation);
    tip << QStringLiteral("注册表：%1").arg(e.registryPath);
    tip << QStringLiteral("卸载命令：%1").arg(
             e.uninstallString.isEmpty() ? QStringLiteral("无") : e.uninstallString);
    tip << QStringLiteral("静默命令：%1").arg(
             e.quietString.isEmpty() ? QStringLiteral("无") : e.quietString);
    if (!e.fallbackUninstaller.isEmpty())
        tip << QStringLiteral("注册表那条用不了，这一项用的是找到的卸载器：%1").arg(e.fallbackUninstaller);
    row->setToolTip(tip.join(QLatin1Char(10)));

    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(6, 4, 6, 4);
    h->setSpacing(kRowGap);

    QStringList sub;
    if (!e.publisher.isEmpty())
        sub << e.publisher;
    if (!e.version.isEmpty())
        sub << QStringLiteral("版本 %1").arg(e.version);
    QString badge;
    InfoRow::Badge kind = InfoRow::Neutral;
    if (!e.runnable) {
        badge = QStringLiteral("卸载程序已不存在");
        kind = InfoRow::Bad;
    } else if (!e.fallbackUninstaller.isEmpty()) {
        badge = QStringLiteral("用的是找到的卸载器");
        kind = InfoRow::Warn;
    }
    auto* info = new InfoRow(e.name, sub.join(QStringLiteral(" · ")), badge, kind, QIcon(), 0);
    // shell 图标现取一次要 3-9ms，而分页会反复建行 —— 解出来的位图存回条目里
    if (e.iconPix.isNull() && !e.icon.isNull())
        e.iconPix = e.icon.pixmap(20, 20);
    info->setIconPixmap(e.iconPix);
    h->addWidget(info, 1);

    const auto col = [row](const QString& text, int w, Qt::Alignment al) {
        auto* l = new QLabel(text, row);
        l->setObjectName(QStringLiteral("muted"));
        l->setFixedWidth(w);
        l->setAlignment(al | Qt::AlignVCenter);
        return l;
    };
    h->addWidget(col(e.installDate.isEmpty() ? QStringLiteral("—") : e.installDate,
                     82, Qt::AlignLeft));
    h->addWidget(col(e.sizeKB > 0 ? humanSize(e.sizeKB) : QStringLiteral("—"),
                     66, Qt::AlignRight));
    h->addWidget(col(e.bitness, 56, Qt::AlignLeft));

    const auto btn = [row](const QString& text, int w) {
        auto* b = new QPushButton(text, row);
        b->setObjectName(QStringLiteral("rowBtn"));
        b->setFixedWidth(w);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);   // 点按钮别把焦点从这一页上抢走
        return b;
    };

    auto* un = btn(QStringLiteral("卸载"), kBtnW);
    un->setEnabled(e.runnable);
    un->setToolTip(e.runnable
                       ? QStringLiteral("调用这个软件自己的卸载程序，执行前会先把命令行给你看")
                       : QStringLiteral("注册表里的卸载命令指向的文件已经没了，安装目录里也没找到卸载器"));
    connect(un, &QPushButton::clicked, this, [this, entryIdx] { doUninstall(entryIdx, false); });

    auto* fw = btn(QStringLiteral("防火墙"), kFwW);
    fw->setToolTip(QStringLiteral("先认这个软件的主程序 exe；再按 Windows 防火墙里现有的规则判断"));
    connect(fw, &QPushButton::clicked, this, [this, entryIdx] {
        const Firewall::Match m = m_fwByExe.value(Firewall::exeKey(m_entries[entryIdx].mainExe));
        runFirewall(entryIdx, !m.allowIn);
    });
    m_fwBtns.insert(entryIdx, fw);

    auto* dir = btn(QStringLiteral("位置"), kBtnW);
    dir->setEnabled(!e.installLocation.isEmpty());
    connect(dir, &QPushButton::clicked, this, [this, entryIdx] { openLocation(entryIdx); });

    auto* more = btn(QStringLiteral("更多 ▾"), kMoreW);
    // 菜单按点击现建：每行挂一个 QMenu + 6 个 QAction 的话，光是样式 polish
    // 就要一两毫秒，一页 20 行白付几十毫秒，而多数行根本不会被点开
    connect(more, &QPushButton::clicked, this, [this, entryIdx, more] {
        const Entry& it = m_entries[entryIdx];
        QMenu menu(more);
        auto* miQuiet = menu.addAction(QStringLiteral("静默卸载"), more,
                                       [this, entryIdx] { doUninstall(entryIdx, true); });
        miQuiet->setEnabled(it.runnable && (!it.productCode.isEmpty() || !it.quietString.isEmpty()
                                            || !silentFlagFor(it.fallbackUninstaller).isEmpty()));
        miQuiet->setToolTip(QStringLiteral(
            "只有软件自己写了 QuietUninstallString（或 MSI 能自动 /qb、"
            "或我们认得出这个卸载器的静默参数）才能点"));
        menu.addAction(QStringLiteral("加入防火墙允许列表"), more,
                       [this, entryIdx] { runFirewall(entryIdx, true); });
        menu.addAction(QStringLiteral("移除 ltool 加的防火墙规则"), more,
                       [this, entryIdx] { runFirewall(entryIdx, false); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("复制注册表路径"), more, [this, entryIdx] {
            QApplication::clipboard()->setText(m_entries[entryIdx].registryPath);
            m_status->setText(QStringLiteral("已复制注册表路径"));
        });
        menu.addAction(QStringLiteral("复制卸载命令"), more, [this, entryIdx] {
            const Entry& e2 = m_entries[entryIdx];
            const QString cmd = !e2.uninstallString.isEmpty()       ? e2.uninstallString
                                : !e2.quietString.isEmpty()         ? e2.quietString
                                : !e2.fallbackUninstaller.isEmpty() ? e2.fallbackUninstaller
                                                                    : QStringLiteral("msiexec.exe /x %1").arg(e2.productCode);
            QApplication::clipboard()->setText(cmd);
            m_status->setText(QStringLiteral("已复制：%1").arg(cmd));
        });
        menu.exec(more->mapToGlobal(QPoint(0, more->height())));
    });
    h->addWidget(un);
    h->addWidget(fw);
    h->addWidget(dir);
    h->addWidget(more);
    return row;
}

void UninstallPage::doUninstall(int entryIdx, bool quiet)
{
    const Entry& e = m_entries[entryIdx];
    QString exe, params, src;
    if (!e.productCode.isEmpty()) {
        exe = QStringLiteral("msiexec.exe");
        params = QStringLiteral("/x %1").arg(e.productCode) + (quiet ? QStringLiteral(" /qb") : QString());
        src = QStringLiteral("MSI 产品码 %1").arg(e.productCode);
    } else {
        const QString cmd = quiet && !e.quietString.isEmpty() ? e.quietString : e.uninstallString;
        QString regExe, regParams;
        bool regOk = false;
        if (!cmd.isEmpty()) {
            splitCommand(cmd, &regExe, &regParams);
            regOk = !Utils::fileTargetMissing(regExe);
        }
        if (regOk) {
            exe = regExe;
            params = regParams;
            src = quiet && !e.quietString.isEmpty()
                      ? QStringLiteral("注册表的 QuietUninstallString")
                      : QStringLiteral("注册表的 UninstallString");
            if (quiet && e.quietString.isEmpty())
                m_status->setText(QStringLiteral("这个软件没写 QuietUninstallString，按普通方式调用它的卸载程序。"));
        } else if (!e.fallbackUninstaller.isEmpty()) {
            exe = e.fallbackUninstaller;
            params = quiet ? silentFlagFor(exe) : QString();
            src = QStringLiteral("注册表那条命令用不了（%1），这一项用的是安装目录里找到的卸载器")
                      .arg(cmd.isEmpty() ? QStringLiteral("根本没写") : cmd);
        } else {
            m_status->setText(QStringLiteral("这一项没有可用的卸载命令。"));
            return;
        }
    }

    const QString line = exe + (params.isEmpty() ? QString() : QLatin1Char(' ') + params);
    if (QMessageBox::question(this, QStringLiteral("确认卸载"),
            QString("将执行：\n\n%1\n\n命令来源：%2\n\n卸载程序自己的界面会弹出来；需要管理员权限时会问 UAC。\n"
                    "ltool 收不到卸载成功与否的回执，卸完请点「重新扫描」核对。").arg(line, src))
        != QMessageBox::Yes)
        return;
    // msiexec 只给了文件名，靠系统目录解析；其它情况 exe 已是绝对路径
    const bool ok = Platform::launch(exe, params, QString(), true);
    m_status->setText(ok ? QStringLiteral("已交给系统执行：%1 · 卸完请点「重新扫描」核对")
                           .arg(line)
                        : QStringLiteral("启动失败：%1").arg(line));
}

void UninstallPage::openLocation(int entryIdx)
{
    const Entry& e = m_entries[entryIdx];
    if (e.installLocation.isEmpty()) {
        m_status->setText(QStringLiteral("注册表没写这个软件的安装位置。"));
        return;
    }
    if (!QFileInfo::exists(e.installLocation)) {
        m_status->setText(QStringLiteral("安装位置已经不存在：%1").arg(e.installLocation));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(e.installLocation));
}

// 整页共用一次全表遍历：按行各查一遍 rulesFor 就是 20 次全表（实测全表 7ms/次）
void UninstallPage::refreshFwMap()
{
    m_fwByExe = Firewall::rulesByExe();
}

void UninstallPage::updateFwButtons()
{
    for (auto it = m_fwBtns.constBegin(); it != m_fwBtns.constEnd(); ++it) {
        QPushButton* b = it.value();
        if (!b || it.key() >= m_entries.size())
            continue;
        const Entry& e = m_entries[it.key()];
        if (e.mainExe.isEmpty()) {
            b->setText(QStringLiteral("防火墙"));
            b->setEnabled(false);
            b->setToolTip(QStringLiteral("认不出这个软件的主程序 exe（注册表里既没写安装位置也没写图标路径），"
                                      "不敢给来路不明的文件开放行规则"));
            continue;
        }
        const Firewall::Match m = m_fwByExe.value(Firewall::exeKey(e.mainExe));
        b->setText(m.allowIn ? QStringLiteral("已允许") : QStringLiteral("未允许"));
        b->setEnabled(!m_fwBusy);
        QString tip = m.allowIn
                          ? QStringLiteral("这个 exe 已经有入站允许规则了：再点一次会删掉 ltool 加的那条")
                          : QStringLiteral("给它开一条入站允许规则（写规则要管理员，可能弹 UAC）");
        if (!m.names.isEmpty())
            tip += QStringLiteral("\n这个 exe 现有的规则：%1").arg(m.names.join(QStringLiteral("、")));
        if (!m.ours.isEmpty())
            tip += QStringLiteral("\nltool 加的：%1").arg(m.ours.join(QStringLiteral("、")));
        if (!e.mainExeSure)
            tip += QStringLiteral("\n注意：主程序是从卸载器所在目录猜的，可能猜错");
        b->setToolTip(tip);
    }
}

void UninstallPage::runFirewall(int entryIdx, bool add)
{
    if (m_fwBusy || entryIdx >= m_entries.size())
        return;
    const Entry& e = m_entries[entryIdx];
    if (e.mainExe.isEmpty()) {
        m_status->setText(QStringLiteral("认不出这个软件的主程序 exe，不敢开规则。"));
        return;
    }
    const Firewall::Match cur = m_fwByExe.value(Firewall::exeKey(e.mainExe));
    if (!add && cur.ours.isEmpty()) {
        m_status->setText(QStringLiteral("这个程序名下没有 ltool 加过的规则，没有可删的。"));
        return;
    }
    const QString exe = e.mainExe;
    const QString display = e.name;
    const QString name = add ? Firewall::ruleNameFor(display) : cur.ours.first();
    const QString ask = add
        ? QStringLiteral("将为这个程序添加一条「入站允许」规则：\n\n规则名：%1\n程序：%2\n\n"
                         "写防火墙规则需要管理员权限：ltool 不是以管理员运行的话会弹一次 UAC，"
                         "点「是」才会写入。\n确定吗？").arg(name, exe)
        : QStringLiteral("将删除这一条规则：\n\n%1\n\n只删 ltool 自己加的这条，软件自己写的规则不动。\n确定吗？").arg(name);
    if (QMessageBox::question(this, add ? QStringLiteral("加入防火墙允许列表")
                                        : QStringLiteral("移除防火墙规则"), ask)
        != QMessageBox::Yes)
        return;

    m_fwBusy = true;
    updateFwButtons();
    m_status->setText(QStringLiteral("正在写入规则（可能在等你回答 UAC）…"));
    if (!m_fwWatcher) {
        m_fwWatcher = new QFutureWatcher<QString>(this);
        connect(m_fwWatcher, &QFutureWatcherBase::finished, this, [this] {
            m_fwBusy = false;
            refreshFwMap();
            m_fwStale = false;
            updateFwButtons();
            m_status->setText(m_fwWatcher->result());
        });
    }
    // 放工作线程：COM 写规则、可能的 UAC 提权、回读确认都可能阻塞几百毫秒到几秒
    m_fwWatcher->setFuture(QtConcurrent::run([exe, name, display, add] {
        return add ? Firewall::allowExe(exe, display) : Firewall::removeRuleNamed(name);
    }));
}
