#include "PinWindow.h"

#include "OcrBundle.h"
#include "OcrPanel.h"
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfoList>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegion>
#include <QScreen>
#include <QThread>

#include "Utils.h"

#include <windows.h>

namespace {

constexpr int kUndoLimit = 24;    // 撤销栈深度

// 置顶窗里画线的粗细：按图像真实像素算，和截图浮层保持一致
QPen strokePen()
{
    QPen pen(QColor(214, 69, 69), 3);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    return pen;
}

} // namespace

PinWindow::PinWindow(const QPixmap& pm, const QString& key, bool showNow)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    , m_key(key)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);   // 撤销要吃 Ctrl+Z
    buildToolbar();
    buildNav();
    setPixmap(pm);
    // 默认把工具栏和往期带都收起来：刚截完图看到的是干净的一张图，
    // 要标注/保存/换图再右键「显示工具栏」或按图上的箭头。
    // 顺序有讲究：先收工具栏再收往期带，两个"收起之前多大"才是各自该记的那份尺寸
    // （--pintool 里"从默认状态放出两样必须回到展开尺寸"这条就是验它）。
    setBarHidden(true);
    setStripHidden(true);
    QScreen* s = QGuiApplication::screenAt(QCursor::pos());
    if (!s)
        s = QGuiApplication::primaryScreen();
    move(s->geometry().center() - QPoint(width() / 2, height() / 2));
    if (!showNow)
        return;   // 无界面自测：不显示、不抢焦点，事件直接送给控件
    show();
    raise();
    activateWindow();
    setFocus();
    applyTopmost(true);   // 刚截完先浮在最前；之后按"自动让位/一直置顶"的选择走
}

void PinWindow::setPixmap(const QPixmap& pm, bool resetUndo, bool fitWindow)
{
    invalidatePick("setPixmap 换图");
    m_pm = pm;
    const auto it = m_edits.constFind(m_key);
    if (it != m_edits.constEnd()) {
        m_pm = it->base;
        m_shapes = it->shapes;
    } else {
        m_shapes.clear();
    }
    m_selShape = kNoDrag;
    rebuildFlat();
    if (resetUndo) {
        m_undo.clear();
        m_pushed = false;
        refreshUndoBtn();
    }
    QScreen* s = screen() ? screen() : QGuiApplication::primaryScreen();
    const QRect avail = s ? s->availableGeometry() : QRect(0, 0, 1280, 800);
    // 窗口按"显示尺寸"定，绘制时把整张图缩放进去。
    // 老写法是窗口按缩放后的尺寸定、图按原始尺寸画，超过屏幕 60% 的截图会被裁掉一截。
    QSize view = m_pm.size();
    const int maxW = qMax(120, int(avail.width() * 0.6));
    const int maxH = qMax(120, int(avail.height() * 0.6) - barH());
    if (view.width() > maxW || view.height() > maxH)
        view.scale(maxW, maxH, Qt::KeepAspectRatio);
    const int stripH = CaptureHistory::count() * (m_thumbH + 4) + 8;
    // 窗口不能比工具栏还窄：那样右边的按钮（📌/✕ 关闭）会被挤没，
    // 用户看到的就成了"没有关闭按钮"。
    const int minW = m_toolbar ? qMax(200, m_toolbar->sizeHint().width()) : 200;
    setMinimumSize(minW, 140);
    if (fitWindow)
        resize(m_stripW + view.width(), qMax(view.height(), stripH) + barH());
    placeToolbar();
    placeNav();
    update();
}

// 四条边/四个角 8px 内算"缩放手柄"
Qt::Edges PinWindow::edgeAt(const QPoint& pos) const
{
    Qt::Edges e;
    if (pos.x() <= kGrab)
        e |= Qt::LeftEdge;
    if (pos.x() >= width() - kGrab)
        e |= Qt::RightEdge;
    if (pos.y() <= kGrab)
        e |= Qt::TopEdge;
    if (pos.y() >= height() - kGrab)
        e |= Qt::BottomEdge;
    return e;
}

void PinWindow::resizeEvent(QResizeEvent*)
{
    placeToolbar();
    placeNav();
}

// 默认不"永远在最前"：一旦别的程序被激活就让位，新开软件的窗口不会被截图盖住。
// 点一下截图窗（重新激活）它又回到最前；工具栏 📌 可以强制一直置顶。
void PinWindow::changeEvent(QEvent* ev)
{
    QWidget::changeEvent(ev);
    if (ev->type() != QEvent::ActivationChange || m_pinned)
        return;
    applyTopmost(isActiveWindow());
}

void PinWindow::applyTopmost(bool on)
{
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd)
        return;
    SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

QRect PinWindow::imageRect() const
{
    const QRect area(m_stripW, 0, width() - m_stripW, height() - barH());
    if (m_pm.isNull())
        return area;
    const QSize s = m_pm.size().scaled(area.size(), Qt::KeepAspectRatio);
    // 用整除自己算居中：area.center() 会把中心往前偏半像素，四舍五入后图的左沿
    // 可能落到 m_stripW 左边一列，那一列就同时属于历史条和图片（点在图上却换成了图）
    return QRect(QPoint(area.left() + (area.width() - s.width()) / 2,
                        area.top() + (area.height() - s.height()) / 2), s);
}

double PinWindow::viewScale() const
{
    if (m_pm.isNull() || m_pm.width() <= 0)
        return 1.0;
    return double(imageRect().width()) / double(m_pm.width());
}

QPoint PinWindow::toImage(const QPoint& widgetPos) const
{
    const double sc = viewScale();
    const QRect r = imageRect();
    QPoint p(int((widgetPos.x() - r.left()) / sc), int((widgetPos.y() - r.top()) / sc));
    return QPoint(qBound(0, p.x(), m_pm.width() - 1), qBound(0, p.y(), m_pm.height() - 1));
}

bool PinWindow::inImage(const QPoint& widgetPos) const
{
    return imageRect().contains(widgetPos);
}

void PinWindow::beginStroke()
{
    if (m_pushed)
        return;
    m_undo.append(Snap{ m_pm, m_shapes });   // QPixmap 隐式共享，拍一张不花钱
    while (m_undo.size() > kUndoLimit)
        m_undo.removeFirst();
    m_pushed = true;
    refreshUndoBtn();
}

