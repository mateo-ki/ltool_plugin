#pragma once

#include <QByteArray>
#include <QString>
#include <QIcon>

// 单实例用的本地套接字名字。右键动作（解压/压缩）是一次性进程，干完活把托盘气泡
// 交给常驻实例去举，也走这条通道 —— 见 plugins/shell/shellaction.cpp 的 trayNote()。
inline constexpr const wchar_t kInstanceKey[] = L"ltool.instance";

namespace Utils {

// %APPDATA%/ltool 及其子目录
QString dataDir();
QString iconsDir();
QString pluginsDir();
QString backupsDir();
// 截图（含置顶窗往期）与录屏产物的目录：配置页里可改（config.ini 的 paths/captures、
// paths/recordings），留空 = 软件目录（exe 旁边）下的 captures / recordings
// mediaRoot() 就是那个"软件目录"，配置页要拿它当占位提示，别让文案和实际落点各说各话。
QString mediaRoot();
QString capturesDir();
QString recordingsDir();
void ensureDirs();

// 图标提取 / 文件版本 / 工作集 / 杀进程树这些也一样在 platform 那边
// （FileMeta.h、ProcInfo.h）；这里只留跨平台的那半。

// 「打开一个东西」「这些目录在哪儿」「展开带占位符的路径」不在 Utils 里 —— 那是平台层的事：
// platform/Launch.h、platform/Paths.h。Windows 那边是 ShellExecuteExW + FOLDERID，
// POSIX 没有 shell 这层，只能按能不能执行分流。

// 通用文件/文件夹图标（仅限 GUI 线程调用，内部有缓存）
QIcon cachedFileIcon(const QString& path);

// 从注册表/服务里那种"命令行字符串"取出可执行文件本身。一堆软件把没加引号的空格路径
// 直接写进注册表（C:\Program Files\...），所以不能见空格就切：先整条试、
// 再看是不是以 .exe/.dll 之类结尾、最后逐个空格前缀试哪个真实存在。
QString exeFromCommand(const QString& cmd);

// 判断一个"启动目标"是不是已经不在了：展开 %VAR%、去引号；
// 只写了文件名的交给 SearchPathW（system32 里那些不能算失效）
bool fileTargetMissing(const QString& raw);

// 程序图标（托盘/窗口共用，程序内绘制生成）
QIcon appIcon();

// 圆角字母图标（菜单/占位图标共用）
QIcon letterIcon(const QString& text, const QColor& color);
QIcon bagIcon(const QColor& color);   // 菜单里「插件商店」那一行画的是一颗袋子，不是第一个字

// 托盘上冒一句就走：不弹框、不抢焦点。优先把这句话交给**常驻实例**去举 —— 一次性进程
// 要是为了"让气泡显示满 N 秒"而自己多活 N 秒，用户看到的就是"这件事要 N 秒"（右键解压
// 实测 0.3 秒的活儿被那 5 秒拖成十几倍）。没有实例在跑才自己举。
// 通道就是单实例那个 socket："note\t标题\t正文"。plugins/shell 和 plugins/linuxdo 都走这一条。
void trayNote(const QString& title, const QString& msg, int msec = 5000);

// 深色弹出菜单那一套样式，**只有这一份**（搜索框的托盘菜单、设置窗、置顶窗的右键菜单共用）。
// 为什么要用样式表而不是靠调色板：Qt6 的 windows11 风格画弹出菜单用的是**系统**的底色，
// 调色板里 QPalette::Window 那格深色它不认 —— 于是"深色字/浅色底"两头各来一半，
// 置顶窗的右键菜单就是这么变成白底压浅灰字、看不见字的（2026-10-17 他截图报的）。
QString darkMenuStyle();

// 悬停说明（tooltip）那一小块的深色样式，**只有这一份**，而且必须挂在 qApp 上。
// 毛病和上面那条一模一样：Qt6 的 windows11 风格画 tooltip 用的是**系统**的浅色底，
// applyDarkPalette 里设的 ToolTipBase/ToolTipText 它根本不读 —— 于是灰字压白底，
// 一行都看不见（2026-10-18 他报的"夜间模式的鼠标悬停说明，字体是灰色，背景是白色"）。
// 为什么不能塞进 darkMenuStyle 由各窗口自己挂：tooltip 是**独立顶层窗口**，
// 挂在某个窗口的样式表管不到它，只有 qApp 那一份能盖住。
QString darkTipStyle();

// 运行日志：追加一行到 %APPDATA%/ltool/log.txt
void log(const QString& msg);

// 控制台输出解码：winget 一类程序混着 UTF-8 和本地 ANSI(GBK)，整行 UTF-8 解不动就退回本地编码。
// feed() 只返回已经完整的行，半行留在内部等下一块，避免跨读取边界时误判编码。
struct ConsoleLines {
    static QString decode(const QByteArray& line);
    QByteArray pending;
    QString feed(const QByteArray& chunk);
    QString tail();   // 收尾：把没有换行符的残余也解出来
};

// 开机自启不在这里：那是平台层第三道缝，见 platform/Autostart.h
// （Windows 是 HKCU 的 Run 值，Linux 是 ~/.config/autostart/ltool.desktop，
//  macOS 是 ~/Library/LaunchAgents 那个 plist —— 三个平台各有各的机制，不是降级）

// 把写好的半成品改成正式名字（压缩包先写成 xxx.7z.part，完了才更名）。
// 刚关上的那一瞬间杀毒/索引常常还抓着这个新文件，所以给它 5 秒重试。
// 改不动时返回 false，*error 是给界面看的实话（半成品留在原地，路径进调用方的 target）。
bool renameStaged(const QString& stagedPath, const QString& destPath, QString* error);

// 版本号比较：<0 / 0 / >0。原来长在「关于」页里，插件商店判"清单这份是不是比本机新"
// 也要同一套规矩，抄第二遍迟早分叉（商店那侧第一次要它，就挪到公用的这一层）。
// 规则：每段按**数字**比（当字符串比 "10" < "9" 会反）；带预发后缀的（0.8.1-beta.1）
// 先只比数字段，数字全等才比后缀，而且**带后缀的那个更旧**（beta 是"奔着正式版去的"）。
// 垃圾输入按 0 段算，绝不抛 —— 这两处都是界面按钮后面的功能。
int compareVersions(const QString& a, const QString& b);

} // namespace Utils
