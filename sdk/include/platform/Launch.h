#pragma once

#include <QString>

class QProcess;

// 平台层第二道缝：打开一个东西、提权打开、在文件管理器里定位到它。
// Windows 三件事全走 ShellExecuteExW —— 关联、DDE、参数切分都交给 shell；POSIX 没有
// shell 这层，只能按「是不是能直接执行的文件」分流：能执行的直接起，其余交给
// xdg-open / open。
namespace Platform {

// async=true：让 shell 异步执行并立即返回。带 DDE 段的 .lnk（如 LX Music）同步调用
// 会卡在 DDE 握手上等回调（实测 729ms），启动器回车必须用这个。
// POSIX 这边 startDetached 本来就不等，参数留着是为了两端签名一致、调用方不用分叉。
bool launch(const QString& path, const QString& params = {}, const QString& workingDir = {},
            bool async = false);

// 提权打开：Windows 用 runas 动词（弹 UAC）；Linux 用 pkexec（Polkit 弹窗）；
// macOS 用 osascript 的 with administrator privileges。用户取消或起不来都是 false，
// 调用方要照这句说实话，别说「已以管理员身份启动」。
bool launchAsAdmin(const QString& path, const QString& params = {}, const QString& workingDir = {});

// 在系统文件管理器里定位到这个文件。macOS 的 open -R 会选中那一项；Linux 的 xdg-open
// 没有「选中」这个语义，只能打开它的父目录 —— 界面上「打开所在位置」那句话在 Linux 要
// 写成「打开所在文件夹」。
void reveal(const QString& path);

// 起这个进程时不要弹控制台窗口。Windows 挂的是 CREATE_NO_WINDOW（schtasks、winget、
// 日志查看这些一律要走它，否则一键修复会连闪好几个黑窗）；POSIX 上没有"控制台窗口"
// 这个东西可藏，所以那边是空动作 —— 不是降级，是这个问题在这里不存在。
// 注：这只管"别开窗"，不管有没有会话（setsid 那类是另一件事，别在这儿假装）。
void runWithoutConsole(QProcess* p);

} // namespace Platform
