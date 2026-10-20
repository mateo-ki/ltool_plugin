#include "RecordUi.h"
#include "platform/Launch.h"

#include "Recorder.h"
#include "Utils.h"
#include "WindowPreview.h"

#include <windows.h>
#include <dwmapi.h>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QComboBox>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

namespace {

struct WinRow {
    WId hwnd = 0;
    QString title;
    QString exe;
    int w = 0;
    int h = 0;
};

// 桌面上"看得见、有标题、够大"的顶层窗口。被虚拟桌面收起（cloaked）的不算，
// 因为那种窗口 WGC 也不会给帧。
QVector<WinRow> visibleWindows(HWND skip1, HWND skip2)
{
    QVector<WinRow> out;
    struct Ctx { QVector<WinRow>* v; HWND s1; HWND s2; } ctx { &out, skip1, skip2 };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(h))
            return TRUE;
        DWORD cloaked = 0;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked || h == c->s1 || h == c->s2)
            return TRUE;
        RECT rc {};
        if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc))))
            GetWindowRect(h, &rc);
        const int w = rc.right - rc.left, hh = rc.bottom - rc.top;
        if (w < 80 || hh < 60)
            return TRUE;
        WCHAR title[512] = {};
        GetWindowTextW(h, title, 512);
        if (!title[0])
            return TRUE;
        WinRow r;
        r.hwnd = reinterpret_cast<WId>(h);
        r.title = QString::fromWCharArray(title);
        r.w = w;
        r.h = hh;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            WCHAR exe[512] = {};
            DWORD n = 512;
            if (QueryFullProcessImageNameW(p, 0, exe, &n))
                r.exe = QFileInfo(QString::fromWCharArray(exe, int(n))).fileName();
            CloseHandle(p);
        }
        c->v->push_back(r);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

Recorder* g_rec = nullptr;
RecordBar* g_bar = nullptr;
WindowPicker* g_picker = nullptr;

// 主显示器那块屏的 HMONITOR + 物理像素尺寸（选择器顶上那条"整个屏幕"就用它）。
// 主屏一定包含虚拟桌面原点，所以不用比屏幕名字（Qt 的名字和 GetMonitorInfo 的没有可靠对应）。
struct ScreenRow { WId mon = 0; int w = 0; int h = 0; };
ScreenRow primaryMonitor()
{
    ScreenRow r;
    const POINT origin { 0, 0 };
    if (HMONITOR m = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY)) {
        MONITORINFO mi {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(m, &mi)) {
            r.mon = reinterpret_cast<WId>(m);
            r.w = mi.rcMonitor.right - mi.rcMonitor.left;
            r.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        }
    }
    return r;
}

struct QualityPreset {
    const char* name;
    double bpp;          // bits per pixel per frame
};
// 顺序就是下拉框里的顺序（存在 config.ini 的 ui/recQuality 里是下标）
const QualityPreset kQualities[] = {
    { "流畅", 0.060 },
    { "标准", 0.145 },     // 就是以前那个固定 8000kbps 在 2560x1440@15 上的等效值
    { "高清", 0.250 },
    { "超清", 0.400 },
};
const int kQualityCount = int(sizeof(kQualities) / sizeof(kQualities[0]));
// 长边上限档位（存在 ui/recMaxWidth 里的是这个数，0 = 不缩）
const int kWidthCaps[] = { 0, 1920, 1280, 854 };
// 那一档显示成什么字：下拉框里那四行和插件状态页上那句「画面档」都由它生成 ——
// 档位表只有一份，字面量抄两遍迟早对不上。
QString widthCapText(int cap)
{
    return cap > 0 ? QStringLiteral("长边 ≤ %1").arg(cap) : QStringLiteral("原始（不缩）");
}

