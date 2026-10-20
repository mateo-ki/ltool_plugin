// ltool 工具插件：F2 录屏
//
// 这是第二个「动作型」插件（第一个是 plugins/shell）。用户按的不是商店里的一张卡片，
// 而是热键 F2 —— 热键注册留在主程序（它必须是那个还能起得来的薄壳），按下之后主程序
// 把动作名 "record" 交给 PluginHost::invoke，路由到这里，剩下的全在这个 dll 里：
//   Recorder    = 取帧（Windows.Graphics.Capture）+ 编码（Media Foundation H.264）+ 写 MP4
//   RecordUi    = 选窗口那一个窗 + 录制中的悬浮条 + 画质/画面两档
//   RecProbe    = 录之前先量机器（编码器在不在、按窗口取帧能不能用）
// 三个源文件是 2026-10-15 从 src/ 原样搬进来的，判据（--recprobe / --recq / --rectest /
// --recordui / --memloop）也跟着从 src/main.cpp 搬到这里：页面代码进了插件，主程序就链接
// 不到它内部的测试函数，只能 extern "C" 导出、由主程序按名字点名要。
//
// 为什么这里没有 ltoolPluginSelftest()：录屏的判据全都要真屏幕（DWM 会话 + WGC），
// 放进 --plugintest 那条快判据路会把六套回归一起拖慢，CI 的 runner 上更是必然红 ——
// 所以只导出被点名跑的那五个，一个都不并进默认那一路。
// 顶高到 Win10：MF 的 Lock2D / MFT_ENUM_CONFIG 那些成员全在 `#if NTDDI_VERSION >= NTDDI_WIN8`
// 后面，Qt/CMake 传进来的默认值是 0x0601，不顶高就报"不是 IMFMediaBuffer 的成员"。
// 这一段必须在所有 #include 之前（Recorder.cpp / RecProbe.cpp 里同样的写法有详细说明）。
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0A00
#ifdef NTDDI_VERSION
#undef NTDDI_VERSION
#endif
#define NTDDI_VERSION 0x0A000000
#ifdef WINVER
#undef WINVER
#endif
#define WINVER 0x0A00

#include "ltool-plugin.h"

#include "Archives.h"
#include "RecProbe.h"
#include "Recorder.h"
#include "RecordUi.h"
#include "WindowPreview.h"
#include "Utils.h"

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScreen>
#include <QSettings>
#include <QSize>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <functional>

// ============================================================================
// 插件本体
// ============================================================================
namespace {

typedef bool (*ActionFn)(const QStringList& args);

// 接住 F2：弹选择窗，选完就开录。**返回值只回答"这条动作我接没接住"** ——
// 录成没录成、用户是不是按了「不要了」，都由悬浮条和日志自己去说；
// 在这儿返回 false 会让主程序再报一次"做不了"（2026-10-15 右键那一路已经错过一次）。
bool aRecord(const QStringList&)
{
    RecordUi::startViaPicker();
    return true;
}

struct Action { const char* name; ActionFn fn; };

// 这张表是"这个插件实现哪些动作"的唯一来源：自报的 actions() 和 invoke 的分发都从它长出来，
// plugin.json 里那份 "Actions" 由 --plugintest 和它对（对不上 = 主程序按牌路由会落空）。
const Action kActions[] = {
    { "record", aRecord },
};

QStringList actionNames()
{
    QStringList out;
    for (const Action& a : kActions)
        out << QLatin1String(a.name);
    return out;
}

const char* kDesc = "按 F2 录整块屏幕或某一个窗口，出 mp4（画质/画面两档可选）";

// 「宽x高」那面旗标参数的唯一一处解析。--rectest 和 --memloop 都收它，
// 以前它俩各写一遍 split('x')，改默认值就得记得改两处。
QSize cardSize(const char* sizeSpec, const QSize& fallback)
{
    const QString s = QString::fromUtf8(sizeSpec ? sizeSpec : "");
    if (s.contains(QLatin1Char('x'))) {
        const int w = s.section(QLatin1Char('x'), 0, 0).toInt();
        const int h = s.section(QLatin1Char('x'), 1, 1).toInt();
        if (w >= 64 && h >= 64)
            return QSize(w, h);
    }
    return fallback;
}

} // namespace

class RecordPlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("record"); }
    QString name() const override { return QStringLiteral("录屏"); }
    QString desc() const override { return QString::fromUtf8(kDesc); }
    QStringList keywords() const override
    {
        return { QStringLiteral("录屏"), QStringLiteral("录像"), QStringLiteral("screen"),
                 QStringLiteral("record"), QStringLiteral("f2") };
    }
    QStringList actions() const override { return actionNames(); }

    bool invoke(const QString& action, const QStringList& args) override
    {
        for (const Action& a : kActions)
            if (action == QLatin1String(a.name))
                return a.fn(args);
        return false;              // 表里没有 = 真没实现，只有这个数能让主程序说"没人接"
    }

    // 动作型插件也得有一张页（商店里点开要有交代）。这页只说实话：这插件干什么、靠 F2 触发、
    // 此刻在不在录、配置里那两档是什么。**不再放第二个选择窗、也不放开始/停止按钮** ——
    // 同一件事两个入口就是两份真相，录屏那条链只认 F2 那一个。
    QWidget* createPage(QWidget* parent) override
    {
        auto* w = new QWidget(parent);
        auto* col = new QVBoxLayout(w);
        col->setContentsMargins(12, 10, 12, 10);
        col->setSpacing(8);
        auto* head = new QLabel(QStringLiteral("F2 录屏"), w);
        head->setStyleSheet(QStringLiteral("font-weight:600;font-size:15px;"));
        col->addWidget(head);
        col->addWidget(new QLabel(QStringLiteral(
            "按 F2 弹一个选择窗：整块屏幕或桌面上任何一个窗口，挑一个就开始录；"
            "选择窗开着的时候再按一次 F2 = 取消。\n"
            "录制中屏幕右下角有一条悬浮窗，上面是计时和帧数，「停止」存文件、"
            "「不要了」把这一局丢掉。产物是普通 mp4（系统 H.264 编码器），落在录像目录。"), w));
        auto* status = new QLabel(w);
        status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        col->addWidget(status);
        col->addWidget(new QLabel(QStringLiteral(
            "这一页不放开始/停止按钮：录屏只认 F2 那一个入口。要改画质或画面档，"
            "在选择窗底下那两颗下拉框改，改一下就记住。"), w));
        col->addStretch(1);
        // 状态会变（正在录 / 刚停下 / 换了档），所以每秒重读一遍 —— 一次算完写死就是撒谎。
        auto* tick = new QTimer(w);
        QObject::connect(tick, &QTimer::timeout, w, [status] {
            status->setText(QStringLiteral("此刻：%1 · 画质档：%2 · 画面档：%3 · 录像存到：%4")
                                .arg(RecordUi::isRecording() ? QStringLiteral("正在录")
                                                             : QStringLiteral("没在录"),
                                     RecordUi::qualityText(), RecordUi::maxWidthText(),
                                     QDir::toNativeSeparators(Utils::recordingsDir())));
        });
        tick->start(1000);
        return w;
    }
};

#include "recordplugin.moc"

