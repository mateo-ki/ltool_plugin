#include "GridPage.h"
#include "platform/Launch.h"

#include "GridBoard.h"
#include "Utils.h"

#include <QColorDialog>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QGuiApplication>
#include <QPixmap>
#include <QScreen>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int kOutGap = 24;         // 合成图上格子之间的缝（板上是 8，出图宽一点好看）

QString imageFilter()
{
    return QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff)");
}

QString previewBgStyle(const QColor& c)
{
    const bool light = c.lightnessF() > 0.6;
    return QStringLiteral("QPushButton{background:%1;color:%2;border:1px solid %3;border-radius:6px;"
                          "padding:4px 10px;}QPushButton:hover{border:1px solid #3f6ae0;}")
        .arg(c.name(), light ? QStringLiteral("#1b1d22") : QStringLiteral("#ffffff"),
             light ? QStringLiteral("#c9ced8") : QStringLiteral("#3a3d45"));
}

QPixmap shrinkTo(const QImage& img, int w, int h)
{
    QPixmap pm = QPixmap::fromImage(img);
    // 只缩不放大：图本来就小就按原尺寸摆，放大出来的糊图会让人以为存出去的也是糊的
    if (pm.width() > w || pm.height() > h)
        pm = pm.scaled(w, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return pm;
}

} // namespace

GridPage::GridPage(QWidget* parent)
    : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    auto* head = new QHBoxLayout;
    head->setSpacing(6);
    head->addWidget(new QLabel(QStringLiteral("格数："), this));
    const QVector<QPair<int, QString>> kinds = { { 4, QStringLiteral("四宫") },
                                                 { 6, QStringLiteral("六宫") },
                                                 { 9, QStringLiteral("九宫") } };
    for (const auto& k : kinds) {
        auto* b = new QPushButton(k.second, this);
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, n = k.first] { setSlots(n); });
        head->addWidget(b);
        m_slotBtns << b;
    }
    m_slotBtns.first()->setChecked(true);

    auto* add = new QPushButton(QStringLiteral("添加图片…"), this);
    add->setCursor(Qt::PointingHandCursor);
    connect(add, &QPushButton::clicked, this, &GridPage::addFromFile);
    head->addWidget(add);

    auto* clear = new QPushButton(QStringLiteral("清空"), this);
    clear->setCursor(Qt::PointingHandCursor);
    connect(clear, &QPushButton::clicked, this, [this] {
        m_board->clearImages();
        showStatus(QStringLiteral("清空了。拖图片进格子，或者点「添加图片…」挑几个文件。"));
    });
    head->addWidget(clear);
    head->addStretch(1);

    auto* gen = new QPushButton(QStringLiteral("生成宫图"), this);
    gen->setCursor(Qt::PointingHandCursor);
    gen->setObjectName(QStringLiteral("gridGenerate"));
    connect(gen, &QPushButton::clicked, this, [this] { generate(); });
    head->addWidget(gen);
    lay->addLayout(head);

    m_board = new GridBoard(this);
    lay->addWidget(m_board, 1);
    connect(m_board, &GridBoard::status, this, &GridPage::showStatus);
    connect(m_board, &GridBoard::pickInto, this, [this](int i) { pickIntoCell(i); });
    connect(m_board, &GridBoard::changed, this, [this] {
        // 顺序/内容一变，之前那张生成出来的图就不算数了：把"打开"两颗收回去
        if (!m_lastFile.isEmpty()) {
            m_lastFile.clear();
            m_openBtn->setEnabled(false);
            m_dirBtn->setEnabled(false);
        }
    });

    auto* foot = new QHBoxLayout;
    foot->setSpacing(6);
    m_status = new QLabel(QStringLiteral("拖图片进格子（支持一次拖多张）；从一格拖到另一格换顺序；点右上角 ✕ 拿掉那一张。"), this);
    m_status->setObjectName(QStringLiteral("muted"));
    m_status->setWordWrap(true);
    foot->addWidget(m_status, 1);
    m_openBtn = new QPushButton(QStringLiteral("打开这张"), this);
    m_openBtn->setCursor(Qt::PointingHandCursor);
    m_openBtn->setEnabled(false);
    m_openBtn->setObjectName(QStringLiteral("gridOpen"));
    connect(m_openBtn, &QPushButton::clicked, this, [this] {
        if (!m_lastFile.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastFile));
    });
    foot->addWidget(m_openBtn);
    m_dirBtn = new QPushButton(QStringLiteral("打开所在文件夹"), this);
    m_dirBtn->setCursor(Qt::PointingHandCursor);
    m_dirBtn->setEnabled(false);
    m_dirBtn->setObjectName(QStringLiteral("gridOpenDir"));
    connect(m_dirBtn, &QPushButton::clicked, this, [this] {
        if (!m_lastFile.isEmpty())
            Platform::reveal(m_lastFile);
    });
    foot->addWidget(m_dirBtn);
    lay->addLayout(foot);

}

