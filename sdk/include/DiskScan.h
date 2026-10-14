#pragma once

#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace DiskScan {

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
};
Timing lastTiming();

// 旧算法（QDirIterator + QFileInfo::size），只给 --disktime 做 A/B 基线用
qint64 legacyWalk(const QString& rootPath);

} // namespace DiskScan
