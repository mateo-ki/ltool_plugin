#pragma once

#include <QFile>
#include <QNetworkAccessManager>
#include "core-api.h"
#include <QObject>
#include <QString>
#include <QVector>

class QNetworkReply;

// 一个大文件里的一段（多线程下载就是把它切成 N 段各拉各的）
struct DownloadPart {
    qint64 start = 0;
    qint64 end = -1;        // -1 = 还不知道总长，整段一起拉
    qint64 received = 0;
    bool done = false;
    QNetworkReply* reply = nullptr;
};

struct DownloadTask {
    QString id;
    QString url;
    QString file;           // 落盘路径
    QString status = QStringLiteral("等待");   // 等待/下载中/已暂停/完成/失败
    QString error;
    qint64 total = 0;       // 0 = 服务器没给长度
    bool acceptRanges = false;   // 支持 Range 才能分段
    int concurrency = 6;
    QVector<DownloadPart> parts;
    QFile* fileHandle = nullptr;   // 只在下载期间持有

    qint64 receivedTotal() const;
};

// 下载器：类 Motrix 那种一个文件多段并行的下载，断点续传靠 Range
class LTOOL_CORE_API DownloadManager : public QObject {
    Q_OBJECT
public:
    static DownloadManager& instance();

    void start();                       // 读回上次的列表（下载中的标成已暂停）

    // 下载默认落点：配置的安装目录，拿不到就用 ~/Downloads
    QString saveDir() const;

    void add(const QString& url, const QString& savePath);
    void pause(const QString& id);
    void resume(const QString& id);
    void remove(const QString& id);     // 连没下完的文件一起删掉

    // 页面直接读这份列表（顺序就是添加顺序）
    const QVector<DownloadTask>& tasks() const { return m_tasks; }

signals:
    void changed();

private:
    DownloadManager() = default;
    DownloadManager(const DownloadManager&) = delete;
    DownloadManager& operator=(const DownloadManager&) = delete;

    void load();
    void save();
    DownloadTask* taskById(const QString& id);
    void startTask(const QString& id);
    void startPart(const QString& id, int index);
    void taskTick(const QString& id);

    QVector<DownloadTask> m_tasks;
};
