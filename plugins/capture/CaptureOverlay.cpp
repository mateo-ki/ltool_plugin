#include "CaptureHistory.h"
#include "CaptureOverlay.h"
#include "OcrPanel.h"
#include "PinWindow.h"
#include "Utils.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QThread>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QButtonGroup>
#include <QPushButton>
#include <QScreen>
#include <QHBoxLayout>
#include <QTimer>

#include <windows.h>
#include <dwmapi.h>

namespace {

CaptureOverlay* s_instance = nullptr;

// 记下按键那一刻谁是前台窗口：F1"没反应"的两种可能（热键没送到 / 浮层被压在下面）
// 靠这条日志才能分开
QString foregroundExeName()
{
    HWND fg = GetForegroundWindow();
    if (!fg)
        return QStringLiteral("(无前台窗口)");
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    QString name = QStringLiteral("?");
    if (HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        WCHAR buf[1024];
        DWORD len = 1024;
        if (QueryFullProcessImageNameW(ph, 0, buf, &len))
            name = QFileInfo(QString::fromWCharArray(buf, int(len))).fileName();
        CloseHandle(ph);
    }
    DWORD style = 0;
    if (HWND root = GetAncestor(fg, GA_ROOT))
        style = GetWindowLongW(root, GWL_EXSTYLE);
    return name + (style & WS_EX_TOPMOST ? QStringLiteral(" (topmost)") : QString());
}

QRect snappedPhysicalRect(HWND h)
{
    // 选区取 DWM 的**可见边界**。本机实测（build/verify/winrect.ps1，1336x808 的窗口）：
    // GetWindowRect 比可见帧**左/右/下各多 7px、上边多 0px**，那三边是"看不见的 resize
    // 边框"——以前拿它当选区，再按左边的余量给上边补 7px 凑对称，等于把余量复制到第四边，
    // 四周各多出一圈没用的桌面。少数窗口（无帧的、1x1 的）DWM 给不出有效矩形，退回 GetWindowRect。
    RECT rc;
    RECT vis{};
    const bool haveVis =
        SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))
        && vis.right > vis.left && vis.bottom > vis.top;
    if (haveVis)
        rc = vis;
    else if (!GetWindowRect(h, &rc))
        return {};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
        // 夹回显示器范围（窗口贴顶/最大化时别把选区推到屏幕外）
        rc.left = qMax(rc.left, LONG(mi.rcMonitor.left));
        rc.top = qMax(rc.top, LONG(mi.rcMonitor.top));
        rc.right = qMin(rc.right, LONG(mi.rcMonitor.right));
        rc.bottom = qMin(rc.bottom, LONG(mi.rcMonitor.bottom));
    }
    if (rc.right <= rc.left || rc.bottom <= rc.top)
        return {};
    return QRect(rc.left, rc.top, int(rc.right - rc.left), int(rc.bottom - rc.top));
}

QRect windowRectAtLogical(const QPoint& logicalPos, qreal dpr, HWND self)
{
    POINT pt;
    pt.x = LONG(logicalPos.x() * dpr);
    pt.y = LONG(logicalPos.y() * dpr);
    // 覆盖层自身挡在最上层，查询时临时加穿透标志跳过它
    LONG ex = GetWindowLongW(self, GWL_EXSTYLE);
    SetWindowLongW(self, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT | WS_EX_LAYERED);
    HWND h = WindowFromPoint(pt);
    SetWindowLongW(self, GWL_EXSTYLE, ex);
    if (!h)
        return {};
    const QRect phys = snappedPhysicalRect(GetAncestor(h, GA_ROOT));
    if (phys.isEmpty())
        return {};
    return QRect(int(phys.x() / dpr), int(phys.y() / dpr),
                 int(phys.width() / dpr), int(phys.height() / dpr));
}



// 工具图标与画箭头都搬进了 Anno（置顶窗编辑要用同一套），这里只留调用

} // namespace

