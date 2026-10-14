#include "OcrPp.h"

#include "OcrBundle.h"
#include "Utils.h"

#include <QStringList>
#include <QtMath>

#include <windows.h>
#include "onnxruntime_c_api.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

// ---- 量出来的常量（rapidocr_onnxruntime 1.4.4 的 config.yaml + 两个模型的元数据）----
constexpr int kMaxSide = 2000;        // Global.max_side_len：截图长边先压到 2000 以内
constexpr int kMinSide = 30;          // Global.min_side_len：整张图太小就放大
constexpr int kDetLimit = 736;        // Det.limit_side_len + limit_type=min
constexpr float kDetThresh = 0.3f;    // Det.thresh
constexpr float kBoxThresh = 0.5f;    // Det.box_thresh
constexpr float kUnclipRatio = 1.6f;  // Det.unclip_ratio
constexpr int kMinSize = 3;           // DBPostProcess.min_size
constexpr int kRecH = 48;             // Rec.rec_img_shape[1]
constexpr int kRecBaseW = 320;        // Rec.rec_img_shape[2]
constexpr int kOrtThreads = 4;        // 别让它把机器钉满：识别是在后台线程跑的

struct Engine {
    HMODULE lib = nullptr;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSessionOptions* opts = nullptr;
    OrtSession* det = nullptr;
    OrtSession* rec = nullptr;
    OrtMemoryInfo* mem = nullptr;
    OrtAllocator* alloc = nullptr;
    QByteArray detIn, detOut, recIn, recOut;
    QStringList dict;      // 下标就是 CTC 类号：0 是 blank，末尾多一个是空格
};

Engine* g = nullptr;
std::mutex g_mtx;

bool fail(OrtStatus* st, QString* err)
{
    if (!st)
        return true;
    const char* m = g->api->GetErrorMessage(st);
    if (err && m)
        *err = QString::fromUtf8(m);
    g->api->ReleaseStatus(st);
    return false;
}

void discard(Engine* e)
{
    if (!e)
        return;
    const OrtApi* a = e->api;
    if (a) {
        if (e->rec)
            a->ReleaseSession(e->rec);
        if (e->det)
            a->ReleaseSession(e->det);
        if (e->mem)
            a->ReleaseMemoryInfo(e->mem);
        if (e->opts)
            a->ReleaseSessionOptions(e->opts);
        if (e->env)
            a->ReleaseEnv(e->env);
    }
    if (e->lib)
        FreeLibrary(e->lib);
    delete e;
}

QString ioName(OrtSession* s, bool input, size_t index, QString* err)
{
    char* raw = nullptr;
    OrtStatus* st = input ? g->api->SessionGetInputName(s, index, g->alloc, &raw)
                          : g->api->SessionGetOutputName(s, index, g->alloc, &raw);
    if (!fail(st, err))
        return {};
    const QString out = QString::fromUtf8(raw);
    g->api->AllocatorFree(g->alloc, raw);
    return out;
}

bool readDict(QString* err)
{
    OrtModelMetadata* md = nullptr;
    if (!fail(g->api->SessionGetModelMetadata(g->rec, &md), err))
        return false;
    char* raw = nullptr;
    const bool ok = fail(g->api->ModelMetadataLookupCustomMetadataMap(md, g->alloc, "character", &raw),
                         err);
    g->api->ReleaseModelMetadata(md);
    if (!ok)
        return false;
    if (!raw) {
        if (err)
            *err = QStringLiteral("rec 模型的元数据里没有 character 字典（模型版本不对？）");
        return false;
    }
    QStringList lines = QString::fromUtf8(raw).split(QLatin1Char('\n'));
    g->api->AllocatorFree(g->alloc, raw);
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();          // python 那边用的是 splitlines()
    // CTCLabelDecode.get_character：字典尾巴补一个空格，最前面补一个 blank
    QStringList d;
    d.reserve(lines.size() + 2);
    d.push_back(QString());
    d.append(lines);
    d.push_back(QStringLiteral(" "));
    g->dict = d;
    return true;
}

