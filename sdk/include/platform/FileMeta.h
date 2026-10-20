#pragma once

#include <QString>

// 平台层第四道缝：一个文件"关于自己"的两件事 —— 里面嵌的图标、和它的版本号。
// 两个都是"有就报，没有就空"，谁都不能拿"读不到"当成"文件坏了"。
namespace Platform {

// 从 exe/dll/ico 里把第 index 个图标抠出来存成 PNG（应用图标缓存用）。
// Windows 走 PrivateExtractIconsW；POSIX 上"exe 里嵌图标"这个概念没有对等物 ——
// 只解 ico/png/svg 这类本身就是图的文件，解不出返回 false，调用方（AppScanner）本来就有
// false 那一条分支。将来 Linux 的应用图标走 .desktop 的 Icon= 键，那是 AppScanner 移植时的事，
// 不在这里假装有。
bool extractIconToPng(const QString& iconFile, int index, const QString& pngOut);

// 文件的版本号字符串。Windows 读 VS_VERSION_INFO；POSIX 上普通文件没有这个概念 →
// 返回空串。空串是这条缝在 POSIX 上的**正常答案**，界面（软件页那几列）已经按"读不到就空着"
// 处理过，不许为了好看去编一个。
QString fileVersion(const QString& path);

} // namespace Platform
