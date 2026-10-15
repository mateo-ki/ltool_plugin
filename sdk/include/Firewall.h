#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

// Windows 防火墙：给某个程序开"入站允许"规则。
//
// 权限实测：读规则列表不需要管理员（非提权能读到全部 400+ 条），但写规则要
// （非提权写会吃 0x80070005）。所以先直接试 COM，被拒了就用一次 UAC 把 ltool
// 自己提权重跑、由子进程写；netsh 那条路已删（实测它报"成功"但规则根本不落地）。
// 写完一律回读规则确认，不看命令的退出码。
namespace Firewall {

struct Match {
    int total = 0;           // 一共扫到多少条规则（0 = 根本没读到，不是"没有规则"）
    bool any = false;          // 有这个 exe 的规则（任何动作）
    bool allowIn = false;      // 其中有"入站 + 允许"的，也就是我们要的那个状态
    QStringList names;         // 这个 exe 的所有规则名
    QStringList ours;          // 其中是 ltool 加的（按规则说明认，改名也不会认错）
};

// 归一化 exe 路径（展开环境变量、去引号、反斜杠、转小写），当 map 的键用
QString exeKey(const QString& path);

// 读：这个 exe 现在有哪些规则（不需要管理员）
Match rulesFor(const QString& exePath);

// 读：一次遍历全表，按 exe 归组。整页几十行都要查状态时用这个，
// 别按行各调一次 rulesFor（那是几十次全表遍历）
QHash<QString, Match> rulesByExe();

// 规则名就用软件自己的名字（去掉安装包塞进 DisplayName 的"版本 x.y.z"）；
// "是谁加的"写在规则说明里，不占名字
QString ruleNameFor(const QString& displayName);

// 写：添加入站允许规则。可以放到工作线程里调用（内部自己初始化 COM）。
// 返回一句给人看的结论（成功 / 失败原因 / 取消了 UAC）。
QString allowExe(const QString& exePath, const QString& displayName);

// 只做一次 COM 写入，不自己提权：给"被 UAC 拉起来的子进程"用，也被上面两个函数复用
bool addRuleNow(const QString& exePath, const QString& ruleName);
bool removeRuleNow(const QString& ruleName);

// 写：按规则名删掉。同样可能弹 UAC。
QString removeRuleNamed(const QString& name);

} // namespace Firewall
