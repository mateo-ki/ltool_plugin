// 图片排版插件自带的判据：格数/列行、收图与拒收、换顺序、合成尺寸与像素、落盘。
//
// 全部离屏跑（不 show 窗口、不合成鼠标键盘事件）：拖放那条链用 addImages()/moveOrSwap()/
// removeAt() 直接驱动 —— 它们就是 dropEvent/mousePress 真正调的那三个函数，不是抄的第二遍。
// 合成图用肉眼看不出的方式验：拿纯色图，验某一格中心那个像素就是那张图的颜色。
#include "GridBoard.h"
#include "GridPage.h"
#include "Utils.h"

#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QLayout>
#include <QSet>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QMouseEvent>
#include <QPushButton>
#include <QStringList>

#include <cstdio>
#include <functional>

namespace {

bool reachedFrom(QWidget* root, QWidget* w)
{
    bool found = false;
    std::function<void(QLayout*)> walk = [&](QLayout* l) {
        if (!l || found)
            return;
        for (int i = 0; i < l->count(); ++i) {
            QLayoutItem* it = l->itemAt(i);
            if (!it)
                continue;
            if (QWidget* cw = it->widget()) {
                if (cw == w) { found = true; return; }
                walk(cw->layout());
            } else if (QLayout* sub = it->layout())
                walk(sub);
        }
    };
    walk(root->layout());
    return found;
}

void clickPageCell(GridPage& page, int i)
{
    GridBoard* b = page.board();
    const QPoint c = b->cellRect(i).center();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(c), b->mapToGlobal(c), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(c), b->mapToGlobal(c), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(b, &press);
    QApplication::sendEvent(b, &release);
}

} // namespace