int recQualityIndex()
{
    const int i = QSettings(Utils::dataDir() + "/config.ini", QSettings::IniFormat)
                      .value(QStringLiteral("ui/recQuality"), 1).toInt();
    return qBound(0, i, kQualityCount - 1);
}
int recMaxWidth()
{
    const int w = QSettings(Utils::dataDir() + "/config.ini", QSettings::IniFormat)
                      .value(QStringLiteral("ui/recMaxWidth"), 0).toInt();
    for (int c : kWidthCaps)
        if (c == w)
            return w;
    return 0;        // 配置里被手改成不认识的数：按"不缩"处理，别拿它去算画幅
}
// 码率按**编码后**的画幅算：缩到 1280 就按 1280 的量给，不然等于白花一倍的钱
int recBitrateKbps(int quality, int w, int h, int fps)
{
    // 算法本体在 Recorder::bitrateKbps（测试 --recq 也调它），这里只挑档
    return Recorder::bitrateKbps(kQualities[qBound(0, quality, kQualityCount - 1)].bpp, w, h, fps);
}

void beginRecording(WId target, const QString& title, bool monitor)
{
    QDir().mkpath(Utils::recordingsDir());
    const QString path = QString("%1/rec-%2.mp4")
                             .arg(Utils::recordingsDir(),
                                  QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
    if (!g_rec)
        g_rec = new Recorder;
    QSettings st(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
    Recorder::Options o;
    o.fps = qBound(5, st.value(QStringLiteral("ui/recFps"), 15).toInt(), 60);
    const int q = recQualityIndex();
    o.maxWidth = recMaxWidth();
    // 码率要看编码后的画幅，所以先把这一局的画幅量出来（屏幕/窗口的原始尺寸都在这儿算）
    int capW = 0, capH = 0;
    if (monitor) {
        MONITORINFO mi {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(reinterpret_cast<HMONITOR>(target), &mi)) {
            capW = mi.rcMonitor.right - mi.rcMonitor.left;
            capH = mi.rcMonitor.bottom - mi.rcMonitor.top;
        }
    } else {
        RECT rc {};
        if (GetWindowRect(reinterpret_cast<HWND>(target), &rc)) {
            capW = rc.right - rc.left;
            capH = rc.bottom - rc.top;
        }
    }
    const QSize enc = Recorder::encodedSize(capW, capH, o.maxWidth);
    o.videoBitrateKbps = recBitrateKbps(q, enc.width(), enc.height(), o.fps);
    QString err;
    if (!g_rec->start(target, o, path, &err, monitor)) {
        Utils::log(QString("record start failed: %1").arg(err));
        QMessageBox::warning(nullptr, QStringLiteral("录不了"),
                             err.isEmpty() ? QStringLiteral("不知道哪里不对，反正没录起来") : err);
        return;
    }
    if (g_bar) {
        g_bar->close();
        g_bar = nullptr;
    }
    g_bar = new RecordBar(g_rec, path);
    g_bar->show();
    int rw = 0, rh = 0;
    if (monitor) {
        MONITORINFO mi {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(reinterpret_cast<HMONITOR>(target), &mi)) {
            rw = mi.rcMonitor.right - mi.rcMonitor.left;
            rh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        }
    } else {
        RECT rc {};
        if (GetWindowRect(reinterpret_cast<HWND>(target), &rc)) {
            rw = rc.right - rc.left;
            rh = rc.bottom - rc.top;
        }
    }
    Utils::log(QString("record start: %1 title=%2 kind=%3 fps=%4 画质=%5 kbps=%6 采集=%7x%8 编码=%9x%10")
                   .arg(path, title, monitor ? QStringLiteral("屏幕") : QStringLiteral("窗口"))
                   .arg(o.fps).arg(QString::fromLatin1(kQualities[q].name))
                   .arg(o.videoBitrateKbps).arg(rw).arg(rh).arg(enc.width()).arg(enc.height()));
}

} // namespace

// ---------------- 窗口选择 ----------------

WindowPicker::WindowPicker(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(6);
    auto* head = new QHBoxLayout;
    head->setSpacing(6);
    auto* title = new QLabel(QStringLiteral("录什么？（整块屏幕 / 某一个窗口）"), this);
    title->setStyleSheet("color:#e8ebf2;font-size:13px;font-weight:600;");
    // 这窗没标题栏，"按住哪儿能挪"得写在人眼看得见的地方
    title->setToolTip(QStringLiteral("按住这一行（或标题旁边的空白处）可以拖动这个窗"));
    // 这窗没标题栏，光靠 Esc / 点到别处 / 再按一次 F2 还是得有个人眼的落点
    m_close = new QPushButton(QStringLiteral("✕"), this);
    m_close->setObjectName(QStringLiteral("close"));
    m_close->setFixedSize(24, 22);
    m_close->setCursor(Qt::PointingHandCursor);
    m_close->setToolTip(QStringLiteral("不录了（同 Esc）"));
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(m_close);
    auto* filter = new QLineEdit(this);
    filter->setPlaceholderText(QStringLiteral("输入关键字过滤（输「屏」就剩整块屏幕）；"
                                              "点一条只是选中并出预览，按「开始录制」或回车才开录，Esc 取消"));
    auto* list = new QListWidget(this);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    // 画质 / 画面大小：录之前顺手能改的两件事，改一下就记住（config.ini），下次按 F2 还是这个。
    // 挂在列表下面一行，上面各行的高度都不动 —— 这个窗每次 F2 都重建，排布必须稳定。
    auto* opts = new QHBoxLayout;
    opts->setSpacing(6);
    auto* qCap = new QLabel(QStringLiteral("画质"), this);
    auto* wCap = new QLabel(QStringLiteral("画面"), this);
    auto* quality = new QComboBox(this);
    quality->setObjectName(QStringLiteral("recQuality"));
    for (const QualityPreset& q : kQualities)
        quality->addItem(QString::fromUtf8(q.name));
    quality->setToolTip(QStringLiteral("标准这一档就是以前默认的那个大小；码率按画幅和帧率算，缩小的画面不会白花空间。"));
    quality->setCurrentIndex(recQualityIndex());
    auto* width = new QComboBox(this);
    width->setObjectName(QStringLiteral("recMaxWidth"));
    for (int cap : kWidthCaps)
        width->addItem(widthCapText(cap), cap);
    width->setToolTip(QStringLiteral("缩的是编码画幅（面积平均，不是隔行取样）。选小一点文件能小好几倍，代价是字不那么锐。"));
    {
        const int want = recMaxWidth();
        for (int i = 0; i < width->count(); ++i)
            if (width->itemData(i).toInt() == want)
                width->setCurrentIndex(i);
    }
    opts->addWidget(qCap);
    opts->addWidget(quality);
    opts->addSpacing(8);
    opts->addWidget(wCap);
    opts->addWidget(width);
    opts->addStretch(1);

    // 「开始录制」：点条目只选中，开录要按这颗（或回车）。摆在最后一行的中间，
    // 这一行其余的控件（画质/画面）都在左边，按钮在中间，右边留白把它顶出来。
    m_rec = new QPushButton(QStringLiteral("开始录制"), this);
    m_rec->setObjectName(QStringLiteral("recStart"));
    m_rec->setCursor(Qt::PointingHandCursor);
    m_rec->setMinimumWidth(120);
    m_rec->setToolTip(QStringLiteral("录当前选中那一条（同回车）"));
    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(6);
    bottom->addLayout(opts);
    bottom->addStretch(1);
    bottom->addWidget(m_rec);
    bottom->addStretch(1);
    // 右边垫一块和左边那组一样宽的空白：这样按钮落在**整个窗的中线上**，
    // 而不是"剩下那点空间的中间"（那样它永远偏右）。
    auto* pad = new QWidget;
    pad->setFixedWidth(opts->sizeHint().width());
    bottom->addWidget(pad);

    // 右边那一块实时预览：拿的就是 Recorder 同一一路（WGC），并且按当前「画面」档
    // 缩到编码尺寸 —— 这块小图就是将要进 mp4 的那一张，不是另画一份示意图。
    m_pv = new WindowPreview(this);
    m_shot = new QLabel(QStringLiteral("没选东西"), this);
    m_shot->setObjectName(QStringLiteral("preview"));
    m_shot->setAlignment(Qt::AlignCenter);
    m_shot->setMinimumSize(260, 196);
    m_shot->setScaledContents(false);
    m_cap = new QLabel(QStringLiteral("选中一条就能看到录进去的画面"), this);
    m_cap->setObjectName(QStringLiteral("previewCap"));
    m_cap->setWordWrap(true);
    auto* right = new QVBoxLayout;
    right->setSpacing(4);
    right->addWidget(m_shot, 1);
    right->addWidget(m_cap);
    auto* mid = new QHBoxLayout;
    mid->setSpacing(8);
    mid->addWidget(list, 5);
    mid->addLayout(right, 4);

    lay->addLayout(head);
    lay->addWidget(filter);
    lay->addLayout(mid, 1);
    lay->addLayout(bottom);
    list->setObjectName(QStringLiteral("list"));
    filter->setObjectName(QStringLiteral("filter"));
    setStyleSheet("QWidget{background:#26272c;border:1px solid #3a3c45;}"
                  "QLineEdit,QListWidget{background:#1b1c20;color:#e8ebf2;border:1px solid #3a3c45;"
                  "border-radius:5px;padding:4px 6px;}"
                  "QListWidget::item{padding:5px 6px;border-radius:4px;}"
                  "QListWidget::item:selected{background:#3f6ae0;}"
                  "QPushButton{background:#3a3d46;color:#e8ebf2;border:none;border-radius:4px;}"
                  "QPushButton:hover{background:#e04a4a;}"
                  "QLabel{border:none;}"
                  "QComboBox{background:#1b1c20;color:#e8ebf2;border:1px solid #3a3c45;"
                  "border-radius:5px;padding:3px 6px;}"
                  "QComboBox QAbstractItemView{background:#1b1c20;color:#e8ebf2;"
                  "border:1px solid #3a3c45;selection-background-color:#3f6ae0;}"
                  "QLabel#preview{background:#101114;color:#8b93a1;border:1px solid #3a3c45;"
                  "border-radius:6px;}"
                  "QLabel#previewCap{background:transparent;border:none;color:#8b93a1;font-size:8.5pt;}"
                  "QPushButton#recStart{background:#3f6ae0;color:#ffffff;font-weight:600;"
                  "border:none;border-radius:5px;padding:6px 18px;}"
                  "QPushButton#recStart:hover{background:#5a82f0;}"
                  "QPushButton#recStart:disabled{background:#3a3d46;color:#8b93a1;}");
    connect(m_close, &QPushButton::clicked, this, &QWidget::hide);
    const auto saveOpts = [quality, width] {
        QSettings st(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
        st.setValue(QStringLiteral("ui/recQuality"), quality->currentIndex());
        st.setValue(QStringLiteral("ui/recMaxWidth"), width->currentData().toInt());
        st.sync();
        Utils::log(QStringLiteral("record opts: 画质=%1 画面=%2")
                       .arg(quality->currentText(), width->currentText()));
    };
    // 用 currentIndexChanged 而不是 activated：activated 只有真人点才算，自测没法
    // 程序化地"按一下下拉框"。回填那两次 setCurrentIndex 都发生在这两条 connect 之前
    // （quality 在创建时就按配置摆好，width 的匹配循环也在加进布局之前），
    // 所以初始化不会触发保存 —— 以后谁把回填挪到 connect 后面，就会每次按 F2 多写一遍配置。
    connect(quality, &QComboBox::currentIndexChanged, quality, [saveOpts](int) { saveOpts(); });
    // 「画面」那一档换掉时只改预览的缩放目标：编码尺寸是每帧现算的，不必重开取帧那一路
    connect(width, &QComboBox::currentIndexChanged, this, [this, saveOpts](int) {
        saveOpts();
        m_pv->setMaxWidth(recMaxWidth());
        refreshPreview();     // 那行字要跟着换（"编码后 1280x720"），图下一帧自己会来
    });
    resize(760, 430);

    const auto rebuild = [this, list, filter] {
        list->clear();
        // 整块屏幕排在最上面一条：录屏的人十次有九次要的是"就录我看到的这一整块"。
        // UserRole+2 是"这条是屏幕不是窗口"的标志，没设的（窗口那些行）toBool() 就是 false。
        const ScreenRow scr = primaryMonitor();
        if (scr.mon) {
            auto* it = new QListWidgetItem(QStringLiteral("整个屏幕 · 主显示器 · %1x%2")
                                               .arg(scr.w).arg(scr.h), list);
            it->setData(Qt::UserRole, qlonglong(scr.mon));
            it->setData(Qt::UserRole + 1, QStringLiteral("整个屏幕"));
            it->setData(Qt::UserRole + 2, true);
        }
        const HWND skip = reinterpret_cast<HWND>(winId());
        for (const WinRow& r : visibleWindows(skip, g_bar ? reinterpret_cast<HWND>(g_bar->winId()) : nullptr)) {
            auto* it = new QListWidgetItem(QStringLiteral("%1  ·  %2  ·  %3x%4")
                                               .arg(r.title.left(46), r.exe).arg(r.w).arg(r.h), list);
            it->setData(Qt::UserRole, qlonglong(r.hwnd));
            it->setData(Qt::UserRole + 1, r.title);
        }
        if (list->count())
            list->setCurrentRow(0);
    };
    rebuild();
    m_list = list;

    connect(filter, &QLineEdit::textChanged, this, [list, filter] {
        const QString needle = filter->text().trimmed();
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setHidden(!needle.isEmpty() && !list->item(i)->text().contains(needle, Qt::CaseInsensitive));
    });
    // 点条目**只选中**（顺带换预览），开录要按「开始录制」那颗或敲回车。
    // 以前是 itemActivated（双击/回车）直接开录 —— 手一抖点两下就开始录了，而且没有任何
    // 一步能反悔，也没有一处能看见"录进去的到底是这么大的一张图"。
    connect(list, &QListWidget::currentRowChanged, this, [this](int) { refreshPreview(); });
    connect(m_rec, &QPushButton::clicked, this, [this] { chooseCurrent(); });
    connect(filter, &QLineEdit::returnPressed, this, [this] {
        // 回车先从可见项里挑第一个（"输入 Qoder 回车"就是录 Qoder）
        for (int i = 0; i < m_list->count(); ++i) {
            if (!m_list->item(i)->isHidden()) {
                m_list->setCurrentRow(i);
                break;
            }
        }
        chooseCurrent();
    });
    connect(m_pv, &WindowPreview::frameReady, this, [this] {
        const QPixmap pm = m_pv->frame();
        if (!pm.isNull()) {
            m_shot->setPixmap(pm.scaled(m_shot->size() - QSize(10, 10), Qt::KeepAspectRatio,
                                        Qt::SmoothTransformation));
        }
        const QString cap = m_pv->lastError().isEmpty()
            ? QStringLiteral("编码后 %1x%2 · 采集 %3x%4 · 已取 %5 帧")
                  .arg(m_pv->shownSize().width()).arg(m_pv->shownSize().height())
                  .arg(m_pv->sourceSize().width()).arg(m_pv->sourceSize().height())
                  .arg(m_pv->framesSeen())
            : m_pv->lastError();
        m_cap->setText(cap);
    });

    // Esc 要能从输入框和列表里也收得到：QLineEdit 会把 Esc 吃掉（它有自己的 escapePressed），
    // 所以除了 keyPressEvent，还得给这两个子控件装事件过滤器。
    filter->installEventFilter(this);
    list->installEventFilter(this);
}

void WindowPicker::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) {
        hide();
        return;
    }
    QWidget::keyPressEvent(e);
}