// 底图叠上所有形状 → m_flat。只在形状或底图变了的时候重算一次：
// 拖动过程中每帧都重新打码会卡（打码要复制区域 + 两次缩放）。
void PinWindow::rebuildFlat()
{
    m_flat = m_pm;
    if (m_shapes.isEmpty())
        return;
    for (const Shape& sh : m_shapes) {
        if (sh.tool == Mosaic)
            Anno::mosaicBlock(m_flat, m_pm, Anno::shapeRect(sh));
    }
    QPainter p(&m_flat);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(strokePen());
    p.setBrush(Qt::NoBrush);
    for (const Shape& sh : m_shapes) {
        if (sh.tool == Mosaic)
            continue;
        Anno::drawShape(p, sh.tool, Anno::shapeRect(sh), sh.a, sh.b);
    }
}

// 对外（历史/缩略图/复制/另存）交合成结果；内存里留住底图+形状，所以切回来还能接着改
void PinWindow::syncResult(bool write)
{
    rebuildFlat();
    invalidatePick("syncResult 图重画");   // 图一叠上新东西，之前的字格全是错的
    m_edits[m_key] = Edit{ m_pm, m_shapes };
    CaptureHistory::replace(m_key, m_flat, write);
    update();
}

void PinWindow::finishShape(const QPoint& upTo)
{
    const QRect r = QRect(m_pressImg, upTo).normalized();
    if (r.width() < 2 || r.height() < 2)
        return;   // 只点了一下没拖动：什么都不改，也别留个空的撤销步骤
    beginStroke();
    Shape sh;
    sh.tool = m_tool;      // 矩形/圆/箭头/打码块都走这条，全是可编辑对象
    sh.a = m_pressImg;
    sh.b = upTo;
    m_shapes.append(sh);
    m_selShape = m_shapes.size() - 1;
    syncResult();
}

// ---- 形状的选中 / 拖动 / 拉手柄 ----
int PinWindow::handleAt(const QPoint& widgetPos) const
{
    if (m_selShape < 0 || m_selShape >= m_shapes.size())
        return kNoDrag;
    const QVector<QRect> hs = widgetShapeHandles();
    for (int i = 0; i < hs.size(); ++i)
        if (hs[i].contains(widgetPos))
            return i;
    return kNoDrag;
}

// 手柄画在窗口坐标上（图像坐标按显示比例换算），拖动判定也用这套，两边不会不一致
QVector<QRect> PinWindow::widgetShapeHandles() const
{
    QVector<QRect> out;
    if (m_selShape < 0 || m_selShape >= m_shapes.size())
        return out;
    const double sc = viewScale();
    const QRect ir = imageRect();
    const Shape& sh = m_shapes[m_selShape];
    for (const QRect& h : Anno::shapeHandles(sh)) {
        out << QRect(ir.left() + int(h.x() * sc), ir.top() + int(h.y() * sc),
                     qMax(1, int(h.width() * sc)), qMax(1, int(h.height() * sc)));
    }
    return out;
}

void PinWindow::beginShapeDrag(int what, const QPoint& imgPos)
{
    if (m_selShape < 0 || m_selShape >= m_shapes.size())
        return;
    m_dragWhat = what;
    m_shapePress = imgPos;
    m_shapeSnap = m_shapes[m_selShape];
    m_shapePushed = false;
}

// 拖手柄=按边伸缩，拖内部=整体平移；都从按下时的快照算，避免一点点累积漂移
void PinWindow::applyShapeDrag(const QPoint& imgPos)
{
    if (m_selShape < 0 || m_selShape >= m_shapes.size())
        return;
    Shape& sh = m_shapes[m_selShape];
    const QPoint d = imgPos - m_shapePress;
    if (d.manhattanLength() > 2 && !m_shapePushed) {
        beginStroke();
        m_shapePushed = true;
    }
    if (m_dragWhat == Anno::kMove) {
        sh.a = m_shapeSnap.a + d;
        sh.b = m_shapeSnap.b + d;
    } else if (sh.tool == Arrow) {
        sh.a = m_dragWhat == 0 ? m_shapeSnap.a + d : m_shapeSnap.a;
        sh.b = m_dragWhat == 1 ? m_shapeSnap.b + d : m_shapeSnap.b;
    } else {
        const QRect n = Anno::resizeRect(Anno::shapeRect(m_shapeSnap), m_dragWhat, d, m_pm.rect());
        sh.a = n.topLeft();
        sh.b = n.bottomRight();
    }
    syncResult(false);      // 拖动过程中别每张都 PNG 落盘，松手再写
}

void PinWindow::deleteSelected()
{
    if (m_selShape < 0 || m_selShape >= m_shapes.size())
        return;
    beginStroke();
    m_shapes.removeAt(m_selShape);
    m_selShape = kNoDrag;
    syncResult();
}

// ---- 图上划词：字就长在图上，拖着选，松手就地复制，窗不关 ----

// 图像坐标的格子 → 窗口坐标（跟着显示比例一起缩放，缩放窗口也不会错位）
QRect PinWindow::toWidgetRect(const QRect& ir) const
{
    const double sc = viewScale();
    const QRect r = imageRect();
    return QRect(r.left() + int(ir.x() * sc), r.top() + int(ir.y() * sc),
                 qMax(1, int(ir.width() * sc)), qMax(1, int(ir.height() * sc)));
}

// 底图/形状一变，之前算好的字格全都对不上了：作废并退出模式
void PinWindow::invalidatePick(const char* why)
{
    if (m_pickOn || !m_cells.isEmpty())
        Utils::log(QString("pin pick 作废: 因为 %1（cells=%2 on=%3）")
                       .arg(QString::fromUtf8(why)).arg(m_cells.size()).arg(m_pickOn ? 1 : 0));
    m_cells.clear();
    m_lineBoxes.clear();
    m_picked.clear();
    m_ocrAll.clear();
    m_pickBand = QRect();
    m_pickDrag = false;
    m_pickBusy = false;
    m_pickOn = false;
    m_pickNote.clear();
    if (m_pickBtn)
        m_pickBtn->setChecked(false);
}

void PinWindow::togglePick()
{
    if (m_pickOn) {
        invalidatePick("用户退出划词");
        setCursor(Qt::ArrowCursor);
        update();
        return;
    }
    if (m_pm.isNull())
        return;
    m_tool = None;          // 画图的工具和划词抢同一颗鼠标
    m_selShape = kNoDrag;
    m_pickOn = true;
    if (m_pickBtn)
        m_pickBtn->setChecked(true);
    setCursor(Qt::IBeamCursor);
    update();
    if (m_cells.isEmpty())
        runOcr();
}