extern "C" Q_DECL_EXPORT int ltoolPluginSelftest()
{
    int bad = 0;
    const auto check = [&bad](const char* name, const QString& got, const QString& want) {
        std::printf("%s %s got=[%s] want=[%s]\n", got == want ? "PASS" : "FAIL", name,
                    got.toUtf8().constData(), want.toUtf8().constData());
        if (got != want)
            ++bad;
        std::fflush(stdout);
    };
    const auto baseNames = [](const QVector<QString>& paths) {
        QStringList out;
        for (const QString& p : paths)
            out << (p.isEmpty() ? QStringLiteral("-") : QFileInfo(p).completeBaseName());
        return out.join(QLatin1Char(','));
    };

    // 造九张纯色图 + 一个假图片文件（后缀对、内容不是图）到临时目录
    const QString box = QDir::tempPath() + QStringLiteral("/ltool-grid-selftest");
    (void)QDir(box).removeRecursively();   // 清不掉也不算这次判据的事（Qt6 上它带 nodiscard）
    QDir().mkpath(box);
    const QVector<QColor> colors = { QColor(220, 30, 30), QColor(30, 200, 30), QColor(30, 30, 210),
                                     QColor(230, 200, 20), QColor(200, 40, 200), QColor(20, 200, 200),
                                     QColor(120, 60, 30), QColor(60, 120, 240), QColor(240, 120, 60) };
    QStringList files;
    for (int i = 0; i < colors.size(); ++i) {
        QImage im(64, 40, QImage::Format_RGB32);
        im.fill(colors[i]);
        const QString f = box + QStringLiteral("/c%1.png").arg(i);
        im.save(f, "PNG");
        files << f;
    }
    const QString fake = box + QStringLiteral("/notimage.txt");
    {
        QFile f(fake);
        check("夹具写得出来", f.open(QIODevice::WriteOnly) ? "yes" : "no", "yes");
        f.write("这不是图片");
    }

    GridPage page;
    page.setAttribute(Qt::WA_DontShowOnScreen, true);
    page.resize(720, 720);

    check("默认四宫", QString::number(page.slotCount()), "4");
    check("四宫 = 2 列 2 行", QStringLiteral("%1x%2").arg(page.board()->cols()).arg(page.board()->rows()), "2x2");
    page.setSlots(6);
    check("六宫 = 3 列 2 行", QStringLiteral("%1x%2").arg(page.board()->cols()).arg(page.board()->rows()), "3x2");
    page.setSlots(9);
    check("九宫 = 3 列 3 行", QStringLiteral("%1x%2").arg(page.board()->cols()).arg(page.board()->rows()), "3x3");
    // 换格数只截断，不重排：九格里摆满再退回四宫，前面四张必须还在原位
    page.board()->clearImages();
    page.board()->addImages(files);
    page.setSlots(4);
    check("换格数不丢前面摆好的图（9→4 还是那四张）", baseNames(page.board()->paths()), "c0,c1,c2,c3");


    page.setSlots(4);
    page.board()->clearImages();
    page.board()->addImages(files);            // 四格收九张
    check("四格只收四张，其余算拒收", QStringLiteral("%1/%2").arg(page.board()->filled()).arg(page.board()->rejectedCount()), "4/5");
    check("收进来的顺序就是给定顺序", baseNames(page.board()->paths()), "c0,c1,c2,c3");
    page.board()->clearImages();
    page.board()->addImages({ fake });
    check("非图片后缀一个都不收", QString::number(page.board()->filled()), "0");
    page.board()->clearImages();
    page.board()->addImages(files.mid(0, 3));
    page.board()->moveOrSwap(0, 3);            // 目标是空格 = 挪过去
    check("拖到空格 = 挪过去（原来那格空出来）", baseNames(page.board()->paths()), "-,c1,c2,c0");
    page.board()->moveOrSwap(1, 2);            // 两边都有图 = 交换
    check("拖到有图的格 = 交换", baseNames(page.board()->paths()), "-,c2,c1,c0");
    page.board()->moveOrSwap(0, 1);            // 空格当起点 = 什么都不动
    check("空格拖不动（不会把空格盖到别的格上）", baseNames(page.board()->paths()), "-,c2,c1,c0");
    page.board()->removeAt(2);
    check("拿掉一格只空那一格", baseNames(page.board()->paths()), "-,c2,-,c0");

    // 合成：四宫、每格 60、缝 10、背景红
    page.setSlots(4);
    page.board()->clearImages();
    page.board()->addImages(files.mid(0, 4));
    const QImage out = page.compose(60, 10, QColor(255, 0, 0));
    check("四宫合成尺寸 = 60*2 + 10*3", QStringLiteral("%1x%2").arg(out.width()).arg(out.height()), "150x150");
    check("四边和缝都是背景色", out.pixelColor(3, 3).name(QColor::HexRgb), QStringLiteral("#ff0000"));
    check("第一格中心 = 第一张图的颜色", out.pixelColor(40, 40).name(QColor::HexRgb), colors[0].name(QColor::HexRgb));
    check("第四格中心 = 第四张图的颜色", out.pixelColor(110, 110).name(QColor::HexRgb), colors[3].name(QColor::HexRgb));
    page.board()->removeAt(1);
    const QImage hole = page.compose(60, 10, QColor(255, 0, 0));
    check("空格子就是背景色（不是黑的）", hole.pixelColor(110, 40).name(QColor::HexRgb), QStringLiteral("#ff0000"));

    // 裁的是中间那块正方形，不是把整张压扁：拿一张 120x40 的横图（红｜绿｜蓝 三条竖带），
    // 中间那条正好是 40x40 —— 压扁的话左右两边会漏出红和蓝。
    const QString band = box + QStringLiteral("/band.png");
    {
        QImage im(120, 40, QImage::Format_RGB32);
        im.fill(QColor(30, 200, 30));
        QPainter bp(&im);
        bp.fillRect(0, 0, 40, 40, QColor(220, 30, 30));
        bp.fillRect(80, 0, 40, 40, QColor(30, 30, 210));
        bp.end();
        im.save(band, "PNG");
    }
    page.board()->clearImages();
    page.board()->addImages({ band });
    const QImage wide = page.compose(60, 10, QColor(255, 0, 0));
    check("横图合成只留中间那块（左边不是红）", wide.pixelColor(12, 40).name(QColor::HexRgb),
          QStringLiteral("#1ec81e"));
    check("横图合成只留中间那块（右边不是蓝）", wide.pixelColor(68, 40).name(QColor::HexRgb),
          QStringLiteral("#1ec81e"));

    // 板上预览必须是同一块构图，否则人摆好的和生成出来的是两张图
    page.board()->setFixedSize(300, 300);        // 四宫：留白 8、缝 8 → 每格 138 见方
    page.board()->ensurePolished();
    const QImage shot = page.board()->grab().toImage();
    check("板上预览也是中间那块（左边不是红）", shot.pixelColor(13, 77).name(QColor::HexRgb),
          QStringLiteral("#1ec81e"));
    check("板上预览也是中间那块（右边不是蓝）", shot.pixelColor(141, 77).name(QColor::HexRgb),
          QStringLiteral("#1ec81e"));
    // 这条是探针自己的体检：离屏 grab 万一画出来是张空图，上面两条会"一起绿"骗人。
    // ✕ 那颗角标是深色半透明底，压在绿图上一定比图本身暗。
    check("板上的角标真的画出来了（grab 不是空图）",
          shot.pixelColor(130, 23).green() < shot.pixelColor(77, 77).green() ? "yes" : "no", "yes");
    page.board()->setMinimumSize(320, 320);
    page.board()->setMaximumSize(QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));

    // 坏文件：不存在的 .png 不收；后缀对、内容不是图的收进来但要说实话
    page.board()->clearImages();
    page.board()->addImages({ box + QStringLiteral("/nope.png") });
    check("盘上没有这个文件就不收（不会摆一个空位骗人）", QString::number(page.board()->filled()), "0");
    const QString broken = box + QStringLiteral("/broken.png");
    {
        QFile f(broken);
        check("夹具写得出来", f.open(QIODevice::WriteOnly) ? "yes" : "no", "yes");
        f.write("这不是 PNG 的字节");
    }
    page.board()->addImages({ broken });
    check("后缀对的坏文件先进格子（要让人看到自己拖错了）", QString::number(page.board()->filled()), "1");
    const QImage badCell = page.compose(60, 10, QColor(255, 0, 0));
    check("坏文件那一格合成时留背景色（不画成黑的）", badCell.pixelColor(40, 40).name(QColor::HexRgb),
          QStringLiteral("#ff0000"));
    const QImage badShot = page.board()->grab().toImage();
    const QRect s0 = page.board()->cellRect(0), s1 = page.board()->cellRect(1);
    check("坏图格与空格这两处采的不是同一个像素（不然这条是白过的）",
          s0.right() < s1.left() ? "yes" : "no", "yes");
    check("坏文件那一格和空格子画得一样素（不是一张黑块）",
          badShot.pixelColor(s0.left() + 5, s0.top() + s0.height() / 4).rgb()
              == badShot.pixelColor(s1.left() + 5, s1.top() + s1.height() / 4).rgb()
              ? "yes" : "no", "yes");

    // 空板：generate() 在弹框之前就拒了（这条顺便证明弹框没挡在这条路上）
    page.board()->clearImages();
    check("空板生成回空串（连弹框都不该有）", page.generate().isEmpty() ? "yes" : "no", "yes");

    // ---- 效果图那个弹框：造出来但不 exec（离屏里 exec 的返回值不作数）----
    page.board()->addImages(files.mid(0, 4));
    const QImage previewImg = page.compose(60, 10, QColor(255, 0, 0));
    QDialog* dlg = page.previewDialog(previewImg);
    auto* saveBtn = dlg->findChild<QPushButton*>(QStringLiteral("gridSave"));
    auto* cancelBtn = dlg->findChild<QPushButton*>(QStringLiteral("gridCancel"));
    auto* capLbl = dlg->findChild<QLabel*>(QStringLiteral("gridPreviewCap"));
    auto* shotLbl = dlg->findChild<QLabel*>(QStringLiteral("gridPreviewShot"));
    bool laid = false;
    if (saveBtn && cancelBtn) {
        QSet<const QWidget*> reached;
        std::function<void(QLayout*)> walk = [&](QLayout* l) {
            if (!l)
                return;
            for (int i = 0; i < l->count(); ++i) {
                QLayoutItem* it = l->itemAt(i);
                if (!it)
                    continue;
                if (QWidget* cw = it->widget()) {
                    reached.insert(cw);
                    walk(cw->layout());
                } else if (QLayout* sub = it->layout())
                    walk(sub);
            }
        };
        walk(dlg->layout());
        laid = reached.contains(saveBtn) && reached.contains(cancelBtn);
    }
    check("弹框里有「保存这张」和「取消（不存）」两颗，且摆在布局里",
          QStringLiteral("%1/%2/%3").arg(saveBtn ? 1 : 0).arg(cancelBtn ? 1 : 0).arg(laid ? 1 : 0),
          QStringLiteral("1/1/1"));
    check("弹框那行字说的就是 previewCaption 那一句",
          capLbl && capLbl->text() == page.previewCaption(previewImg, page.backgroundColor()) ? "yes" : "no", "yes");
    check("那行字里有全尺寸（150×150）", capLbl && capLbl->text().contains(QStringLiteral("150×150"))
              ? "yes" : "no", "yes");
    check("弹框里那块缩图真有像素（不是空标签）",
          shotLbl && !shotLbl->pixmap().isNull() ? "yes" : "no", "yes");
    cancelBtn->click();
    check("按「取消」= 这个框回绝（generate() 因此不会写文件）",
          dlg->result() == QDialog::Rejected ? "yes" : "no", "yes");
    delete dlg;

    // ---- 顺序本身：没看过效果图就不许落盘（把弹框换成一道"门"）----
    {
        const QString dir0 = Utils::capturesDir();
        const auto countFiles = [&dir0] {
            return QDir(dir0).entryList({QStringLiteral("grid-*.png")}, QDir::Files).size();
        };
        const int before = countFiles();
        page.setPreviewGate([](const QImage&) { return false; });
        const QString refused = page.generate();
        check("门回绝时 generate() 不写文件（弹框被摘掉就会红）",
              QStringLiteral("%1/%2/%3").arg(refused.isEmpty() ? 1 : 0).arg(countFiles() == before ? 1 : 0)
                  .arg(page.findChild<QPushButton*>(QStringLiteral("gridOpen"))->isEnabled() ? 1 : 0),
              QStringLiteral("1/1/0"));
        check("回绝之后状态行说实话（说的是没存，不是已生成）",
              page.findChild<QLabel*>(QStringLiteral("muted"))
                      && page.findChild<QLabel*>(QStringLiteral("muted"))->text().startsWith(QStringLiteral("没存"))
                  ? "yes" : "no", "yes");
        page.setPreviewGate([](const QImage&) { return true; });
        const QString accepted = page.generate();
        check("门同意时才真落盘",
              QFileInfo::exists(accepted) && countFiles() == before + 1 ? "yes" : "no", "yes");
        if (accepted.startsWith(dir0) && QFileInfo(accepted).fileName().startsWith(QStringLiteral("grid-")))
            QFile::remove(accepted);
        page.setPreviewGate(nullptr);
    }

    // ---- 落盘那一步（判据绕开模态框直接调 saveComposed）----
    const QString file = page.saveComposed(previewImg);
    check("按「保存」写出真文件", QFileInfo::exists(file) && QFileInfo(file).size() > 0 ? "yes" : "no", "yes");
    check("文件名是 grid-<时间>-<n>宫.png", QFileInfo(file).fileName().startsWith(QStringLiteral("grid-"))
                  && QFileInfo(file).fileName().endsWith(QStringLiteral("4宫.png")) ? "yes" : "no", "yes");
    const QString dir = Utils::capturesDir();
    if (file.startsWith(dir) && QFileInfo(file).fileName().startsWith(QStringLiteral("grid-")))
        QFile::remove(file);                    // 只删自己刚算出来那一个，不碰目录里别的
    check("保存之后「打开这张」那颗该能点", page.findChild<QPushButton*>(QStringLiteral("gridOpen"))
                  && page.findChild<QPushButton*>(QStringLiteral("gridOpen"))->isEnabled() ? "yes" : "no", "yes");

    // ---- 背景色的选择挪进了这个弹框：对着效果图挑才看得准（顶栏那颗是盲选）----
    int headBg = 0;
    for (QPushButton* btn : page.findChildren<QPushButton*>())
        if (btn->text().startsWith(QStringLiteral("背景色")))
            ++headBg;
    check("顶栏里不再有那颗「背景色」（是挪进弹框，不是两处都留）", QString::number(headBg), "0");
    {
        QDialog* d2 = page.previewDialog(previewImg);
        auto* bgBtn = d2->findChild<QPushButton*>(QStringLiteral("gridPreviewBg"));
        check("弹框里有「背景色」那颗，且摆在布局里",
              bgBtn && reachedFrom(d2, bgBtn) ? "yes" : "no", "yes");
        check("那颗上写着当前挑的色", bgBtn ? bgBtn->text() : QString(), QStringLiteral("背景色 #FFFFFF"));
        page.setPreviewBgGate([](const QColor&) { return QColor(0, 0, 255); });
        bgBtn->click();
        check("在弹框里换色 = 效果图当场重画（角像素是新色）",
              page.previewImage().pixelColor(2, 2).name(QColor::HexRgb), QStringLiteral("#0000ff"));
        auto* cap2 = d2->findChild<QLabel*>(QStringLiteral("gridPreviewCap"));
        check("那行字跟着换成新色号", cap2 && cap2->text().contains(QStringLiteral("#0000FF")) ? "yes" : "no", "yes");
        check("那颗按钮的文字也跟着走", bgBtn->text(), QStringLiteral("背景色 #0000FF"));
        check("没按保存之前，页面认的还是老色（取消就不算）",
              page.backgroundColor().name(QColor::HexRgb), QStringLiteral("#ffffff"));
        d2->reject();
        delete d2;
        check("取消之后页面记的还是老色", page.backgroundColor().name(QColor::HexRgb),
              QStringLiteral("#ffffff"));
        const QString saved = page.commitPreview(page.previewImage(), page.previewBackground());
        page.setPreviewBgGate(nullptr);
        check("按「保存」才认这个色", page.backgroundColor().name(QColor::HexRgb),
              QStringLiteral("#0000ff"));
        check("存出去那张的角像素就是新背景",
              QImage(saved).pixelColor(2, 2).name(QColor::HexRgb), QStringLiteral("#0000ff"));
        if (saved.startsWith(Utils::capturesDir()) && QFileInfo(saved).fileName().startsWith(QStringLiteral("grid-")))
            QFile::remove(saved);
        page.setBackgroundColor(Qt::white);      // 还回默认，别把后面的判据带跑
    }

    // ---- 点空格子 = 往里放一张（原来点它什么都不发生）----
    // 这一组用一块**独立的板子**：页面那条连接会去弹真选图框，判据一点就把进程卡死在模态框里
    // （离屏看不见，但就是不动 —— 第一次跑就是这么哑的）。板子层的信号就该在板子层验。
    GridBoard board;
    board.setSlots(4);
    board.setFixedSize(300, 300);            // 四宫：每格 138 见方，坐标算得准
    int picked = -1;
    QObject::connect(&board, &GridBoard::pickInto, &board, [&picked](int i) { picked = i; });
    const auto clickAt = [&board](const QPoint& where, const QPoint& releaseAt) {
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(where), board.mapToGlobal(where),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(releaseAt),
                            board.mapToGlobal(releaseAt), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&board, &press);
        QApplication::sendEvent(&board, &release);
    };
    const auto clickCell = [&board, &clickAt](int i) {
        const QPoint c = board.cellRect(i).center();
        clickAt(c, c);
    };
    clickCell(2);
    check("点空格子 = 要往里放一张（报的是那一格，不是第一格）", QString::number(picked), "2");
    picked = -1;
    clickAt(QPoint(board.cellRect(1).right() - 15, board.cellRect(1).top() + 15),
            QPoint(board.cellRect(1).right() - 15, board.cellRect(1).top() + 15));
    check("空格子上那圈虚线里哪儿点都算（点偏右上角也行）", QString::number(picked), "1");
    picked = -1;
    check("putAt 收图并只动那一格", board.putAt(0, files.at(0)) ? "yes" : "no", "yes");
    clickCell(0);
    check("点已经摆了图的格子不算（那是拖动的起点）", QString::number(picked), "-1");
    picked = -1;
    {   // 按下去拖到别的格子上再松 = 不是点那一格
        const QPoint from = board.cellRect(3).center();
        const QPoint to = board.cellRect(2).center();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(from), board.mapToGlobal(from),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent move(QEvent::MouseMove, QPointF(to), board.mapToGlobal(to), Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(to), board.mapToGlobal(to),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&board, &press);
        QApplication::sendEvent(&board, &move);
        QApplication::sendEvent(&board, &release);
    }
    check("按下去拖出那一格再松 = 不算点它", QString::number(picked), "-1");
    check("putAt 之后别的格没动",
          QStringLiteral("%1/%2").arg(QFileInfo(board.paths()[0]).fileName(),
                                      board.paths()[1].isEmpty() ? QStringLiteral("e") : QStringLiteral("f")),
          QStringLiteral("c0.png/e"));
    check("putAt 拒非图片后缀", board.putAt(1, fake) ? "yes" : "no", "no");
    check("putAt 拒越界格子号", (board.putAt(-1, files.at(0)) || board.putAt(9, files.at(0))) ? "yes" : "no", "no");
    check("putAt 能盖掉已有那张", board.putAt(0, files.at(1))
              && QFileInfo(board.paths()[0]).fileName() == QStringLiteral("c1.png") ? "yes" : "no", "yes");
    {
        const QRect cell = board.cellRect(0);
        const QPoint x(cell.right() - 15, cell.top() + 15);
        clickAt(x, x);
        check("点那一格右上角的 ✕ = 只拿掉它，不发选图",
              board.paths()[0].isEmpty() && picked < 0 ? "yes" : "no", "yes");
    }
    // 端到端（这一次走页面）：先装好"门"，点空格子 → 图落在**那一格**
    page.board()->clearImages();
    page.board()->setFixedSize(300, 300);
    page.setPickGate([&files] { return files.at(2); });
    clickPageCell(page, 3);
    check("点空格子 → 选图框 → 图落在**那一格**",
          QFileInfo(page.board()->paths()[3]).fileName(), QStringLiteral("c2.png"));
    auto* foot = page.findChild<QLabel*>(QStringLiteral("muted"));
    check("放好之后状态行说的是那一格", foot && foot->text().startsWith(QStringLiteral("第 4 格放了"))
              ? "yes" : "no", "yes");
    page.setPickGate([] { return QString(); });            // 选图框里按取消
    clickPageCell(page, 2);
    check("选图框按取消 = 那一格还是空的", page.board()->paths()[2].isEmpty() ? "yes" : "no", "yes");
    page.setPickGate(nullptr);
    page.board()->setMinimumSize(320, 320);
    page.board()->setMaximumSize(QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));

    (void)QDir(box).removeRecursively();   // 清不掉也不算这次判据的事（Qt6 上它带 nodiscard）              // 这次自测造的临时图，绝对路径算出来的
    std::printf("grid: 失败 %d 项\n", bad);
    std::fflush(stdout);
    return bad;
}