// ---------------- 图像：BGR888 裸字节（通道顺序和 cv2.imread 一致）----------------

struct Bgr {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> px;   // 紧凑排列，无行末填充
};

Bgr toBgr(const QImage& in)
{
    const QImage img = in.convertToFormat(QImage::Format_BGR888);
    Bgr o;
    o.w = img.width();
    o.h = img.height();
    if (o.w <= 0 || o.h <= 0)
        return o;
    o.px.assign(size_t(o.w) * o.h * 3, 0);
    for (int y = 0; y < o.h; ++y)
        memcpy(o.px.data() + size_t(y) * o.w * 3, img.constScanLine(y), size_t(o.w) * 3);
    return o;
}

Bgr scaledBgr(const QImage& src, int w, int h)
{
    return toBgr(src.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

// (v/255 - 0.5)/0.5 —— 和参考实现同一档，就是 v/127.5 - 1
std::vector<float> toChw(const Bgr& img)
{
    const size_t plane = size_t(img.w) * img.h;
    std::vector<float> out(plane * 3);
    for (size_t i = 0; i < plane; ++i) {
        for (int c = 0; c < 3; ++c)
            out[c * plane + i] = float(img.px[i * 3 + c]) / 127.5f - 1.0f;
    }
    return out;
}

// 一行 crop → 缩到 (rw, 48) → 归一化 → 右侧补零到 imgW。
// 补零这一步不能省：模型的画幅是按这一批最宽的那行定的，比它窄就要 pad（参考实现就是
// padding_im[:, :, 0:resized_w] = resized_image），省了就是拿 rw 的缓冲去填 imgW 的形状，
// ORT 当场报 "not enough space"。
std::vector<float> recInput(const QImage& crop, int rw, int imgW)
{
    const Bgr b = scaledBgr(crop, rw, kRecH);
    const size_t plane = size_t(imgW) * kRecH;
    std::vector<float> out(plane * 3, 0.0f);
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < kRecH; ++y) {
            const uint8_t* srcRow = b.px.data() + size_t(y) * b.w * 3;
            float* dst = out.data() + c * plane + size_t(y) * imgW;
            for (int x = 0; x < b.w; ++x)
                dst[x] = float(srcRow[x * 3 + c]) / 127.5f - 1.0f;
        }
    }
    return out;
}

// ---------------- 跑一个 session ----------------

struct Tensor {
    std::vector<float> data;
    std::vector<int64_t> dims;
};

bool runOne(OrtSession* s, const QByteArray& inName, const QByteArray& outName,
            std::vector<float>& data, const std::vector<int64_t>& dims, Tensor* out,
            QString* err)
{
    OrtValue* value = nullptr;
    const size_t bytes = data.size() * sizeof(float);
    if (!fail(g->api->CreateTensorWithDataAsOrtValue(g->mem, data.data(), bytes, dims.data(),
                                                     dims.size(),
                                                     ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                     &value), err))
        return false;
    const OrtValue* inputs[1] = { value };
    const char* inNames[1] = { inName.constData() };
    OrtValue* outputs[1] = { nullptr };
    const char* outNames[1] = { outName.constData() };
    const bool ran = fail(g->api->Run(s, nullptr, inNames, inputs, 1, outNames, 1, outputs), err);
    g->api->ReleaseValue(value);
    if (!ran)
        return false;

    OrtTensorTypeAndShapeInfo* ti = nullptr;
    bool ok = fail(g->api->GetTensorTypeAndShape(outputs[0], &ti), err);
    size_t nDims = 0;
    if (ok)
        ok = fail(g->api->GetDimensionsCount(ti, &nDims), err);
    out->dims.assign(std::max<size_t>(nDims, 1), 0);
    size_t got = 0;
    if (ok)
        ok = fail(g->api->GetDimensions(ti, out->dims.data(), nDims), err);
    if (ok)
        ok = fail(g->api->GetTensorShapeElementCount(ti, &got), err);
    g->api->ReleaseTensorTypeAndShapeInfo(ti);
    float* ptr = nullptr;
    if (ok)
        ok = fail(g->api->GetTensorMutableData(outputs[0], reinterpret_cast<void**>(&ptr)), err);
    if (ok && got > 0)
        out->data.assign(ptr, ptr + got);
    g->api->ReleaseValue(outputs[0]);
    if (!ok)
        return false;
    if (out->data.empty()) {
        if (err)
            *err = QStringLiteral("模型没吐出任何东西");
        return false;
    }
    return true;
}

