#pragma once

#include <QString>
#include <functional>

class QWidget;
class QNetworkAccessManager;
#include <QStringList>
#include <QVector>

// 开发环境插件的一个"组件"：检测命令 + 可选的 winget 安装包
struct DevItem {
    QString label;        // 如 "JDK (javac)"
    QStringList cmd;      // 检测命令，如 ["javac", "-version"]
    QString wingetId;     // 缺失时用 winget 安装的包 ID（可为空）
    QString note;         // 无 winget 时的说明（如"需通过 Visual Studio 安装"）
    // 以下为检测结果
    bool ok = false;
    QString version;
};

// 一个开发环境插件（对应 %APPDATA%/ltool/plugins/<id>.json，用户可自定义）
struct DevPlugin {
    QString id;    // java / c++ / node / python / rust / 自定义
    QString name;  // 显示名称
    QVector<DevItem> items;
};

namespace DevEnv {

// 首次运行时写入默认插件 JSON（java/c++/node/python/rust）
void ensureDefaultPlugins();

QVector<DevPlugin> loadPlugins();

// 阻塞检测插件的所有组件（在工作线程调用）
void detect(DevPlugin* p);

// 已安装的语言版本探测（按常见安装目录 / 版本管理器目录）
struct LangVersion {
    QString label;   // 显示名，如 "jdk-21.0.4" / "3.12"
    QString path;    // 根目录（原生分隔符）
};
QVector<LangVersion> detectVersions(const QString& langId);

// 把某个语言"接进"开发环境：PATH / JAVA_HOME / pyenv 选择器 / rustup 默认工具链。
// activate=true 表示"把这个版本设为当前使用"（下拉切换），false 表示"刚装完，先让它可用"。
// what 回填做了什么（给状态行和安装日志看台显示）。
bool applyEnvFor(const QString& langId, const QString& path, const QString& label,
                 bool activate, QString* what, QString* err = nullptr);

bool wingetAvailable();

// 「验证一下」用：把注册表里当前的 PATH（含刚装完写入的、本进程还没吸收的）拉平，
// 然后原样跑一次检测命令，把 stdout+stderr 一起带回来。
// 直接在自己进程里跑会误判 —— 环境变量改动只对"新开的进程"生效。
struct ProbeResult {
    bool ok = false;
    QString program;
    QStringList args;
    QString output;      // 原样输出（含 stderr），已经解码过
    int exitCode = -1;
    QString resolved;    // 命令实际解析到的 exe 全路径（找不到为空）
};
ProbeResult probe(const QStringList& cmd);

// 在"注册表里的 PATH"（机器 + 用户，已展开 %VAR%）中找某个命令对应的 exe；找不到返回空
QString whichInRegistryPath(const QString& program);

// 下载代理：配置页 [net] proxy 优先，没填就用 Windows 系统代理（IE/Edge 设置里那个）。
// 返回空表示不用代理。形如 http://127.0.0.1:10808
QString proxyUrl();
QString systemProxyUrl();     // 只读系统设置，不读配置

// 让 QNetworkAccessManager 用同一个代理（下载器、网站图标抓取都走这里）
void applyProxy(QNetworkAccessManager* nam);

// 配置页里的安装目录（[install] path）；空字符串表示不指定 --location。
// 配置的盘符在这台机器上不存在时（换机器/虚拟机/没插盘）退到 C 盘同一路径。
QString installLocation();

// 安装目录：配置页的「安装目录」再套一层软件自己的子目录（subdirName），
// 避免 7-Zip 这类"把文件直接铺进目标目录"的安装器把根目录搞乱
QString sanitizeDirName(const QString& name);
QString installDirFor(const QString& subdirName);

// 静默安装参数：winget install --id X [-v 版本] -e --silent --source winget
//              --disable-interactivity --accept-* [--location 安装目录/软件名] [--proxy ...]
// args 用于直接启动进程（不过 shell），command 只是给人看的同一份命令。
// useProxy=false 用来生成不带 --proxy 的同一命令（winget 的命令行代理要管理员先启用）。
QStringList wingetInstallArgs(const QString& wingetId, const QString& version = QString(),
                              const QString& subdirName = QString(), bool useProxy = true);
QString wingetInstallCommand(const QString& wingetId, const QString& version = QString(),
                             const QString& subdirName = QString(), bool useProxy = true);

// 后台静默发起安装（无控制台窗口、全程不弹确认框）：输出实时写进「静默安装日志」看台
// 并落 log.txt，但不会自动弹看台——托盘图标转圈代表有安装在跑，点它才打开。
// msg 回填：失败时是原因，成功时是实际执行的命令。返回 false 表示没发起。
// subdirName 为空表示不带 --location（系统运行库这类必须装到系统默认位置）。
// done 可选：安装结束时回调退出码 + 输出全文 + 任务号（被「取消安装」打断时用
// SilentInstallLog::wasCancelled 区分）。
bool startSilentInstall(const QString& wingetId, const QString& version = QString(),
                        const QString& subdirName = QString(), QString* msg = nullptr,
                        std::function<void(int, const QString&, int)> done = nullptr);

// 静默安装成功后自动把该语言接进开发环境（PATH / JAVA_HOME 等），结果写回同一个
// 日志任务。故意不依赖界面：页面切走/关掉也要照样配好，才谈得上"零门槛"。
// 返回值是给界面状态行显示的一句话摘要（跳过时为空）。
QString postInstallEnvSetup(const QString& langId, const QString& displayName, int code,
                            const QString& output, int jobId);

// winget 可用版本列表：持久化在 versions.json，几乎不变，只有手动刷新才联网
QStringList cachedVersions(const QString& wingetId);
qint64 versionCacheTime(const QString& wingetId);       // 0 = 从未抓取
void fetchVersionsAsync(const QString& wingetId, std::function<void(QStringList)> done);

// 弹出安装过程对话框（模态），返回 true 表示已发起
bool runInstall(QWidget* parent, const DevItem& item, QString* err = nullptr);
QString wingetCommandLine(const DevItem& item);

} // namespace DevEnv