QRect CaptureOverlay::snapRectFor(WId hwnd)
{
    return snappedPhysicalRect(reinterpret_cast<HWND>(uintptr_t(hwnd)));
}

void CaptureOverlay::start()
{
    if (s_instance) {
        Utils::log(QString("capture: F1 被忽略，浮层已经开着（前台=%1）").arg(foregroundExeName()));
        return; // 已在截图中
    }
    Utils::log(QString("capture: start（前台=%1）").arg(foregroundExeName()));
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QPixmap shot = screen->grabWindow(0);
    auto* o = new CaptureOverlay(shot);
    o->setAttribute(Qt::WA_DeleteOnClose);
    o->show();
    o->raise();
    o->activateWindow();
    // Qt 的 WindowStaysOnTopHint 只是把窗口放进 topmost 带：前台那个软件自己也
    // topmost（播放器、游戏 HUD、Task Manager…），我们就会被它压在下面。
    // 更要命的是 SetForegroundWindow 会被前台锁挡掉：旧日志实测 30 次呼出里有 20 次
    // active=0 —— 没拿到焦点时 Esc 和方向键完全不理，用户视角就是"按了 F1 什么都没发生"。
    // 所以按一次 Alt + 接上当前前台线程的输入队列再置前，没成功就重试，最后把真实结果写日志。
    if (HWND hwnd = reinterpret_cast<HWND>(o->winId())) {
        keybd_event(VK_MENU, 0, 0, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        bool fg = false;
        int attempt = 0;
        while (!fg && attempt < 4) {
            ++attempt;
            HWND cur = GetForegroundWindow();
            DWORD fgThread = cur ? GetWindowThreadProcessId(cur, nullptr) : 0;
            const DWORD own = GetCurrentThreadId();
            const bool attached = fgThread && fgThread != own
                && AttachThreadInput(own, fgThread, TRUE);
            SetForegroundWindow(hwnd);
            BringWindowToTop(hwnd);
            if (attached)
                AttachThreadInput(own, fgThread, FALSE);
            fg = GetForegroundWindow() == hwnd;
            if (!fg) {
                QCoreApplication::processEvents();
                QThread::msleep(30);
            }
        }
        POINT pt {};
        GetCursorPos(&pt);
        const bool onTop = GetAncestor(WindowFromPoint(pt), GA_ROOT) == hwnd;
        Utils::log(QString("capture: shown geo=%1x%2 at %3,%4 visible=%5 前台=%6 在最上=%7 试了%8次")
                       .arg(o->width()).arg(o->height())
                       .arg(o->x()).arg(o->y()).arg(o->isVisible() ? 1 : 0)
                       .arg(fg ? 1 : 0).arg(onTop ? 1 : 0).arg(attempt));
    }

}

bool CaptureOverlay::isActive()
{
    // 状态位存在 core（src/CaptureHistory.cpp）：那边搜索框要读它，而 core 链接不到这个 dll。
    return CaptureState::isActive();
}

void CaptureOverlay::cancelActive()
{
    if (s_instance)
        s_instance->cancel();
}

CaptureOverlay::CaptureOverlay(const QPixmap& shot)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    , m_shot(shot)
    , m_canvas(shot)
{
    s_instance = this;
    CaptureState::setActive(true);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    setGeometry(screen->geometry());
}

QRect CaptureOverlay::selNorm() const
{
    return m_sel.normalized();
}

void CaptureOverlay::pushUndo()
{
    m_undo.append({ m_canvas, m_shapes });
    if (m_undo.size() > 12)
        m_undo.removeFirst();
}

void CaptureOverlay::undoStep()
{
    if (m_undo.isEmpty())
        return;
    const Snapshot s = m_undo.takeLast();
    m_canvas = s.canvas;
    m_shapes = s.shapes;
    m_selShape = -1;
    update();
}

// 画布 + 形状，并按选区裁好：所有"要输出的图"都走这里，别再直接用 m_canvas
QRect CaptureOverlay::deviceRectFor(const QRect& logicalSel, qreal dpr)
{
    if (logicalSel.isEmpty() || dpr <= 0.0 || dpr == 1.0)
        return logicalSel;
    const int px = int(logicalSel.x() * dpr);
    const int py = int(logicalSel.y() * dpr);
    return QRect(px, py, int((logicalSel.x() + logicalSel.width()) * dpr) - px,
                 int((logicalSel.y() + logicalSel.height()) * dpr) - py);
}

QPixmap CaptureOverlay::cropCanvas(const QPixmap& canvas, const QRect& logicalSel)
{
    // 选区是**逻辑**坐标（鼠标事件、吸附换算出来的都是），画布是**物理像素**。
    // QPixmap::copy 按设备像素解释它拿到的矩形 —— 不换算的话 Windows 缩放 150% 时
    // 全选整屏只裁到左上 1707x960（本机实测差 854x480，整屏少掉三分之一），就是他说的
    // "缩放不是 100% 就截不了全屏"。100% 下两套坐标重合，所以这个坑平时看不见。
    const qreal dpr = canvas.devicePixelRatio();
    QPixmap out = canvas.copy(deviceRectFor(logicalSel, dpr));
    out.setDevicePixelRatio(dpr);   // 不写回去的话置顶窗/保存会按 100% 铺开放大
    return out;
}

QPixmap CaptureOverlay::rendered() const
{
    const QRect sel = selNorm();
    if (sel.isEmpty())
        return m_canvas;
    QPixmap out = cropCanvas(m_canvas, sel);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(-sel.topLeft());
    p.setClipRect(sel);
    drawShapes(p);
    p.end();
    return out;
}

void CaptureOverlay::drawShapes(QPainter& p) const
{
    QPen pen(QColor(214, 69, 69), 3);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    for (const Shape& s : m_shapes)
        Anno::drawShape(p, s.tool, QRect(s.a, s.b).normalized(), s.a, s.b);
}

void CaptureOverlay::paintEvent(QPaintEvent*)
{
    static bool logged = false;
    if (!logged) {
        logged = true;
        Utils::log("capture: first paint");
    }
    QPainter p(this);
    p.drawPixmap(0, 0, m_canvas);

    if (m_mode == Selecting) {
        p.fillRect(rect(), QColor(0, 0, 0, 90));
        const QRect show = (m_dragging && m_moved) ? selNorm()
                           : (m_hasSuggest ? m_suggest : QRect());
        if (!show.isEmpty()) {
            // 目标矩形是逻辑的（画在控件上），**来源矩形是按画布的设备像素解释的**
            //（--capgeo 实测：不换算的话 150% 下这块亮的预览显示的是左上角放大版的错位画面）
            p.drawPixmap(show, m_canvas, deviceRectFor(show, m_canvas.devicePixelRatio()));
            QPen pen(QColor(63, 106, 224), 3);
            p.setPen(pen);
            p.setBrush(QColor(63, 106, 224, 30));
            p.drawRect(show);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(63, 106, 224));
            p.drawRect(show.left(), show.top() - 24, 84, 22);
            p.setPen(Qt::white);
            // 角标写的是"你将拿到的图有多少像素"，所以取设备尺寸 —— 150% 下逻辑尺寸是它的 2/3，
            // 写逻辑数就等于界面和成品对不上
            const QSize px = deviceRectFor(show, m_canvas.devicePixelRatio()).size();
            p.drawText(show.left(), show.top() - 24, 84, 22, Qt::AlignCenter,
                       QString("%1×%2").arg(px.width()).arg(px.height()));
        } else {
            p.setPen(QColor(255, 255, 255, 200));
            p.drawText(rect(), Qt::AlignCenter,
                       "F1 截图：拖动框选区域，鼠标移到窗口上自动吸附，Enter 确认，Esc 取消");
        }
        return;
    }

    // Annotate：选区外压暗
    const QRect sel = selNorm();
    if (!sel.isEmpty()) {
        p.fillRect(rect(), QColor(0, 0, 0, 90));
        p.drawPixmap(sel, m_canvas, deviceRectFor(sel, m_canvas.devicePixelRatio()));
        // 形状画在选区里（超出选区的部分切掉，和最终输出一致）
        p.setClipRect(sel);
        drawShapes(p);
        p.setClipping(false);
        QPen pen(QColor(63, 106, 224), 2);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(sel);
        if (m_selShape >= 0) {
            // 选中了某个形状：只画它的手柄，拖手柄改大小、拖本体挪位置
            p.setPen(QPen(QColor(255, 190, 60), 1));
            p.setBrush(Qt::white);
            for (const QRect& h : shapeHandles(m_shapes[m_selShape]))
                p.drawRect(h);
            p.setPen(QPen(QColor(255, 190, 60), 1, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(shapeRect(m_shapes[m_selShape]).adjusted(-4, -4, 4, 4));
        } else {
            // 八个手柄：拖它们改裁剪范围（无工具状态下）
            p.setPen(QPen(QColor(63, 106, 224), 1));
            p.setBrush(Qt::white);
            for (const QRect& h : handleRects())
                p.drawRect(h);
        }
        if (m_dragWhat != kNoDrag && m_tool == None && m_selShape < 0) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(63, 106, 224));
            p.drawRect(sel.left(), sel.top() - 24, 84, 22);
            p.setPen(Qt::white);
            p.drawText(sel.left(), sel.top() - 24, 84, 22, Qt::AlignCenter,
                       QString("%1×%2").arg(sel.width()).arg(sel.height()));
        }
    }

    // 进行中的笔迹预览（拖已有形状时不画）
    if (m_dragging && m_tool != None && m_dragWhat == kNoDrag) {
        QPen pen(m_tool == Mosaic ? Qt::transparent : QColor(214, 69, 69), 3);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        Anno::drawShape(p, m_tool, QRect(m_press, m_last).normalized(), m_press, m_last);
    }
}

