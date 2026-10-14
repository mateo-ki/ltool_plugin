#pragma once

#include <QString>
#include <QStringList>

#include <functional>

// 压缩包读写：后端是 libarchive（vcpkg 的 libarchive:x64-windows），
// 所以 zip / 7z / tar / gz / bz2 / xz / zstd / iso 这些都能读；
// 写只开 zip 和 7z 两种（RAR 的写格式是专有授权，做不了，读也只到"部分支持"）。
namespace Archive {

// 看起来像压缩包吗（右键菜单对所有文件都出现，得能回答"这个不是压缩包"）
bool looksLikeArchive(const QString& path);

struct Progress {
    qint64 done = 0;      // 已处理的字节（解压时是解压出来的量）
    qint64 total = 0;     // 0 = 总量未知（有些格式头里没写）
    QString current;      // 正在处理的条目名
};

// 返回 true 表示继续，false 表示取消
using Cancel = std::function<bool()>;
using Report = std::function<bool(const Progress&)>;

struct Result {
    bool ok = false;
    bool cancelled = false;
    QString error;
    QString target;       // 解压：写到的目录；压缩：生成的包
    int files = 0;
    int dirs = 0;
    qint64 bytes = 0;
};

// 把 archivePath 解到 destDir（destDir 不存在会建）。
Result extract(const QString& archivePath, const QString& destDir, const Report& report, const Cancel& cancel);

// 把 inputs（文件或目录）打包成 destFile；sevenZip=true 用 7z，否则 zip。
// 两条路的后端不一样：
//   · 7z —— 7-Zip 自己的编码器（SevenZip::compress，多线程）。level 是 7-Zip 的档位，
//     只认它定义的 0/1/3/5/7/9，默认 5 就是 7-Zip 的默认档。
//   · zip —— libarchive 的 deflate（6 档、单线程），实测本来就不比 7-Zip 慢，不动。
Result compress(const QStringList& inputs, const QString& destFile, bool sevenZip,
                const Report& report, const Cancel& cancel, int level = 5);

} // namespace Archive