void GridPage::setSlots(int n)
{
    m_board->setSlots(n);
    for (int i = 0; i < m_slotBtns.size(); ++i) {
        const int kinds[3] = { 4, 6, 9 };
        m_slotBtns[i]->setChecked(kinds[i] == n);
    }
}

int GridPage::slotCount() const
{
    return m_board->slotCount();
}

void GridPage::setBackgroundColor(const QColor& c)
{
    if (!c.isValid())
        return;
    m_bg = c;
}

QString GridPage::commitPreview(const QImage& img, const QColor& bg)
{
    // 按「保存」才认这个色：在弹框里换了色又按取消，页面这边什么都没发生过。
    setBackgroundColor(bg);
    return saveComposed(img);
}

void GridPage::pickPreviewBg()
{
    const QColor c = m_bgGate ? m_bgGate(m_previewBg)
                              : QColorDialog::getColor(m_previewBg, this, QStringLiteral("选一个背景色"));
    if (!c.isValid())
        return;                                 // 取消就是取消，别把颜色改了还说改了
    m_previewBg = c;
    m_previewImg = compose(m_cellPx, kOutGap, m_previewBg);
    auto* dlg = findChild<QDialog*>(QStringLiteral("gridPreview"));
    if (!dlg)
        return;
    if (auto* shot = dlg->findChild<QLabel*>(QStringLiteral("gridPreviewShot"))) {
        QPixmap pm = QPixmap::fromImage(m_previewImg);
        const QSize room = shot->size() - QSize(12, 12);
        if (room.isValid() && (pm.width() > room.width() || pm.height() > room.height()))
            pm = pm.scaled(room, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        shot->setPixmap(pm);
    }
    if (auto* cap = dlg->findChild<QLabel*>(QStringLiteral("gridPreviewCap")))
        cap->setText(previewCaption(m_previewImg, m_previewBg));
    if (auto* btn = dlg->findChild<QPushButton*>(QStringLiteral("gridPreviewBg")))
        btn->setText(QStringLiteral("背景色 %1").arg(m_previewBg.name().toUpper()));
}

void GridPage::pickIntoCell(int index)
{
    // 选图框只挑一张：这一格就是它的位置。想连着填几格还是用「添加图片…」或者整批拖进来。
    const QString file = m_pickGate
        ? m_pickGate()
        : QFileDialog::getOpenFileName(this, QStringLiteral("给第 %1 格选一张图").arg(index + 1),
                                       Utils::capturesDir(), imageFilter());
    if (file.isEmpty()) {
        showStatus(QStringLiteral("第 %1 格还是空的 —— 选图框里按了取消。").arg(index + 1));
        return;
    }
    if (!m_board->putAt(index, file))
        showStatus(QStringLiteral("这张放不进第 %1 格（不是图片文件，或者那一格已经没了）。").arg(index + 1));
}

void GridPage::addFromFile()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("添加图片"), Utils::capturesDir(), imageFilter());
    if (files.isEmpty())
        return;
    m_board->addImages(files);
}

