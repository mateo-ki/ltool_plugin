#pragma once

#include "Ocr.h"

#include <QImage>
#include <QString>
#include <QVector>

// PaddleOCR 的 PP-OCRv4（det 找行 + rec 认字）跑在 ONNX Runtime 上。
//
// 为什么不链接进 exe：这三个文件（det 4.5MB + rec 10.4MB + onnxruntime.dll 17.6MB）是
// 首次用到时从 PyPI 现下的（见 OcrBundle），装完之后完全离线。所以这里是 LoadLibrary +
// C API 函数指针，编译期只需要 third_party/onnxruntime/onnxruntime_c_api.h 这一个头。
//
// 参数、张量名、预处理、DB 后处理、CTC 解码全部对着 rapidocr_onnxruntime 1.4.4 的实现量出来的，
// 不是照着记忆写的。简化了两处（都在注释里点名）：
//   1) 只做正放的文字：det 出来的连通域取轴对齐外接矩形，不做 minAreaRect/透视矫正，
//      因为对外 API（Ocr::Line/Word）本来就是 QRect，旋转框从来表达不了；
//   2) 不做 letterbox 补高（极扁的图 w/h>8 才触发），屏幕截图走不到那一支。
namespace OcrPp {

// 打开 DLL、建两个 session、把字典从 rec.onnx 的元数据里读出来。线程安全的懒加载，
// 已经建好就直接返回 true。err 说清楚缺什么（缺文件/DLL 起不来/模型名对不上）。
bool load(QString* err = nullptr);

// 整图识别：det → 每行 crop 后 rec → 行框 + 按 CTC 槽位算出的词框。
// 坐标是这张图的像素。阻塞（一张 2560x1440 截图一秒量级），调用方负责放后台线程。
QVector<Ocr::Line> run(const QImage& image, QString* err = nullptr);

} // namespace OcrPp