// ---------------- DB 后处理：二值化 + 膨胀 + 8 邻域连通域 ----------------

struct Region {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    int pixels = 0;
    int w() const { return x1 - x0 + 1; }
    int h() const { return y1 - y0 + 1; }
};

struct Find {
    std::vector<int> parent;
    int find(int i)
    {
        int r = i;
        while (parent[r] != r)
            r = parent[r];
        while (parent[i] != r) {
            const int n = parent[i];
            parent[i] = r;
            i = n;
        }
        return r;
    }
    void un(int a, int b)
    {
        a = find(a);
        b = find(b);
        if (a != b)
            parent[std::max(a, b)] = std::min(a, b);
    }
};

// 阈值 → 2x2 膨胀（等价 np.ones((2,2)) 的 cv2.dilate）→ 两遍扫描的连通域
QVector<Region> components(const std::vector<float>& prob, int W, int H)
{
    std::vector<uint8_t> bin(size_t(W) * H);
    for (size_t i = 0; i < bin.size(); ++i)
        bin[i] = prob[i] > kDetThresh ? 1 : 0;
    std::vector<uint8_t> dil(size_t(W) * H, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t v = bin[size_t(y) * W + x];
            if (!v && x > 0)
                v = bin[size_t(y) * W + x - 1];
            if (!v && y > 0)
                v = bin[size_t(y - 1) * W + x];
            if (!v && x > 0 && y > 0)
                v = bin[size_t(y - 1) * W + x - 1];
            dil[size_t(y) * W + x] = v;
        }
    }
    std::vector<int> lab(size_t(W) * H, 0);
    Find f;
    f.parent.push_back(0);   // 0 号不用（背景）
    int next = 1;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const size_t i = size_t(y) * W + x;
            if (!dil[i])
                continue;
            int best = 0;
            const auto take = [&](int cand) {
                if (cand && (!best || f.find(cand) < f.find(best)))
                    best = cand;
            };
            if (x > 0)
                take(lab[i - 1]);
            if (y > 0) {
                take(lab[i - W]);
                if (x > 0)
                    take(lab[i - W - 1]);
                take(lab[i - W + 1]);
            }
            if (!best) {
                lab[i] = next;
                f.parent.push_back(next);
                ++next;
            } else {
                lab[i] = best;
                // 上面那行的 label 可能比左边的小，得并回去，否则一条字被拆成两截
                if (x > 0 && lab[i - 1])
                    f.un(lab[i - 1], best);
                if (y > 0) {
                    if (lab[i - W])
                        f.un(lab[i - W], best);
                    if (x > 0 && lab[i - W - 1])
                        f.un(lab[i - W - 1], best);
                    if (lab[i - W + 1])
                        f.un(lab[i - W + 1], best);
                }
            }
        }
    }
    QVector<Region> rs(next);
    for (int i = 0; i < next; ++i) {
        rs[i].x0 = W;
        rs[i].y0 = H;
        rs[i].x1 = -1;
        rs[i].y1 = -1;
    }
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const int l = lab[size_t(y) * W + x];
            if (!l)
                continue;
            Region& r = rs[f.find(l)];
            r.x0 = std::min(r.x0, x);
            r.y0 = std::min(r.y0, y);
            r.x1 = std::max(r.x1, x);
            r.y1 = std::max(r.y1, y);
            ++r.pixels;
        }
    }
    QVector<Region> out;
    for (int i = 1; i < next; ++i) {
        if (rs[i].x1 < 0)
            continue;
        out.push_back(rs[i]);
    }
    return out;
}

