#include "RepairPage.h"
#include "platform/Launch.h"
#include "DevEnv.h"
#include "SilentInstallLog.h"
#include "Utils.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <windows.h>

namespace {

struct WingetPkg {
    const char* id;
    const char* arch;  // "x64" / "x86" / "" = 不分架构
    const char* file;  // 该架构安装后必然存在的系统文件（检测兜底），空 = 只查注册表
};

struct RepairRow {
    const char* name;
    QString match;              // 卸载表显示名包含片段
    QVector<WingetPkg> pkgs;    // 可安装的包
    const char* url;            // 无 winget 时打开
    bool builtin;               // Win10+ 系统内置
};

const RepairRow kRows[] = {
    {".NET Desktop Runtime", "windows desktop runtime",
     {{"Microsoft.DotNet.DesktopRuntime.8", "", ""}}, "https://dotnet.microsoft.com/download/dotnet", false},
    {"VC++ 2005 Redistributable", "visual c++ 2005",
     {{"Microsoft.VCRedist.2005.x64", "x64", "msvcr80.dll"},
      {"Microsoft.VCRedist.2005.x86", "x86", "msvcr80.dll"}}, "", false},
    {"VC++ 2008 Redistributable", "visual c++ 2008",
     {{"Microsoft.VCRedist.2008.x64", "x64", "msvcr90.dll"},
      {"Microsoft.VCRedist.2008.x86", "x86", "msvcr90.dll"}}, "", false},
    {"VC++ 2010 Redistributable", "visual c++ 2010",
     {{"Microsoft.VCRedist.2010.x64", "x64", "msvcr100.dll"},
      {"Microsoft.VCRedist.2010.x86", "x86", "msvcr100.dll"}}, "", false},
    {"VC++ 2012 Redistributable", "visual c++ 2012",
     {{"Microsoft.VCRedist.2012.x64", "x64", "msvcr110.dll"},
      {"Microsoft.VCRedist.2012.x86", "x86", "msvcr110.dll"}}, "", false},
    {"VC++ 2013 Redistributable", "visual c++ 2013",
     {{"Microsoft.VCRedist.2013.x64", "x64", "msvcr120.dll"},
      {"Microsoft.VCRedist.2013.x86", "x86", "msvcr120.dll"}}, "", false},
    {"VC++ 2015-2022 Redistributable", "visual c++ redistributable",
     {{"Microsoft.VCRedist.2015+.x64", "x64", "vcruntime140.dll"},
      {"Microsoft.VCRedist.2015+.x86", "x86", "vcruntime140.dll"}}, "", false},
    {"Visual Studio 2010 Tools For Office Runtime", "visual studio 2010 tools for office runtime",
     {}, "https://www.microsoft.com/zh-cn/download/details.aspx?id=48208", false},
    {"DirectX End-User Runtime", "directx runtime",
     {{"Microsoft.DirectX", "", "d3dx9_43.dll"}}, "", false},
    {"Universal CRT", "universal c runtime",
     {}, "", true},
};

struct InstalledInfo {
    QString name;
    QString version;
    bool wow = false; // 来自 32 位视图（WOW6432Node）
};

QVector<InstalledInfo> installedDisplayNames()
{
    QVector<InstalledInfo> names;
    struct Root { HKEY hive; const wchar_t* sub; bool wow; };
    const Root roots[] = {
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", false},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall", true},
        {HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", false},
    };
    for (const Root& root : roots) {
        HKEY k = nullptr;
        if (RegOpenKeyExW(root.hive, root.sub, 0, KEY_READ, &k) != ERROR_SUCCESS)
            continue;
        wchar_t name[256];
        DWORD nameLen = 256;
        DWORD idx = 0;
        while (RegEnumKeyExW(k, idx++, name, &nameLen, nullptr, nullptr, nullptr, nullptr)
               == ERROR_SUCCESS) {
            nameLen = 256;
            HKEY sk = nullptr;
            if (RegOpenKeyExW(k, name, 0, KEY_READ, &sk) == ERROR_SUCCESS) {
                DWORD type = 0, size = 0;
                if (RegQueryValueExW(sk, L"DisplayName", nullptr, &type, nullptr, &size)
                    == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
                    QByteArray buf(int(size), 0);
                    if (RegQueryValueExW(sk, L"DisplayName", nullptr, &type,
                                         (BYTE*)buf.data(), &size) == ERROR_SUCCESS) {
                        QString dn = QString::fromWCharArray(
                            (const wchar_t*)buf.constData(), size / 2);
                        while (dn.endsWith(QChar(0)))
                            dn.chop(1);
                        QString ver;
                        DWORD vs = 0;
                        if (RegQueryValueExW(sk, L"DisplayVersion", nullptr, &type,
                                             nullptr, &vs) == ERROR_SUCCESS) {
                            QByteArray vb(int(vs), 0);
                            if (RegQueryValueExW(sk, L"DisplayVersion", nullptr, &type,
                                                 (BYTE*)vb.data(), &vs) == ERROR_SUCCESS) {
                                ver = QString::fromWCharArray(
                                    (const wchar_t*)vb.constData(), vs / 2);
                                while (ver.endsWith(QChar(0)))
                                    ver.chop(1);
                            }
                        }
                        if (!dn.trimmed().isEmpty())
                            names.append({dn.trimmed().toLower(), ver.trimmed(), root.wow});
                    }
                }
                RegCloseKey(sk);
            }
        }
        RegCloseKey(k);
    }
    return names;
}

bool fileExistsIn(const QStringList& dirs, const QString& fileName)
{
    for (const QString& d : dirs)
        if (QFileInfo::exists(d + "/" + fileName))
            return true;
    return false;
}

// 这一行里还没装上的 winget 包：注册表条目按架构匹配，匹配不到再看关键系统文件
QStringList missingPkgs(const RepairRow& r, const QVector<InstalledInfo>& names)
{
    QStringList miss;
    for (const WingetPkg& pkg : r.pkgs) {
        const bool isX64 = QString(pkg.arch) == QLatin1String("x64");
        bool have = false;
        for (const InstalledInfo& n : names) {
            if (!n.name.contains(r.match, Qt::CaseInsensitive))
                continue;
            const bool archOk = pkg.arch[0] == 0
                || (isX64 ? (!n.wow || n.name.contains("x64"))
                          : (n.wow || n.name.contains("x86")));
            if (archOk) {
                have = true;
                break;
            }
        }
        if (!have && pkg.file[0] != 0) {
            have = fileExistsIn({isX64 ? "C:/Windows/System32" : "C:/Windows/SysWOW64"},
                                QString::fromLatin1(pkg.file));
        }
        if (!have)
            miss.append(QString::fromLatin1(pkg.id));
    }
    return miss;
}

} // namespace