void CaptureOverlay::buildToolbar()
{
    if (m_toolbar)
        m_toolbar->deleteLater();
    m_toolbar = new QWidget(this);
    m_toolbar->setAttribute(Qt::WA_StyledBackground);
    m_toolbar->setStyleSheet(
        "QWidget{background:#26272c;border:1px solid #3a3c45;border-radius:8px;}"
        "QPushButton{background:transparent;color:#e8ebf2;border:none;border-radius:6px;"
        "padding:4px 8px;font-size:12px;}"
        "QPushButton:hover{background:#3a3d46;}"
        "QPushButton:checked{background:#3f6ae0;}");
    auto* lay = new QHBoxLayout(m_toolbar);
    lay->setContentsMargins(4, 2, 4, 2);
    lay->setSpacing(2);

    // label 是给读屏/自动化用的可访问名：图标按钮没有文字，UIA 里查不到它们
    const struct { const char* tip; const char* label; Tool tool; } tools[] = {
        {"选区（拖动边角可伸缩，单击选区内=复制）", "选区", None},
        {"矩形", "矩形", Rect}, {"圆", "圆", Ellipse}, {"箭头", "箭头", Arrow},
        {"文字", "文字", Text}, {"马赛克", "马赛克", Mosaic},
    };
    // 图标按钮没有文字，必须靠 tooltip 说明；选中态用 checkable + 互斥组显示出来
    auto* group = new QButtonGroup(m_toolbar);
    group->setExclusive(true);
    for (const auto& t : tools) {
        auto* b = new QPushButton(m_toolbar);
        b->setCheckable(true);
        b->setToolTip(t.tip);
        b->setAccessibleName(QString::fromUtf8(t.label));
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedWidth(t.tool == None ? 44 : 30);
        if (t.tool == None)
            b->setText(QStringLiteral("选区"));
        else
            b->setIcon(Anno::toolIcon(t.tool));
        group->addButton(b);
        if (t.tool == None)
            b->setChecked(true);
        connect(b, &QPushButton::clicked, this, [this, tool = t.tool] {
            m_tool = tool;
            m_selShape = -1;   // 换工具时先放开选中的形状
            setCursor(tool == None ? Qt::ArrowCursor : Qt::CrossCursor);
        });
        lay->addWidget(b);
    }
    auto* pin = new QPushButton(Anno::toolIcon(Anno::Pin), QString(), m_toolbar);
    pin->setToolTip("置顶为悬浮窗");
    pin->setCursor(Qt::PointingHandCursor);
    pin->setFixedWidth(30);
    connect(pin, &QPushButton::clicked, this, [this] {
        const QPixmap pm = rendered();
        (new PinWindow(pm, CaptureHistory::add(pm)))->show();
        cancel();
    });
    lay->addWidget(pin);
    auto* undo = new QPushButton("撤销", m_toolbar);
    undo->setToolTip("撤销上一步（Ctrl+Z）；选中形状后按 Delete 可删掉它");
    connect(undo, &QPushButton::clicked, this, [this] { undoStep(); });
    lay->addWidget(undo);
    lay->addSpacing(6);
    auto* copy = new QPushButton("复制", m_toolbar);
    copy->setObjectName("primary");
    connect(copy, &QPushButton::clicked, this, [this] { toClipboard(); });
    lay->addWidget(copy);
    auto* ocr = new QPushButton("提取文字", m_toolbar);
    ocr->setToolTip("把选区里的文字认出来（自带的 PaddleOCR，第一次用要下载一次模型，之后离线）\n"
                    "结果自动进剪贴板，并开一个小窗给你看/改/存");
    connect(ocr, &QPushButton::clicked, this, [this] { extractText(); });
    lay->addWidget(ocr);
    auto* save = new QPushButton("保存", m_toolbar);
    connect(save, &QPushButton::clicked, this, [this] { saveFile(); });
    lay->addWidget(save);
    auto* cancelBtn = new QPushButton("取消", m_toolbar);
    connect(cancelBtn, &QPushButton::clicked, this, [this] { cancel(); });
    lay->addWidget(cancelBtn);

    m_toolbar->adjustSize();
    placeToolbar();
    m_toolbar->show();
    m_toolbar->raise();
}

