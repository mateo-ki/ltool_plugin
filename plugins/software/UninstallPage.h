#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QVector>
#include <QWidget>

#include "Firewall.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;
template <class T> class QFutureWatcher;

// 工具 - 软件列表页：这台机器装了什么（读注册表 Uninstall），一行一个软件，
// 行右侧直接放「卸载 / 防火墙 / 位置 / 更多」，中间几列是日期·占用·位数。
// 一屏放不下 148 条，所以分页（每页 20 行）而不是长列表滚动。
// 数据源是注册表的 Uninstall 三处（HKLM 64 位 / HKLM Wow6432Node / HKCU），
// SystemComponent=1 的那些（Windows 更新、运行库组件）默认不显示。
// MSI 安装的走 msiexec /x {产品码}，其余走注册表里那条 UninstallString；
// 注册表那条用不了时，会去安装目录（含 bin）找软件自己的卸载器。
class UninstallPage : public QWidget {
    Q_OBJECT
public:
    explicit UninstallPage(QWidget* parent = nullptr);

protected:
    // 第一次真正显示出来才去读注册表：点"软件"菜单时这个页还藏在第二个 tab 后面
    void showEvent(QShowEvent* e) override;

private:
    struct Entry {
        QString name;
        QString version;
        QString publisher;
        QString installDate;         // 已格式化成 yyyy-MM-dd，空 = 没写
        QString installLocation;
        QString displayIcon;         // 多数软件这里写的就是主程序 exe（可能带 ",0"）
        QString uninstallString;
        QString quietString;
        QString registryPath;        // 人在注册表编辑器里能照着找到的位置
        QString productCode;         // MSI 的 {GUID}，非 MSI 为空
        QString fallbackUninstaller; // 注册表命令用不了时，我们在安装目录（含 bin）里找到的卸载器
        qint64 sizeKB = 0;
        bool system = false;
        bool runnable = true;        // 有没有可用的卸载程序
        QString bitness;             // 64 位 / 32 位 / 当前用户
        QIcon icon;                  // 主程序（或安装目录）的图标，reload 时取一次
        QPixmap iconPix;             // 上面那个图标解出来的 20x20 位图：shell 图标每次现取要 3-9ms，
                                     // 分页会反复建行，所以解一次就存下来
        QString mainExe;             // 防火墙要针对的那个 exe，reload 时推出来
        bool mainExeSure = false;    // false = 只从卸载器目录猜的，可能就是卸载器本身
    };

    static constexpr int kPageSize = 20;

    void reload();
    void applyFilter();              // 只重算"哪些条目算在这一页里"，不建行控件
    void rebuildList();              // 建当前页这一屏的行
    void showPage(int page);
    QWidget* makeRow(int entryIdx);
    void updatePager();
    static QString guessMainExe(const Entry& e, bool* sure);

    void doUninstall(int entryIdx, bool quiet);
    void openLocation(int entryIdx);
    void runFirewall(int entryIdx, bool add);
    void refreshFwMap();             // 一次遍历防火墙全表：哪个 exe 已有入站允许规则
    void updateFwButtons();          // 按 m_fwByExe 刷新当前页所有行的防火墙按钮

    QLineEdit* m_search = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_pageInfo = nullptr;
    QPushButton* m_prev = nullptr;
    QPushButton* m_next = nullptr;
    QTimer* m_debounce = nullptr;    // 搜索框防抖：连打时别每键都重排一页
    QElapsedTimer m_pageClock;       // 诊断用：一页建完花了多久
    qint64 m_msClear = 0;
    qint64 m_msFw = 0;
    qint64 m_msMake = 0;
    qint64 m_msAttach = 0;
    int m_page = 0;
    QVector<Entry> m_entries;
    QVector<int> m_shown;            // 过滤后的条目顺序（分页按它切）
    QHash<QString, Firewall::Match> m_fwByExe;   // 归一化 exe 路径 → 规则情况
    bool m_fwStale = true;                       // 翻页不重读全表，扫描过 / 写过规则才刷新
    QHash<int, QPushButton*> m_fwBtns;           // 条目下标 → 该行的防火墙按钮
    QFutureWatcher<QString>* m_fwWatcher = nullptr;
    bool m_showSystem = false;
    bool m_fwBusy = false;
    bool m_loaded = false;           // 首次显示才扫描
};
