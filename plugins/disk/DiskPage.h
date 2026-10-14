#pragma once

#include <QDateTime>
#include <QHash>
#include <QPair>
#include <QVector>
#include <QWidget>

#include <atomic>
#include <memory>
#include <mutex>

#include "DiskScan.h"

class QLabel;
class QFrame;
class QPushButton;
class QStackedWidget;
class QTabBar;
class QTimer;
class QVBoxLayout;
class QCheckBox;
class QComboBox;
class TreemapWidget;

// 配置 - 磁盘清理：顶部汇总卡 + 两个 tab。
//   顶部下拉选盘符（只影响容量汇总和「占用分析」；「可清理位置」是系统和用户目录的
//   缓存/临时文件，天然在系统盘上，换盘符不会变）
//   「可清理位置」勾选 + 一键清理
//   「占用分析」WizTree 式矩形树图 + 排行；整棵目录树一次遍历进内存（DiskScan），
//                之后下钻/返回只查表，不再扫盘
class DiskPage : public QWidget {
    Q_OBJECT
public:
    explicit DiskPage(QWidget* parent = nullptr);

private slots:
    void startAll();          // 「整理」：两个 tab 的扫描一起启动
    void cleanSelected();
    void analysisUp();
    void showNode(int node);  // 下钻（查表，不重扫）
    void tick();              // 轮询后台扫描进度/结果

private:
    struct CleanRow {
        QCheckBox* box = nullptr;
        QFrame* bar = nullptr;
        QLabel* size = nullptr;
        QString path;
        bool admin = false;
    };

    // 后台扫描线程与界面之间只通过它通信（原子量 + 一个 shared_ptr 结果）。
    // 分两段回结果：可清理位置先出（只查那几个目录，秒级），整棵 C 盘树后到。
    struct Task {
        std::atomic<qint64> bytes{0};
        std::atomic<qint64> dirs{0};
        std::mutex m;
        QString current;                 // 正在扫的分支
        // 已经整体扫完的一级分支（DiskScan 报的定稿值）：整盘要 14~19 秒，
        // 但小分支几百毫秒就出得来，界面先摆这些，别让人干等到最后一秒才有数字
        QVector<QPair<QString, qint64>> branches;
        int branchTotal = 0;
        std::atomic<bool> cleanDone{false};
        QVector<qint64> cleanSizes;      // 与 cleanItems() 同序，cleanDone=true 后才可读
        std::atomic<bool> done{false};
        std::shared_ptr<DiskScan::Tree> tree;
    };

    void clearRows();
    void updateSummary();
    void refreshDrives();     // 枚举逻辑磁盘填下拉框
    void renderCleanFromTree();   // 可清理位置的大小直接查目录树，不再单独扫一遍
    void renderClean(const QVector<qint64>& sizes);   // 快速通道的结果（还没整棵树时先填上）
    void renderAnalysis();
    void renderBranches();    // 扫描途中：把已扫完的一级分支先列出来
    void showCached(const QString& root);   // 这个盘扫过就别再扫第二遍

    QLabel* m_cleanTotal = nullptr;
    QLabel* m_usedTitle = nullptr;
    QLabel* m_diskUsed = nullptr;
    QLabel* m_diskFree = nullptr;
    QLabel* m_status = nullptr;
    QComboBox* m_drive = nullptr;
    QString m_root = QStringLiteral("C:/");   // 当前选中的卷根，占用分析扫它
    QVBoxLayout* m_rowsLay = nullptr;
    QVector<CleanRow> m_rows;
    QPushButton* m_analyzeBtn = nullptr;
    QPushButton* m_cleanBtn = nullptr;
    QTabBar* m_tabs = nullptr;
    QStackedWidget* m_stack = nullptr;

    QPushButton* m_upBtn = nullptr;
    QLabel* m_analysisPath = nullptr;
    QLabel* m_analysisStatus = nullptr;
    TreemapWidget* m_map = nullptr;
    QVBoxLayout* m_analysisLay = nullptr;
    QVector<QWidget*> m_analysisRows;

    std::shared_ptr<Task> m_task;
    std::shared_ptr<DiskScan::Tree> m_tree;
    // 扫过的盘留在内存里（这个页是一次 DLL 插件页，创建后就一直活着）：
    // 换回来看第二个盘时不该再等十几秒，「整理」才是要重扫的意思。
    QHash<QString, std::shared_ptr<DiskScan::Tree>> m_byDrive;
    QHash<QString, QDateTime> m_scannedAt;
    QVector<qint64> m_cleanSizes;   // 上一次量到的可清理大小，换盘符后树上查不到时用它兜底
    int m_cleanPhase = 0;    // 0=还没数, 1=快速通道已填, 2=按整棵目录树定稿
    int m_node = -1;
    QTimer* m_timer = nullptr;
};

// 磁盘页两栏列表的右键自测（--rowmenu）：验菜单里那一项的文案、灰没灰、点开要开的是哪个目录。
// 放在 DiskPage.cpp 里是因为行卡片（RowCard）就藏在那个文件的匿名 namespace 里。
int diskRowMenuSelftest();
// --diskui：慢的那条（真扫一整盘），由 disk 插件的 ltoolPluginUiSelftest 导出
int diskUiSelftest();