// 工具栏贴在选区下方，放不下就翻到上方；改完选区后要用它重新对位
void CaptureOverlay::placeToolbar()
{
    if (!m_toolbar)
        return;
    const QRect sel = selNorm();
    QPoint pos(sel.left(), sel.bottom() + 8);
    if (pos.y() + m_toolbar->height() > height())
        pos.setY(qMax(0, sel.top() - m_toolbar->height() - 8));
    pos.setX(qBound(0, pos.x(), width() - m_toolbar->width() - 4));
    m_toolbar->move(pos);
}

QVector<QRect> CaptureOverlay::handlesFor(const QRect& r)
{
    return Anno::boxHandles(r);
}

QVector<QRect> CaptureOverlay::handleRects() const
{
    return handlesFor(selNorm());
}

QRect CaptureOverlay::shapeRect(const Shape& s)
{
    return Anno::shapeRect(s);
}

QVector<QRect> CaptureOverlay::shapeHandles(const Shape& s) const
{
    return Anno::shapeHandles(s);
}

bool CaptureOverlay::hitsShape(const Shape& s, const QPoint& pos) const
{
    return Anno::hitsShape(s, pos);
}

int CaptureOverlay::shapeAt(const QPoint& pos) const
{
    return Anno::hitShape(m_shapes, pos);
}