void GridPage::showStatus(const QString& text)
{
    m_status->setText(text);
}

QImage GridPage::compose(int cellPx, int gap, const QColor& bg) const
{
    const int cols = m_board->cols();
    const int rows = m_board->rows();
    const int w = cols * cellPx + (cols + 1) * gap;
    const int h = rows * cellPx + (rows + 1) * gap;
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    out.fill(bg);
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QVector<QString> paths = m_board->paths();
    for (int i = 0; i < paths.size(); ++i) {
        if (paths[i].isEmpty())
            continue;                             // 空格子 = 只留背景色
        QImageReader r(paths[i]);
        r.setAutoTransform(true);                 // EXIF 方向：手机照片不转就是倒的
        // 原图 4000x3000 没必要整张吃进来 —— 出图只要 cellPx 见方那一大块。
        // 短边先缩到 cellPx，剩下的居中裁正方形，和板上预览同一条算法。
        const QSize hinted = r.size();
        if (hinted.isValid() && qMin(hinted.width(), hinted.height()) > cellPx) {
            const double k = double(cellPx) / qMin(hinted.width(), hinted.height());
            r.setScaledSize(QSize(qMax(1, qRound(hinted.width() * k)),
                                  qMax(1, qRound(hinted.height() * k))));
        }
        const QImage src = r.read();
        if (src.isNull()) {
            Utils::log(QStringLiteral("grid: %1 读不出来：%2").arg(paths[i], r.errorString()));
            continue;
        }
        const int side = qMin(src.width(), src.height());
        const QRect crop((src.width() - side) / 2, (src.height() - side) / 2, side, side);
        const int col = i % cols;
        const int row = i / cols;
        const QRect target(gap + col * (cellPx + gap), gap + row * (cellPx + gap), cellPx, cellPx);
        p.drawImage(target, src, crop);
    }
    p.end();
    return out;
}

QString GridPage::previewCaption(const QImage& img, const QColor& bg) const
{
    // 弹框上面那行字说的是"存出去的那张"，不是屏上这块缩图 —— 两边数字不一样是应该的，
    // 但必须写明白，不然人以为存下来也只有这么大。
    return QStringLiteral("存出去的是 %1×%2 原尺寸（屏上这块是缩到放得下的样子），%3 格，背景 %4，落在 %5")
        .arg(img.width()).arg(img.height()).arg(m_board->slotCount())
        .arg(bg.name().toUpper()).arg(Utils::capturesDir());
}

// 弹框本体单独做成"造出来"而不是"弹出来"：判据要能在不 exec 的前提下检查那三颗按钮、
// 那行字、那张缩图，还要能在框里换一次背景色看它重不重画（离屏里 exec() 的返回值不作数）。
QDialog* GridPage::previewDialog(const QImage& img)
{
    m_previewBg = m_bg;
    m_previewImg = img;
    auto* box = new QDialog(this);
    box->setObjectName(QStringLiteral("gridPreview"));
    box->setWindowTitle(QStringLiteral("效果图：看过再存"));
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    auto* capRow = new QHBoxLayout;
    capRow->setSpacing(6);
    auto* cap = new QLabel(previewCaption(img, m_previewBg), box);
    cap->setObjectName(QStringLiteral("gridPreviewCap"));
    cap->setWordWrap(true);
    capRow->addWidget(cap, 1);
    // 背景色的选择挪到这里：对着效果图挑才看得准（顶栏那颗是盲选）
    auto* bgBtn = new QPushButton(QStringLiteral("背景色 %1").arg(m_previewBg.name().toUpper()), box);
    bgBtn->setObjectName(QStringLiteral("gridPreviewBg"));
    bgBtn->setCursor(Qt::PointingHandCursor);
    bgBtn->setMinimumWidth(112);
    bgBtn->setStyleSheet(previewBgStyle(m_previewBg));
    connect(bgBtn, &QPushButton::clicked, box, [this] { pickPreviewBg(); });
    capRow->addWidget(bgBtn);

    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    const int maxW = qMax(320, qMin(900, avail.width() - 260));
    const int maxH = qMax(240, qMin(620, avail.height() - 260));
    auto* shot = new QLabel(box);
    shot->setObjectName(QStringLiteral("gridPreviewShot"));
    shot->setAlignment(Qt::AlignCenter);
    shot->setMinimumSize(maxW, maxH);
    shot->setStyleSheet(QStringLiteral("background:#101114;border:1px solid #3a3c45;border-radius:6px;"));
    shot->setPixmap(shrinkTo(img, maxW - 12, maxH - 12));

    auto* btns = new QHBoxLayout;
    btns->setSpacing(6);
    btns->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("取消（不存）"), box);
    cancel->setObjectName(QStringLiteral("gridCancel"));
    cancel->setCursor(Qt::PointingHandCursor);
    auto* save = new QPushButton(QStringLiteral("保存这张"), box);
    save->setObjectName(QStringLiteral("gridSave"));
    save->setCursor(Qt::PointingHandCursor);
    save->setDefault(true);
    connect(cancel, &QPushButton::clicked, box, &QDialog::reject);
    connect(save, &QPushButton::clicked, box, &QDialog::accept);
    btns->addWidget(cancel);
    btns->addWidget(save);

    lay->addLayout(capRow);
    lay->addWidget(shot, 1);
    lay->addLayout(btns);
    box->resize(maxW + 40, maxH + 140);
    return box;
}


