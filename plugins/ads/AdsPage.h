#pragma once

#include <QWidget>

#include "AdBlocker.h"

class QListWidget;
class QCheckBox;
class QLabel;

// 配置 - 广告弹窗页：
//   当前桌面上命中的弹窗（关窗 / 结束进程 / 加进名单）
//   最近拦截记录（让人看得见"它到底有没有在工作"）
//   自动拦截开关（常驻，关掉设置页也继续生效）+ 观察模式（只记录不关，先验证会不会误杀）
class AdsPage : public QWidget {
    Q_OBJECT
public:
    explicit AdsPage(QWidget* parent = nullptr);

private slots:
    void rescan();
    void closeSelected();
    void killSelected();
    void blockSelected();
    void allowSelected();
    void dryRun();
    void editRules();
    void rebuildHistory();

private:
    void renderStats();

    QListWidget* m_list = nullptr;
    QListWidget* m_history = nullptr;
    QCheckBox* m_auto = nullptr;
    QCheckBox* m_observe = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_stats = nullptr;
    QVector<AdBlocker::Hit> m_hits;
};
