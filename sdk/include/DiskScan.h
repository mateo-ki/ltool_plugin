#pragma once

#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace DiskScan {

// 这次扫描走的哪条路，名字只有这一处：界面拿它跟 Tree::engine / Progress::engine 比，
// 别在两边各自抄一遍字符串。
inline constexpr const char* kEngineMft = "MFT 直读";
inline constexpr const char* kEngineWalk = "常规遍历";
inline QString engineMft() { return QString::fromUtf8(kEngineMft); }
inline QString engineWalk() { return QString::fromUtf8(kEngineWalk); }

// 一次遍历整棵目录树后的扁平结果：之后"下钻/返回"只查表，不再扫盘。
struct Tree {
    QVector<QString> name;       // 节点在父目录下的名字（根节点是卷根）
    QVector<qint64> size;        // 子树总字节
    QVector<qint64> fileBytes;   // 本目录直属文件的字节
    QVector<int> parent;
    QVector<int> firstKid;       // kids 数组中的起点
    QVector<int> kidCount;
    QVector<int> kids;           // 孩子下标，按 size 降序
    int root = 0;
    QString rootPath;
    qint64 total = 0;
    int pruned = 0;              // 因为太小而没有列出的子目录数
    // 这棵树的数字是怎么来的：「MFT 直读」= 一次内核枚举 + 一遍 $MFT，
    // 「常规遍历」= 逐目录 FindFirstFileEx。回退时 engineNote 带人话原因。
    // 界面必须把它显示出来：同一句"扫描完成"背后可能是 2 秒和 19 秒，别让人猜。
    QString engine;
    QString engineNote;

    bool isEmpty() const { return name.isEmpty(); }
    QString pathOf(int node) const;
    // 在 node 里找名为 name 的孩子（大小写无关）；没有返回 -1
    int child(int node, const QString& name) const;
    // 按绝对路径定位节点（大小写/分隔符无关）；不在树上返回 -1
    int nodeForPath(const QString& absPath) const;
};

// 扫描进度：字节、已见目录数、正在扫的分支。回调在扫描线程里触发，别碰界面。
//
// doneBranches 是**已经整体扫完的一级分支**（名字 + 子树字节）和总数 ——
// 加它是因为"整盘 14 秒"没法再压（实测 16 线程只比 4 线程快 1%），那界面就不能
// 让人干等到最后一秒才有数字：小分支几秒内就该出现在列表里。
// 注意"完成"的定义是"这个分支再没有任务在飞"，所以这里的数字是定稿值，不是下界。
struct Progress {
    qint64 bytes = 0;
    qint64 dirs = 0;
    QString current;
    QVector<QPair<QString, qint64>> doneBranches;
    int branches = 0;
    // 这次扫描走的是哪条路（"MFT 直读" / "常规遍历"）。界面按它换一句话：
    // MFT 那边报的是"已枚举多少条记录"，跟"已统计多少目录"不是一回事，别混着显示。
    QString engine;
};

// 遍历卷根下面所有目录。minBytes 以下的兄弟目录不列出（根层永远保留）。
Tree run(const QString& rootPath, qint64 minBytes,
         const std::function<void(const Progress&)>& progress = {});

// 一个可清理位置。放在扫描器这边是因为界面和 --disktime 诊断要用同一份清单。
struct CleanTarget {
    const char* name;
    QString path;
    bool admin;    // 清理它需要管理员权限
};
QVector<CleanTarget> cleanTargets();

// 只统计给定这几个目录的子树大小（并行，走同一套 FindFirstFileEx）。
// 给"C盘清理"用：可清理位置先出结果，不用等整棵 C 盘扫完。
QVector<qint64> sizesFor(const QStringList& paths,
                         const std::function<void(const Progress&)>& progress = {});

// 上一次 run() 各段花了多少毫秒。<analysis> 报"磁盘分析太慢"时先跑 `--disktime`
// 看这三个数，别凭直觉改：扫盘 / 合并片段 / 排孩子表，三段是三种完全不同的修法。
struct Timing {
    qint64 files = 0;
    qint64 dirsSeen = 0;
    qint64 walkMs = 0;
    qint64 mergeMs = 0;
    qint64 sortMs = 0;
    int jobs = 0;         // 拆出来的任务数（预算用完就靠单个线程自己扫剩下的）
    int threads = 0;
    qint64 enumMs = 0;    // MFT 那一段：FSCTL_ENUM_USN_DATA 结构枚举
    qint64 sizeMs = 0;    // MFT 那一段：顺序读 $MFT 取大小
    QString engine;       // 这次实际用了哪条路（和 Tree::engine 同一个值）
};
Timing lastTiming();

// 这台机器现在能不能走 MFT 直读（要管理员 + 卷上有 USN 日志）。
// why 填人话给界面用 —— 回退到遍历要慢一个数量级，得说清这次为什么没走快的。
bool mftUsable(const QString& root, QString* why = nullptr);

// 卷的容量。这是文件系统自己报的数，和"扫出来多少"完全无关 —— 判据拿它当**第三条独立量法**：
// MFT 解析错位的话，整盘合计会跟它差出量级（口径差异是有的：它按簇算分配量，我们按逻辑大小）。
struct Usage {
    bool ok = false;
    qint64 total = 0;
    qint64 free = 0;
    qint64 used = 0;
};
Usage usageOf(const QString& root);

// 把一棵树写/读成一个二进制文件。存在的理由只有一个：MFT 直读要管理员权限，
// 而 ltool 平时**不是**提权跑的（一个启动器不该要求管理员）。于是界面起一个提权的自己
// （`ltool.exe --mftree <盘> <文件>`）把整盘读出来写成这个文件，界面再读回来。
// 格式带魔数和版本，读方对不上就当这文件不存在（回退遍历），不猜。
bool writeTree(const Tree& t, const QString& path, QString* err = nullptr);
Tree readTree(const QString& path, QString* err = nullptr);

// 旧算法（QDirIterator + QFileInfo::size），只给 --disktime 做 A/B 基线用
qint64 legacyWalk(const QString& rootPath);

} // namespace DiskScan