void CaptureOverlay::beginShapeDrag(int what, const QPoint& pos)
{
    if (m_selShape < 0 || m_selShape >= m_shapes.size())
        return;
    m_dragWhat = what;
    m_press = pos;
    m_shapeSnap = m_shapes[m_selShape];
    m_shapePushed = false;
    m_dragging = true;
    m_moved = false;
}

// 拖手柄=按边伸缩，拖内部=整体平移；都从 m_shapeSnap 快照算，避免累积误差
void CaptureOverlay::applyShapeDrag(const QPoint& pos)
{
    if (m_selShape < 0 || m_selShape >= m_shapes.size())
        return;
    Shape& sh = m_shapes[m_selShape];
    const QPoint d = pos - m_press;
    if (d.manhattanLength() > 3 && !m_shapePushed) {
        pushUndo();          // 真正动了才占一个撤销步骤
        m_shapePushed = true;
    }
    if (m_dragWhat == kMove) {
        sh.a = m_shapeSnap.a + d;
        sh.b = m_shapeSnap.b + d;
        return;
    }
    if (sh.tool == Arrow) {
        if (m_dragWhat == 0)
            sh.a = m_shapeSnap.a + d;
        else
            sh.b = m_shapeSnap.b + d;
        return;
    }
    const QRect n = Anno::resizeRect(shapeRect(m_shapeSnap), m_dragWhat, d, rect());
    sh.a = n.topLeft();
    sh.b = n.bottomRight();
}