void PinWindow::runOcr()
{
    if (m_pickBusy)
        return;
    QString why;
    if (!Ocr::available(&why)) {
        // 没引擎就先下（第一次用那一趟 26MB），进度就写在图上那条提示里，下完自动接着认。
        // 中途按 Esc 只是不要这个回调，下载本身照跑完 —— 下次划词就是就绪状态。
        m_pickBusy = true;
        m_pickNote = why;
        update();
        Utils::log(QString("pin pick: 引擎没就位 %1").arg(why));
        OcrBundle::fetchLater(
            this, [this](int pct, const QString& note) {
                m_pickNote = QStringLiteral("正在下载识别引擎 %1% · %2").arg(pct).arg(note);
                update();
            },
            [this](bool ok, const QString& err, const QString&) {
                m_pickBusy = false;
                if (!m_pickOn)
                    return;
                if (!ok) {
                    m_pickNote = QStringLiteral("识别引擎没装上：%1").arg(err);
                    update();
                    return;
                }
                m_pickNote.clear();
                runOcr();
            });
        return;
    }
    m_pickBusy = true;
    m_pickNote.clear();
    update();
    const QImage img = (m_flat.isNull() ? m_pm : m_flat).toImage();
    Ocr::recognizeLayoutLater(img, this, [this](const QVector<Ocr::Line>& lines,
                                                const QString& err, qint64 ms) {
        m_pickBusy = false;
        if (!m_pickOn)      // 识别这几百毫秒里用户可能已经按了 Esc
            return;
        fillCells(lines);
        if (lines.isEmpty())
            m_pickNote = err.isEmpty() ? QStringLiteral("这张图上没认出文字") : err;
        Utils::log(QString("pin pick: lines=%1 cells=%2 ms=%3 err=%4")
                       .arg(lines.size()).arg(m_cells.size()).arg(ms).arg(err));
        update();
    });
}

// 识别结果 → 一格一字的表。行的先后按引擎给的顺序（它给的就是阅读顺序，
// 纯文本结果能连成句子就靠这个），所以选中的字拼回去不用排序。
void PinWindow::fillCells(const QVector<Ocr::Line>& lines)
{
    m_cells.clear();
    m_lineBoxes.clear();
    m_picked.clear();
    QString all;
    for (int i = 0; i < lines.size(); ++i) {
        const Ocr::Line& l = lines.at(i);
        if (!all.isEmpty())
            all += QLatin1Char('\n');
        all += l.text;
        m_lineBoxes << l.rect;
        for (Ocr::Cell c : Ocr::cells(l)) {
            c.line = i;
            m_cells << c;
        }
    }
    m_ocrAll = all;
}

QString PinWindow::pickProbe(const QRect& band)
{
    QString err;
    fillCells(Ocr::recognizeLayout((m_flat.isNull() ? m_pm : m_flat).toImage(), &err));
    m_pickOn = true;
    m_pickBand = band.normalized();
    m_picked = cellsIn(m_pickBand);
    return pickedText();
}

QVector<int> PinWindow::cellsIn(const QRect& band) const
{
    QVector<int> out;
    if (band.width() <= 3 && band.height() <= 3) {
        // 没拖动=点了一下：整词选中
        int hit = -1;
        for (int i = 0; i < m_cells.size(); ++i) {
            if (m_cells[i].rect.adjusted(-1, -1, 1, 1).contains(band.topLeft())) {
                hit = i;
                break;
            }
        }
        if (hit < 0)
            return out;
        const int ln = m_cells[hit].line, wd = m_cells[hit].word;
        for (int i = 0; i < m_cells.size(); ++i)
            if (m_cells[i].line == ln && m_cells[i].word == wd)
                out << i;
        return out;
    }
    for (int i = 0; i < m_cells.size(); ++i) {
        const Ocr::Cell& c = m_cells[i];
        const QRect lb = m_lineBoxes.value(c.line);
        // 整行被带子盖住 → 这一行全要；只盖住半行的（首尾行）→ 按带的左右范围挑字
        if (lb.isValid() && lb.top() >= band.top() && lb.bottom() <= band.bottom()) {
            out << i;
            continue;
        }
        const QPoint cc = c.rect.center();
        if (cc.x() >= band.left() && cc.x() <= band.right()
            && cc.y() >= band.top() - 4 && cc.y() <= band.bottom() + 4)
            out << i;
    }
    return out;
}

QString PinWindow::pickedText() const
{
    // 引擎会把"软件"拆成两个词，两个都是汉字就别补空格（补了就成"软 件"）
    const auto cjk = [](const QChar ch) {
        const ushort u = ch.unicode();
        return (u >= 0x3000 && u <= 0x303F) || (u >= 0x4E00 && u <= 0x9FFF)
            || (u >= 0xFF00 && u <= 0xFFEF);
    };
    QString out;
    int prev = -1;
    for (const int idx : m_picked) {
        const Ocr::Cell& c = m_cells[idx];
        if (prev >= 0) {
            const Ocr::Cell& p = m_cells[prev];
            if (c.line != p.line)
                out += QLatin1Char('\n');
            else if (c.word != p.word && !(cjk(p.ch) && cjk(c.ch)))
                out += QLatin1Char(' ');
        }
        out += c.ch;
        prev = idx;
    }
    return out;
}

void PinWindow::copyPicked(bool all)
{
    const QString t = all ? m_ocrAll : pickedText();
    if (t.trimmed().isEmpty()) {
        m_pickNote = all ? QStringLiteral("这张图上没认出文字") : QStringLiteral("没选中字");
        update();
        return;
    }
    // Windows 的剪贴板可能被别的程序 momentarily 占着，Qt 的 setText 会**静默失败**：
    // 不写回来的话，界面就成了"说已复制其实没复制"。写一次读一次，最多试三下。
    QClipboard* clip = QGuiApplication::clipboard();
    bool ok = false;
    int tries = 0;
    // 5 次、退避 60/120/180/240ms（合计约 0.9 秒）：实测这台机器上确实出现过 3 次全失败的情况
    // （log.txt 里有 9 条 copy FAILED），那时用户看到的是"没复制上"。
    for (int i = 0; i < 5 && !ok; ++i) {
        clip->setText(t);
        QCoreApplication::processEvents();
        ok = (clip->text() == t);
        tries = i + 1;
        if (!ok)
            QThread::msleep(60 * (i + 1));
    }
    if (ok) {
        m_pickNote = QStringLiteral("已复制 %1 字").arg(t.size());
        Utils::log(QString("pin pick copy: chars=%1 all=%2 试了%3次").arg(t.size()).arg(all ? 1 : 0).arg(tries));
    } else {
        m_pickNote = QStringLiteral("剪贴板此刻被别的程序占着，没复制上，再划一次或按 Ctrl+C");
        Utils::log(QString("pin pick copy FAILED: chars=%1 all=%2 试了%3次").arg(t.size()).arg(all ? 1 : 0).arg(tries));
    }
    update();
}

