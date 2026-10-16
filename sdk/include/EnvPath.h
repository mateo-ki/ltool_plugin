#pragma once

#include <QString>
#include <QStringList>

namespace EnvPath {

// 用户 PATH 的原始条目（未展开 %VAR%，去除空项）
QStringList userPathEntries();

// 判断目录是否已在 PATH 中（按展开后比较，忽略大小写）
bool userPathContains(const QString& dir);

// 追加到用户 PATH（HKCU\Environment\Path），成功后广播 WM_SETTINGCHANGE
bool addToUserPath(const QString& dir, QString* err = nullptr);

// 从用户 PATH 移除
bool removeFromUserPath(const QString& dir, QString* err = nullptr);

// 设置/删除用户环境变量（HKCU\Environment），并广播变化
bool setUserEnv(const QString& name, const QString& value, QString* err = nullptr);
bool removeUserEnv(const QString& name, QString* err = nullptr);

// 广播环境变量变化，让资源管理器/新进程感知
void broadcastChange();

// 注册表里的 PATH（机器 + 用户，已展开 %VAR%、转成原生分隔符）。
// 本进程启动时那份 env 是旧的：刚写进 PATH 的目录要用这个函数才看得到。
QString registryPath();

// 把注册表里的 PATH（机器+用户）重新读进本进程：安装器/winget 自己改的 PATH 也能立刻
// 被 ltool 后续 spawn 的子进程看到，不必重开 ltool
void syncProcessPath();

} // namespace EnvPath
