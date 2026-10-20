#pragma once

#include <QWidget>

class QTimer;
class QVBoxLayout;

// 配置 - 下载器：类 Motrix 多线程下载（进度条/暂停/继续/删除）
class DownloadsPage : public QWidget {
    Q_OBJECT
public:
    explicit DownloadsPage(QWidget* parent = nullptr);

private slots:
    void addDownload();
    void refreshRows();

private:
    void clearRows();
    void rebuild();

    QVBoxLayout* m_rowsLay = nullptr;
    QTimer* m_poll = nullptr;
};
