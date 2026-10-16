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

enum class KnownFolder {
    Desktop,
    PublicDesktop,
    UserStartMenuPrograms,
    CommonStartMenuPrograms,
};
QString knownFolderPath(KnownFolder kf);

// 从 exe/dll/ico 提取图标并保存为 PNG（用于缓存），成功返回 true
bool extractIconToPng(const QString& iconFile, int iconIndex, const QString& pngOut);

// 通用文件/文件夹图标（仅限 GUI 线程调用，内部有缓存）
QIcon cachedFileIcon(const QString& path);

// 用 ShellExecuteEx 打开文件/lnk/目录
// async=true：让 shell 异步执行并立即返回。带 DDE 段的 .lnk（如 LX Music）同步调用
// 会卡在 DDE 握手上等回调（实测 729ms），启动器回车必须用这个。
bool launch(const QString& path, const QString& params = {}, const QString& workingDir = {},
            bool async = false);
bool launchAsAdmin(const QString& path, const QString& params = {}, const QString& workingDir = {});

// 在资源管理器中定位文件
void revealInExplorer(const QString& path);

QString expandEnv(const QString& s);

// 从注册表/服务里那种"命令行字符串"取出可执行文件本身。一堆软件把没加引号的空格路径
// 直接写进注册表（C:\Program Files\...），所以不能见空格就切：先整条试、
// 再看是不是以 .exe/.dll 之类结尾、最后逐个空格前缀试哪个真实存在。
QString exeFromCommand(const QString& cmd);

// 判断一个"启动目标"是不是已经不在了：展开 %VAR%、去引号；
// 只写了文件名的交给 SearchPathW（system32 里那些不能算失效）
bool fileTargetMissing(const QString& raw);

// 程序图标（托盘/窗口共用，程序内绘制生成）
QIcon appIcon();

// 读取 exe 的文件版本（FileVersionInfo），失败返回空串
QString fileVersion(const QString& exePath);

// 圆角字母图标（菜单/占位图标共用）
QIcon letterIcon(const QString& text, const QColor& color);
QIcon bagIcon(const QColor& color);   // 菜单里「插件商店」那一行画的是一颗袋子，不是第一个字

// 让系统立刻回收工作集（任务管理器里的"内存"立即下降）。
// 注意：这会让下一次交互把所有页换回来，别在常驻的热键路径上用它
void trimWorkingSet();

// 当前进程工作集（MB），-1 = 读不到。诊断"唤出慢"用
int workingSetMB();

// 托盘上冒一句就走：不弹框、不抢焦点。优先把这句话交给**常驻实例**去举 —— 一次性进程
// 要是为了"让气泡显示满 N 秒"而自己多活 N 秒，用户看到的就是"这件事要 N 秒"（右键解压
// 实测 0.3 秒的活儿被那 5 秒拖成十几倍）。没有实例在跑才自己举。
// 通道就是单实例那个 socket："note\t标题\t正文"。plugins/shell 和 plugins/linuxdo 都走这一条。
void trayNote(const QString& title, const QString& msg, int msec = 5000);

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

// 开机自启（HKCU Run 键）
void setAutostart(bool on);
bool autostartEnabled();

// 终止进程及其全部子孙（winget 只是壳，真正在装的是它拉起的 msiexec/setup.exe，
// 只杀父进程装还是会跑完）。已提权的子进程杀不动，返回 false 让调用方如实说。
bool killProcessTree(qint64 pid);

// 把写好的半成品改成正式名字（压缩包先写成 xxx.7z.part，完了才更名）。
// 刚关上的那一瞬间杀毒/索引常常还抓着这个新文件，所以给它 5 秒重试。
// 改不动时返回 false，*error 是给界面看的实话（半成品留在原地，路径进调用方的 target）。
bool renameStaged(const QString& stagedPath, const QString& destPath, QString* error);

} // namespace Utils
