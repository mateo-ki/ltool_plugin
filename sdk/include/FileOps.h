#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// 右键菜单背后那几个动作的干活部分：占用分析、粉碎、谁占用着它。
// 全部是纯 Win32，不依赖 Qt 界面，方便单独测。
namespace FileOps {

struct Info {
    bool ok = false;
    QString error;
    QString path;
    QString fsName;              // NTFS / exFAT / ...
    qint64 size = 0;             // 逻辑大小
    qint64 alloc = 0;            // 实际占的簇
    qint64 cluster = 0;
    int hardLinks = 0;
    bool sparse = false;
    bool compressed = false;
    bool encrypted = false;
    bool isDir = false;
    QStringList streams;         // 备用数据流：名字 (大小)
    QString lastWrite;
    QString attrs;
};

Info info(const QString& path);

// 递归求和：一个文件或整个目录树一共占了多少。目录得走一遍所有子文件，
// 大目录可能要几秒，所以单独拿出来放后台线程跑。
struct Usage {
    bool ok = false;
    QString error;
    qint64 bytes = 0;            // 逻辑大小合计
    qint64 alloc = 0;            // 实际占簇合计（压缩/稀疏文件会小于 bytes）
    int files = 0;
    int dirs = 0;
};

Usage usage(const QString& path);

// 覆写后删除（不进回收站）。thorough=true 时盖三遍并先把文件名改成乱码。
// 返回抹掉的字节数，出错时 err 里是原因。目录会递归处理里面的文件。
qint64 shred(const QString& path, bool thorough, QString* err);

struct Holder {
    quint64 pid = 0;
    QString processName;
    QString exePath;
    int handles = 0;             // 它对这个文件开了几个句柄
    QStringList objects;         // 匹配上的对象名（去重后最多几条）
};

// 枚举系统句柄表，找出正开着这个文件的进程。可能要跑几百毫秒到几秒。
QVector<Holder> whoHas(const QString& path, QString* err);

bool killProcess(quint64 pid, QString* err);

// 给界面用的可读字节数
QString human(qint64 bytes);

} // namespace FileOps