void PinWindow::smudgeMosaic(const QPoint& imgPos)
{
    // 笔刷在屏幕上保持一样粗，所以按显示比例换算到图像真实像素
    const double sc = viewScale();
    const int radius = qMax(6, int(14.0 / (sc > 0.01 ? sc : 1.0)));
    beginStroke();
    // 直接改 m_pm：撤销栈里那一份（beginStroke 存的）就是这一笔开始前的图，
    // 拿它当打码源，既不用每个鼠标事件复制整张图，也不会把已打码的区域反复打码
    const QPixmap before = m_undo.isEmpty() ? m_pm : m_undo.last().pm;
    Anno::mosaicSmudge(m_pm, before, imgPos, radius);
    syncResult(false);      // 松手时再落盘
}

void PinWindow::applyCrop(const QRect& r)
{
    const QRect cr = r.normalized().intersected(m_pm.rect());
    if (cr.width() < 16 || cr.height() < 16)
        return;
    const QPixmap cut = m_pm.copy(cr);
    beginStroke();
    QVector<Shape> kept;
    for (const Shape& sh : m_shapes) {
        Shape t = sh;
        t.a -= cr.topLeft();
        t.b -= cr.topLeft();
        if (QRect(t.a, t.b).normalized().intersects(cut.rect()))
            kept << t;
    }
    m_selShape = kNoDrag;
    m_edits[m_key] = Edit{ cut, kept };
    setPixmap(cut, false);   // 保留撤销栈：撤销要把尺寸一起还原
    syncResult();
    refreshUndoBtn();
}

void PinWindow::undo()
{
    if (m_undo.isEmpty())
        return;
    m_pushed = false;
    const Snap s = m_undo.takeLast();
    const bool fit = s.pm.size() != m_pm.size();
    m_selShape = kNoDrag;
    m_edits[m_key] = Edit{ s.pm, s.shapes };
    // 撤销矩形/马赛克这种"尺寸没变"的一笔时别顺手把窗口也重置了；只有撤销裁剪才重新适配
    setPixmap(s.pm, false, fit);
    syncResult();
    refreshUndoBtn();
}