// ============================================================================
// 判据本体（从 src/main.cpp 原样搬进来的一条都没改）
//
// 形状保持原样：打 PASS/FAIL/INFO 行，最后打 "<旗标>: 失败 N 项"，返回那个 N。
// N 就是主程序那边的退出码；主程序只在**符号找不到**时自己补一行 FAIL + 失败 1 项，
// 免得"插件没加载起来"伪装成"测试全绿"。
// ============================================================================
namespace {

// --recq 用：把那张自制的卡录一小段，然后把产物读回来数一遍。
// 判据全在返回值里，比较由调用方做 —— 这样"缩小"和"画质"两组用的是同一条录制路径，
// 不会一边测真编码器、另一边测我手搓的期望值。
// 卡片图案：上半白 / 下半黑 / 左沿一条黑带（朝向判据照 --rectest 那条教训：
// 上下左右都对称的图案，翻了也量不出来）。noise=true 时中间那条带每个 tick 换一次随机块，
// 让码率真的有事可做 —— 大片静止色块下"超清"和"流畅"能编出一样大的文件，测不出档位生效。
struct RecRun {
    int w = 0, h = 0;
    qint64 frames = 0;
    qint64 bytes = 0;
    bool topBright = false;     // 解出来的第 0 行确实比末行亮（缩完朝向还对）
    QString err;
};

QString sizeText(const QSize& s)
{
    return QStringLiteral("%1x%2").arg(s.width()).arg(s.height());
}

RecRun recordCardRun(int id, int cw, int ch, int maxWidth, double bpp, int ms, bool noise)
{
    RecRun out;
    QWidget* card = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint);
    card->resize(cw, ch);
    auto* topHalf = new QLabel(card);
    topHalf->setGeometry(0, 0, cw, ch / 2);
    topHalf->setStyleSheet(QStringLiteral("background:#ffffff;"));
    auto* botHalf = new QLabel(card);
    botHalf->setGeometry(0, ch / 2, cw, ch - ch / 2);
    botHalf->setStyleSheet(QStringLiteral("background:#000000;"));
    auto* stripe = new QLabel(card);
    stripe->setGeometry(0, 0, cw / 8, ch / 2);
    stripe->setStyleSheet(QStringLiteral("background:#000000;"));
    auto* lab = new QLabel(QStringLiteral("0"), card);
    lab->setGeometry(cw - 96, ch - 46, 94, 44);   // 右下角每 30ms 换一个数：不重绘 WGC 一帧也不给
    lab->setAlignment(Qt::AlignCenter);
    lab->setStyleSheet(QStringLiteral("background:#000000;color:#e8ebf2;font-size:26px;"));
    QLabel* blocks = nullptr;
    if (noise) {
        blocks = new QLabel(card);
        blocks->setGeometry(0, ch / 3, cw, ch / 3);
        blocks->setScaledContents(true);
    }
    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    card->move(avail.right() - cw - 12, avail.bottom() - ch - 12);
    card->setAttribute(Qt::WA_ShowWithoutActivating);
    card->show();
    QCoreApplication::processEvents();

    int tick = 0;
    QTimer anim;
    QObject::connect(&anim, &QTimer::timeout, card, [&] {
        lab->setText(QString::number(++tick));
        if (blocks) {         // 逐像素随机灰噪点放大到整条带：高，编码器必须真花码率才行
            // （以前是 24x12 的大块，放大后太平滑，两档码率编出来只差 7% —— 那条断言
            //   测的其实是内容，不是档位）
            QImage img(cw / 3, ch / 6, QImage::Format_RGB32);
            for (int y = 0; y < img.height(); ++y) {
                auto* row = reinterpret_cast<QRgb*>(img.scanLine(y));
                for (int x = 0; x < img.width(); ++x)
                    row[x] = QColor(QRandomGenerator::global()->bounded(256)).rgb();
            }
            blocks->setPixmap(QPixmap::fromImage(img));
        }
    });
    anim.start(30);

    const QString path = QDir::tempPath() + QStringLiteral("/ltool-recq-%1.mp4").arg(id);
    QFile::remove(path);
    Recorder rec;
    Recorder::Options o;
    o.fps = 15;
    o.maxWidth = maxWidth;
    const QSize enc = Recorder::encodedSize(cw, ch, maxWidth);
    o.videoBitrateKbps = Recorder::bitrateKbps(bpp, enc.width(), enc.height(), o.fps);
    QString err;
    if (!rec.start(WId(card->winId()), o, path, &err)) {
        out.err = err.isEmpty() ? QStringLiteral("没录起来") : err;
        anim.stop();
        card->close();
        card->deleteLater();
        QFile::remove(path);
        return out;
    }
    const qint64 until = QDateTime::currentMSecsSinceEpoch() + ms;
    while (QDateTime::currentMSecsSinceEpoch() < until) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    rec.stop();
    anim.stop();
    for (int i = 0; i < 30; ++i)
        QCoreApplication::processEvents();
    card->close();
    card->deleteLater();

    const Recorder::Mp4Info info = Recorder::inspectMp4(path);
    out.w = info.w;
    out.h = info.h;
    out.frames = info.frames;
    out.bytes = info.bytes;
    if (!info.ok)
        out.err = QStringLiteral("文件读不回来：%1").arg(info.error);
    const Recorder::LumaGrid g = Recorder::inspectLuma(path, 8);
    if (!g.ok) {
        if (out.err.isEmpty())
            out.err = QStringLiteral("亮度网格解不出来：%1").arg(g.error);
    } else {
        double top = 0, bot = 0;
        for (int c = 0; c < g.n; ++c) {
            top += g.at(0, c);
            bot += g.at(g.n - 1, c);
        }
        out.topBright = top > bot;
    }
    QFile::remove(path);
    return out;
}

} // namespace

// --recprobe [观察毫秒]：录屏之前先量机器（有没有 H.264/AAC 编码器、MP4 写得出吗、
// Win11 的按窗口取帧能不能用、一帧拷到 CPU 要多久）。会弹一个自己的小窗动 1 秒多，
// 不碰用户其它窗口。
extern "C" Q_DECL_EXPORT int ltoolRecprobeRun(int ms)
{
    const QString rep = RecProbe::run(ms);
    std::printf("%s\n", rep.toUtf8().constData());
    std::fflush(stdout);
    return 0;
}

