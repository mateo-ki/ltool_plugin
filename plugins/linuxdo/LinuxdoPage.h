#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

// linux.do 盯帖那一页：上面摆配置（开关 / 间隔 / 关键词 / 中转前缀），下面摆最近命中的帖子。
// 引擎不在这里（是 LinuxdoWatch 单例，启动时由插件的 ltoolPluginActivate 叫起来），
// 这一页只是它的看台 —— 没打开过页面也照样在后台查，打开了就能看到上一次查的结果。
class LinuxdoPage : public QWidget {
    Q_OBJECT
public:
    explicit LinuxdoPage(QWidget* parent = nullptr);

private:
    void refresh();                     // 把引擎的状态和命中列表原样摆出来
    void openCurrent();

    QCheckBox* m_on = nullptr;
    QSpinBox* m_minutes = nullptr;
    QLineEdit* m_words = nullptr;
    QLineEdit* m_prefix = nullptr;
    QPushButton* m_now = nullptr;
    QPushButton* m_open = nullptr;
    QListWidget* m_hits = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_cats = nullptr;
};