void CaptureOverlay::applyDrag(const QPoint& pos)
{
    const QPoint d = pos - m_press;
    if (m_dragWhat == kMove) {
        QRect s = m_sel0;
        s.translate(qBound(-m_sel0.left(), d.x(), width() - 1 - m_sel0.right()),
                    qBound(-m_sel0.top(), d.y(), height() - 1 - m_sel0.bottom()));
        m_sel = s;
        return;
    }
    const QRect n = Anno::resizeRect(m_sel0, m_dragWhat, d, rect());
    if (n.width() >= 8 && n.height() >= 8)
        m_sel = n;
}

void CaptureOverlay::finishSelect()
{
    if (selNorm().width() < 6 || selNorm().height() < 6)
        return;
    m_mode = Annotate;
    m_hasSuggest = false;
    m_dragging = false;
    setCursor(Qt::ArrowCursor);
    Utils::log(QString("capture: selected %1x%2 at %3,%4")
                   .arg(selNorm().width()).arg(selNorm().height())
                   .arg(selNorm().x()).arg(selNorm().y()));
    buildToolbar();
    update();
}

void CaptureOverlay::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::RightButton) {
        cancel(); // 右键取消截图
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    if (m_mode == Selecting) {
        m_press = e->pos();
        m_dragging = true;
        m_moved = false; // 未拖动时保持窗口吸附建议，左键单击即确认
        update();
        return;
    }
    // Annotate：没选工具时，先照顾"已画形状"的选中/拖动/拉伸，再回到选区本身：
    // 拖选区手柄改裁剪范围、拖选区内部移动选区、单击选区内=完成复制、点选区外=取消
    if (m_tool == None) {
        const QRect sel = selNorm();
        if (m_selShape >= 0 && m_selShape < m_shapes.size()) {
            const Shape& sh = m_shapes[m_selShape];
            const QVector<QRect> hs = shapeHandles(sh);
            for (int i = 0; i < hs.size(); ++i) {
                if (hs[i].contains(e->pos())) {
                    beginShapeDrag(i, e->pos());
                    return;
                }
            }
            if (hitsShape(sh, e->pos())) {
                beginShapeDrag(kMove, e->pos());
                return;
            }
            m_selShape = -1;   // 点的是别处：先取消选中，再走选区逻辑
        }
        const int hit = shapeAt(e->pos());
        if (hit >= 0) {
            m_selShape = hit;
            beginShapeDrag(kMove, e->pos());
            update();
            return;
        }
        const QVector<QRect> hs = handleRects();
        for (int i = 0; i < hs.size(); ++i) {
            if (!hs[i].contains(e->pos()))
                continue;
            m_dragWhat = i;
            m_sel0 = sel;
            m_press = e->pos();
            m_dragging = true;
            return;
        }
        if (sel.contains(e->pos())) {
            m_dragWhat = kMove;
            m_sel0 = sel;
            m_press = e->pos();
            m_last = e->pos();
            m_moved = false;
            m_dragging = true;
            return;
        }
        cancel();
        return;
    }
    if (m_tool == Text) {
        m_textPos = e->pos();
        if (!m_edit) {
            m_edit = new QLineEdit(this);
            m_edit->setStyleSheet("background:#232429;color:#e8ebf2;border:1px solid #3f6ae0;"
                                  "border-radius:4px;padding:2px 6px;font-size:14px;");
            connect(m_edit, &QLineEdit::editingFinished, this, [this] {
                if (m_edit && !m_edit->text().isEmpty()) {
                    pushUndo();
                    QPainter p(&m_canvas);
                    p.setPen(QColor(214, 69, 69));
                    QFont f("Microsoft YaHei UI", 14);
                    f.setBold(true);
                    p.setFont(f);
                    p.drawText(QRect(m_textPos, QSize(400, 30)), Qt::AlignLeft | Qt::AlignTop,
                               m_edit->text());
                    update();
                }
                if (m_edit) {
                    m_edit->deleteLater();
                    m_edit = nullptr;
                }
            });
        }
        m_edit->setGeometry(e->pos().x(), e->pos().y(), 260, 28);
        m_edit->show();
        m_edit->raise();
        m_edit->setFocus();
        return;
    }
    if (m_tool == None)
        return;
    pushUndo();
    m_press = e->pos();
    m_last = e->pos();
    m_dragging = true;
}