RepairPage::RepairPage(QWidget* parent)
    : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 4, 4, 4);

    auto* head = new QHBoxLayout;
    auto* tip = new QLabel("启动软件提示 dll 缺失？检测微软常用运行库（区分 x64/x86），整行点击安装缺失项。");
    tip->setObjectName("tip");
    tip->setWordWrap(true);
    head->addWidget(tip, 1);
    m_allBtn = new QPushButton("一键修复", this);
    m_allBtn->setObjectName("primary");
    m_allBtn->setEnabled(false);
    head->addWidget(m_allBtn);
    auto* rescanBtn = new QPushButton("重新检测", this);
    head->addWidget(rescanBtn);
    lay->addLayout(head);

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
    scroll->setStyleSheet("QScrollArea{background:transparent;border:none;}");
    lay->addWidget(scroll, 1);

    m_status = new QLabel(this);
    m_status->setObjectName("muted");
    lay->addWidget(m_status);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumHeight(150);
    m_log->setPlaceholderText("安装过程日志…");
    lay->addWidget(m_log);

    connect(rescanBtn, &QPushButton::clicked, this, &RepairPage::rescan);
    connect(m_allBtn, &QPushButton::clicked, this, &RepairPage::repairAll);
    rescan();
}

void RepairPage::clearRows()
{
    while (m_rowsLay->count() > 1) {
        QLayoutItem* item = m_rowsLay->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
}

void RepairPage::rescan()
{
    clearRows();
    m_missing.clear();
    const QVector<InstalledInfo> names = installedDisplayNames();

    bool allOk = true;
    for (const RepairRow& r : kRows) {
        const int rowIndex = int(&r - kRows);
        bool installed = false;
        QString note;
        bool needX64 = false, needX86 = false;

        if (r.builtin) {
            installed = true;
            note = "系统内置";
        } else if (!r.pkgs.isEmpty() && r.pkgs.first().arch[0] != 0) {
            // 分架构检测：注册表条目 或 关键系统文件（新版 VC++ 注册名可能不带年份）
            for (const WingetPkg& pkg : r.pkgs) {
                bool have = false;
                QString ver;
                const bool isX64 = QString(pkg.arch) == "x64";
                for (const InstalledInfo& n : names) {
                    if (!n.name.contains(r.match, Qt::CaseInsensitive))
                        continue;
                    const bool archOk = isX64
                        ? (!n.wow || n.name.contains("x64"))
                        : (n.wow || n.name.contains("x86"));
                    if (!archOk)
                        continue;
                    have = true;
                    if (ver.isEmpty() && !n.version.isEmpty())
                        ver = n.version;
                    break;
                }
                if (!have && pkg.file[0] != 0) {
                    have = fileExistsIn({isX64 ? "C:/Windows/System32" : "C:/Windows/SysWOW64"},
                                        pkg.file);
                }
                if (have) {
                    installed = true;
                    if (ver.isEmpty())
                        ver = pkg.file;
                    continue;
                }
                if (isX64) needX64 = true;
                else needX86 = true;
            }
            if (needX64 || needX86) {
                installed = false;
                note.clear();
            }
        } else {
            for (const InstalledInfo& n : names) {
                if (n.name.contains(r.match, Qt::CaseInsensitive)) {
                    installed = true;
                    if (!n.version.isEmpty())
                        note = QString("v%1").arg(n.version);
                    break;
                }
            }
            if (!installed && QString(r.name).startsWith("DirectX"))
                installed = fileExistsIn({"C:/Windows/SysWOW64"}, "d3dx9_43.dll");
        }
        if (!installed) {
            allOk = false;
            m_missing.append(rowIndex);
        }

        // 行：名称 + 状态/版本 + [安装]（仅缺失项）
        auto* row = new QWidget(this);
        row->setAttribute(Qt::WA_StyledBackground);
        row->setObjectName("row");
        row->setCursor(installed ? Qt::ArrowCursor : Qt::PointingHandCursor);
        auto* rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(10, 5, 8, 5);
        rowLay->setSpacing(10);

        auto* nameLabel = new QLabel(r.name, row);
        QFont f = nameLabel->font();
        f.setBold(true);
        nameLabel->setFont(f);
        rowLay->addWidget(nameLabel);
        rowLay->addStretch(1);

        auto* status = new QLabel(row);
        if (installed) {
            status->setText(note.isEmpty() ? "✔ 正常" : "✔ 正常  " + note);
            status->setStyleSheet("color:#2ea058;background:transparent;");
        } else if (needX64 || needX86) {
            QStringList miss;
            if (needX64) miss.append("x64");
            if (needX86) miss.append("x86");
            status->setText(QString("◐ 部分安装 · 缺少 %1").arg(miss.join('+')));
            status->setStyleSheet("color:#d6800f;background:transparent;");
        } else {
            status->setText("✘ 未安装");
            status->setStyleSheet("color:#d6800f;background:transparent;");
        }
        rowLay->addWidget(status);

        if (!installed) {
            auto* installBtn = new QPushButton("安装", row);
            installBtn->setObjectName("primary");
            installBtn->setFixedWidth(72);
            installBtn->setCursor(Qt::PointingHandCursor);
            connect(installBtn, &QPushButton::clicked, this,
                    [this, rowIndex] { installRow(rowIndex); });
            rowLay->addWidget(installBtn);
        }

        row->installEventFilter(this);
        row->setProperty("repairIndex", rowIndex);
        row->setProperty("repairMissing", !installed);

        m_rowsLay->insertWidget(m_rowsLay->count() - 1, row);
    }

    int autoFixable = 0;
    for (int idx : m_missing)
        if (!kRows[idx].pkgs.isEmpty())
            ++autoFixable;
    m_allBtn->setEnabled(autoFixable > 0);
    m_allBtn->setText(autoFixable > 0 ? QString("一键修复（%1 项）").arg(autoFixable)
                                      : QStringLiteral("一键修复"));

    m_status->setText(allOk
        ? "全部正常 ✓"
        : (autoFixable > 0
               ? QString("有 %1 项可自动安装：点「一键修复」全装，或点击单行只修那一项。")
                     .arg(autoFixable)
               : "存在未安装项，但没有可自动安装的包：点击该行会打开官方下载页。"));
}

bool RepairPage::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() == QEvent::MouseButtonPress) {
        auto* row = qobject_cast<QWidget*>(obj);
        if (row && row->property("repairMissing").toBool()) {
            installRow(row->property("repairIndex").toInt());
            return true;
        }
    }
    return QWidget::eventFilter(obj, e);
}

