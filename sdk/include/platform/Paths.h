#pragma once

#include <QString>

// 平台层第一道缝：这些东西放在哪儿、以及注册表那种带占位符的路径字符串怎么展开。
// 调用方只问「用户的桌面在哪、启动条目放哪儿」，不该知道答案背后是 FOLDERID、XDG 目录
// 还是 /Applications。现在问这一层的有 AppScanner（扫快捷方式）、FileIndexer（桌面进索引）、
// SearchWindow（直达那几条桌面项）、main.cpp --smoke（打印扫描根）。
namespace Platform {

enum class KnownDir {
    UserDesktop,       // 用户桌面
    SharedDesktop,     // 全体用户共用的桌面：Windows 有，Linux/macOS 没这回事 → 空串
    UserApps,          // 用户级启动条目：开始菜单 Programs / ~/.local/share/applications / ~/Applications
    SystemApps,        // 全体用户级：CommonPrograms / /usr/share/applications / /Applications
};

// 空串 = 这个平台上没有这个概念，或系统给不出。拿它当扫描根之前先判空。
// 目录本身不存在时照样返回规范位置（和 Windows 上 KF_FLAG_DONT_VERIFY 同一个分寸：
// 「用户还没建 Applications 文件夹」不等于「这个位置上不该扫」）。
QString knownDir(KnownDir d);

// 展开环境变量占位：Windows 认 %VAR%，POSIX 认 $VAR / ${VAR}。
// 认不出来的名字原样留着 —— 调用方拿展开结果判断「这个启动目标还在不在」，
// 把字符串吃掉比不展开更糟。
QString expandEnv(const QString& s);

// 只给了个文件名（没写路径）时，它在不在系统找得到的位置上。
// Windows 是 SearchPathW（system32 里那些不能算失效），POSIX 是 $PATH 里找。
// 找不到返回空串。
QString findExecutable(const QString& name);

} // namespace Platform