void PinWindow::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(24, 25, 29));

    // 左侧历史缩略图（工具栏「历史」按钮可以把它收起来：m_stripW=0 时整段不画）
    if (m_stripW > 0) {
        p.fillRect(0, 0, m_stripW, height() - barH(), QColor(31, 32, 37));
        const int n = CaptureHistory::count();
        for (int i = 0; i < n; ++i) {
            const CaptureHistory::Item it = CaptureHistory::at(i);
            const QRect tr(6, 4 + i * (m_thumbH + 4), m_stripW - 12, m_thumbH);
            const QPixmap thumb = it.pm.scaled(tr.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            p.drawPixmap(tr.center() - thumb.rect().center(), thumb);
            p.setPen(it.key == m_key ? QColor(63, 106, 224) : QColor(255, 255, 255, 40));
            p.drawRect(tr.adjusted(0, 0, -1, -1));
        }
        if (n == 0) {
            p.setPen(QColor(120, 128, 140));
            p.drawText(QRect(0, 0, m_stripW, 60), Qt::AlignCenter, "无历史");
        }
    }

    // 主图：整张缩放进显示区
    const QRect ir = imageRect();
    p.setRenderHint(QPainter::SmoothPixmapTransform, viewScale() < 0.999);
    p.drawPixmap(ir, m_flat.isNull() ? m_pm : m_flat);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.setPen(QColor(63, 106, 224, 160));
    p.drawRect(ir.adjusted(0, 0, -1, -1));

    // 划词层：选中的字涂一块蓝，底下压着的还是原图；左下角一句话说明怎么用
    if (m_pickOn) {
        p.setClipRect(ir);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(63, 106, 224, 120));
        for (const int idx : m_picked)
            p.drawRect(toWidgetRect(m_cells[idx].rect));
        p.setBrush(Qt::NoBrush);
        if (m_pickDrag) {
            p.setPen(QPen(QColor(63, 106, 224), 1, Qt::DashLine));
            p.drawRect(toWidgetRect(m_pickBand));
        }
        p.setClipping(false);
        QString hint;
        if (m_pickBusy)
            hint = QStringLiteral("正在认字…（第一次要几百毫秒）");
        else if (!m_pickNote.isEmpty())
            hint = m_pickNote;
        else if (m_cells.isEmpty())
            hint = QStringLiteral("这张图上没认出可点的字");
        else
            hint = QStringLiteral("拖动选字 · 单击选整词 · 松手即复制 · Enter 复制全部 · Esc 退出");
        p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9));
        const QFontMetrics fm(p.font());
        const QRect chip(ir.left() + 6, ir.bottom() - fm.height() - 14,
                         fm.horizontalAdvance(hint) + 16, fm.height() + 8);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(24, 25, 29, 220));
        p.drawRoundedRect(chip, 4, 4);
        p.setPen(QColor(232, 235, 242));
        p.drawText(chip, Qt::AlignCenter, hint);
    }

    // 选中的形状：虚线包围盒 + 手柄，跟截图浮层一个样子
    if (m_selShape >= 0 && m_selShape < m_shapes.size()) {
        const double sc = viewScale();
        const QRect sr = Anno::shapeRect(m_shapes[m_selShape]);
        const QRect vsr(ir.left() + int(sr.x() * sc), ir.top() + int(sr.y() * sc),
                        qMax(1, int(sr.width() * sc)), qMax(1, int(sr.height() * sc)));
        p.setPen(QPen(QColor(63, 106, 224), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(vsr.adjusted(-2, -2, 2, 2));
        p.setPen(QColor(63, 106, 224));
        p.setBrush(Qt::white);
        for (const QRect& h : widgetShapeHandles())
            p.drawRect(h);
    }

    // 拖中的一笔预览（Alt 涂抹那种边拖边改，不需要预览）
    if (m_editing && !(m_tool == Mosaic && m_mosaicBrush)) {
        const double sc = viewScale();
        if (m_tool == Crop) {
            const QRect r = QRect(m_pressImg, m_lastImg).normalized();
            const QRect vr(r.left() * sc + ir.left(), r.top() * sc + ir.top(),
                           r.width() * sc, r.height() * sc);
            p.setClipRegion(QRegion(ir) - vr);
            p.fillRect(ir, QColor(0, 0, 0, 130));
            p.setClipping(false);
            p.setPen(QPen(Qt::white, 1, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(vr);
        } else {
            p.save();
            p.setClipRect(ir);
            p.translate(ir.topLeft());
            p.scale(sc, sc);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(strokePen());
            p.setBrush(Qt::NoBrush);
            Anno::drawShape(p, m_tool, QRect(m_pressImg, m_lastImg).normalized(),
                            m_pressImg, m_lastImg);
            p.restore();
        }
    }
}

int PinWindow::thumbIndexAt(const QPoint& pos) const
{
    // 历史条占 0..m_stripW-1 这一整列，图从 m_stripW 开始：边界那一列只能算一个，
    // 否则贴着图左沿按一下就被当成"换图"，划词的格子跟着没了
    if (pos.x() >= m_stripW)
        return -1;
    const int i = (pos.y() - 4) / (m_thumbH + 4);
    if (i < 0 || i >= CaptureHistory::count())
        return -1;
    return i;
}

void PinWindow::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::RightButton) {
        const int ti = thumbIndexAt(e->pos());
        if (ti >= 0)
            thumbContextMenu(ti, e->globalPosition().toPoint());   // 右键往期缩略图 = 删掉那一张
        else
            showContextMenu(e->globalPosition().toPoint());
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    const Qt::Edges edge = edgeAt(e->pos());
    // 划词模式开着的时候，图内的按压一律算选字。默认收起工具栏之后窗口就是图片本身，
    // 图最外圈那 8px 也正好是要选的字，不能被"缩放手柄"吃掉（要缩放先 Esc 退出划词）。
    if (edge != Qt::Edges() && !(m_pickOn && inImage(e->pos()))) {
        m_edge = edge;
        m_geoAtPress = geometry();
        m_pressGlobal = e->globalPosition().toPoint();
        return;
    }
    const int ti = thumbIndexAt(e->pos());
    if (ti >= 0) {
        // 换图 = 换一次编辑会话，撤销栈跟着清掉（不然撤销会把上一张的中间态端上来）
        const CaptureHistory::Item it = CaptureHistory::at(ti);
        m_key = it.key;
        m_editing = false;
        setPixmap(it.pm, true, false);   // 和箭头一样：换图不换窗口大小
        return;
    }
    if (m_pickOn && inImage(e->pos())) {
        m_pickDrag = true;
        m_pickPress = toImage(e->pos());
        m_pickBand = QRect(m_pickPress, m_pickPress);
        m_pickNote.clear();
        m_picked = cellsIn(m_pickBand);
        update();
        return;
    }
    if (m_tool != None) {
        if (!inImage(e->pos()))
            return;   // 开着工具时不许拖窗口，否则会误以为"拖不动就是在画图"
        m_editing = true;
        m_pushed = false;
        m_mosaicBrush = m_tool == Mosaic && (e->modifiers() & Qt::AltModifier);
        m_pressImg = m_lastImg = toImage(e->pos());
        update();
        return;
    }
    // 不画图的那把工具下：点已画的框/圆/箭头/打码块 = 选中，选中后拖=移动、拖手柄=改大小
    if (inImage(e->pos())) {
        const QPoint ip = toImage(e->pos());
        const int h = handleAt(e->pos());
        if (h != kNoDrag) {
            beginShapeDrag(h, ip);
            return;
        }
        const int hit = Anno::hitShape(m_shapes, ip);
        if (hit >= 0) {
            m_selShape = hit;
            beginShapeDrag(Anno::kMove, ip);
            update();
            return;
        }
        if (m_selShape != kNoDrag) {
            m_selShape = kNoDrag;
            update();
        }
    }
    m_press = e->globalPosition().toPoint() - geometry().topLeft();
    m_dragging = true;
}

void PinWindow::mouseMoveEvent(QMouseEvent* e)
{
    if (m_edge != Qt::Edges() && (e->buttons() & Qt::LeftButton)) {
        QRect g = m_geoAtPress;
        const QPoint d = e->globalPosition().toPoint() - m_pressGlobal;
        if (m_edge & Qt::LeftEdge)
            g.setLeft(qMin(g.left() + d.x(), g.right() - 140));
        if (m_edge & Qt::RightEdge)
            g.setRight(qMax(g.right() + d.x(), g.left() + 140));
        if (m_edge & Qt::TopEdge)
            g.setTop(qMin(g.top() + d.y(), g.bottom() - 100));
        if (m_edge & Qt::BottomEdge)
            g.setBottom(qMax(g.bottom() + d.y(), g.top() + 100));
        setGeometry(g);
        return;
    }
    if (m_dragWhat != kNoDrag && m_selShape >= 0 && (e->buttons() & Qt::LeftButton)) {
        applyShapeDrag(toImage(e->pos()));
        return;
    }
    if (m_pickDrag && (e->buttons() & Qt::LeftButton)) {
        m_pickBand = QRect(m_pickPress, toImage(e->pos())).normalized();
        m_picked = cellsIn(m_pickBand);
        update();
        return;
    }
    if (m_editing) {
        m_lastImg = toImage(e->pos());
        if (m_tool == Mosaic && m_mosaicBrush)
            smudgeMosaic(m_lastImg);
        else
            update();
        return;
    }
    if (m_dragging && e->buttons() & Qt::LeftButton) {
        move(e->globalPosition().toPoint() - m_press);
        return;
    }
    if (!(e->buttons() & Qt::LeftButton) && m_tool == None) {
        // 光标提示哪条边可以拖
        const Qt::Edges edge = edgeAt(e->pos());
        if (edge == (Qt::LeftEdge | Qt::TopEdge) || edge == (Qt::RightEdge | Qt::BottomEdge))
            setCursor(Qt::SizeFDiagCursor);
        else if (edge == (Qt::RightEdge | Qt::TopEdge) || edge == (Qt::LeftEdge | Qt::BottomEdge))
            setCursor(Qt::SizeBDiagCursor);
        else if (edge & Qt::LeftEdge || edge & Qt::RightEdge)
            setCursor(Qt::SizeHorCursor);
        else if (edge & Qt::TopEdge || edge & Qt::BottomEdge)
            setCursor(Qt::SizeVerCursor);
        else
            unsetCursor();
    }
}

void PinWindow::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_edge != Qt::Edges()) {
        m_edge = Qt::Edges();
        return;
    }
    if (m_pickDrag) {
        m_pickDrag = false;
        if (!m_picked.isEmpty())
            copyPicked();      // 松手就进剪贴板，窗不关，接着划下一处
        else
            m_pickBand = QRect();
        update();
        return;
    }
    if (m_dragWhat != kNoDrag) {
        const bool moved = m_shapePushed;
        m_dragWhat = kNoDrag;
        if (moved)
            syncResult();          // 拖完才写一次盘，过程中只改内存
        update();
        return;
    }
    if (m_editing) {
        m_editing = false;
        const QPoint upTo = toImage(e->pos());
        if (m_tool == Crop)
            applyCrop(QRect(m_pressImg, upTo));
        else if (m_tool == Mosaic && m_mosaicBrush)
            syncResult();          // 涂抹只改底图，松手写一次盘
        else
            finishShape(upTo);
        m_mosaicBrush = false;
        update();
        return;
    }
    m_dragging = false;
}