void RepairPage::installRow(int row)
{
    static const int rowCount = int(sizeof(kRows) / sizeof(kRows[0]));
    if (row < 0 || row >= rowCount)
        return;
    const RepairRow& r = kRows[row];

    if (!r.pkgs.isEmpty()) {
        const QStringList miss = missingPkgs(r, installedDisplayNames());
        if (miss.isEmpty()) {
            m_status->setText(QString("%1 已完整安装。").arg(QString::fromLatin1(r.name)));
            return;
        }
        installQueue(QString::fromLatin1(r.name), miss);
        return;
    }
    if (QString(r.url).isEmpty()) {
        QMessageBox::information(this, "ltool", "该组件没有配置自动安装方式。");
        return;
    }
    Platform::launch(r.url);
    QMessageBox::information(this, "ltool", "已打开官方下载页，安装后点击\"重新检测\"。");
}

void RepairPage::repairAll()
{
    const QVector<InstalledInfo> names = installedDisplayNames();
    QStringList ids, what;
    for (int idx : m_missing) {
        const RepairRow& r = kRows[idx];
        const QStringList miss = missingPkgs(r, names);
        if (miss.isEmpty())
            continue;
        ids += miss;
        what << QString("  · %1 → %2").arg(QString::fromLatin1(r.name), miss.join(", "));
    }
    ids.removeDuplicates();
    if (ids.isEmpty()) {
        m_status->setText("没有可以自动安装的缺失项（有些组件只能开官网手动下载）。");
        return;
    }
    if (QMessageBox::question(this, "ltool",
            QString("将用 winget 依次安装 %1 个组件：\n\n%2\n\n"
                    "运行库装到系统默认位置（不套「安装目录」），过程中可能弹管理员授权。")
                .arg(ids.size()).arg(what.join("\n"))) != QMessageBox::Yes)
        return;
    installQueue("一键修复", ids);
}

