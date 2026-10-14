#pragma once

#include <QString>
#include <QVector>

// 注册表分析：扫出"指向已经不存在的东西"的键值，以及 9x 时代留下来的死键。
// 每一项都带风险级别，删除前自动把原内容导出成 .reg 备份（见 backups\）。
//
// 覆盖：
//   卸载残留      Uninstall 项的卸载程序/安装目录已不存在
//   启动项失效    Run / RunOnce 指向的程序已不存在
//   缓存残留      MuiCache 里记录的应用路径已不存在
//   App Paths     按名字启动用的 App Paths 指向的 exe 已不存在
//   文件关联      .扩展名 指向的 ProgID 不存在，或打开命令里的 exe 不存在
//   COM 组件      CLSID\...\InprocServer32 / LocalServer32 指向的文件已不存在
//   服务残留      Services 里的 ImagePath 指向的 exe/dll 已不存在
//   死盘符        MountedDevices / MountPoints2 里还留着已经不存在的盘
//   PATH 失效项   注册表 PATH 里写了但已经不存在的目录（只提示，不整值删除）
//   9x 遗留键     RunServices / SharedDLLs 这类早就不起作用的键
struct RegIssue {
    enum class Kind {
        UninstallOrphan, RunOrphan, MuiCacheOrphan, AppPathsOrphan, AssociationOrphan,
        ComOrphan, ServiceOrphan, DeadDrive, PathEntry, LegacyKey,
    };
    // Safe = 删了没副作用；Careful = 能删但会改变某个行为（关联/COM）；
    // ReadOnly = 只给建议，绝不能自动删（比如整条 PATH 值）
    enum class Risk { Safe, Careful, ReadOnly };

    Kind kind = Kind::UninstallOrphan;
    Risk risk = Risk::Safe;
    QString hiveName;      // "HKEY_LOCAL_MACHINE" / "HKEY_CURRENT_USER"
    QString subKey;        // 完整子键路径
    QString valueName;     // 为空表示删除整个键
    QString title;         // 列表显示标题
    QString detail;        // 原因说明
    QString badPath;       // 那个"已经不存在"的路径
    bool isKey = false;    // true=删除整键 false=删除值

    bool deletable() const { return risk != Risk::ReadOnly; }
};

namespace RegClean {

// 扫描（同步，重活：COM/关联要遍历上万个键，约 1~3 秒，请放到后台线程调用）
QVector<RegIssue> scan();

// 只扫得快的几类（卸载残留/启动项/MuiCache/App Paths/PATH/遗留键），给搜索框这种
// 不能等两秒的地方用；分析页再跑完整 scan()
QVector<RegIssue> quickScan();

// 删除单项：先把要删的内容导出备份到 %APPDATA%/ltool/backups/*.reg，再删除
bool deleteIssue(const RegIssue& it, QString* err = nullptr);

QString categoryLabel(const RegIssue& it);
QString riskLabel(const RegIssue& it);

} // namespace RegClean
