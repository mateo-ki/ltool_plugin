#pragma once

#include <QString>

// Windows 资源管理器的右键菜单集成：在 HKCU\Software\Classes 下写静态 shell 项，
// 让"文件 / 文件夹"右键里多一个 ltool 子菜单（解压 / 压缩 / 粉碎 / 占用分析 / 谁占用着它）。
//
// 只写 HKCU，不碰 HKLM —— 不需要管理员，也不会影响这台机器上的其他用户。
// 默认关闭：配置页里勾上才会写入，取消勾选会把这些键删干净。
namespace ShellMenu {

// 已经集成过了吗（看 HKCU 下我们的根键在不在）
bool installed();

// 写入 / 撤销。返回一句给人看的结论（失败时会带上 Windows 的错误码）
QString install();
QString remove();

// 已经集成过、但表里新加了项（老机器的右键里不会自己长出来）—— 启动时调一下，
// 发现缺项就整表重写一遍（install 本身是幂等的）
void refreshIfStale();

// 给配置页显示"会往哪儿写"
QString locationHint();

} // namespace ShellMenu
