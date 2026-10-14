#pragma once

#include <QString>

// F2 录屏之前的可行性探针（`ltool.exe --recprobe`）：三件事都必须实测，不能靠文档猜。
//  1) Media Foundation 能不能在这台机器上写出 MP4/H.264（mfh264enc.dll 在不在不算证据）；
//  2) 有没有 AAC 编码器（没音轨的录屏价值减半，有音轨又得知道容器里塞不塞得进）；
//  3) "只录某一个软件的页面"——Windows.Graphics.Capture 按窗口取帧在这台机器上能不能用、
//     帧到得多快、拷一次到 CPU 要多久（这决定能给的帧率）。
//
// 2026-10-15：实现（RecProbe.cpp）跟着录屏那一路搬进了 plugins/record，只有那个插件编它；
// 头文件留在这里是因为 ltoolcore 的包含目录是 PUBLIC 的，插件也走同一份 src/。
// --recprobe 现在由主程序点名插件导出的 ltoolRecprobeRun(int) 跑。
namespace RecProbe {

// 跑一遍，返回人话报告（每行一条结论）。targetSeconds：按窗口取帧观察多久。
QString run(int observeMs);

} // namespace RecProbe
