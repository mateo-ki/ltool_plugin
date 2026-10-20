#include "OcrBundle.h"

#include "Archives.h"
#include "DevEnv.h"
#include "Utils.h"

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>

#include <functional>
#include <atomic>
#include <memory>
#include <mutex>

namespace {

// 钉死的下载源：PyPI 上两个 wheel 的固定版本 + SHA256。
// 只抽我们要的成员，其余（python 源码、测试数据）不落地。
struct Member {
    const char* inWheel;
    const char* outName;
    qint64 minBytes;      // 解出来后至少多大（小了就是下坏了）
};

struct Artifact {
    const char* url;
    const char* sha256;
    qint64 size;
    const Member* members;
    int memberCount;
};

const Member kRottMembers[] = {
    { "rapidocr_onnxruntime/models/ch_PP-OCRv4_det_infer.onnx", "ppocr_det.onnx", 3 * 1024 * 1024 },
    { "rapidocr_onnxruntime/models/ch_PP-OCRv4_rec_infer.onnx", "ppocr_rec.onnx", 8 * 1024 * 1024 },
};
const Member kOrtMembers[] = {
    { "onnxruntime/capi/onnxruntime.dll", "onnxruntime.dll", 10 * 1024 * 1024 },
};

const Artifact kArtifacts[] = {
    { "https://files.pythonhosted.org/packages/ba/12/1e5497183bdbe782dbb91bad1d0d2297dba4d2831b2652657f7517bfc6df/"
      "rapidocr_onnxruntime-1.4.4-py3-none-any.whl",
      "971d7d5f223a7a808662229df1ef69893809d8457d834e6373d3854bc1782cbf",
      14915192, kRottMembers, 2 },
    { "https://files.pythonhosted.org/packages/a6/13/0f1699f6de549c9324bc9112a2a85b14c517904cd11b562a654643b755a1/"
      "onnxruntime-1.30.0-cp312-cp312-win_amd64.whl",
      "f3501472571f1b1eee50e017851e7929f5ea37312d2d8c2494a19e8fc58b4a38",
      14311470, kOrtMembers, 1 },
};

const qint64 kTotalBytes = kArtifacts[0].size + kArtifacts[1].size;

using Progress = std::function<void(int, const QString&)>;

QString getOne(const QString& url, const QString& to, const Progress& progress, qint64 baseDone,
              QString* err)
{
    QNetworkAccessManager nam;
    DevEnv::applyProxy(&nam);   // 和产品里其他下载同一颗：留空 = 跟着系统代理走
    const QString px = DevEnv::proxyUrl();
    QNetworkRequest req { QUrl(url) };
    req.setRawHeader("User-Agent", "ltool/0.4 ocr-bundle");
    QNetworkReply* reply = nam.get(req);
    QFile out(to);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *err = QStringLiteral("写不了 %1").arg(to);
        reply->abort();
        reply->deleteLater();
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QEventLoop loop;
    qint64 got = 0;
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        const QByteArray chunk = reply->readAll();
        hash.addData(chunk);
        out.write(chunk);
        got += chunk.size();
        if (progress && got / 524288 != (got - chunk.size()) / 524288) {   // 每 0.5MB 报一次
            progress(int((baseDone + got) * 100 / kTotalBytes),
                     QStringLiteral("下载 %1%").arg((baseDone + got) * 100 / kTotalBytes));
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    out.flush();
    const QNetworkReply::NetworkError netErr = reply->error();
    const QString netText = reply->errorString();
    reply->deleteLater();
    out.close();
    if (netErr != QNetworkReply::NoError) {
        // 这句话要说清走的哪条路：境外源 + 本机有代理时，"下不动"多半不是源的问题，
        // 是代理那一跳没通 —— 不点名的话就只能靠猜。
        *err = QStringLiteral("下载失败：%1（%2）")
                   .arg(netText,
                        px.isEmpty()
                            ? QStringLiteral("这台机器没检测到代理，这趟是直连")
                            : QStringLiteral("这趟走的是本地代理 %1，想换一个就在配置页「下载代理」那格填新的地址").arg(px));
        QFile::remove(to);
        return {};
    }
    const QString digest = QString::fromLatin1(hash.result().toHex());   // 大小对不对由 SHA256 说了算
    if (got <= 0) {
        *err = QStringLiteral("下到一个空文件");
        QFile::remove(to);
        return {};
    }
    return digest;
}

bool takeMember(const QString& wheel, const QString& tmpDir, const Member& m, const QString& dst,
                QString* err)
{
    // 用现成的 libarchive 解整个 wheel（几百个条目里只要两个），再从临时目录挪过去。
    // 比自己在 OcrBundle 里再写一遍 archive_read 划算，那条路已经过 13 项回归。
    QDir().mkpath(tmpDir);
    const Archive::Result r = Archive::extract(wheel, tmpDir, nullptr, nullptr);
    if (!r.ok) {
        *err = QStringLiteral("解不开下载包：%1").arg(r.error);
        return false;
    }
    const QString src = tmpDir + "/" + QString::fromUtf8(m.inWheel);
    if (!QFile::exists(src)) {
        *err = QStringLiteral("包里找不到 %1（版本换了？）").arg(QString::fromUtf8(m.inWheel));
        return false;
    }
    const qint64 bytes = QFileInfo(src).size();
    if (bytes < m.minBytes) {
        *err = QStringLiteral("%1 只有 %2 KB，不像真的模型").arg(QString::fromUtf8(m.inWheel)).arg(bytes / 1024);
        return false;
    }
    QFile::remove(dst);
    if (!QFile::rename(src, dst) && !QFile::copy(src, dst)) {
        *err = QStringLiteral("挪不动 %1").arg(QString::fromUtf8(m.outName));
        return false;
    }
    return true;
}

} // namespace

namespace OcrBundle {

QString dir()
{
    return Utils::dataDir() + "/ocr";
}

Files status()
{
    Files f;
    f.dir = dir();
    f.det = f.dir + "/ppocr_det.onnx";
    f.rec = f.dir + "/ppocr_rec.onnx";
    f.onnxruntime = f.dir + "/onnxruntime.dll";
    QStringList miss;
    if (QFileInfo(f.det).size() < 3 * 1024 * 1024)
        miss << QStringLiteral("文字检测模型");
    if (QFileInfo(f.rec).size() < 8 * 1024 * 1024)
        miss << QStringLiteral("文字识别模型");
    if (QFileInfo(f.onnxruntime).size() < 10 * 1024 * 1024)
        miss << QStringLiteral("推理运行库");
    f.present = miss.isEmpty();
    f.missing = miss.join(QStringLiteral("、"));
    return f;
}

QString describe()
{
    return QStringLiteral("PaddleOCR PP-OCRv4 中文模型 + ONNX Runtime，共 %1 MB，"
                          "下到 %2 之后完全离线")
        .arg(kTotalBytes / 1048576.0, 0, 'f', 1, QLatin1Char(' '))
        .arg(dir());
}

bool fetchSync(QString* err, const Progress& progress)
{
    const Files f = status();
    QDir().mkpath(f.dir);
    const QString tmpRoot = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + "/ltool-ocr-dl";
    QDir().mkpath(tmpRoot);
    qint64 baseDone = 0;
    for (const Artifact& a : kArtifacts) {
        const QString url = QString::fromLatin1(a.url);
        const QString whl = tmpRoot + "/" + QUrl(url).fileName();
        if (progress)
            progress(int(baseDone * 100 / kTotalBytes), QStringLiteral("连接下载源…"));
        const QString digest = getOne(url, whl, progress, baseDone, err);
        if (digest.isEmpty())
            return false;
        if (digest.compare(QString::fromLatin1(a.sha256), Qt::CaseInsensitive) != 0) {
            *err = QStringLiteral("校验不过（SHA256 不匹配），不敢用：下到的 %1 ≠ 期望 %2")
                       .arg(digest.left(16), QString::fromLatin1(a.sha256).left(16));
            QFile::remove(whl);
            return false;
        }
        const QString tmpDir = tmpRoot + "/x";
        QDir(tmpDir).removeRecursively();
        for (int i = 0; i < a.memberCount; ++i) {
            QString e2;
            if (!takeMember(whl, tmpDir, a.members[i], f.dir + "/" + a.members[i].outName, &e2)) {
                *err = e2;
                QFile::remove(whl);
                QDir(tmpDir).removeRecursively();
                return false;
            }
        }
        baseDone += a.size;
        QFile::remove(whl);
        QDir(tmpDir).removeRecursively();
    }
    if (progress)
        progress(100, QStringLiteral("装好了"));
    return status().present;
}

namespace {
struct AsyncResult {
    bool ok = false;
    QString err;
    QString note;
};
std::atomic<bool> g_fetching { false };

// 进度只通过这个共享结构传递：工作线程写、GUI 线程读。
// 以前是工作线程直接 QMetaObject::invokeMethod(ctx, …) —— 下载跑到一半窗口被关，
// 那个 ctx 就是悬垂指针。共享状态 + 挂在窗口上的定时器没有这个问题。
struct ProgressState {
    std::mutex m;
    int pct = -1;
    QString note;
};
} // namespace

bool fetching()
{
    return g_fetching.load();
}

void fetchLater(QObject* ctx, std::function<void(int, const QString&)> progress,
                std::function<void(bool, const QString&, const QString&)> done)
{
    if (g_fetching.exchange(true)) {
        // 已经有一个在跑了：不要再下一个，直接告诉调用方等着
        QMetaObject::invokeMethod(ctx, [done] {
            done(false, QStringLiteral("识别引擎正在下载中（第一次用要下一趟），稍后再试一次就行。"),
                 QStringLiteral("下载中"));
        }, Qt::QueuedConnection);
        return;
    }
    auto st = std::make_shared<ProgressState>();
    // watcher 和轮询定时器都挂 ctx：ctx 一死它们就跟着没，不会有"回调摸野指针"
    auto* w = new QFutureWatcher<AsyncResult>(ctx);
    auto* poll = progress ? new QTimer(ctx) : nullptr;
    if (poll) {
        QObject::connect(poll, &QTimer::timeout, ctx, [st, progress, w, poll] {
            int pct;
            QString note;
            {
                std::lock_guard<std::mutex> lk(st->m);
                pct = st->pct;
                note = st->note;
            }
            if (pct >= 0)
                progress(pct, note);
            if (w->isFinished())
                poll->stop();
        });
        poll->start(200);
    }
    QObject::connect(w, &QFutureWatcherBase::finished, ctx, [w, done, poll, st, progress] {
        if (poll)
            poll->stop();
        // 收尾补一次最终进度：定时器可能刚好没赶上"100%"那一拍，界面就该停在 100 而不是 98
        if (progress) {
            std::lock_guard<std::mutex> lk(st->m);
            if (st->pct >= 0)
                progress(st->pct, st->note);
        }
        const AsyncResult r = w->result();
        done(r.ok, r.err, r.note);
        w->deleteLater();
    });
    w->setFuture(QtConcurrent::run([st] {
        AsyncResult r;
        QString err;
        r.ok = fetchSync(&err, [st](int pct, const QString& note) {
            std::lock_guard<std::mutex> lk(st->m);
            st->pct = pct;
            st->note = note;
        });
        r.err = err;
        r.note = r.ok ? QStringLiteral("OCR 引擎已就绪") : QStringLiteral("OCR 引擎没装上");
        Utils::log(QString("ocr bundle: ok=%1 err=%2").arg(r.ok ? 1 : 0).arg(r.err));
        g_fetching.store(false);   // 归还在工作线程：GUI 那头可能已经把窗口关了
        return r;
    }));
}

} // namespace OcrBundle