bool WindowPicker::eventFilter(QObject* watched, QEvent* e)
{
    if (e->type() == QEvent::KeyPress
        && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
        hide();
        return true;
    }
    if (watched == m_list && e->type() == QEvent::KeyPress) {
        const int k = static_cast<QKeyEvent*>(e)->key();
        if (k == Qt::Key_Return || k == Qt::Key_Enter) {
            // 列表里按回车 = 按「开始录制」。条目本身不再因为双击开录了，键盘这条快路留着。
            chooseCurrent();
            return true;
        }
    }
    if (e->type() == QEvent::WindowDeactivate) {
        // 点到别处 = 不录了。这个窗是 activateWindow 过的，失焦就是用户真的走开了；
        // 不收起的话它会一直压在要录的那个窗口上面。
        hide();
        return false;
    }
    return QWidget::eventFilter(watched, e);
}

void WindowPicker::chooseCurrent()
{
    auto* it = m_list ? m_list->currentItem() : nullptr;
    if (!it || it->isHidden())
        return;
    emit picked(WId(it->data(Qt::UserRole).toLongLong()), it->data(Qt::UserRole + 1).toString(),
                it->data(Qt::UserRole + 2).toBool());
}

void WindowPicker::refreshPreview()
{
    auto* it = m_list ? m_list->currentItem() : nullptr;
    if (!isVisible() || !it || it->isHidden()) {
        m_pv->stop();
        m_shot->clear();
        m_shot->setText(it ? QStringLiteral("这条看不见") : QStringLiteral("没选东西"));
        m_cap->setText(QStringLiteral("选中一条就能看到录进去的画面"));
        return;
    }
    m_pv->setMaxWidth(recMaxWidth());
    QString err;
    if (!m_pv->begin(WId(it->data(Qt::UserRole).toLongLong()),
                     it->data(Qt::UserRole + 2).toBool(), &err)) {
        m_shot->clear();
        m_shot->setText(QStringLiteral("开不起预览"));
        m_cap->setText(err);
    } else {
        m_shot->clear();
        m_shot->setText(QStringLiteral("等第一帧…"));
        m_cap->setText(QStringLiteral("编码后 %1")
                           .arg(m_pv->shownSize().isValid() && m_pv->shownSize().width()
                                    ? QStringLiteral("%1x%2")
                                          .arg(m_pv->shownSize().width())
                                          .arg(m_pv->shownSize().height())
                                    : QStringLiteral("?")));
    }
}