void PinWindow::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (m_pickOn)
        return;        // 划词模式下单击已经在选词了，双击不能再把窗口关掉
    if (m_tool == None && e->pos().x() > m_stripW)
        close();
}

void PinWindow::keyPressEvent(QKeyEvent* e)
{
    if (m_pickOn) {
        if (e->key() == Qt::Key_Escape) {     // 先退出划词，别一按 Esc 就把截图窗关了
            invalidatePick("按 Esc 退出划词");
            setCursor(Qt::ArrowCursor);
            update();
            return;
        }
        if (e->matches(QKeySequence::Copy)) {
            copyPicked(!m_picked.isEmpty());
            return;
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            copyPicked(true);                  // 没挑、整张都要
            return;
        }
    }
    if (e->key() == Qt::Key_Escape) {
        if (m_selShape != kNoDrag) {      // 先取消选中，别一按 Esc 就把截图窗关了
            m_selShape = kNoDrag;
            update();
            return;
        }
        close();
        return;
    }
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
        deleteSelected();
        return;
    }
    if ((e->modifiers() & Qt::AltModifier) && e->key() == Qt::Key_Left) {
        stepHistory(-1);
        return;
    }
    if ((e->modifiers() & Qt::AltModifier) && e->key() == Qt::Key_Right) {
        stepHistory(1);
        return;
    }
    if (e->matches(QKeySequence::Undo)) {
        undo();
        return;
    }
    QWidget::keyPressEvent(e);
}

void PinWindow::buildToolbar()
{
    m_toolbar = new QWidget(this);
    m_toolbar->setAttribute(Qt::WA_StyledBackground);
    m_toolbar->setStyleSheet(
        "QWidget{background:#26272c;border-top:1px solid #3a3c45;}"
        "QPushButton{background:transparent;color:#e8ebf2;border:none;border-radius:6px;"
        "padding:4px 8px;font-size:12px;}"
        "QPushButton:hover{background:#3a3d46;}"
        "QPushButton:checked{background:#3f6ae0;}"
        "QPushButton:disabled{color:#6b7280;}");
    auto* lay = new QHBoxLayout(m_toolbar);
    lay->setContentsMargins(6, 2, 6, 2);
    lay->setSpacing(2);

    // label 是给读屏/自动化用的可访问名：图标按钮没有文字，UIA 里查不到它们
    const struct { const char* tip; const char* label; Tool tool; } tools[] = {
        {"不画图：拖窗口；点已画的框/圆/箭头/打码块 = 选中，可拖动、拉手柄改大小、Delete 删除", "拖动/选择", None},
        {"矩形", "矩形", Rect}, {"圆", "圆", Ellipse}, {"箭头", "箭头", Arrow},
        {"马赛克：拖一个框 = 打码块（之后可以选中移动/拉手柄/Delete 删除）；按住 Alt 拖 = 自由涂抹，烙进图里改不了", "马赛克", Mosaic},
        {"裁剪（拖动框出要保留的部分）", "裁剪", Crop},
    };
    auto* group = new QButtonGroup(m_toolbar);
    group->setExclusive(true);
    for (const auto& t : tools) {
        auto* b = new QPushButton(m_toolbar);
        b->setCheckable(true);
        b->setToolTip(t.tip);
        b->setAccessibleName(QString::fromUtf8(t.label));
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedWidth(t.tool == None ? 62 : 30);
        if (t.tool == None)
            b->setText(QStringLiteral("拖动/选择"));
        else
            b->setIcon(Anno::toolIcon(t.tool));
        group->addButton(b);
        if (t.tool == None)
            b->setChecked(true);
        connect(b, &QPushButton::clicked, this, [this, tool = t.tool] {
            m_tool = tool;
            m_editing = false;
            // 换到画图的工具时先放开选中的形状，免得手柄跟着画新笔；
            // 切到「拖动/选择」要留住刚画的那一个，否则拖手柄会变成整体挪动
            if (tool != None)
                m_selShape = kNoDrag;
            setCursor(tool == None ? Qt::ArrowCursor : Qt::CrossCursor);
            update();
        });
        lay->addWidget(b);
    }
    lay->addSpacing(8);
    m_undoBtn = new QPushButton("撤销", m_toolbar);
    m_undoBtn->setToolTip("撤销上一步图片操作（Ctrl+Z）");
    m_undoBtn->setCursor(Qt::PointingHandCursor);
    connect(m_undoBtn, &QPushButton::clicked, this, &PinWindow::undo);
    lay->addWidget(m_undoBtn);
    auto* copyBtn = new QPushButton("复制", m_toolbar);
    copyBtn->setToolTip("把当前（含标注）的图复制到剪贴板");
    connect(copyBtn, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setPixmap(m_flat.isNull() ? m_pm : m_flat);
    });
    lay->addWidget(copyBtn);
    auto* ocrBtn = new QPushButton("划词", m_toolbar);
    ocrBtn->setToolTip("在这张图上直接划着选字（像微信那样）\n"
                       "拖动选一片，单击选整词，松手就进剪贴板，窗不关\n"
                       "Enter 复制整张图的文字，Esc 退出\n"
                       "要改字或存 txt：在图上右键 →「识别全文」\n"
                       "用的是自带的 PaddleOCR（第一次用会先下模型，之后离线）");
    ocrBtn->setCheckable(true);
    ocrBtn->setCursor(Qt::PointingHandCursor);
    ocrBtn->setAccessibleName(QStringLiteral("划词"));
    connect(ocrBtn, &QPushButton::clicked, this, [this] { togglePick(); });
    m_pickBtn = ocrBtn;
    lay->addWidget(ocrBtn);
    m_histBtn = new QPushButton(QStringLiteral("历史"), m_toolbar);
    m_histBtn->setToolTip("显示 / 收起左边那条往期截图。\n"
                          "想把底部这条工具栏也收起来：在图上右键 →「隐藏工具栏」。");
    m_histBtn->setCheckable(true);
    m_histBtn->setChecked(true);   // 默认往期带是显示着的
    m_histBtn->setCursor(Qt::PointingHandCursor);
    m_histBtn->setAccessibleName(QStringLiteral("历史"));
    connect(m_histBtn, &QPushButton::clicked, this, [this] { setStripHidden(!m_histBtn->isChecked()); });
    lay->addWidget(m_histBtn);
    auto* saveBtn = new QPushButton("保存", m_toolbar);
    saveBtn->setToolTip("另存为 PNG");
    connect(saveBtn, &QPushButton::clicked, this, [this] {
        const QString path = QString("%1/pin-%2.png")
                                 .arg(Utils::capturesDir())
                                 .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
        if ((m_flat.isNull() ? m_pm : m_flat).save(path))
            Utils::revealInExplorer(path);
    });
    lay->addWidget(saveBtn);
    lay->addStretch(1);
    m_pinBtn = new QPushButton(QStringLiteral("📌 自动让位"), m_toolbar);
    m_pinBtn->setToolTip("默认：别的程序被激活时，截图窗不再压着它；点一下截图窗又回到最前。\n"
                         "按这里改成「一直置顶」。");
    m_pinBtn->setCursor(Qt::PointingHandCursor);
    m_pinBtn->setFixedWidth(96);
    connect(m_pinBtn, &QPushButton::clicked, this, [this] {
        m_pinned = !m_pinned;
        m_pinBtn->setText(m_pinned ? QStringLiteral("📌 一直置顶")
                                   : QStringLiteral("📌 自动让位"));
        m_pinBtn->setChecked(m_pinned);
        applyTopmost(true);
    });
    lay->addWidget(m_pinBtn);
    auto* closeBtn = new QPushButton(QStringLiteral("✕ 关闭"), m_toolbar);
    closeBtn->setToolTip("关闭这个截图窗（Esc / 双击图片 也可以，右键是复制和另存为）");
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setFixedWidth(60);
    connect(closeBtn, &QPushButton::clicked, this, &PinWindow::close);
    lay->addWidget(closeBtn);
    refreshUndoBtn();
    placeToolbar();
}