void CaptureOverlay::mouseMoveEvent(QMouseEvent* e)
{
    if (m_mode == Selecting) {
        if (m_dragging) {
            if ((e->pos() - m_press).manhattanLength() > 6) {
                m_moved = true;
                m_hasSuggest = false; // 一旦拖动，切换为手动框选
            }
            if (m_moved)
                m_sel = QRect(m_press, e->pos());
            update();
        } else {
            const QRect r = windowRectAtLogical(e->pos(), screen()->devicePixelRatio(),
                                                reinterpret_cast<HWND>(winId()));
            m_hasSuggest = r.isValid();
            m_suggest = r;
            update();
        }
        return;
    }
    // 标注阶段没选工具：拖形状手柄拉伸/拖形状本体挪位 / 拖选区手柄伸缩 / 悬停给手型反馈
    if (m_tool == None) {
        if (m_selShape >= 0 && m_dragWhat != kNoDrag) {
            applyShapeDrag(e->pos());
            update();
            return;
        }
        if (m_dragWhat != kNoDrag) {
            if ((e->pos() - m_press).manhattanLength() > 4)
                m_moved = true;
            if (m_moved || m_dragWhat != kMove)
                applyDrag(e->pos());
            update();
            return;
        }
        static constexpr Qt::CursorShape kShapes[kHandleCount] = {
            Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor,
            Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor };
        Qt::CursorShape sh = Qt::ArrowCursor;
        if (m_selShape >= 0) {
            const QVector<QRect> hs = shapeHandles(m_shapes[m_selShape]);
            for (int i = 0; i < hs.size(); ++i) {
                if (!hs[i].contains(e->pos()))
                    continue;
                sh = m_shapes[m_selShape].tool == Arrow ? Qt::CrossCursor : kShapes[i];
                break;
            }
            if (sh == Qt::ArrowCursor && hitsShape(m_shapes[m_selShape], e->pos()))
                sh = Qt::SizeAllCursor;
            setCursor(sh);
            return;
        }
        const QVector<QRect> hs = handleRects();
        for (int i = 0; i < hs.size(); ++i) {
            if (hs[i].contains(e->pos())) { sh = kShapes[i]; break; }
        }
        if (sh == Qt::ArrowCursor && shapeAt(e->pos()) >= 0)
            sh = Qt::PointingHandCursor;   // 有已画的形状可以点
        else if (sh == Qt::ArrowCursor && selNorm().contains(e->pos()))
            sh = Qt::SizeAllCursor;
        setCursor(sh);
        return;
    }
    if (!m_dragging)
        return;
    m_last = e->pos();
    if (m_tool == Mosaic) {
        // 马赛克：沿轨迹把原图区域打码画到画布
        Anno::mosaicSmudge(m_canvas, m_shot, m_last, 14);
        update();
        return;
    }
    update();
}