void WindowPicker::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        m_press = e->globalPosition().toPoint() - geometry().topLeft();
    }
}

void WindowPicker::mouseMoveEvent(QMouseEvent* e)
{
    if (m_dragging && (e->buttons() & Qt::LeftButton))
        move(e->globalPosition().toPoint() - m_press);
}

void WindowPicker::mouseReleaseEvent(QMouseEvent*)
{
    m_dragging = false;
}

void WindowPicker::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refreshPreview();
}

void WindowPicker::hideEvent(QHideEvent* e)
{
    // 收起来就把 WGC 那一路放掉：不放的话它一直在别人窗口上面抓帧，白烧 CPU，
    // 而且紧接着 Recorder 要对同一个窗口再开一路，两路叠着没好处。
    m_pv->stop();
    QWidget::hideEvent(e);
}

// ---------------- 悬浮条 ----------------

RecordBar::RecordBar(Recorder* rec, const QString& path, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
      m_rec(rec), m_path(path)
{
    // 别抢前台：他正要点的是被录的那个窗口
    setAttribute(Qt::WA_ShowWithoutActivating);
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(10, 4, 8, 4);
    lay->setSpacing(8);
    auto* dot = new QLabel(QStringLiteral("●"), this);
    dot->setStyleSheet("color:#e04a4a;border:none;font-size:13px;");
    m_text = new QLabel(QStringLiteral("正在录…"), this);
    m_text->setObjectName(QStringLiteral("text"));
    m_stop = new QPushButton(QStringLiteral("停止"), this);
    m_stop->setObjectName(QStringLiteral("stop"));
    m_stop->setCursor(Qt::PointingHandCursor);
    m_cancel = new QPushButton(QStringLiteral("不要了"), this);
    m_cancel->setObjectName(QStringLiteral("cancel"));
    m_cancel->setCursor(Qt::PointingHandCursor);
    m_cancel->setToolTip(QStringLiteral("停下这一局并且把已经录的丢掉（文件删掉，不留在录像目录里）"));
    lay->addWidget(dot);
    lay->addWidget(m_text);
    lay->addStretch(1);
    lay->addWidget(m_cancel);
    lay->addWidget(m_stop);
    setStyleSheet("QWidget{background:#26272c;border:1px solid #4a4d57;border-radius:8px;}"
                  "QLabel{color:#e8ebf2;border:none;font-size:12px;}"
                  "QPushButton{background:#3a3d46;color:#e8ebf2;border:none;border-radius:5px;"
                  "padding:3px 10px;}"
                  "QPushButton:hover{background:#4a4f5c;}");
    setAttribute(Qt::WA_TranslucentBackground, false);
    adjustSize();
    setFixedWidth(320);

    const QRect a = screen()->availableGeometry();
    move(a.right() - width() - 16, a.bottom() - height() - 12);

    connect(m_stop, &QPushButton::clicked, this, [this] {
        m_stop->setEnabled(false);
        m_cancel->setEnabled(false);
        m_text->setText(QStringLiteral("正在收尾（编码要把最后一帧写完）…"));
        QCoreApplication::processEvents();
        m_rec->stop();
        finish(m_rec->errorText());
    });
    // 「不要了」= 停下这一局并且把文件删掉。Recorder 没有"只停不写盘"这条路
    // （MP4 是收尾才落盘的），所以照常收尾，然后把那个废文件原地删了 ——
    // 录到一半的东西留在录像目录里就是垃圾，还得人去删。
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        m_stop->setEnabled(false);
        m_cancel->setEnabled(false);
        m_text->setText(QStringLiteral("正在收尾（要把这一局丢掉）…"));
        QCoreApplication::processEvents();
        m_discard = true;
        m_rec->stop();
        finish(m_rec->errorText());
    });

    auto* tick = new QTimer(this);
    connect(tick, &QTimer::timeout, this, &RecordBar::poll);
    tick->start(200);
}