// 排队串行安装：winget 同时跑多个会互相抢安装锁，所以一个装完再下一个
void RepairPage::installQueue(const QString& title, const QStringList& pkgIds)
{
    if (m_installing) {
        QMessageBox::information(this, "ltool", "上一批安装还在进行，请等它结束。");
        return;
    }
    m_queue = pkgIds;
    m_queueTotal = pkgIds.size();
    m_queueTitle = title;
    m_log->appendPlainText(QString("=== %1：%2 个组件排队 ===").arg(title).arg(m_queueTotal));
    installNext();
}

void RepairPage::installNext()
{
    if (m_queue.isEmpty()) {
        m_installing = false;
        m_status->setText(QString("%1 完成：共 %2 个组件。详情见托盘图标的「安装日志」。")
                              .arg(m_queueTitle).arg(m_queueTotal));
        m_queueTitle.clear();
        m_queueTotal = 0;
        rescan();
        return;
    }
    const QString id = m_queue.takeFirst();
    m_status->setText(QString("%1：%2 剩余 %3 个")
                          .arg(m_queueTitle, id).arg(m_queue.size()));
    QString msg;
    m_installing = true;
    // 安装进程归 qApp 管，回调可能比这一页活得久：设置页关了就必须丢掉这次回调
    const QPointer<RepairPage> guard(this);
    const bool started = DevEnv::startSilentInstall(
        id, QString(), QString(), &msg, [this, guard, id](int code, const QString& out, int jobId) {
            if (!guard)
                return;
            // winget 的失败码是 0x8A15xxxx 的 HRESULT，打十进制没人看得懂
            const QString hex = QString("0x%1").arg(quint32(code), 8, 16, QChar('0')).toUpper();
            if (code == 0 || code == -1978335189 /* 0x8A15002B 已是最新，不算失败 */) {
                m_log->appendPlainText(QString("++ %1 完成").arg(id));
            } else if (SilentInstallLog::wasCancelled(jobId)) {
                m_log->appendPlainText(QString("-- %1 已取消，剩下 %2 个不再继续")
                                           .arg(id).arg(m_queue.size()));
                m_queue.clear();
            } else {
                m_log->appendPlainText(QString("!! %1 失败（%2）").arg(id, hex));
                if (out.contains("1603"))
                    m_log->appendPlainText(
                        "提示：MSI 返回 1603，说明系统里这个运行库的安装记录已损坏"
                        "（常见于 VC++ 2005 这类老组件）。可用微软“程序安装和卸载疑难解答”"
                        "清理残留后再试；若无程序依赖它，忽略即可。");
                else if (out.contains("0x80070005"))
                    m_log->appendPlainText("提示：权限不足，请以管理员身份重试该组件。");
            }
            installNext();
        });
    if (!started) {
        m_installing = false;
        m_log->appendPlainText(QString("!! %1 无法发起：%2").arg(id, msg));
        installNext();
    }
}
