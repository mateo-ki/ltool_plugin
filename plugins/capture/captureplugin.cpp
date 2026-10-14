// ltool 工具插件：F1 截图（截图浮层 + 置顶窗 + 标注 + OCR 取字）
//
// 这是第三个「动作型」插件（前两个：plugins/shell 右键那一路、plugins/record 的 F2）。
// 用户按的是热键 F1 —— 注册留在主程序那层薄壳里（它必须是那个还能起得来的壳），
// 按下之后主程序把动作名 "capture" 交给 PluginHost::invoke，路由到这里，剩下的全在这个 dll 里：
//   CaptureOverlay = 全屏浮层：框选、悬停吸附到别人家窗口、标注（矩形/圆/箭头/文字/马赛克）、撤销
//   PinWindow      = 置顶截图窗：左侧往期、图上划词取字、标注对象还能选中改大小
//   Anno           = 上面两家共用的那一套工具图标与形状几何（手感必须一致，所以只有一份）
//   Ocr / OcrPp / OcrBundle / OcrPanel = 取字：PP-OCRv4 + ONNX Runtime，引擎包首次用到时下载
// 这七个文件是 2026-10-15 从 src/ 原样搬进来的，十二套判据（--snaprect / --ocrtest / --ocrlayout /
// --pinpick / --ocrchain / --pintool / --pindel / --ocrget / --ocrpanel / --pinshot / --capgeo /
// --capflash）也跟着搬到 captureselftest.cpp：页面代码进了插件，主程序就链接不到它内部的测试函数，
// 只能 extern "C" 导出、由主程序按名字点名要（找不到符号 = 判失败，不许伪装成绿）。
//
// 有两样东西**故意留在 core**，不是忘了搬：
//   CaptureHistory（往期列表）—— 配置页要改存放目录并 reload()，core 不能反向依赖插件 dll；
//   CaptureState::isActive()   —— 搜索框要靠它决定"浮层举着的时候先别收起"。
// 详见 src/CaptureHistory.h。
#include "ltool-plugin.h"

#include "CaptureHistory.h"
#include "CaptureOverlay.h"
#include "Ocr.h"
#include "OcrBundle.h"
#include "PinWindow.h"
#include "Utils.h"

#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

namespace {

typedef bool (*ActionFn)(const QStringList& args);

// 接住 F1：把全屏浮层开起来。**返回值只回答"这条动作我接没接住"** ——
// 浮层开起来之后用户截不截、按不按 Esc，都由浮层自己和日志去说；在这儿返回 false
// 会让主程序再报一次"做不了"（2026-10-15 右键那一路已经错过一次）。
// 已经开着浮层时 start() 自己会写一条 "capture: F1 被忽略" 的日志，不会叠两层。
bool aCapture(const QStringList&)
{
    CaptureOverlay::start();
    return true;
}

struct Action { const char* name; ActionFn fn; };

// 这张表是"这个插件实现哪些动作"的唯一来源：自报的 actions() 和 invoke 的分发都从它长出来，
// plugin.json 里那份 "Actions" 由 --plugintest 和它对（对不上 = 主程序按牌路由会落空）。
const Action kActions[] = {
    { "capture", aCapture },
};

QStringList actionNames()
{
    QStringList out;
    for (const Action& a : kActions)
        out << QLatin1String(a.name);
    return out;
}

const char* kDesc = "按 F1 全屏截图：框选/窗口吸附/标注，再置顶、取字（OCR 首次用要下一趟引擎）";

} // namespace

class CapturePlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("capture"); }
    QString name() const override { return QStringLiteral("截图"); }
    QString desc() const override { return QString::fromUtf8(kDesc); }
    QStringList keywords() const override
    {
        return { QStringLiteral("截图"), QStringLiteral("截屏"), QStringLiteral("标注"),
                 QStringLiteral("置顶"), QStringLiteral("提取文字"), QStringLiteral("ocr"),
                 QStringLiteral("capture"), QStringLiteral("f1") };
    }
    QStringList actions() const override { return actionNames(); }

    bool invoke(const QString& action, const QStringList& args) override
    {
        for (const Action& a : kActions)
            if (action == QLatin1String(a.name))
                return a.fn(args);
        return false;              // 表里没有 = 真没实现，只有这个数能让主程序说"没人接"
    }

    // 动作型插件也得有一张页（商店里点开要有交代）。这页只说实话：这插件干什么、靠 F1 触发、
    // 此刻浮层开没开、往期几张、取字的引擎在不在。**不放第二颗"开始截图"按钮** ——
    // 同一件事两个入口就是两份真相，截图那条链只认 F1 那一个。
    QWidget* createPage(QWidget* parent) override
    {
        auto* w = new QWidget(parent);
        auto* col = new QVBoxLayout(w);
        col->setContentsMargins(12, 10, 12, 10);
        col->setSpacing(8);
        auto* head = new QLabel(QStringLiteral("F1 截图"), w);
        head->setStyleSheet(QStringLiteral("font-weight:600;font-size:15px;"));
        col->addWidget(head);
        col->addWidget(new QLabel(QStringLiteral(
            "按 F1 把整块屏拍下来并盖上一层浮层：拖动框选，鼠标停在别的窗口上会自己吸附过去；"
            "框好之后能画矩形/圆/箭头/写字/打马赛克，Ctrl+Z 撤销。\n"
            "松手之后可以「复制」「存文件」「置顶」—— 置顶窗左边是往期截图，"
            "在图上按住拖动就能选字（要取整段文字就点「提取文字」）。"), w));
        auto* status = new QLabel(w);
        status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        col->addWidget(status);
        col->addWidget(new QLabel(QStringLiteral(
            "这一页不放第二颗「开始截图」按钮：截图只认 F1 那一个入口。"
            "取字用的引擎不在安装包里，第一次用到时才会下载（约 28MB），没下之前"
            "「提取文字」会先问你要不要下。"), w));
        col->addStretch(1);
        // 这四样都会变（浮层开合、截新图、换存放目录、引擎下没下），所以每秒重读一遍 ——
        // 一次算完写死就是撒谎。
        auto* tick = new QTimer(w);
        QObject::connect(tick, &QTimer::timeout, w, [status] {
            const OcrBundle::Files eng = OcrBundle::status();
            status->setText(QStringLiteral("此刻：%1 · 往期 %2 张 · 截图存到：%3 · 取字引擎：%4")
                                .arg(CaptureState::isActive() ? QStringLiteral("浮层开着")
                                                              : QStringLiteral("没在截图"),
                                     QString::number(CaptureHistory::count()),
                                     QDir::toNativeSeparators(Utils::capturesDir()),
                                     eng.present ? QStringLiteral("已就位")
                                                 : QStringLiteral("还没下载（第一次用到时问你要）")));
        });
        tick->start(1000);
        return w;
    }
};

#include "captureplugin.moc"
