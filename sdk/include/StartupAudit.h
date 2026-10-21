#pragma once

#include <QString>
#include <QVector>

// 开机启动分析：把"开机 / 登录时会被拉起来的东西"一次列全，来源包括
//   注册表 Run / RunOnce / Policies\Explorer\Run（含 32 位视图）
//   启动文件夹（用户 + 全体用户，.lnk 会解析到真实目标）
//   自动启动的服务（StartType=自动，含"自动(延迟启动)"）
//   计划任务里触发器是"启动时 / 登录时"的那些（含隐藏任务）
//   AppInit_DLLs、Browser Helper Objects 这类挂在进程里的
// 每一项都标出：注册在哪、命令指向的文件还在不在、有没有数字签名、签名者是谁、
// 在任务管理器里是不是被禁用。扫描只读；**动手只有"应用启动项开/关"这一件**（见 setStartupEnabled）。
// 「任务管理器 → 启动」那个开关只有两个状态；放在全局是因为它和 StartupItem 一样是数据。
enum class Approved { On, Off };

struct StartupItem {
    enum class Source { RegistryRun, StartupFolder, Service, Task, ShellExtension };

    Source source = Source::RegistryRun;
    QString name;
    QString command;        // 注册表原值 / 命令行 / 服务 ImagePath / 任务 Command
    QString targetPath;     // 从命令里取出的 exe
    QString location;       // 人在资源管理器/注册表编辑器里能找到的位置
    QString note;           // RunOnce / 自动(延迟启动) / 触发器=登录 等
    bool enabled = true;    // StartupApproved / 服务启动类型 / 任务是否启用
    bool targetMissing = false;
    bool checkedSig = false;
    bool hasSignature = false;
    bool signatureValid = false;
    QString publisher;
    // 「任务管理器 → 启动」把开关状态存在另一个键里（StartupApproved\Run 或 \StartupFolder），
    // 值名就是这条的名字 / 那个 .lnk 的文件名。扫描时顺手记下整条路径，
    // 这样"改它"不用再从 location 反推一遍（反推就是第二份规则，早晚和这里说法不一）。
    // **这两颗排在最后**：老插件按老偏移读的 name/command/enabled 那些一个字都不受影响，
    // 只有新加的这两颗读不到（空 = 这一条不能由这页开关，正好是它该有的说法）。
    QString approvedPath;   // 形如 HKCU\Software\...\Explorer\StartupApproved\Run，空 = 这条不能由这页开关
    QString approvedValue;  // 上面那个键里的值名
};

namespace StartupAudit {

// 全量扫描（含签名读取，几百个文件，秒级：请放后台线程调用）
QVector<StartupItem> scan();

QString sourceLabel(StartupItem::Source s);

// ---- 这一页动手的那三件事，全做成能单独问的函数 ----
// 「任务管理器 → 启动」那个开关的真实存储：`StartupApproved\<Run|StartupFolder>` 下的一个
// REG_BINARY，**首字节 0x03 = 已被关掉，0x02 = 开着；压根没有这个值 = 也从没被碰过 = 开着**。
//（这里以前是反的：注释写着"0x03=启用"，代码就照着 `enabled = (blob[0] == 0x03)` 写。
//  本机拿真注册表对出来：`ltool` 和 `org.yuanli.utools` 是 `02 00…`（它们确实在自启动），
//  `Steam` / `NeatDM` / `CiscoMeetingDaemon` / `MicrosoftEdgeAutoLaunch…` 是 `03 + 一个 FILETIME`
//  —— 那四条才是被关掉的形状。判据现在钉的就是这两组真字节。）
Approved approvedState(const QByteArray& blob);
// 写回去的那份：长度照原来那份（UWP 那种是 24 字节），只改首字节；原来没有就给 12 字节。
QByteArray approvedBlobFor(Approved to, const QByteArray& existing);

// 系统服务：ImagePath 落在 %SystemRoot% 下面就算。这一页默认不列它们（他 2026-10-21 点名），
// 纯函数是为了判据能拿几条真路径问，不用真去枚举服务。
bool isSystemService(const QString& imagePath, const QString& systemRoot);

// 把某一条**应用**启动项改成开/关：只认 RegistryRun 与 StartupFolder 两种来源，
// 走的就是任务管理器那个存储（不动 Run 值本身，所以随时能反回来）。
// 返回空串 = 改了并且读回来核对过；不空 = 给人看的那句原因（要管理员 / 这条不归这页管 / 核对不上）。
QString setStartupEnabled(const StartupItem& it, bool on);

} // namespace StartupAudit