// 矩形内的概率均值（box_score_fast 在正放的框下就是这个）
float meanIn(const std::vector<float>& prob, int W, const Region& r)
{
    double sum = 0.0;
    int n = 0;
    for (int y = r.y0; y <= r.y1; ++y) {
        for (int x = r.x0; x <= r.x1; ++x) {
            sum += prob[size_t(y) * W + x];
            ++n;
        }
    }
    return n ? float(sum / n) : 0.0f;
}

struct Cand {
    QRect rect;      // 换算回"喂进 det 的那张图"的坐标
    float score = 0.0f;
};

// ---------------- det：整页找文字行 ----------------

QVector<Cand> detectLines(const QImage& prep, QString* err)
{
    const int ow = prep.width(), oh = prep.height();
    // limit_type=min：短边不到 736 就整体放大，再取 32 的倍数
    double ratio = 1.0;
    if (std::min(ow, oh) < kDetLimit)
        ratio = double(kDetLimit) / std::min(ow, oh);
    int dw = int(std::round(ow * ratio / 32.0) * 32);
    int dh = int(std::round(oh * ratio / 32.0) * 32);
    dw = qMax(dw, 32);
    dh = qMax(dh, 32);

    Tensor out;
    std::vector<float> chw = toChw(scaledBgr(prep, dw, dh));
    const std::vector<int64_t> dims { 1, 3, dh, dw };
    if (!runOne(g->det, g->detIn, g->detOut, chw, dims, &out, err))
        return {};
    if (out.dims.size() != 4 || out.dims[2] != dh || out.dims[3] != dw) {
        if (err)
            *err = QStringLiteral("det 输出形状不是 [1,1,H,W]，不敢按这个读");
        return {};
    }

    QVector<Cand> cands;
    for (const Region& r0 : components(out.data, dw, dh)) {
        if (std::min(r0.w(), r0.h()) < kMinSize)
            continue;
        const float score = meanIn(out.data, dw, r0);
        if (score < kBoxThresh)
            continue;
        // unclip：矩形膨胀 distance = area*ratio/周长
        const double dist = double(r0.w()) * r0.h() * kUnclipRatio / (2.0 * (r0.w() + r0.h()));
        Region r = r0;
        r.x0 = qMax(0, int(std::lround(r0.x0 - dist)));
        r.y0 = qMax(0, int(std::lround(r0.y0 - dist)));
        r.x1 = qMin(dw - 1, int(std::lround(r0.x1 + dist)));
        r.y1 = qMin(dh - 1, int(std::lround(r0.y1 + dist)));
        if (std::min(r.w(), r.h()) < kMinSize + 2)
            continue;
        // 回算到喂进 det 的那张图的坐标（det 内部是放大过的）
        const auto back = [&](int v, int lim) {
            return qBound(0, int(std::lround(double(v) / lim * (lim == dw ? ow : oh))),
                          lim == dw ? ow - 1 : oh - 1);
        };
        cands.push_back(Cand { QRect(QPoint(back(r.x0, dw), back(r.y0, dh)),
                                     QPoint(back(r.x1, dw), back(r.y1, dh))), score });
    }
    return cands;
}

// 阅读顺序：先按 y 再按 x，然后同一行内（y 差 < 10px）按 x 冒一下
QVector<Cand> sortByReading(QVector<Cand> boxes)
{
    std::stable_sort(boxes.begin(), boxes.end(), [](const Cand& a, const Cand& b) {
        if (a.rect.top() != b.rect.top())
            return a.rect.top() < b.rect.top();
        return a.rect.left() < b.rect.left();
    });
    for (int i = 0; i + 1 < boxes.size(); ++i) {
        for (int j = i; j >= 0; --j) {
            if (std::abs(boxes[j + 1].rect.top() - boxes[j].rect.top()) < 10
                && boxes[j + 1].rect.left() < boxes[j].rect.left())
                std::swap(boxes[j], boxes[j + 1]);
            else
                break;
        }
    }
    return boxes;
}

// ---------------- rec：一行认一次 ----------------

struct Glyph {
    QChar ch;
    int slot = 0;      // CTC 槽位，用来把字宽映射回这一行的像素
};