void RecordBar::poll()
{
    if (m_rec->isDone()) {              // 窗口关了/改尺寸了：worker 自己收的尾
        if (!m_finished)
            finish(m_rec->errorText());
        return;
    }
    const qint64 ms = m_rec->elapsedMs();
    // 不报"已写多少 KB"：MP4 是收尾才落盘的，录的过程中一直是 0，写上去就是撒谎
    m_text->setText(QStringLiteral("⏺ %1:%2 · %3 帧")
                        .arg(ms / 60000)
                        .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
                        .arg(m_rec->framesCaptured()));
}

void RecordBar::finish(const QString& error)
{
    m_finished = true;   // 只收一次尾：以前这里每 200ms 重进一遍，日志被刷爆
    if (m_discard) {
        QFile::remove(m_path);
        m_text->setText(QStringLiteral("已取消，没保存（丢掉 %1 帧）").arg(m_rec->framesCaptured()));
        m_stop->setText(QStringLiteral("关闭"));
        m_cancel->setVisible(false);
        Utils::log(QString("record cancelled: %1 frames=%2 文件已删")
                       .arg(m_path).arg(m_rec->framesCaptured()));
        m_stop->setEnabled(true);
        disconnect(m_stop, nullptr, this, nullptr);
        connect(m_stop, &QPushButton::clicked, this, [this] { close(); });
        auto* t = new QTimer(this);
        connect(t, &QTimer::timeout, this, &QWidget::close);
        t->setSingleShot(true);
        t->start(8000);
        return;
    }
    if (!error.isEmpty()) {
        m_text->setText(QStringLiteral("没录成：%1").arg(error));
        m_stop->setText(QStringLiteral("知道了"));
        Utils::log(QString("record failed: %1 path=%2").arg(error, m_path));
    } else {
        const qint64 bytes = m_rec->fileBytes();
        // 13KB 显示成 "0.0 MB" 就是没说实话
        const QString size = bytes >= 1024 * 1024
            ? QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1)
            : QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
        m_text->setText(QStringLiteral("已保存 %1 帧 / %2 → %3")
                            .arg(m_rec->framesCaptured())
                            .arg(size, QFileInfo(m_path).fileName()));
        m_stop->setText(QStringLiteral("打开所在目录"));
        Utils::log(QString("record saved: %1 frames=%2 bytes=%3")
                       .arg(m_path).arg(m_rec->framesCaptured()).arg(bytes));
    }
    m_stop->setEnabled(true);
    disconnect(m_stop, nullptr, this, nullptr);
    connect(m_stop, &QPushButton::clicked, this, [this, error] {
        if (error.isEmpty())
            Platform::reveal(m_path);
        close();
    });
    auto* t = new QTimer(this);
    connect(t, &QTimer::timeout, this, &QWidget::close);
    t->setSingleShot(true);
    t->start(error.isEmpty() ? 30000 : 12000);   // 结果留在条上，不用弹窗打断
}

