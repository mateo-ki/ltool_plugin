#include "Ocr.h"

#include "OcrBundle.h"
#include "OcrPp.h"
#include "Utils.h"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

// 引擎是自带的 PaddleOCR（见 OcrPp/OcrBundle），系统那套 Windows.Media.Ocr 已经完全不用了：
// 它取决于对方机器装没装中文语言包，而"很多人机器上没有可选功能"这件事是用户报的。
namespace Ocr {

bool available(QString* why)
{
    const OcrBundle::Files f = OcrBundle::status();
    if (!f.present && why)
        *why = QStringLiteral("识别引擎还没下载（缺 %1）。%2")
                   .arg(f.missing, OcrBundle::describe());
    return f.present;
}

QVector<Line> recognizeLayout(const QImage& image, QString* err)
{
    return OcrPp::run(image, err);
}

QString recognize(const QImage& image, QString* err)
{
    const QVector<Line> lines = recognizeLayout(image, err);
    QString out;
    for (const Line& l : lines) {
        if (!out.isEmpty())
            out += QLatin1Char('\n');
        out += l.text;
    }
    return out;
}

// 词内拆到字：按词的包围盒等宽切。汉字本来就是等宽方格，切出来准；
// 英文词切得不准，但一个英文词一般也不会只要半个，划选时按整词连上就行。
QVector<Cell> cells(const Line& line)
{
    QVector<Cell> out;
    const auto push = [&out](const QRect& r, const QString& text, int word) {
        const int n = text.size();
        if (n == 0 || r.width() < 1)
            return;
        for (int i = 0; i < n; ++i) {
            const int x0 = r.left() + r.width() * i / n;
            const int x1 = r.left() + r.width() * (i + 1) / n;
            out.push_back(Cell { QRect(x0, r.top(), qMax(1, x1 - x0), r.height()), text.at(i), word });
        }
    };
    int w = 0;
    for (const Word& word : line.words)
        push(word.rect, word.text, w++);
    if (out.isEmpty())
        push(line.rect, line.text, 0);
    return out;
}

namespace {
struct Outcome {
    QString text;
    QString err;
    qint64 ms = 0;
};
struct LayoutOutcome {
    QVector<Ocr::Line> lines;
    QString err;
    qint64 ms = 0;
};
} // namespace

void recognizeLater(const QImage& image,
                    std::function<void(const QString& text, const QString& err, qint64 ms)> done)
{
    auto* w = new QFutureWatcher<Outcome>();
    QObject::connect(w, &QFutureWatcherBase::finished, w, [w, done] {
        const Outcome r = w->result();
        done(r.text, r.err, r.ms);
        w->deleteLater();
    });
    w->setFuture(QtConcurrent::run([image] {
        Outcome r;
        QElapsedTimer t;
        t.start();
        r.text = recognize(image, &r.err);
        r.ms = t.elapsed();
        return r;
    }));
}

// 后台线程带位置识别。ctx 一死回调就断掉（置顶窗可能在识别途中被关掉）。
void recognizeLayoutLater(const QImage& image, QObject* ctx,
                          std::function<void(const QVector<Line>& lines, const QString& err,
                                             qint64 ms)> done)
{
    // watcher 挂在 ctx 上：ctx 先死的话，回调不跑，但 watcher 和它 holding 的识别结果
    // 也跟着被销毁 —— 不挂的话每弃一次就永久漏一个 watcher + 一份 QVector<Line>。
    auto* w = new QFutureWatcher<LayoutOutcome>(ctx);
    QObject::connect(w, &QFutureWatcherBase::finished, ctx, [w, done] {
        const LayoutOutcome r = w->result();
        done(r.lines, r.err, r.ms);
        w->deleteLater();
    });
    w->setFuture(QtConcurrent::run([image] {
        LayoutOutcome r;
        QElapsedTimer t;
        t.start();
        r.lines = recognizeLayout(image, &r.err);
        r.ms = t.elapsed();
        return r;
    }));
}

} // namespace Ocr
