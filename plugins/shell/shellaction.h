#pragma once

#include <QString>
#include <QStringList>

// 资源管理器右键菜单点进来之后，由这个进程负责弹界面。
// flag 是 ShellMenu 写进注册表的那个开关（--shred / --fileinfo / --whohas …）。
//
// 2026-10-15：这一层搬进了 plugins/shell（动作型插件），主程序只负责把旗标转成动作名
// 交给 PluginHost::invoke —— 它不再链接这里的实现，所以这个头文件也只在插件目录里。
namespace ShellAction {

// **只有这一个数**代表"这条动作我根本没实现" —— 插件的 invoke 拿它判真假。
// 其余的返回值（0/1/2）是窗口自己的结果或"这一步没做成"的说明：压缩包打不开、用户按了取消、
// 解压中途失败……那些**已经由弹窗/托盘/日志跟人说清了**，不能反过来再让主程序盖一句"做不了"。
// 2026-10-15 就是这么错过一次的：把"窗口正常关闭"当成失败，
// 于是一次成功的压缩在屏幕上弹出了「这个右键动作做不了：shell 接了但没干成」。
constexpr int kNotHandled = 3;

// 这张表就是"我实现了哪些动作"的唯一来源 —— 插件自报的 actions() 直接拿它，
// plugin.json 里那份牌由 --plugintest 对（对不上就红，因为主程序的路由最终以这里为准）。
QStringList implemented();

int run(const QString& flag, const QString& path);

} // namespace ShellAction