// --recq：录屏那两个新选择（画面大小 / 画质）到底进没进编码器。
// 全部拿真编码器量：录自制卡 → 把 mp4 解回来说话。目标窗口是自己造的、
// 摆在屏幕右下角、不抢焦点，不碰用户桌面上任何东西。
extern "C" Q_DECL_EXPORT int ltoolRecqSelftest()
{
    int bad = 0;
    const auto check = [&bad](const char* name, const QString& got, const QString& want) {
        const bool ok = got == want;
        if (!ok)
            ++bad;
        std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                    got.toUtf8().constData(), want.toUtf8().constData());
    };
    const auto has = [&bad](const char* name, bool ok) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok)
            ++bad;
    };
    // 先量算法本身（这几条不用录）
    check("不缩：2560x1440 原样", sizeText(Recorder::encodedSize(2560, 1440, 0)),
          QStringLiteral("2560x1440"));
    check("长边≤1280：等比到 1280x720", sizeText(Recorder::encodedSize(2560, 1440, 1280)),
          QStringLiteral("1280x720"));
    check("奇数先取偶再缩：1281x721 ≤640 → 640x360", sizeText(Recorder::encodedSize(1281, 721, 640)),
          QStringLiteral("640x360"));
    check("只缩不放大：1920x1080 要 ≤2560 还是 1920x1080",
          sizeText(Recorder::encodedSize(1920, 1080, 2560)), QStringLiteral("1920x1080"));
    check("结果永远是偶数（H.264 拒奇数画幅）",
          sizeText(Recorder::encodedSize(999, 555, 333)), QStringLiteral("332x184"));
    // 码率按**编码后**画幅算：同一档 bpp，缩一半面积就该给一半不到
    const int kFull = Recorder::bitrateKbps(0.145, 2560, 1440, 15);
    const int kHalf = Recorder::bitrateKbps(0.145, 1280, 720, 15);
    std::printf("INFO 标准档码率：2560x1440=%d kbps，1280x720=%d kbps\n", kFull, kHalf);
    check("标准档在 1440p15 上就是以前那个 8000", QString::number(kFull / 1000 * 1000),
          QStringLiteral("8000"));
    has("缩了以后码率跟着降（不然是白花一倍的钱）", kHalf * 2 < kFull * 3);

    // 再拿编码器量三局：原始/标准、≤320/标准、原始/超清 vs 原始/流畅（带噪声）
    const RecRun full = recordCardRun(1, 640, 400, 0, 0.145, 2500, false);
    // 变量名别叫 small：Windows 的 rpcndr.h 里 `#define small char`，
    // 写它会编成 "const RecRun char = ..."（本机实测撞过）
    const RecRun shrunk = recordCardRun(2, 640, 400, 320, 0.145, 2500, false);
    std::printf("INFO 三局的错：%s / %s\n", full.err.toUtf8().constData(),
                shrunk.err.toUtf8().constData());
    check("不缩那局：文件画幅就是卡片取偶", QString("%1x%2").arg(full.w).arg(full.h),
          QStringLiteral("640x400"));
    check("选 ≤320 那局：文件画幅真的缩到了 320x200", QString("%1x%2").arg(shrunk.w).arg(shrunk.h),
          QStringLiteral("320x200"));
    std::printf("INFO 帧数：原始 %lld，缩小 %lld\n", full.frames, shrunk.frames);
    has("两局都解得出帧（不是只写出一个空壳文件）",
        full.frames > 10 && shrunk.frames > 10);
    has("缩小那局朝向还是正的（上亮下暗）", shrunk.topBright);
    std::printf("INFO 同内容同长度：原始 %lld 字节，缩小 %lld 字节\n", full.bytes, shrunk.bytes);
    has("缩小那局文件真的更小", shrunk.bytes < full.bytes);

    const int lowKbps = Recorder::bitrateKbps(0.060, 640, 400, 15);
    const int highKbps = Recorder::bitrateKbps(0.400, 640, 400, 15);
    const RecRun low = recordCardRun(3, 640, 400, 0, 0.060, 2500, true);
    const RecRun high = recordCardRun(4, 640, 400, 0, 0.400, 2500, true);
    std::printf("INFO 噪声画面：流畅 %lld 字节，超清 %lld 字节\n", low.bytes, high.bytes);
    std::printf("INFO 两档比值 %.0f%%\n", 100.0 * double(high.bytes) / double(low.bytes));
    // 这台机器的 MF 编码器把 MF_MT_AVG_BITRATE 当**上限**用（实测：内容不够复杂时
    // 它不会硬凑到目标码率）。所以"档位进去了"的判据不是"文件大一倍"，而是
    // "低那一档贴着它自己的上限"—— 差 15% 以内说明那个数真的交给了编码器。
    // 这台机器的 MF 编码器把 MF_MT_AVG_BITRATE 当**参考**而不是硬上限
    // （低档目标 400kbps，实测那 2.5 秒写出 ~900kbps；高档目标 1536kbps 实测 ~1180kbps）。
    // 所以判据不能写"贴着上限±15%"（我第一版就是这么写的，当场红），要写能站住的：
    // 同一份内容、同一时长，档位调高之后文件必须**明显不变小**（实测两遍 +26% / +35%）。
    std::printf("INFO 流畅 %lld 字节（目标 %d kbps）、超清 %lld 字节（目标 %d kbps）\n",
                low.bytes, lowKbps, high.bytes, highKbps);
    has("换到超清这一档，同内容下文件明显变大（档位真的进了编码器）",
        high.bytes > low.bytes * 110 / 100);
    has("两局噪声都录成了", low.frames > 10 && high.frames > 10);

    std::printf("recq: 失败 %d 项\n", bad);
    std::fflush(stdout);
    return bad;
}