void CaptureOverlay::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_mode == Selecting) {
        m_dragging = false;
        // 未拖动且有窗口吸附建议 → 左键单击确认截取该窗口
        if (!m_moved && m_hasSuggest)
            m_sel = m_suggest;
        else
            m_sel = QRect(m_press, e->pos());
        finishSelect();
        return;
    }
    // 拖完一个已有形状：挪位/拉伸都只改对象，不碰画布
    if (m_selShape >= 0 && m_dragWhat != kNoDrag) {
        m_dragWhat = kNoDrag;
        m_dragging = false;
        m_moved = false;
        m_shapePushed = false;
        update();
        return;
    }
    // 刚拖完手柄/移动完选区：重新对位工具栏；没拖动过的"点选区内"仍是完成复制
    if (m_dragWhat != kNoDrag) {
        const bool clickToCopy = (m_dragWhat == kMove && !m_moved);
        m_dragWhat = kNoDrag;
        m_dragging = false;
        m_moved = false;
        placeToolbar();
        update();
        if (clickToCopy)
            toClipboard();
        return;
    }
    if (!m_dragging || m_tool == Mosaic) {
        m_dragging = false;
        update();
        return;
    }
    m_dragging = false;
    // 新画的形状进列表（保持可编辑），不烙进画布
    const QRect r = QRect(m_press, e->pos()).normalized();
    const bool bigEnough = m_tool == Arrow ? (m_press - e->pos()).manhattanLength() > 6
                                           : (r.width() >= 3 && r.height() >= 3);
    if (bigEnough) {
        pushUndo();
        m_shapes.append({ m_tool, m_press, e->pos() });
        m_selShape = m_shapes.size() - 1;   // 画完立刻就能拖它、拉手柄
        placeToolbar();
    }
    update();
}

void CaptureOverlay::keyPressEvent(QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_Escape:
        cancel();
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_mode == Selecting)
            finishSelect();
        else
            toClipboard();
        break;
    case Qt::Key_C:
        if (e->modifiers() & Qt::ControlModifier)
            toClipboard();
        break;
    case Qt::Key_Z:
        if (e->modifiers() & Qt::ControlModifier)
            undoStep();
        break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        // 选中一个形状后按 Delete 删掉它（撤销还能捞回来）
        if (m_selShape >= 0 && m_selShape < m_shapes.size()) {
            pushUndo();
            m_shapes.removeAt(m_selShape);
            m_selShape = -1;
            update();
        }
        break;
    default:
        break;
    }
}

void CaptureOverlay::toClipboard()
{
    const QPixmap pm = rendered();
    CaptureHistory::add(pm);
    QGuiApplication::clipboard()->setPixmap(pm);
    Utils::log("capture copied to clipboard");
    cancel();
}

void CaptureOverlay::extractText()
{
    const QImage img = rendered().toImage();
    CaptureHistory::add(QPixmap::fromImage(img));
    cancel();                       // 先把全屏遮罩收掉，结果窗不会被压在下面
    OcrPanel::extractFrom(img);
}

void CaptureOverlay::saveFile()
{
    const QPixmap pm = rendered();
    CaptureHistory::add(pm);
    QDir().mkpath(Utils::capturesDir());
    const QString path = QString("%1/capture-%2.png")
                             .arg(Utils::capturesDir(),
                                  QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss"));
    pm.save(path);
    QGuiApplication::clipboard()->setPixmap(pm);
    Utils::log(QString("capture saved: %1").arg(path));
    cancel();
}

void CaptureOverlay::cancel()
{
    s_instance = nullptr;
    CaptureState::setActive(false);
    close();
    Utils::trimWorkingSet(); // 释放全屏位图占用的内存
}
