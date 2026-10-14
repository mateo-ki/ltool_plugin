#include "OcrPanel.h"

#include "Ocr.h"
#include "OcrBundle.h"
#include "Utils.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <atomic>
#include <memory>

namespace OcrPanel {

void extractFrom(const QImage& image, QWidget* parent)
{
    auto* d = new QDialog(parent);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle(QStringLiteral("提取文字 · 正在识别…"));
    d->resize(580, 400);
    auto* lay = new QVBoxLayout(d);

    auto* note = new QLabel(QStringLiteral(
        "用的是自带的 PaddleOCR（第一次用要下载一次模型，之后完全离线）。"
        "认得不准的地方直接在框里改。"), d);
    note->setObjectName(QStringLiteral("muted"));
    note->setWordWrap(true);
    lay->addWidget(note);

    auto* edit = new QPlainTextEdit(d);
    edit->setPlainText(QStringLiteral("正在识别…"));
    lay->addWidget(edit, 1);

    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    auto* copy = new QPushButton(QStringLiteral("复制全部"), d);
    auto* save = new QPushButton(QStringLiteral("存为 TXT"), d);
    auto* close = new QPushButton(QStringLiteral("关闭"), d);
    btns->addWidget(copy);
    btns->addWidget(save);
    btns->addWidget(close);
    lay->addLayout(btns);

    QObject::connect(close, &QPushButton::clicked, d, &QDialog::accept);
    QObject::connect(copy, &QPushButton::clicked, d, [d, copy, edit] {
        QGuiApplication::clipboard()->setText(edit->toPlainText());
        copy->setText(QStringLiteral("已复制"));
        QTimer::singleShot(1500, d, [copy] { copy->setText(QStringLiteral("复制全部")); });
    });
    QObject::connect(save, &QPushButton::clicked, d, [d, edit] {
        const QString dir = Utils::capturesDir();
        QDir().mkpath(dir);
        const QString suggested = QStringLiteral("%1/ocr-%2.txt").arg(
            dir, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss")));
        const QString got = QFileDialog::getSaveFileName(d, QStringLiteral("存为 TXT"),
                                                         suggested, QStringLiteral("文本文件 (*.txt)"));
        if (got.isEmpty())
            return;
        QFile f(got);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            Utils::log(QStringLiteral("ocr save failed: %1").arg(got));
            return;
        }
        f.write(edit->toPlainText().toUtf8());
        Utils::log(QStringLiteral("ocr text saved: %1").arg(got));
    });

    // 识别要几百毫秒，这期间用户可能已经把窗关了 —— 回调前先确认窗口还在
    auto alive = std::make_shared<std::atomic<bool>>(true);
    QObject::connect(d, &QObject::destroyed, [alive] { alive->store(false); });

    const auto startRecognize = [d, edit, alive, image] {
        Ocr::recognizeLater(image, [d, edit, alive](const QString& text, const QString& err,
                                                    qint64 ms) {
            if (!alive->load())
                return;
            if (text.isEmpty()) {
                edit->setPlainText(err.isEmpty()
                                       ? QStringLiteral("没认出任何文字。试试框大一点、或者字清楚一点的区域。")
                                       : err);
                d->setWindowTitle(QStringLiteral("提取文字 · 没认出来"));
                return;
            }
            edit->setPlainText(text);
            QGuiApplication::clipboard()->setText(text);
            d->setWindowTitle(QStringLiteral("提取文字 · %1 字 / %2ms · 已复制到剪贴板").arg(text.size()).arg(ms));
            Utils::log(QStringLiteral("ocr done: chars=%1 ms=%2").arg(text.size()).arg(ms));
        });
    };

    QString why;
    if (Ocr::available(&why)) {
        startRecognize();
    } else {
        // 第一次用要下引擎：就地给看台（进度写在正文里、标题上带百分比），下完自动接着识别
        edit->setPlainText(QStringLiteral("第一次用要下载识别引擎，下完就一直离线可用。\n\n%1")
                              .arg(OcrBundle::describe()));
        d->setWindowTitle(QStringLiteral("提取文字 · 正在下载识别引擎"));
        Utils::log(QStringLiteral("ocr bundle needed: %1").arg(why));
        OcrBundle::fetchLater(
            d, [edit, d, alive](int pct, const QString& note) {
                if (!alive->load())
                    return;
                edit->setPlainText(QStringLiteral("正在下载识别引擎 %1% · %2\n\n%3")
                                       .arg(pct)
                                       .arg(note, OcrBundle::describe()));
                d->setWindowTitle(QStringLiteral("提取文字 · 下载中 %1%").arg(pct));
            },
            [d, edit, alive, startRecognize](bool ok, const QString& err, const QString&) {
                if (!alive->load())
                    return;
                if (!ok) {
                    edit->setPlainText(QStringLiteral("识别引擎没装上：%1\n\n装不上就重试一次，"
                                                      "或者在能上网的时候手动跑 ltool.exe --ocrget")
                                           .arg(err));
                    d->setWindowTitle(QStringLiteral("提取文字 · 引擎没装上"));
                    return;
                }
                d->setWindowTitle(QStringLiteral("提取文字 · 引擎装好了，正在识别"));
                startRecognize();
            });
    }

    d->show();
    d->raise();
    d->activateWindow();
}

} // namespace OcrPanel
