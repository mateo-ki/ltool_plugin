#pragma once

#include <QObject>
#include <QString>

// OCR 引擎的分发包：PaddleOCR 的 PP-OCRv4 中文模型（det+rec）+ ONNX Runtime 的 DLL。
//
// 两处都不打进安装包（27.9MB），而是第一次用到时下载 —— 因为这套的可用性只取决于
// 一个 HTTP 请求，而 Windows 自带的 OCR 取决于对方机器装没装中文语言包（很多机器没有）。
// 下载源用 PyPI 上两个 wheel 的固定版本 + 钉死 SHA256，解包只抽我们要的几个成员：
//   rapidocr_onnxruntime/models/ch_PP-OCRv4_det_infer.onnx   4.53 MB
//   rapidocr_onnxruntime/models/ch_PP-OCRv4_rec_infer.onnx  10.35 MB（字典就嵌在这个模型的元数据里）
//   onnxruntime/capi/onnxruntime.dll                        17.59 MB
// 落在 %APPDATA%\ltool\ocr\ 下，之后完全离线。
namespace OcrBundle {

struct Files {
    QString dir;
    QString det;
    QString rec;
    QString onnxruntime;
    bool present = false;      // 四个文件都在且大小对得上
    QString missing;           // 给界面说的话
};

QString dir();
Files status();

// 下载（阻塞式，给 --ocrget 这类无界面入口用）。进度回调 0..100 + 一句话状态。
bool fetchSync(QString* err, const std::function<void(int, const QString&)>& progress = nullptr);

// 现在有一个下载在跑吗（同一进程里只准有一个，第二个请求不该把同一个文件写两遍）
bool fetching();

// 异步版：结果和进度都回到 GUI 线程（ctx 一死回调就断掉，但下载本身会继续跑完）。
// progress 可能一次都不来（已经在下载中时直接给 done）。
void fetchLater(QObject* ctx,
                std::function<void(int, const QString&)> progress,
                std::function<void(bool ok, const QString& err, const QString& note)> done);

// 一共多大、下到哪儿，给 UI 写提示用
QString describe();

} // namespace OcrBundle