struct RecResult {
    QString text;
    float conf = 0.0f;
    QVector<Glyph> glyphs;

};

RecResult recognizeLine(const QImage& prep, const QRect& box, QString* err)
{
    RecResult r;
    const QRect area = box.intersected(prep.rect());
    if (area.width() < 2 || area.height() < 2)
        return r;
    const int cropW = area.width(), cropH = area.height();
    const double whRatio = double(cropW) / cropH;
    // 一批里最大的宽高比决定画幅；单行就按这一行自己来，但要保住 320/48 的下限
    const double maxWh = std::max(double(kRecBaseW) / kRecH, whRatio);
    const int imgW = int(kRecH * maxWh);
    int rw = int(std::ceil(kRecH * whRatio));
    if (rw > imgW)
        rw = imgW;
    rw = qMax(rw, 1);

    std::vector<float> chw = recInput(prep.copy(area), rw, imgW);
    Tensor out;
    const std::vector<int64_t> dims { 1, 3, kRecH, imgW };
    if (!runOne(g->rec, g->recIn, g->recOut, chw, dims, &out, err))
        return r;
    if (out.dims.size() != 3) {
        if (err)
            *err = QStringLiteral("rec 输出不是三维");
        return r;
    }
    const int slotCount = int(out.dims[1]);
    const int classes = int(out.dims[2]);
    if (classes != g->dict.size()) {
        if (err)
            *err = QStringLiteral("rec 输出 %1 类，字典只有 %2 条，对不上")
                       .arg(classes).arg(g->dict.size());
        return r;
    }

    // 贪心解码：每步取 argmax，折掉相邻重复，丢掉 blank
    int prev = 0;
    double confSum = 0.0;
    int confN = 0;
    QString text;
    for (int t = 0; t < slotCount; ++t) {
        const float* row = out.data.data() + size_t(t) * classes;
        int best = 0;
        float bp = row[0];
        for (int c = 1; c < classes; ++c) {
            if (row[c] > bp) {
                bp = row[c];
                best = c;
            }
        }
        if (best != prev) {
            if (best != 0 && best < g->dict.size()) {
                const QString& ch = g->dict.at(best);
                if (!ch.isEmpty()) {
                    text += ch;
                    r.glyphs.push_back(Glyph { ch.at(0), t });
                    confSum += bp;
                    ++confN;
                }
            }
        }
        prev = best;
    }
    r.text = text;
    r.conf = confN ? float(confSum / confN) : 0.0f;
    return r;
}

bool isCjk(const QChar c)
{
    const ushort u = c.unicode();
    return (u >= 0x3000 && u <= 0x303F) || (u >= 0x4E00 && u <= 0x9FFF)
        || (u >= 0xFF00 && u <= 0xFFEF);
}

} // namespace

// ---------------- 对外 ----------------

