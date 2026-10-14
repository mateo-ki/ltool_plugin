#pragma once

#include <QDialog>
#include <QVector>

#include "Utils.h"

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QProcess;

// 后台静默安装的日志看台：全局唯一、非模态。
// 装了任务不再自动弹这个窗——托盘图标转圈表示"有安装在跑"，点一下托盘才打开。
// 安装进程挂在 qApp 名下，关掉这个窗口只会隐藏它，安装继续跑，历史任务和输出都还在。
class SilentInstallLog : public QDialog {
    Q_OBJECT
public:
    // 登记一次安装并返回任务号，后续 append/finish 用它定位
    static int begin(const QString& title, const QString& dir, const QString& command);
    // 原始输出字节（winget 混着 UTF-8 和 GBK，按行试 UTF-8、失败退回本地编码）
    static void append(int jobId, const QByteArray& chunk);
    static void finish(int jobId, int exitCode);
    // 弹出看台；jobId < 0 表示选最后一个任务。设置页开着时作为它的子窗口，
    // 这样始终压在设置页上面（而不是被盖住）。
    static void showLog(int jobId = -1);

    // 把发起安装的进程交给看台保管，用来支持「取消安装」（终止整棵进程树）
    static void attachProcess(int jobId, QProcess* proc);
    static bool busy();                 // 还有在跑的安装
    static int busyCount();
    // 终止这次安装（连子进程一起）。杀不动（安装器已提权）时返回 false，
    // 但照样标成"已取消"，让排队安装的下一项能接着走。
    static bool cancel(int jobId);
    static bool wasCancelled(int jobId);   // done 回调据此区分"取消"和"失败"

private:
    struct Job {
        int id = -1;
        QString title;
        QString dir;
        QString command;
        QString log;
        Utils::ConsoleLines lines;   // 攒整行再解码
        int code = -1;    // -1 = 进行中
        bool cancelled = false;   // 用户主动取消（结束时按"已取消"而不是"失败"显示）
    };

    explicit SilentInstallLog(QWidget* parent);

    static QVector<Job>& jobs();
    static int indexOf(int jobId);
    static void showText(int jobId, const QString& text);
    int rowOf(int jobId) const;
    void rebuildJobs();
    void selectRow(int row);
    void refreshCancel();

    QListWidget* m_jobList = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QLabel* m_head = nullptr;
    QPushButton* m_cancel = nullptr;
    int m_shownJob = -1;
};
