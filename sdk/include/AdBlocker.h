#pragma once

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QPair>
#include <QStringList>
#include <QVector>
#include <functional>

#include <windows.h>

// 广告弹窗拦截器：进程级常驻（不是挂在设置页上）。
// 之前定时器是 AdsPage 的子对象，而设置页的页面是懒加载、切走即销毁 ——
// 用户勾了"自动拦截"，关掉设置页其实就停了，这就是"感觉没用起来"的病根。
class AdBlocker
{
public:
    struct Hit {
        HWND hwnd = nullptr;   // 只在本次扫描的上下文里用（跨轮次会失效，所以不存进历史）
        QString title;
        QString process;
        DWORD pid = 0;
        int width = 0;         // 窗口尺寸：用来区分"右下角小弹窗"和正常主窗口
        int height = 0;
    };
    struct Record {
        QDateTime when;
        QString title;
        QString process;
        DWORD pid = 0;
        bool closed = false;   // false = 只记录没动手（观察模式）
    };

    // 一个窗口在当前规则下的处置结果，"试跑规则"直接把它列出来
    enum class Verdict { NoMatch, WillClose, TooBig, Allowed, WouldCloseIfNotObserving };

    static AdBlocker& instance();

    void start();                       // 装定时器（幂等），按配置决定是否真关
    QVector<Hit> scan() const;
    static QString exeNameOfPid(DWORD pid);

    bool autoEnabled() const { return m_auto; }
    void setAutoEnabled(bool on);
    bool observeOnly() const { return m_observe; }
    void setObserveOnly(bool on);

    QStringList procPatterns() const;
    QStringList titlePatterns() const;
    static QStringList defaultProcPatterns();
    static QStringList defaultTitlePatterns();
    void setPatterns(const QStringList& proc, const QStringList& title);
    void addProcPattern(const QString& p);
    bool matches(const QString& title, const QString& process) const;

    QStringList allowProcPatterns() const;
    QStringList allowTitlePatterns() const;
    void setAllowPatterns(const QStringList& proc, const QStringList& title);
    void addAllowProc(const QString& p);
    void addAllowTitle(const QString& t);
    bool allowed(const QString& title, const QString& process) const;

    Verdict verdictOf(const Hit& h) const;
    static QString verdictText(Verdict v);

    const QVector<Record>& history() const { return m_history; }
    int closedToday() const;
    void clearHistory();
    QVector<QPair<QDate, int>> dailyCounts(int days) const;
    QVector<QPair<QString, int>> topProcesses(int days, int limit) const;

    // 关掉一个弹窗：先 WM_CLOSE，下一轮还在就可选地结束进程
    int closeHits(const QVector<Hit>& hits);

    // 窗口尺寸超过这个值就当正常程序主界面，不自动关
    static constexpr int kBigWidth = 1100;
    static constexpr int kBigHeight = 820;

private:
    AdBlocker() = default;
    void load();
    void save() const;
    void loadHistory();
    void saveHistory() const;
    void tick();
    void record(const Hit& h, bool closed);

    QVector<Record> m_history;
    QStringList m_proc, m_title;
    QStringList m_allowProc, m_allowTitle;
    QHash<QString, QDateTime> m_lastRecorded;   // 同一个窗口反复命中时别刷屏记数
    bool m_auto = false;
    bool m_observe = false;
    bool m_started = false;
};