void RecordBar::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        m_press = e->globalPosition().toPoint() - geometry().topLeft();
    }
}

void RecordBar::mouseMoveEvent(QMouseEvent* e)
{
    if (m_dragging && (e->buttons() & Qt::LeftButton))
        move(e->globalPosition().toPoint() - m_press);
}

void RecordBar::mouseReleaseEvent(QMouseEvent*)
{
    m_dragging = false;
}

// ---------------- 入口 ----------------

namespace RecordUi {

void startViaPicker()
{
    if (isRecording()) {
        if (g_bar)
            g_bar->raise();
        return;
    }
    if (!g_rec)
        g_rec = new Recorder;
    QString why;
    if (!Recorder::available(&why)) {
        QMessageBox::warning(nullptr, QStringLiteral("录屏"), why);
        return;
    }
    // 选择窗开着的时候再按一次 F2 = 取消（它没有标题栏、没有关闭按钮，
    // 光靠 Esc 和"点到别处"还不够：手就放在热键上，再按一次最自然）
    if (g_picker && g_picker->isVisible()) {
        g_picker->hide();
        return;
    }
    // 每次都要重新数一遍窗口（他可能刚开/刚关了几个）
    if (g_picker) {
        g_picker->hide();
        delete g_picker;
        g_picker = nullptr;
    }
    g_picker = new WindowPicker;
    QObject::connect(g_picker, &WindowPicker::picked,
                     [](WId target, const QString& title, bool monitor) {
        if (g_picker)
            g_picker->hide();
        beginRecording(target, title, monitor);
    });
    const QPoint cur = QCursor::pos();
    const QRect a = QGuiApplication::screenAt(cur)
                        ? QGuiApplication::screenAt(cur)->availableGeometry()
                        : QApplication::primaryScreen()->availableGeometry();
    g_picker->move(qMin(cur.x() + 12, a.right() - g_picker->width() - 8),
                   qMin(cur.y() + 12, a.bottom() - g_picker->height() - 8));
    g_picker->show();
    g_picker->raise();
    g_picker->activateWindow();
}

bool isRecording()
{
    return g_rec && g_rec->running();
}

QString qualityText()
{
    return QString::fromUtf8(kQualities[recQualityIndex()].name);
}

QString maxWidthText()
{
    return widthCapText(recMaxWidth());
}

} // namespace RecordUi
