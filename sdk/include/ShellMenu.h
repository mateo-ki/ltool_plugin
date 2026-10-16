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

// 只在"该有"的时候用：已经集成过、但表里新加了项或删了项（老机器的右键不会自己变）——
// 发现对不上就整表重写一遍（install 本身是幂等的）。启动时由 syncWithPlugin() 顺带调用。
void refreshIfStale();

// 给配置页显示"会往哪儿写"
QString locationHint();

// 右键那几项的"该不该在"是两件事的与：**用户想不想要**（配置里那个勾，落盘在 shell/intent）
// 和**提供者在不在了**（「右键动作」那个插件装着且没禁用）。
// 卸载了插件就要从右键里消失 —— 静态注册表项不会自己判断，所以得有人按这个规则写/删。
bool providerPresent();                 // plugins/<shell> 在且没被改名 .off
bool intent();                          // 用户那个勾（不是注册表现状）
bool intentExplicitlySet();             // 配置里这个开关被人为动过没有（键在不在）
void setIntent(bool on);
inline bool menuShouldBe(bool intent, bool provider) { return intent && provider; }

// 缺键时该不该把"装了这把插件"当成"想要右键那几项"认领成显式意图 —— 单独抽出来是因为
// 原来那个缺键默认读的是**注册表现状**，而现状正是这条规则自己会改的东西：
// 插件被卸载 → 菜单删掉 → 缺键默认变成 false → 再装回来永远解不开（他 2026-10-16 报的就是这个）。
// 默认值不能读自己刚写过的东西。规则：**这个开关从没被人为动过，且提供者在了**，就认领成"要"；
// 键存在（人明确勾掉过）就尊重那个 false，绝不替他打开。
inline bool shouldAdoptIntent(bool keyExists, bool provider) { return !keyExists && provider; }

// 按上面那条规则把注册表对齐（该有而没有就写，不该有却还在就删干净）。
// 启动时、以及插件装了/卸了/禁用/启用之后各调一次。回一句给人看的话（没动就回空串）。
// 进来第一件事就是认领意图（shouldAdoptIntent），四个调用点都走这一条，不分叉。
QString syncWithPlugin();

} // namespace ShellMenu