// 右键菜单：这张图最常做的两件事就是"再复制一遍"和"存成文件"，
// 所以放到图上右键就能做，不用非看清底部工具栏那几个字。
void PinWindow::showContextMenu(const QPoint& globalPos)
{
    QMenu menu(this);
    // 工具栏收起来的时候，图上右键只留两样：把工具栏叫回来、关掉这张。
    // 这时候人是要干净地看图，不该再被一排菜单挡一次。
    if (m_barHidden) {
        QAction* showAct = menu.addAction(QStringLiteral("显示工具栏"));
        QAction* closeOnly = menu.addAction(QStringLiteral("关闭"));
        QAction* got = menu.exec(globalPos);
        if (got == showAct)
            setBarHidden(false);
        else if (got == closeOnly)
            close();
        return;
    }
    QAction* copyAct = menu.addAction(QStringLiteral("复制到剪贴板"));
    QAction* saveAct = menu.addAction(QStringLiteral("另存为…"));
    QAction* ocrAct = menu.addAction(QStringLiteral("识别全文（可编辑/存 txt）"));
    menu.addSeparator();
    QAction* histAct = menu.addAction(m_stripHidden ? QStringLiteral("显示往期截图")
                                                    : QStringLiteral("收起往期截图"));
    QAction* hideBarAct = menu.addAction(QStringLiteral("隐藏工具栏"));
    menu.addSeparator();
    QAction* closeAct = menu.addAction(QStringLiteral("关闭"));
    QAction* picked = menu.exec(globalPos);
    if (!picked)
        return;
    if (picked == histAct) {
        setStripHidden(!m_stripHidden);
    } else if (picked == hideBarAct) {
        setBarHidden(true);
    } else if (picked == copyAct) {
        QApplication::clipboard()->setPixmap(m_pm);
    } else if (picked == ocrAct) {
        OcrPanel::extractFrom((m_flat.isNull() ? m_pm : m_flat).toImage(), this);
    } else if (picked == saveAct) {
        const QString def = QString("%1/pin-%2.png")
                                .arg(Utils::capturesDir())
                                .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("另存为截图"),
                                                          def, QStringLiteral("PNG 图片 (*.png)"));
        if (!path.isEmpty() && m_pm.save(path))
            Utils::revealInExplorer(path);
    } else {
        close();
    }
}

QString PinWindow::deleteThumb(int index)
{
    const CaptureHistory::Item it = CaptureHistory::at(index);
    if (it.pm.isNull())
        return m_key;                              // 越界：什么都不动
    const bool wasCurrent = !m_key.isEmpty() && it.key == m_key;
    CaptureHistory::removeAt(index);
    if (!it.key.isEmpty())
        m_edits.remove(it.key);                    // 这张的底图+形状也别留在内存里
    if (!wasCurrent) {
        update();
        return m_key;
    }
    // 删的正是当前看着的这张：挪到它原来位置的邻居（下面没有了就往上挪一格）
    const int n = CaptureHistory::count();
    m_editing = false;
    if (n == 0) {
        m_key = QString();   // 往期空了：这张继续显示着，但改动没处可写回（replace 对空 key 是空操作）
        update();
        return m_key;
    }
    const CaptureHistory::Item nx = CaptureHistory::at(qMin(index, n - 1));
    m_key = nx.key;
    setPixmap(nx.pm, true, false);   // 换图不换窗口大小，和点缩略图那条路一致
    return m_key;
}

void PinWindow::thumbContextMenu(int index, const QPoint& globalPos)
{
    QMenu menu(this);
    QAction* del = menu.addAction(QStringLiteral("删除这一张"));
    if (menu.exec(globalPos) == del)
        deleteThumb(index);
}

