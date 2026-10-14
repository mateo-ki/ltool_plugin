#pragma once

#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

#include <functional>

class QObject;

// 图里取字：引擎是自带的 PaddleOCR PP-OCRv4（跑在 ONNX Runtime 上，见 OcrPp/OcrBundle），
// 系统那套 Windows.Media.Ocr 已经不用了 —— 它取决于对方装没装中文语言包。
// 模型和运行库不打进安装包，第一次用到时下载（见 Ocr::available 的提示）。
// recognize() 会阻塞（一张屏幕截图一秒量级），必须放后台线程调。
namespace Ocr {

// 引擎给的每个词的位置（都是"喂进去的那张图"的像素坐标，调用方自己按比例换算）。
// 划词要用它：recognize() 只回纯文本，位置丢了。
struct Word {
    QRect rect;
    QString text;
};
struct Line {
    QRect rect;
    QString text;
    QVector<Word> words;
};

// 引擎齐不齐（模型 + onnxruntime.dll 都下载到位）。没齐时 why 里写清楚缺什么、要多大
bool available(QString* why = nullptr);

// 识别图片里的文字，按行返回；出错时 err 里是人话
QString recognize(const QImage& image, QString* err = nullptr);

// 带位置的识别：每行每词都给出包围盒
QVector<Line> recognizeLayout(const QImage& image, QString* err = nullptr);

// 把一行切成一格一格的字（"词内拆到字"）：引擎只给到词的包围盒，划选要点中单个字
// 就得按词的宽度和字数切开。空格会被丢掉（这引擎在汉字之间塞空格）。
// word/line 是回传给调用方的分组号：单击要整词选中，跨行拖动要整行选中。
struct Cell {
    QRect rect;
    QChar ch;
    int word = 0;
    int line = 0;
};
QVector<Cell> cells(const Line& line);

// 后台线程识别，完成后回到 GUI 线程回调。text 为空时看 err。
void recognizeLater(const QImage& image,
                    std::function<void(const QString& text, const QString& err, qint64 ms)> done);

// 后台线程带位置识别。ctx 是"这个回调是给谁的"：它一死回调就断掉
// （置顶窗可能在几百毫秒的识别途中被关掉，回调再摸 this 就是野指针）。
void recognizeLayoutLater(const QImage& image, QObject* ctx,
                          std::function<void(const QVector<Line>& lines, const QString& err,
                                             qint64 ms)> done);

} // namespace Ocr