// --rectest [秒]：验一遍"只录某个窗口 → mp4 → 再读回来数帧"。
// 目标窗口是自己造的一张每 30ms 换一个数字的卡片，不碰用户桌面上任何东西；
// 判据不是"文件存在"，而是拿 SourceReader 把文件解回来数出 N 帧、且 N 和采集到的帧数一致。
//
// 这面旗以前还带一个可选的「宽x高」（--rectest 4 2560x1440），用来把"奇数尺寸被 H.264 拒"
// 那一条钉死。搬进插件之后主程序只传得进一个整数（签名就是 int(*)(int)），所以这一路
// 固定按 640x400 跑；主程序看到第二个参数会把这话说出来，不装作它生效了。
// 画幅算法那几条（1281x721→640x360、999x555→332x184）由 --recq 继续量，一条没少。
extern "C" Q_DECL_EXPORT int ltoolRectestRun(int ms, const char* sizeSpec)
{
    const int secs = ms;                 // 旗标上那个数的单位本来就是**秒**
    const QSize dims = cardSize(sizeSpec, QSize(640, 400));
    const int cw = dims.width(), ch = dims.height();
    int bad = 0;
    const auto check = [&bad](const char* name, const QString& got, const QString& want) {
        const bool ok = got == want;
        if (!ok)
            ++bad;
        std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                    got.toUtf8().constData(), want.toUtf8().constData());
    };
    const QString path = QDir::tempPath() + "/ltool-rectest.mp4";
    QFile::remove(path);

    QWidget* card = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint);
    card->resize(cw, ch);
    // 图案必须上下左右都不对称：以前是一张"居中跳数字"的深色卡，
    // 画面翻过来的时候网格长得一模一样，朝向这种错根本抓不到（用户报的"录出来是倒的"）。
    // 上半白、下半黑、左沿抹一条黑带 → 解回来的亮度网格能同时判上下和左右。
    auto* topHalf = new QLabel(card);
    topHalf->setGeometry(0, 0, cw, ch / 2);
    topHalf->setStyleSheet(QStringLiteral("background:#ffffff;"));
    auto* botHalf = new QLabel(card);
    botHalf->setGeometry(0, ch / 2, cw, ch - ch / 2);
    botHalf->setStyleSheet(QStringLiteral("background:#000000;"));
    auto* stripe = new QLabel(card);
    stripe->setGeometry(0, 0, cw / 8, ch / 2);
    stripe->setStyleSheet(QStringLiteral("background:#000000;"));
    auto* lab = new QLabel(QStringLiteral("0"), card);
    lab->setGeometry(cw - 96, ch - 46, 94, 44);   // 右下角跳动：不重绘的话 WGC 一帧也不给
    lab->setAlignment(Qt::AlignCenter);
    lab->setStyleSheet(QStringLiteral("background:#000000;color:#e8ebf2;font-size:26px;"));
    // 必须真在屏幕上：完全在显示器外的窗口 Qt 直接跳过重绘，WGC 就一帧也不给（实测只出 1 帧）。
    // 挑右下角，且不抢焦点，尽量不打扰正在用机器的人。
    {
        const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
        card->move(avail.right() - cw - 12, avail.bottom() - ch - 12);
    }
    card->setAttribute(Qt::WA_ShowWithoutActivating);
    card->show();
    QCoreApplication::processEvents();

    int tick = 0;
    QTimer anim;
    QObject::connect(&anim, &QTimer::timeout, card, [&] { lab->setText(QString::number(++tick)); });
    anim.start(30);

    Recorder rec;
    Recorder::Options o;
    o.fps = 15;
    o.videoBitrateKbps = 8000;
    QString err;
    const bool started = rec.start(card->winId(), o, path, &err);
    std::printf("INFO start=%d err=%s\n", started ? 1 : 0, err.toUtf8().constData());
    if (started) {
        QElapsedTimer wall;
        wall.start();
        while (wall.elapsed() < secs * 1000LL) {
            QCoreApplication::processEvents();
            QThread::msleep(20);
        }
        anim.stop();
        const qint64 wantMs = wall.elapsed();
        rec.stop();
        check("录制没有报错", rec.errorText(), QString());
        const int frames = rec.framesCaptured();
        std::printf("INFO 采到 %d 帧 / %lldms（约 %.1f fps）\n", frames, wantMs,
                    frames * 1000.0 / std::max<qint64>(1, wantMs));
        check("帧数够（至少 8fps 的量）", frames >= secs * 8 ? "yes" : "no", "yes");
        const Recorder::Mp4Info mp4 = Recorder::inspectMp4(path);
        std::printf("INFO 读回: %lld 帧 / %lldms / %lld 字节 / 音轨=%d err=%s\n",
                    long long(mp4.frames), long long(mp4.durationMs), long long(mp4.bytes),
                    mp4.hasAudio ? 1 : 0, mp4.error.toUtf8().constData());
        check("读回来没报错", mp4.error, QString());
        // 读回来的帧数可以比写进去的多：SourceReader 会按流声明的 15fps 补帧（窗口静止时
        // WGC 不给帧，我这边不凑帧），所以这里判的是"不少、且不离谱地多"。
        check("解出来的帧数不少于采集的", mp4.frames >= frames ? "yes" : "no", "yes");
        check("补帧没失控", mp4.frames <= frames * 3 / 2 + 10 ? "yes" : "no", "yes");
        check("时长和录的秒数对得上",
              qAbs(mp4.durationMs - wantMs) < wantMs / 4 ? "yes" : "no", "yes");
        check("默认不录声", mp4.hasAudio ? "yes" : "no", "no");
        check("文件不是空壳", mp4.bytes > 2000 ? "yes" : "no", "yes");
        // ---- 朝向：把文件解一帧出来看亮度网格（NV12 的 Y 平面第 0 行 = 画面上沿）----
        const Recorder::LumaGrid lg = Recorder::inspectLuma(path, 8);
        std::printf("INFO 亮度网格 %dx%d（帧 %dx%d pitch=%ld）err=%s\n", lg.n, lg.n, lg.w,
                    lg.h, lg.pitch, lg.error.toUtf8().constData());
        if (lg.ok) {
            static const char kRamp[] = " .:-=+*#%@";
            for (int r = 0; r < lg.n; ++r) {
                std::printf("INFO |");
                for (int c = 0; c < lg.n; ++c) {
                    const int idx = qBound(0, int(lg.at(r, c) * 9 / 255.0), 9);
                    std::printf("%c", kRamp[idx]);
                }
                std::printf("|  %d\n", int(lg.at(r, 0)));
            }
        }
        const auto meanRows = [&lg](int from, int to, int c0, int c1) {
            double s = 0.0;
            int k = 0;
            for (int r = from; r <= to; ++r)
                for (int c = c0; c <= c1; ++c) {
                    s += lg.at(r, c);
                    ++k;
                }
            return k ? s / k : 0.0;
        };
        const double topMean = meanRows(1, 2, 2, lg.n - 1);
        const double botMean = meanRows(lg.n - 3, lg.n - 2, 0, lg.n - 1);
        std::printf("INFO 上排均亮=%.0f 下排=%.0f 左上=%.0f 右上=%.0f\n", topMean, botMean,
                    lg.at(1, 0), lg.at(1, lg.n - 1));
        check("画面没上下颠倒（白的在上、黑的在下）",
              lg.ok && topMean > botMean + 60 ? "yes" : "no", "yes");
        check("画面没左右镜像（左沿那条黑带还在左边）",
              lg.ok && lg.at(1, lg.n - 1) > lg.at(1, 0) + 60 ? "yes" : "no", "yes");
    } else {
        ++bad;
    }
    // ---- 第二路：录整块屏幕（F2 选择器最上面那一条）。上面那张卡还在每 30ms 换一个数字，
    //      所以"屏幕一定有变化"是确定的，不用去合成鼠标移动。
    //      朝向不在这里重验：编码端 stride/直拷那一路两种模式共用，窗口那一路已经验过。----
    {
        const HMONITOR mon = MonitorFromPoint(POINT { 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi {};
        mi.cbSize = sizeof(mi);
        int mw = 0, mh = 0;
        if (mon && GetMonitorInfoW(mon, &mi)) {
            mw = mi.rcMonitor.right - mi.rcMonitor.left;
            mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        }
        const QString mpath = QDir::tempPath() + "/ltool-rectest-screen.mp4";
        QFile::remove(mpath);
        Recorder mrec;
        Recorder::Options mo;
        mo.fps = 15;
        QString merr;
        const bool mstarted = mrec.start(WId(mon), mo, mpath, &merr, true);
        std::printf("INFO 录屏幕 start=%d err=%s 屏幕=%dx%d\n", mstarted ? 1 : 0,
                    merr.toUtf8().constData(), mw, mh);
        check("录整块屏幕能开起来", mstarted ? "yes" : "no", "yes");
        if (mstarted) {
            check("录屏幕时 running() 是真的", mrec.running() ? "yes" : "no", "yes");
            QElapsedTimer w2;
            w2.start();
            while (w2.elapsed() < 1500) {
                QCoreApplication::processEvents();   // 让卡片的 QTimer 跑起来，屏幕才有变化
                QThread::msleep(10);
            }
            const qint64 recMs = w2.elapsed();
            mrec.stop();
            const int mf = mrec.framesCaptured();
            std::printf("INFO 录屏幕采到 %d 帧 err=%s\n", mf,
                        mrec.errorText().toUtf8().constData());
            check("录屏幕采到帧了", mf > 0 ? "yes" : "no", "yes");
            check("录屏幕全程没报错", mrec.errorText(), QString());
            check("录屏幕收尾后 running() 回 false", mrec.running() ? "yes" : "no", "no");
            const Recorder::Mp4Info mm = Recorder::inspectMp4(mpath);
            std::printf("INFO 录屏幕读回: %lld 帧 / %lldms / %dx%d / %lld 字节\n",
                        long long(mm.frames), long long(mm.durationMs), mm.w, mm.h,
                        long long(mm.bytes));
            check("录屏幕的画幅=这块屏幕的物理尺寸（H.264 只吃偶数）",
                  QStringLiteral("%1x%2").arg(mm.w).arg(mm.h),
                  QStringLiteral("%1x%2").arg(mw - mw % 2).arg(mh - mh % 2));
            check("录屏幕的时长对得上",
                  qAbs(mm.durationMs - recMs) < recMs / 2 + 500 ? "yes" : "no", "yes");
        }
        QFile::remove(mpath);
    }
    delete card;
    std::printf("rectest: 失败 %d 项\n", bad);
    std::fflush(stdout);
    return bad;
}

// --recordui [秒]：验 F2 那套界面的链条（选窗口的列表有没有内容、悬浮条会不会计数、
// 点了停止之后文件能不能解回帧）。按钮是用 click() 程序化按的，不合成鼠标，不抢焦点。
extern "C" Q_DECL_EXPORT int ltoolRecorduiRun(int ms)
{
    const int secs = ms;                 // 旗标上那个数的单位本来就是**秒**
    int bad = 0;
    const auto check = [&bad](const char* name, const QString& got, const QString& want) {
        const bool ok = got == want;
        if (!ok)
            ++bad;
        std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                    got.toUtf8().constData(), want.toUtf8().constData());
    };
    const auto has = [](const QString& s, const char* needle) {
        return s.contains(QString::fromUtf8(needle)) ? QStringLiteral("yes") : QStringLiteral("no");
    };

    // 目标窗口：右上角一张每 30ms 换一个数字的小卡，不抢焦点。
    // 它建在选择窗**之前**：选择器那张列表是构造时枚举一次桌面的，卡片后建就根本不在列表里，
    // 而预览那几条判据要的正是"选中一条 → 出帧"，所以顺序不能倒过来。
    // 标题也给它起一个：visibleWindows() 不收没标题的窗口。
    QWidget* card = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint);
    card->resize(360, 240);
    card->setWindowTitle(QStringLiteral("ltool-rec-card"));
    auto* lab = new QLabel(QStringLiteral("0"), card);
    lab->setGeometry(0, 0, 360, 240);
    lab->setAlignment(Qt::AlignCenter);
    lab->setStyleSheet("background:#1d2026;color:#e8ebf2;font-size:30px;");
    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    card->move(avail.left() + 20, avail.top() + 20);
    card->setAttribute(Qt::WA_ShowWithoutActivating);
    card->show();
    int tick = 0;
    QTimer anim;
    QObject::connect(&anim, &QTimer::timeout, card, [&] { lab->setText(QString::number(++tick)); });
    anim.start(30);
    QCoreApplication::processEvents();

    WindowPicker picker;
    auto* plist = picker.findChild<QListWidget*>(QStringLiteral("list"));
    // 新加的两颗下拉框（画质 / 画面）。这两条盯的是"改了到不到配置"：
    // 选择窗每次按 F2 都重建，不落盘的话用户下次按 F2 就全回默认档。
    {
        auto* q = picker.findChild<QComboBox*>(QStringLiteral("recQuality"));
        auto* w = picker.findChild<QComboBox*>(QStringLiteral("recMaxWidth"));
        QSet<const QWidget*> laid;
        std::function<void(QLayout*)> walk = [&](QLayout* l) {
            if (!l)
                return;
            for (int i = 0; i < l->count(); ++i) {
                QLayoutItem* it = l->itemAt(i);
                if (!it)
                    continue;
                if (QWidget* cw = it->widget()) {
                    laid.insert(cw);
                    walk(cw->layout());
                } else if (QLayout* sub = it->layout())
                    walk(sub);
            }
        };
        walk(picker.layout());
        check("选择窗有画质和画面两颗下拉框",
              QStringLiteral("%1/%2").arg(q != nullptr ? 1 : 0).arg(w != nullptr ? 1 : 0),
              QStringLiteral("1/1"));
        check("两颗下拉框都在布局里（不是只 new 出来）",
              QStringLiteral("%1/%2").arg(laid.contains(q) ? 1 : 0).arg(laid.contains(w) ? 1 : 0),
              QStringLiteral("1/1"));
        check("四档画质 / 四种画面", QStringLiteral("%1/%2").arg(q->count()).arg(w->count()),
              QStringLiteral("4/4"));
        // 初始值必须来自配置（缺省=标准 / 原始）：这是"回填和读取同源"的判据
        QSettings st(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
        const int savedQ = st.value(QStringLiteral("ui/recQuality"), 1).toInt();
        const int savedW = st.value(QStringLiteral("ui/recMaxWidth"), 0).toInt();
        check("下拉框当前档 == 配置里存的档",
              QStringLiteral("%1/%2").arg(q->currentIndex()).arg(w->currentData().toInt()),
              QStringLiteral("%1/%2").arg(savedQ).arg(savedW));
        // 程序化改档（不合成鼠标）之后必须立刻落盘
        const int origQ = q->currentIndex(), origW = w->currentData().toInt();
        q->setCurrentIndex(3);
        w->setCurrentIndex(2);
        st.sync();
        QSettings st2(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
        check("改了画质/画面立刻写进配置",
              QStringLiteral("%1/%2").arg(st2.value(QStringLiteral("ui/recQuality"), -1).toInt())
                  .arg(st2.value(QStringLiteral("ui/recMaxWidth"), -1).toInt()),
              QStringLiteral("3/1280"));
        // 再建一个选择窗（模拟下一次按 F2）：必须回填成刚选的那档
        WindowPicker again;
        auto* q2 = again.findChild<QComboBox*>(QStringLiteral("recQuality"));
        auto* w2 = again.findChild<QComboBox*>(QStringLiteral("recMaxWidth"));
        check("下一次打开选择窗还是刚选那两档",
              QStringLiteral("%1/%2").arg(q2->currentIndex()).arg(w2->currentData().toInt()),
              QStringLiteral("3/1280"));
        // 收尾：把用户原来的档还回去（这条测试跑在他的真实配置上）
        q->setCurrentIndex(origQ);
        w->setCurrentIndex(0);   // 先退回不缩，再按原来的值找回去
        for (int i = 0; i < w->count(); ++i)
            if (w->itemData(i).toInt() == origW)
                w->setCurrentIndex(i);
        st2.sync();
        QSettings st3(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
        check("跑完把用户原来的档还回去了",
              QStringLiteral("%1/%2").arg(st3.value(QStringLiteral("ui/recQuality"), -1).toInt())
                  .arg(st3.value(QStringLiteral("ui/recMaxWidth"), -1).toInt()),
              QStringLiteral("%1/%2").arg(origQ).arg(origW));
    }
    // 右上角那颗「✕」：这窗没标题栏，Esc / 点到别处（失焦）/ 再按 F2 都是"看不见的退路"，
    // 界面上得有一个落点。判据走布局可达性而不是 findChild —— 后者只能证明
    // 控件被构造过，证明不了它摆在了页面上（配置页那一栏刚在这上面栽过）。
    const auto inLayoutOf = [](QWidget* root, QWidget* w) {
        bool found = false;
        std::function<void(QLayout*)> walk = [&](QLayout* l) {
            if (!l || found)
                return;
            for (int i = 0; i < l->count(); ++i) {
                QLayoutItem* it = l->itemAt(i);
                if (!it)
                    continue;
                if (QWidget* cw = it->widget()) {
                    if (cw == w) {
                        found = true;
                        return;
                    }
                    walk(cw->layout());
                } else if (QLayout* sub = it->layout())
                    walk(sub);
            }
        };
        walk(root->layout());
        return found;
    };
    auto* pclose = picker.findChild<QPushButton*>(QStringLiteral("close"));
    check("选择窗右上角有关闭按钮", pclose && pclose->text() == QStringLiteral("✕")
              && pclose->isVisibleTo(&picker) ? "yes" : "no", "yes");
    check("关闭按钮真的摆在布局里", pclose && inLayoutOf(&picker, pclose) ? "yes" : "no", "yes");
    std::printf("INFO 选择器列了 %d 个窗口\n", plist ? plist->count() : -1);
    check("选择器列出了窗口", plist && plist->count() > 2 ? "yes" : "no", "yes");
    // 整块屏幕那条必须排在最上面，并且带着"这条是屏幕不是窗口"的标志位
    // （录的时候靠它决定走 CreateForMonitor 还是 CreateForWindow）
    auto* first = plist ? plist->item(0) : nullptr;
    auto* second = plist && plist->count() > 1 ? plist->item(1) : nullptr;
    std::printf("INFO 第一条：%s\n", first ? first->text().toUtf8().constData() : "");
    check("第一条是整个屏幕", first && first->text().startsWith(QStringLiteral("整个屏幕")) ? "yes" : "no", "yes");
    check("第一条带屏幕标志", first && first->data(Qt::UserRole + 2).toBool() ? "yes" : "no", "yes");
    check("第二条是窗口（不带屏幕标志）", second && !second->data(Qt::UserRole + 2).toBool() ? "yes" : "no", "yes");

    // ---- 这一轮新加的三件事：这个窗能拖动、选中一条就出预览、开录要按「开始录制」 ----
    auto* recBtn = picker.startButton();
    check("中间有「开始录制」那颗", recBtn && recBtn->text() == QStringLiteral("开始录制")
              && recBtn->isVisibleTo(&picker) ? "yes" : "no", "yes");
    check("「开始录制」真的摆在布局里", recBtn && inLayoutOf(&picker, recBtn) ? "yes" : "no", "yes");
    auto* shot = picker.findChild<QLabel*>(QStringLiteral("preview"));
    check("右边那块预览真的摆在布局里", shot && inLayoutOf(&picker, shot) ? "yes" : "no", "yes");

    // 后面这几条要点条目、要拖窗、要拿帧，所以先把这个窗摆成"可见"：
    // WA_DontShowOnScreen 让它走完布局和显示流程而不往屏幕上贴像素（不抢他的桌面）。
    picker.setAttribute(Qt::WA_DontShowOnScreen, true);
    picker.move(120, 120);
    picker.show();
    QCoreApplication::processEvents();

    int picks = 0;
    WId pickedTarget = 0;
    QObject::connect(&picker, &WindowPicker::picked, &picker,
                     [&](WId target, const QString&, bool) { ++picks; pickedTarget = target; });
    // 点一条 = 只选中：单击、双击都不许开录（以前双击直接开录，手一抖就录了一局）
    const auto clickRow = [plist](int row, QEvent::Type pressType) {
        QWidget* vp = plist->viewport();
        const QPoint at = plist->visualItemRect(plist->item(row)).center();
        const QPointF lp(at);
        const QPointF gp(vp->mapToGlobal(at));
        QMouseEvent press(pressType, lp, gp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, lp, gp, Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(vp, &press);
        QApplication::sendEvent(vp, &release);
        QCoreApplication::processEvents();
    };
    plist->setCurrentRow(-1);
    clickRow(1, QEvent::MouseButtonPress);
    check("单击一条 = 只选中，没开录", QStringLiteral("%1/%2").arg(picks).arg(plist->currentRow()),
          QStringLiteral("0/1"));
    clickRow(1, QEvent::MouseButtonDblClick);
    check("双击同一条也不开录（要按中间那颗）", QString::number(picks), "0");
    recBtn->click();
    check("按「开始录制」才开录，录的就是当前选中那条",
          QStringLiteral("%1/%2").arg(picks)
              .arg(pickedTarget == WId(plist->item(1)->data(Qt::UserRole).toLongLong()) ? 1 : 0),
          QStringLiteral("1/1"));

    // 没标题栏就得按住窗体自己拖。事件直接送给这个窗（不往他桌面上派）
    const QPoint before = picker.pos();
    const QPoint grab(picker.width() - 40, 12);        // 标题那一行右边的空白
    const QPoint moved = grab + QPoint(60, 40);
    const auto sendAt = [&](QEvent::Type ty, Qt::MouseButtons buttons, const QPoint& where) {
        QMouseEvent e(ty, QPointF(where), picker.mapToGlobal(where), Qt::LeftButton, buttons,
                      Qt::NoModifier);
        QApplication::sendEvent(&picker, &e);
    };
    sendAt(QEvent::MouseButtonPress, Qt::LeftButton, grab);
    sendAt(QEvent::MouseMove, Qt::LeftButton, moved);
    sendAt(QEvent::MouseButtonRelease, Qt::NoButton, moved);
    QCoreApplication::processEvents();
    check("按住标题那一行能把这个窗拖走", picker.pos() == before + QPoint(60, 40) ? "yes" : "no",
          "yes");

    // 预览：拿的是 WGC 同一一路，所以要有真窗口在重绘 —— 就是上面那张每 30ms 换数字的卡。
    auto* filterEdit = picker.findChild<QLineEdit*>(QStringLiteral("filter"));
    filterEdit->setText(QStringLiteral("ltool-rec-card"));
    int cardRow = -1;
    for (int i = 0; i < plist->count(); ++i)
        if (!plist->item(i)->isHidden()) { cardRow = i; break; }
    check("列表里找得到那张自造的卡", cardRow >= 0 ? "yes" : "no", "yes");
    plist->setCurrentRow(cardRow);
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 2500 && picker.preview()->framesSeen() == 0)
        QCoreApplication::processEvents();
    std::printf("INFO 预览：源=%dx%d 摆出来=%dx%d 帧数=%d 那句实话=%s\n",
                picker.preview()->sourceSize().width(), picker.preview()->sourceSize().height(),
                picker.preview()->shownSize().width(), picker.preview()->shownSize().height(),
                picker.preview()->framesSeen(),
                picker.preview()->lastError().toUtf8().constData());
    check("选中一条就开这一路预览，并且真拿到过帧",
          picker.preview()->framesSeen() > 0 ? "yes" : "no", "yes");
    auto* capBox = picker.findChild<QComboBox*>(QStringLiteral("recMaxWidth"));
    const int origCap = capBox->currentData().toInt();
    const QSize src = picker.preview()->sourceSize();
    check("预览摆出来的就是按当前档算出来的编码画幅",
          picker.preview()->shownSize() == Recorder::encodedSize(src.width(), src.height(), origCap)
              ? "yes" : "no", "yes");
    // 换档要跟着换：那张卡只有 360 宽，任何档都不缩，所以这条拿整块屏幕来量
    filterEdit->clear();
    plist->setCurrentRow(0);
    wait.restart();
    while (wait.elapsed() < 2500 && picker.preview()->framesSeen() == 0)
        QCoreApplication::processEvents();
    const QSize monSrc = picker.preview()->sourceSize();
    for (int i = 0; i < capBox->count(); ++i)
        if (capBox->itemData(i).toInt() == 1280)
            capBox->setCurrentIndex(i);
    wait.restart();
    const int seenBefore = picker.preview()->framesSeen();
    while (wait.elapsed() < 2500 && picker.preview()->framesSeen() == seenBefore)
        QCoreApplication::processEvents();
    check("换「画面」档，预览跟着换成编码后的画幅（长边 ≤1280）",
          QStringLiteral("%1/%2").arg(picker.preview()->shownSize().width() <= 1280 ? 1 : 0)
              .arg(picker.preview()->shownSize()
                       == Recorder::encodedSize(monSrc.width(), monSrc.height(), 1280)
                   ? 1 : 0),
          QStringLiteral("1/1"));
    for (int i = 0; i < capBox->count(); ++i)
        if (capBox->itemData(i).toInt() == origCap)
            capBox->setCurrentIndex(i);
    picker.hide();
    check("窗一收起来就把取帧那一路放掉（不在背后空转）",
          picker.preview()->active() ? "yes" : "no", "no");

    const QString path = QDir::tempPath() + "/ltool-recordui.mp4";
    QFile::remove(path);
    Recorder rec;
    Recorder::Options o;
    o.fps = 15;
    QString err;
    const bool started = rec.start(card->winId(), o, path, &err);
    std::printf("INFO start=%d err=%s\n", started ? 1 : 0, err.toUtf8().constData());
    if (started) {
        RecordBar bar(&rec, path);
        bar.show();
        auto* text = bar.findChild<QLabel*>(QStringLiteral("text"));
        auto* stop = bar.findChild<QPushButton*>(QStringLiteral("stop"));
        QElapsedTimer wall;
        wall.start();
        while (wall.elapsed() < secs * 1000LL) {
            QCoreApplication::processEvents();
            QThread::msleep(20);
        }
        const QString midText = text ? text->text() : QString();
        std::printf("INFO 条上写的：%s\n", midText.toUtf8().constData());
        check("悬浮条在计数（有秒数和帧数）",
              midText.contains(QStringLiteral(":")) && midText.contains(QStringLiteral("帧")) ? "yes" : "no",
              "yes");
        stop->click();                       // 程序化按"停止"
        QCoreApplication::processEvents();
        const QString endText = text ? text->text() : QString();
        std::printf("INFO 停止后写的：%s\n", endText.toUtf8().constData());
        check("停止后报的是保存成功", has(endText, "已保存"), "yes");
        check("按钮变成打开所在目录", stop->text(), QStringLiteral("打开所在目录"));
        const Recorder::Mp4Info mp4 = Recorder::inspectMp4(path);
        std::printf("INFO 读回: %lld 帧 / %lldms / %lld 字节\n", long long(mp4.frames),
                    long long(mp4.durationMs), long long(mp4.bytes));
        check("文件能解出帧", mp4.frames > 0 ? "yes" : "no", "yes");
        bar.close();

        // ---- 「不要了」那一局：停下并且把文件删掉，别在录像目录里留废 MP4 ----
        const QString pathX = QDir::tempPath() + "/ltool-recordui-x.mp4";
        QFile::remove(pathX);
        QString errX;
        if (rec.start(card->winId(), o, pathX, &errX)) {
            RecordBar barX(&rec, pathX);
            barX.show();
            auto* textX = barX.findChild<QLabel*>(QStringLiteral("text"));
            auto* cancel = barX.findChild<QPushButton*>(QStringLiteral("cancel"));
            check("悬浮条上有「不要了」", cancel ? "yes" : "no", "yes");
            for (int i = 0; i < 30 && cancel; ++i) {
                QCoreApplication::processEvents();
                QThread::msleep(20);
            }
            cancel->click();
            QCoreApplication::processEvents();
            std::printf("INFO 取消后写的：%s\n", textX ? textX->text().toUtf8().constData() : "");
            check("取消之后条上说实话（已取消，没保存）",
                  textX && textX->text().contains(QStringLiteral("已取消")) ? "yes" : "no", "yes");
            check("取消之后文件真的不在了", QFile::exists(pathX) ? "还在" : "已删", "已删");
            barX.close();
        } else {
            std::printf("FAIL 取消那一局起不来：%s\n", errX.toUtf8().constData());
            ++bad;
        }

        // ---- 选择窗的退路：Esc、点到别处（失焦）、再按一次 F2 ----
        picker.show();
        QCoreApplication::processEvents();
        const bool stayed = picker.isVisible();
        std::printf("INFO 选择窗 show 之后还在=%d（不在=失焦自动收起也生效）\n", stayed ? 1 : 0);
        if (stayed) {
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(plist, &esc);     // 从列表里按 Esc（焦点在子控件上）
            QCoreApplication::processEvents();
            check("Esc 收得起选择窗", picker.isVisible() ? "还开着" : "已收起", "已收起");
        }
        // 失焦收起：直接给它派一个 WindowDeactivate（不合成鼠标，不动他的桌面）
        picker.show();
        QCoreApplication::processEvents();
        QEvent de(QEvent::WindowDeactivate);
        QApplication::sendEvent(&picker, &de);
        QCoreApplication::processEvents();
        check("点到别处（失焦）会自动收起", picker.isVisible() ? "还开着" : "已收起", "已收起");
        // 那颗「✕」按下去要真的收得起（click() 是程序化的按，不合成鼠标、不抢焦点）
        picker.show();
        QCoreApplication::processEvents();
        if (pclose) {
            pclose->click();
            QCoreApplication::processEvents();
            check("点「✕」收得起选择窗", picker.isVisible() ? "还开着" : "已收起", "已收起");
        }
        picker.hide();
        // F2 那一档走的是全局那个 picker，不是上面这个局部对象，所以按顶层窗口找它
        const auto openPicker = [] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* p = qobject_cast<WindowPicker*>(w))
                    if (p->isVisible())
                        return p;
            return static_cast<WindowPicker*>(nullptr);
        };
        RecordUi::startViaPicker();
        QCoreApplication::processEvents();
        const bool opened = openPicker() != nullptr;
        check("F2 能把选择窗开起来", opened ? "yes" : "no", "yes");
        if (opened) {
            RecordUi::startViaPicker();               // 开着的时候再按一次 F2 = 取消
            QCoreApplication::processEvents();
            check("再按一次 F2 收得起选择窗", openPicker() ? "还开着" : "已收起", "已收起");
        }

        // ---- 第二局：中途改大小；第三局：紧接着再开一次 ----
        // 已实测：MP4 的画幅改不了（1280x801 这类奇数尺寸会被 H.264 拒：0xc00d36b4），
        //   所以目标窗口改了大小这一局必须收尾；"改大小之后 WGC 到底还在不在出帧"没定论，
        //   下面每 60ms 打一次帧数，跑一次就有数据。
        //   第三局才是这次用户报的 bug 的正身："录失败过一次，F2 就再也按不动了" ——
        //   自己收尾的那一局会把 m_running 卡在 true，下一局必须照样能起来。
        const QString logPath = Utils::dataDir() + "/log.txt";
        qint64 logMark = QFileInfo(logPath).size();
        const QString path2 = QDir::tempPath() + "/ltool-recordui2.mp4";
        QFile::remove(path2);
        QString err2;
        if (rec.start(card->winId(), o, path2, &err2)) {
            RecordBar bar2(&rec, path2);
            bar2.show();
            QCoreApplication::processEvents();
            // 这里必须泵事件：目标窗口的数字是靠 QTimer 换的，不泵事件它就不重画，
            // WGC 自然一帧都不给 —— 上一版拿 msleep(900) 测出"只有 1 帧"，是测试自己的坑，不是产品。
            for (int i = 0; i < 45; ++i) {
                QCoreApplication::processEvents();
                QThread::msleep(20);
            }
            std::printf("INFO 改大小前 %d 帧\n", rec.framesCaptured());
            check("改大小之前已经录到的帧保住了（第二局前半段）",
                  rec.framesCaptured() > 5 ? "yes" : "no", "yes");
            card->resize(420, 300);            // 改大小：看门狗应当收尾
            std::printf("INFO 改大小那一瞬 %d 帧\n", rec.framesCaptured());
            for (int i = 0; i < 20; ++i) {     // 每 60ms 看一次帧数还涨不涨（涨=还在出帧）
                QCoreApplication::processEvents();
                QThread::msleep(60);
                std::printf("INFO +%dms 帧=%d done=%d err=%s\n", (i + 1) * 60,
                            rec.framesCaptured(), rec.isDone() ? 1 : 0,
                            rec.errorText().toUtf8().constData());
                if (rec.isDone())
                    break;
            }
            // 收尾是 worker 线程写的，条上的话是 200ms 定时器写的 —— 必须真的把事件泵起来
            for (int i = 0; i < 60 && rec.running(); ++i) {
                QCoreApplication::processEvents();
                QThread::msleep(20);
            }
            for (int i = 0; i < 25; ++i) {
                QCoreApplication::processEvents();
                QThread::msleep(20);
            }
            check("改大小会收尾并说清楚", rec.errorText().contains(QStringLiteral("改了大小")) ? "yes" : "no",
                  "yes");
            check("收尾后 running() 变回 false（F2 能再用）", rec.running() ? "yes" : "no", "no");
            std::printf("INFO 收尾时 %d 帧，err=%s\n", rec.framesCaptured(),
                        rec.errorText().toUtf8().constData());
            // 条上写的是"已录下的都在文件里"——那就真的把文件解一遍，别让它撒谎
            const Recorder::Mp4Info mp42 = Recorder::inspectMp4(path2);
            std::printf("INFO 第二局读回: %lld 帧 / %lldms / %lld 字节\n", long long(mp42.frames),
                        long long(mp42.durationMs), long long(mp42.bytes));
            check("半路收尾的文件能解出帧（没录成的那半截没毁掉整段）",
                  mp42.frames > 0 && mp42.bytes > 1024 ? "yes" : "no", "yes");
            auto* t2 = bar2.findChild<QLabel*>(QStringLiteral("text"));
            check("条上说了没录成", t2 && t2->text().contains(QStringLiteral("没录成")) ? "yes" : "no",
                  "yes");
            QFile lg(logPath);
            if (lg.open(QIODevice::ReadOnly | QIODevice::Append)) {
                lg.seek(logMark);
                // 日志里是 "record failed: <原因> path=<文件>"，路径在标记后面，
                // 所以必须按行匹配，不能拿 indexOf 之后的 20 个字符去比前缀
                const QStringList lines = QString::fromUtf8(lg.readAll()).split('\n');
                int seen = 0;
                for (const QString& ln : lines) {
                    if (ln.contains(QStringLiteral("record failed"))
                        && ln.contains(QStringLiteral("ltool-recordui2.mp4")))
                        ++seen;
                }
                std::printf("INFO 这一局自己收尾写的日志行数：%d\n", seen);
                check("一次失败只写一条日志", seen == 1 ? "yes" : "no", "yes");
                lg.close();
            }
            bar2.close();

            // 第三局：上一局是工作线程自己收尾的 —— 这正是"F2 失灵"那条路
            const QString path3 = QDir::tempPath() + "/ltool-recordui3.mp4";
            QFile::remove(path3);
            QString err3;
            const bool third = rec.start(card->winId(), o, path3, &err3);
            std::printf("INFO 第三局 start=%d err=%s\n", third ? 1 : 0,
                        err3.toUtf8().constData());
            check("自己收尾之后还能再开一局（F2 活过来了）", third ? "yes" : "no", "yes");
            if (third) {
                for (int i = 0; i < 25; ++i) {
                    QCoreApplication::processEvents();
                    QThread::msleep(20);
                }
                rec.stop();
                check("第三局也真的在录", rec.framesCaptured() > 2 ? "yes" : "no", "yes");
                std::printf("INFO 第三局 %d 帧 / %lld 字节\n", rec.framesCaptured(),
                            long long(QFileInfo(path3).size()));
            }
            QFile::remove(path3);
        } else {
            check("第二局能起来", err2, QString());
        }
    } else {
        ++bad;
    }
    anim.stop();
    delete card;
    std::printf("recordui: 失败 %d 项\n", bad);
    std::fflush(stdout);
    return bad;
}