namespace OcrPp {

bool load(QString* err)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g)
        return true;
    const OcrBundle::Files f = OcrBundle::status();
    if (!f.present) {
        if (err)
            *err = QStringLiteral("识别引擎还没下载完，缺 %1").arg(f.missing);
        return false;
    }
    Engine* e = new Engine;
    g = e;
    const auto bail = [&](const QString& m) {
        discard(e);
        g = nullptr;
        if (err)
            *err = m;
        return false;
    };
    e->lib = LoadLibraryW(f.onnxruntime.toStdWString().c_str());
    if (!e->lib)
        return bail(QStringLiteral("onnxruntime.dll 加载失败（0x%1）")
                        .arg(quint32(GetLastError()), 8, 16, QLatin1Char('0')));
    using GetBaseFn = const OrtApiBase* (*)();
    auto* getBase = reinterpret_cast<GetBaseFn>(GetProcAddress(e->lib, "OrtGetApiBase"));
    if (!getBase)
        return bail(QStringLiteral("这个 onnxruntime.dll 里没有 OrtGetApiBase"));
    const OrtApiBase* base = getBase();
    e->api = base->GetApi(ORT_API_VERSION);
    if (!e->api)
        return bail(QStringLiteral("ONNX Runtime 接口版本对不上（要 %1）").arg(ORT_API_VERSION));

    if (!fail(e->api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "ltool", &e->env), err))
        return bail(err && !err->isEmpty() ? *err : QStringLiteral("建推理环境失败"));
    if (!fail(e->api->CreateSessionOptions(&e->opts), err))
        return bail(QStringLiteral("建不出 session 选项"));
    e->api->SetIntraOpNumThreads(e->opts, kOrtThreads);
    if (!fail(e->api->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &e->mem), err))
        return bail(QStringLiteral("建不出内存信息"));
    if (!fail(e->api->GetAllocatorWithDefaultOptions(&e->alloc), err))
        return bail(QStringLiteral("建不出分配器"));
    const auto open = [&](OrtSession** dst, const QString& path, const char* what) -> bool {
        const std::wstring w = path.toStdWString();
        if (!fail(e->api->CreateSession(e->env, w.c_str(), e->opts, dst), err)) {
            if (err)
                *err = QStringLiteral("%1 模型打不开：%2").arg(QString::fromUtf8(what), *err);
            return false;
        }
        return true;
    };
    if (!open(&e->det, f.det, "文字检测"))
        return bail(err && !err->isEmpty() ? *err : QStringLiteral("det 模型打不开"));
    if (!open(&e->rec, f.rec, "文字识别"))
        return bail(err && !err->isEmpty() ? *err : QStringLiteral("rec 模型打不开"));

    e->detIn = ioName(e->det, true, 0, err).toUtf8();
    e->detOut = ioName(e->det, false, 0, err).toUtf8();
    e->recIn = ioName(e->rec, true, 0, err).toUtf8();
    e->recOut = ioName(e->rec, false, 0, err).toUtf8();
    if (e->detIn.isEmpty() || e->detOut.isEmpty() || e->recIn.isEmpty() || e->recOut.isEmpty())
        return bail(QStringLiteral("问不到模型的输入/输出名"));
    if (!readDict(err))
        return bail(err && !err->isEmpty() ? *err : QStringLiteral("读不到字典"));

    Utils::log(QStringLiteral("ocr engine: %1 · 字典 %2 类（0=blank，尾巴是空格）· det %3→%4 · rec %5→%6")
                   .arg(QString::fromUtf8(base->GetVersionString()))
                   .arg(e->dict.size())
                   .arg(QString::fromLatin1(e->detIn.constData()),
                        QString::fromLatin1(e->detOut.constData()),
                        QString::fromLatin1(e->recIn.constData()),
                        QString::fromLatin1(e->recOut.constData())));
    return true;
}

