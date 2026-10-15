#pragma once

#include <QString>
#include <QVector>

// 一个可启动的快捷方式（应用搜索的那份索引里的一条）
struct AppEntry {
    QString name;       // 显示名称（lnk 文件名）
    QString target;     // 解析出的目标程序（可能为空，如 UWP）
    QString args;       // 启动参数
    QString lnkPath;    // .lnk 完整路径（启动入口）
    QString iconFile;   // 缓存的 PNG 图标路径（可能为空）
    QString category;   // desktop / startmenu / system（system 只存在于内存，见 SysTools.h）
};

namespace AppScanner {

// 同步扫描（在工作线程调用）：桌面(含一层子目录) + 公共桌面 + 用户/公共开始菜单，
// 外加配置页「应用目录」里用户自己加的 exe/文件夹（见 apps/roots、apps/depth）
QVector<AppEntry> scan();

// 读写索引缓存 %APPDATA%/ltool/apps.json（JSON 数组，字段名与结构体同名）
QVector<AppEntry> loadIndex();
void saveIndex(const QVector<AppEntry>& list);

} // namespace AppScanner
