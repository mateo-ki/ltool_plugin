#pragma once

#include <QByteArray>
#include <QString>
#include <QIcon>

// 单实例用的本地套接字名字。右键动作（解压/压缩）是一次性进程，干完活把托盘气泡
// 交给常驻实例去举，也走这条通道 —— 见 plugins/shell/shellaction.cpp 的 trayNote()。
inline constexpr const wchar_t kInstanceKey[] = L"ltool.instance";

namespace Utils {

// 数据目录：配置、日志、应用/文件索引、剪贴板历史与图、OCR 模型、图标缓存全在这儿。
// **默认落在软件目录下 —— `<exe 目录>\data`，不往 C 盘的 %APPDATA% 堆**（他 2026-10-21 点名）。
// 三档优先：`LTOOL_DATA_DIR` 环境变量（自测沙箱 / 便携模式）→ 软件目录（写得进去的话）
// → `%APPDATA%\ltool`（装到只读位置的退路，会留一条日志）。
QString dataDir();
// 上面那三档的算法，抽成纯函数是为了判据能**不碰真文件系统**就问它（三个输入都给死）。
QString resolveDataDir(const QString& appDir, const QString& envOverride, const QString& appData,
                       bool appDirWritable);
// 一次性搬家：老 `%APPDATA%\ltool` 还在、新目录是空的 → 逐个复制并按 sha256 对过。
// **旧目录原样留着不删**（那是真实数据，删错了没法复），返回搬了几个文件。
int migrateLegacyDataDir(QString* note);
// 搬家前先给"程序自己写的默认值"让路：目标目录里那份**内容逐字节等于内置默认**的文件先删掉，
// 好让老目录里那份真数据进来（其余同名文件照旧不覆盖）。
// 为什么单独抽出来：2026-10-21 真丢过一次 —— deliver 的 `installed` 那一栏会先拿装好的 exe 跑
// `--plugintest`，那一趟在还没搬的数据目录里写下了默认 `webs.json`（2 条），于是"已有同名不覆盖"
// 把你那份 24 条网站关键字挡在老目录里，界面上就只剩默认那 2 条。
int clearStockDefaultsForMigration(const QString& dir);
// 搬家整条链"两个目录都给死"的那一版：查标记 → 默认表让路 → 逐文件按 sha256 复制 → 落标记，
// 全在这一颗里；`migrateLegacyDataDir` 只负责算出这两个目录再交给它（`LTOOL_DATA_DIR` 设着时根本不叫它）。
// 为什么要单独有一版：判据得能拿两个临时目录跑**这条真链**，而不是只量其中一颗 helper ——
// 上面那次事故的形状恰恰是"helper 都在、也测绿了，只是搬家那一步漏了调用它"。
int migrateBetween(const QString& legacy, const QString& now, QString* note);
// 「这个数据目录还没接住你的真数据」：老目录还在、而新目录里既没有搬过的标记也没有 config.ini
//（和搬家那三条守卫同一套判断，同一个来源）。设着 `LTOOL_DATA_DIR` 时永远返回 false ——
// 沙箱/便携模式那个目录就是它自己的，往里写默认值是对的。
// 为什么要有这颗：**任何**往还没搬的目录里写"程序自带的默认表"的路径，都会让
// 搬家那条"同名不覆盖"把真数据挡下来（2026-10-21 那次事故的一般形状，不止 webs.json 一处）。
bool legacyMigrationPending();
// 上面那颗拆开问的那一版：两个目录给死，判据不碰真文件系统就能问（同 `resolveDataDir` 的规矩）。
bool migrationPendingFor(const QString& legacy, const QString& now);
// 已经"搬过"的那些安装里补一次（一次性脚本 `build/verify/migrate_ltool_data.ps1` 的程序版）：
// 只按两条明确的规矩，其余一概不动 ——
//   ① 目标里那份**内容逐字节等于内置默认**的表，而老目录里有一份不同的（= 用户的真那份）→ 用老的；
//   ② 目标里 `clipboard.json` 现在**正引用着**、而本地缺失的图片 → 从老目录补进来。
// 不做"整棵树补缺"：那样会把用户后来自己删掉的东西（往期截图、清理过的剪贴板图）每次启动都复活。
// 返回补了几个文件，`actions` 里逐条写清补了什么（进日志）。
int backfillFromLegacy(const QString& legacy, const QString& now, QStringList* actions);
// 搬家那一段真正干活的部分：整棵树复制过去，**每个文件都按 sha256 对过**才算搬成，
// 对不上的删掉半成品并记进 problems。源目录一个字都不动。抽出来是为了判据能拿两个临时目录真跑一遍。
int copyVerifiedTree(const QString& from, const QString& to, QStringList* problems, int* skipped = nullptr);
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
