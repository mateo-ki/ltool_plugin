#pragma once

#include <QHash>
#include <QVector>
#include <QWidget>

#include "DevEnv.h"

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;

// 配置 - 语言页（向导式三步，步骤可随便点回去看）：
//   ① 检测环境   每个组件一行：状态徽章 + 版本 + 检测命令/winget 包
//   ② 安装缺失   单装、全部静默安装、按 winget 版本装；装不了的组件把办法写在行里
//   ③ 配好环境   下拉只切换本机已装版本，写 PATH / JAVA_HOME 走按钮
// 关联软件与配置文件收进底部折叠区，默认不占地方。
class LanguagePage : public QWidget {
    Q_OBJECT
public:
    explicit LanguagePage(QWidget* parent = nullptr);

private slots:
    void selectLanguage(int row);
    void rescan();
    void setStep(int step);
    void installAllMissing();
    void installOne(const QString& wingetId, const QString& label, const QString& version);
    void installNext();
    void applyActiveVersion();
    void fixEnv();
    void showProbe(const QString& label, const QStringList& cmd);

private:
    struct Row {
        QWidget* w = nullptr;
        QLabel* status = nullptr;
    };

    void buildShell();
    void renderAll();
    void renderDetect();
    void renderInstall();
    void renderEnv();
    void renderMore();
    void updateHeader();
    void setStatus(const QString& text);
    QWidget* makeRow(const QString& name, const QString& badge, const QString& badgeColor,
                     const QString& detail, QLabel** statusOut = nullptr);
    QStringList missingIds(QStringList* labelsOut) const;
    QString activeVersionPath() const;
    void openConfigFile(QListWidget* list, int row);
    void openRelatedSoftware(QListWidget* list, int row);

    // 骨架
    QListWidget* m_langList = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_summary = nullptr;
    QPushButton* m_rescanBtn = nullptr;
    QToolButton* m_chips[3] = {};
    QStackedWidget* m_steps = nullptr;
    QVBoxLayout* m_lays[3] = {};
    QLabel* m_status = nullptr;
    QToolButton* m_moreBtn = nullptr;
    QWidget* m_morePanel = nullptr;
    QVBoxLayout* m_moreLay = nullptr;

    // 第 3 步的控件（重建时复用）
    QComboBox* m_versionBox = nullptr;
    QPushButton* m_useBtn = nullptr;
    QLabel* m_envDetail = nullptr;

    // 数据
    DevPlugin m_plugin;
    QString m_currentLang;
    bool m_hasDetect = false;
    bool m_detecting = false;

    // 安装队列（winget 同一时刻只允许一个安装，所以串行）
    QString m_queueLang;
    QStringList m_queueIds;
    QStringList m_queueLabels;
    int m_queueDone = 0;
    QHash<QString, Row> m_rows;   // wingetId → 第 2 步的行，用来就地更新状态
};
