#pragma once

#include <QImage>
class QWidget;

// 截图 / 置顶窗共用的"提取文字"结果窗：非模态小窗，文字可改、可再复制、可存 txt。
namespace OcrPanel {

void extractFrom(const QImage& image, QWidget* parent = nullptr);

} // namespace OcrPanel