// --memloop [轮数] [宽x高]：内存这事只能量，不能猜。每轮做一遍"解压一个压不动的 32MB 包"
// + "录一段指定尺寸的窗口"，每一步采 私有字节/工作集/句柄数。
// 私有字节随轮数线性涨 = 真泄漏；涨一两轮就封顶 = 分配器留着页不还操作系统（那不是泄漏）。
// 夹具和产物都在 %TEMP%\ltool-memloop 里，跑完删掉。
extern "C" Q_DECL_EXPORT int ltoolMemloopRun(int rounds, const char* sizeSpec)
{
    const QSize dims = cardSize(sizeSpec, QSize(1280, 800));
    const int cw = dims.width(), ch = dims.height();
    struct Snap {
        quint64 priv = 0;
        quint64 ws = 0;
        quint32 handles = 0;
    };
    const auto snap = [] {
        PROCESS_MEMORY_COUNTERS_EX pmc {};
        pmc.cb = sizeof(pmc);
        K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
        DWORD h = 0;
        GetProcessHandleCount(GetCurrentProcess(), &h);
        return Snap { quint64(pmc.PrivateUsage), quint64(pmc.WorkingSetSize), h };
    };
    const auto mb = [](quint64 bytes) { return double(bytes) / 1048576.0; };
    const auto delta = [mb](quint64 now, quint64 before) { return mb(now) - mb(before); };

    const QString root = QDir::tempPath() + "/ltool-memloop";
    QDir(root).removeRecursively();
    QDir().mkpath(root + "/src");
    {   // 32MB 伪随机字节：随机数压不动，这样"解压出来的量"和包大小对得上
        quint64 s = 88172645463325252ULL;
        QByteArray chunk(1 << 20, '\0');
        for (int f = 0; f < 2; ++f) {
            QFile o(root + QString("/src/big%1.bin").arg(f));
            if (!o.open(QIODevice::WriteOnly | QIODevice::Truncate))
                continue;
            for (int i = 0; i < 16; ++i) {
                for (int b = 0; b < chunk.size(); ++b) {
                    s ^= s << 13;
                    s ^= s >> 7;
                    s ^= s << 17;
                    chunk[b] = char(int(s & 0xff));
                }
                o.write(chunk);
            }
        }
    }
    const QString zipPath = root + "/pack.zip";
    const Archive::Result mk = Archive::compress({ root + "/src" }, zipPath, false, nullptr,
                                                 nullptr);
    std::printf("INFO 夹具：make ok=%d err=%s 包=%.1fMB\n", mk.ok ? 1 : 0,
                mk.error.toUtf8().constData(), mb(QFileInfo(zipPath).size()));

    QWidget* card = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint);
    card->resize(cw, ch);
    auto* lab = new QLabel(QStringLiteral("0"), card);
    lab->setGeometry(cw - 96, ch - 46, 94, 44);
    lab->setStyleSheet(QStringLiteral("background:#000000;color:#e8ebf2;font-size:26px;"));
    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    card->move(avail.right() - cw - 12, avail.bottom() - ch - 12);
    card->setAttribute(Qt::WA_ShowWithoutActivating);
    card->show();
    int tick = 0;
    QTimer anim;
    QObject::connect(&anim, &QTimer::timeout, card, [&] { lab->setText(QString::number(++tick)); });
    anim.start(30);
    QCoreApplication::processEvents();

    Recorder rec;                 // 和产品一样：一个 Recorder 从头用到尾
    Recorder::Options o;
    o.fps = 15;
    const Snap base = snap();
    std::printf("INFO 起始 私有=%.1fMB 工作集=%.1fMB 句柄=%u（卡片 %dx%d，一帧 RGBA=%.1fMB）\n",
                mb(base.priv), mb(base.ws), base.handles, cw, ch,
                mb(quint64(cw - cw % 2) * (ch - ch % 2) * 4));
    for (int i = 1; i <= rounds; ++i) {
        const Snap a = snap();
        const QString outDir = root + QString("/x%1").arg(i);
        const Archive::Result ex = Archive::extract(zipPath, outDir, nullptr, nullptr);
        const Snap b = snap();
        std::printf("轮 %d  解压完：私有 %+0.1fMB 句柄 %+d（解出 %lldMB）\n", i,
                    delta(b.priv, a.priv),
                    int(b.handles) - int(a.handles),
                    long long(ex.bytes / 1048576));
        const QString vid = root + QString("/r%1.mp4").arg(i);
        QFile::remove(vid);
        QString err;
        const bool started = rec.start(card->winId(), o, vid, &err);
        int frames = 0;
        if (started) {
            QElapsedTimer w;
            w.start();
            while (w.elapsed() < 2000) {
                QCoreApplication::processEvents();
                QThread::msleep(20);
            }
            rec.stop();
            frames = rec.framesCaptured();
        }
        for (int k = 0; k < 15; ++k) {      // 让收尾的释放都跑到
            QCoreApplication::processEvents();
            QThread::msleep(20);
        }
        const Snap c = snap();
        std::printf("      录 %d 帧：私有 %+0.1fMB 句柄 %+lld | 收尾后私有=%0.1f 工作集=%0.1f 句柄=%u err=%s\n",
                    frames, delta(c.priv, b.priv),
                    long long(c.handles) - long long(b.handles), mb(c.priv), mb(c.ws), c.handles,
                    err.toUtf8().constData());
        QDir(outDir).removeRecursively();
        QFile::remove(vid);
    }
    const Snap z = snap();
    std::printf("INFO 结束 私有=%.1fMB（%d 轮净涨 %+0.1fMB，每轮 %+0.2fMB）工作集=%.1fMB 句柄=%u\n",
                mb(z.priv), rounds, delta(z.priv, base.priv),
                (mb(z.priv) - mb(base.priv)) / rounds, mb(z.ws), z.handles);
    anim.stop();
    delete card;
    QDir(root).removeRecursively();
    std::fflush(stdout);
    return 0;
}
