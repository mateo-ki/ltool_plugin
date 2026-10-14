#pragma once

#include <QString>
#include <QVector>

// 文件右键/搜索框里那两件事（解除占用、粉碎）的薄封装：
// 真正的枚举系统句柄、覆写删除都在 FileOps 里，这一层只负责把它们变成
// 界面直接能用的形状（找到的占用者列表、以及"空字符串=成功、非空=给界面看的原因"）。
namespace FileTools {

struct Lock {
    QString name;       // 进程名
    quint64 pid = 0;
};

// 谁正开着这个文件（没有则返回空表）
QVector<Lock> findLocks(const QString& path);

// 结束所有占用者。返回空表示全成功了，否则是没结束掉的那些的原因。
QString unlock(const QString& path);

// 按 pid 结束（广告弹窗页停进程用那条）
QString unlockByPid(quint64 pid);

// 三轮覆写后永久删除（不进回收站）。空字符串=成功。
QString shred(const QString& path);

} // namespace FileTools