QVector<Ocr::Line> run(const QImage& image, QString* err)
{
    if (!load(err))
        return {};
    if (image.width() < 8 || image.height() < 8) {
        if (err)
            *err = QStringLiteral("图太小（%1x%2），认不了")
                       .arg(image.width()).arg(image.height());
        return {};
    }
    std::lock_guard<std::mutex> lk(g_mtx);

    // 全局预处理：长边压到 2000 以内、短边不小于 30，再取 32 的倍数（rapidocr 就是这么做的）
    const int rawW = image.width(), rawH = image.height();
    double r = 1.0;
    if (std::max(rawW, rawH) > kMaxSide)
        r = double(kMaxSide) / std::max(rawW, rawH);
    if (int(std::min(rawW, rawH) * r) < kMinSide)
        r = double(kMinSide) / std::min(rawW, rawH);
    int pw = int(std::floor(rawW * r / 32.0) * 32);
    int ph = int(std::floor(rawH * r / 32.0) * 32);
    pw = qMax(pw, 32);
    ph = qMax(ph, 32);
    const double backX = double(rawW) / pw;
    const double backY = double(rawH) / ph;
    const QImage prep = (pw == rawW && ph == rawH)
        ? image.convertToFormat(QImage::Format_BGR888)
        : image.scaled(pw, ph, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
              .convertToFormat(QImage::Format_BGR888);

    QVector<Cand> boxes = sortByReading(detectLines(prep, err));
    if (err && !err->isEmpty() && boxes.isEmpty())
        return {};

    QVector<Ocr::Line> lines;
    for (const Cand& c : boxes) {
        const QRect in = c.rect.intersected(prep.rect());
        if (in.width() < 3 || in.height() < 3)
            continue;
        QString e2;
        const RecResult rec = recognizeLine(prep, in, &e2);
        if (!e2.isEmpty()) {
            if (err && err->isEmpty())
                *err = e2;
            continue;
        }
        if (rec.text.trimmed().isEmpty() || rec.glyphs.isEmpty())
            continue;

        // 词分组：空格断开、中英文切换断开、CTC 槽位之间空得太多（>4）也断开。
        // 空格本身不算进任何一个词（它是"词和词之间的缝"），所以划词时单击一个英文词
        // 选中的就是那个词，不会把整行捞进来。
        QVector<QPair<int, int>> bounds;
        {
            int a = 0;
            for (int i = 1; i <= rec.glyphs.size(); ++i) {
                const bool end = i == rec.glyphs.size();
                bool brk = end;
                if (!end) {
                    const QChar cur = rec.glyphs[i].ch;
                    const QChar prev = rec.glyphs[i - 1].ch;
                    brk = cur == QLatin1Char(' ') || prev == QLatin1Char(' ')
                        || isCjk(cur) != isCjk(prev)
                        || rec.glyphs[i].slot - rec.glyphs[i - 1].slot > 4;
                }
                if (brk) {
                    bounds.push_back(qMakePair(a, i));
                    a = i;
                }
            }
        }
        const int cropW = in.width();
        // 槽位 → 像素：把首字对到框左沿、末字"加一个字距"对到右沿。
        // 两个坑都实测过：直接按 slot*下采样倍率 换像素会整体右移半个字（CTC 前面本来就留着
        // 空槽）；而拿"+1 个槽"当一个字宽又会把最后一个字压成 4px —— 模型给的中文字距是
        // 十来个槽，不是一个。
        const int s0 = rec.glyphs.first().slot;
        const int sN = rec.glyphs.last().slot;
        const double pitch = rec.glyphs.size() >= 2 ? double(sN - s0) / (rec.glyphs.size() - 1)
                                                     : 1.0;
        const double span = double(sN - s0) + pitch;
        const auto xOf = [&](int slot) {
            return span > 0.0 ? int(std::lround(double(slot - s0) / span * cropW)) : 0;
        };

        Ocr::Line L;
        L.rect = QRect(QPoint(int(std::lround(in.left() * backX)), int(std::lround(in.top() * backY))),
                       QPoint(int(std::lround(in.right() * backX)),
                              int(std::lround(in.bottom() * backY))));
        L.text = rec.text;
        for (const QPair<int, int>& gw : bounds) {
            int a = gw.first;
            int b = gw.second;
            while (a < b && rec.glyphs[a].ch == QLatin1Char(' '))
                ++a;
            while (b > a && rec.glyphs[b - 1].ch == QLatin1Char(' '))
                --b;
            if (a >= b)
                continue;
            const int next = (b < rec.glyphs.size()) ? b : -1;
            Ocr::Word W;
            QString text;
            for (int i = a; i < b; ++i)
                text += rec.glyphs[i].ch;
            W.text = text;
            const int x0 = xOf(rec.glyphs[a].slot);
            // 右沿就是下一个词的左沿；这一行最后一个词就顶到框的右沿（归一化已经把
            // "末字+一个字距"对到了那里）
            const int x1 = qMax((next >= 0) ? xOf(rec.glyphs[next].slot) : cropW, x0 + 1);
            QRect wr(int(std::lround(in.left() + x0 * backX)), L.rect.top(),
                     qMax(1, int(std::lround((x1 - x0) * backX))), L.rect.height());
            W.rect = wr.intersected(QRect(0, 0, rawW, rawH));
            if (W.rect.isNull())
                W.rect = QRect(L.rect.left(), L.rect.top(), 1, qMax(1, L.rect.height()));
            L.words.push_back(W);
        }
        lines.push_back(L);
    }
    return lines;
}

} // namespace OcrPp
