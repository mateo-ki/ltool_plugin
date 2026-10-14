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
// 在任务管理器里是不是被禁用。只读，不改任何东西。
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
};

namespace StartupAudit {

// 全量扫描（含签名读取，几百个文件，秒级：请放后台线程调用）
QVector<StartupItem> scan();

QString sourceLabel(StartupItem::Source s);

} // namespace StartupAudit