// 图片左右两边各一个箭头：上一张 / 下一张（历史就是左侧那一列，箭头只是不用去点缩略图）
void PinWindow::buildNav()
{
    const auto mk = [this](const QString& text, const QString& tip) {
        auto* b = new QPushButton(text, this);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(26, 46);
        b->setStyleSheet(
            "QPushButton{background:rgba(38,39,44,200);color:#e8ebf2;border:1px solid #3a3c45;"
            "border-radius:6px;font-size:15px;}"
            "QPushButton:hover{background:rgba(63,106,224,230);}"
            "QPushButton:disabled{color:#6b7280;background:rgba(38,39,44,120);}");
        return b;
    };
    m_prevBtn = mk(QStringLiteral("‹"), QStringLiteral("上一张截图（Alt+←）"));
    m_nextBtn = mk(QStringLiteral("›"), QStringLiteral("下一张截图（Alt+→）"));
    connect(static_cast<QPushButton*>(m_prevBtn), &QPushButton::clicked, this, [this] { stepHistory(-1); });
    connect(static_cast<QPushButton*>(m_nextBtn), &QPushButton::clicked, this, [this] { stepHistory(1); });
    placeNav();
}

void PinWindow::placeNav()
{
    if (!m_prevBtn || !m_nextBtn)
        return;
    const int areaH = height() - barH();
    const int y = (areaH - m_prevBtn->height()) / 2;
    // 左箭头贴在缩略图条右侧、右箭头离右边框 12px：都不要压着 8px 的缩放手柄，
    // 否则中间那一段会既点不动缩略图又拖不了边框。
    m_prevBtn->move(m_stripW + 8, y);
    m_nextBtn->move(width() - m_nextBtn->width() - 12, y);
    const int n = CaptureHistory::count();
    int cur = -1;
    for (int i = 0; i < n; ++i)
        if (CaptureHistory::at(i).key == m_key)
            cur = i;
    m_prevBtn->setVisible(n > 1);
    m_nextBtn->setVisible(n > 1);
    m_prevBtn->setEnabled(cur > 0);
    m_nextBtn->setEnabled(cur >= 0 && cur + 1 < n);
    m_prevBtn->raise();
    m_nextBtn->raise();
}

void PinWindow::stepHistory(int delta)
{
    const int n = CaptureHistory::count();
    int cur = 0;
    for (int i = 0; i < n; ++i)
        if (CaptureHistory::at(i).key == m_key)
            cur = i;
    const int next = qBound(0, cur + delta, n - 1);
    if (next == cur)
        return;
    const CaptureHistory::Item it = CaptureHistory::at(next);
    if (it.pm.isNull())
        return;
    m_key = it.key;
    m_editing = false;
    setPixmap(it.pm, true, false);   // 保持当前窗口尺寸，图自己缩放进去
    placeNav();
}

int PinWindow::barH() const
{
    return m_barHidden ? 0 : kToolbarH;
}

// 收起/放出工具栏。关键是**图区尺寸一点不动**：窗口高度跟着工具栏一起收放。
// 尺寸不能靠"当前值加减"来还原：窗口有最小尺寸（工具栏那一排按钮的宽度），
// 收起时可能被夹住、放出时又加一遍，来回一次就不等价了 —— 所以记住收起前的尺寸，放出时原样还。
// 工具栏收起时窗口应该抱多紧：往期带（可能已经是 0）+ 图实际占的那块。
// 两个开关都要走它 —— 不然先收工具栏把最小宽度定成"带+图"，再收往期带就缩不动了。
void PinWindow::applyHug()
{
    if (!m_barHidden)
        return;
    const QRect fit = imageRect();
    const int hug = qMax(60, m_stripW + fit.width());
    setMinimumWidth(hug);
    resize(hug, qMax(60, fit.height()));
}

void PinWindow::setBarHidden(bool hide)
{
    if (m_barHidden == hide)
        return;
    const QRect fit = imageRect();   // 收起前图实际占的那块（含往期带右边的居中留白）
    m_barHidden = hide;
    if (m_toolbar)
        m_toolbar->setVisible(!hide);
    if (hide) {
        m_sizeBeforeBar = size();
        m_minWBeforeBar = minimumWidth();
        // 工具栏都不显示了，就不该再拿它的宽度当底线：这时候让窗口抱住图，
        // 左右那点黑边（最小宽度是工具栏撑出来的）一并收掉
        const int hug = qMax(60, m_stripW + fit.width());
        setMinimumWidth(hug);
        resize(hug, qMax(60, fit.height()));
    } else {
        if (m_minWBeforeBar > 0)
            setMinimumWidth(m_minWBeforeBar);
        resize(m_sizeBeforeBar.width(), m_sizeBeforeBar.height());
    }
    placeToolbar();
    placeNav();
    update();
    Utils::log(QStringLiteral("pin: 工具栏%1").arg(hide ? QStringLiteral("收起") : QStringLiteral("放出")));
}

// 左边那条往期带同理：m_stripW 就是"现在占多宽"，收起来置 0 之后
// thumbIndexAt / 拖动命中 全都自动不再认领那一列，不用到处加 if
void PinWindow::setStripHidden(bool hide)
{
    if (m_stripHidden == hide)
        return;
    m_stripHidden = hide;
    const int was = m_stripW;
    m_stripW = hide ? 0 : kStripW;
    if (m_histBtn)
        m_histBtn->setChecked(!hide);   // 右键菜单也能改，按钮状态要跟着走
    const int minW = qMax(60, minimumWidth());
    if (hide) {
        m_sizeBeforeStrip = size();
        resize(qMax(minW, width() + (m_stripW - was)), height());
    } else {
        resize(qMax(minW, m_sizeBeforeStrip.width()), height());
    }
    applyHug();   // 工具栏也收着的话，往期带这一收窗口还能再窄一截
    placeNav();
    update();
    Utils::log(QStringLiteral("pin: 往期带%1").arg(hide ? QStringLiteral("收起") : QStringLiteral("放出")));
}

void PinWindow::placeToolbar()
{
    if (m_toolbar)
        m_toolbar->setGeometry(0, height() - barH(), width(), barH() - kGrab);
}

void PinWindow::refreshUndoBtn()
{
    if (!m_undoBtn)
        return;
    m_undoBtn->setEnabled(!m_undo.isEmpty());
    m_undoBtn->setText(m_undo.isEmpty() ? QStringLiteral("撤销")
                                       : QString("撤销(%1)").arg(m_undo.size()));
}