QString GridPage::generate()
{
    if (m_board->filled() == 0) {
        showStatus(QStringLiteral("一张图都还没有 —— 先拖几张进来（或者点「添加图片…」）。"));
        return QString();
    }
    const QImage img = compose(m_cellPx, kOutGap, m_bg);
    if (m_gate) {                                  // 判据那条路：不弹框，只问"存不存"
        if (!m_gate(img)) {
            showStatus(QStringLiteral("没存 —— 你在效果图那一步按了取消。摆的还在，改完再按一次「生成宫图」。"));
            return QString();
        }
        return commitPreview(img, m_bg);
    }
    QDialog* box = previewDialog(img);
    const bool agreed = box->exec() == QDialog::Accepted;
    const QImage shown = m_previewImg;
    const QColor bg = m_previewBg;
    delete box;
    if (!agreed) {
        // 取消就是取消：别写文件、别把"打开这张"点亮、别说成已生成
        showStatus(QStringLiteral("没存 —— 你在效果图那一步按了取消。摆的还在，改完再按一次「生成宫图」。"));
        return QString();
    }
    return commitPreview(shown, bg);        // 保存才认弹框里挑的那个色
}

QString GridPage::saveComposed(const QImage& img)
{
    const QString dir = Utils::capturesDir();
    QDir().mkpath(dir);
    const QString name = QStringLiteral("grid-%1-%2宫.png")
                             .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"))
                             .arg(m_board->slotCount());
    const QString file = QDir(dir).absoluteFilePath(name);
    if (!img.save(file, "PNG")) {
        showStatus(QStringLiteral("写不下去：%1（看日志）").arg(file));
        Utils::log(QStringLiteral("grid: 写 %1 失败").arg(file));
        return QString();
    }
    m_lastFile = file;
    m_openBtn->setEnabled(true);
    m_dirBtn->setEnabled(true);
    const qint64 kb = QFileInfo(file).size() / 1024;
    showStatus(QStringLiteral("已生成 %1（%2×%3，%4 张图，背景 %5，%6 KB）")
                   .arg(QFileInfo(file).fileName()).arg(img.width()).arg(img.height())
                   .arg(m_board->filled()).arg(m_bg.name().toUpper()).arg(kb));
    Utils::log(QStringLiteral("grid: 生成 %1（%2x%3，%4/%5 格，背景 %6）")
                   .arg(file).arg(img.width()).arg(img.height()).arg(m_board->filled())
                   .arg(m_board->slotCount()).arg(m_bg.name()));
    emit generated(file);
    return file;
}
