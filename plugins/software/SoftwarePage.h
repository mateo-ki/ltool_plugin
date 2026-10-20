#pragma once

#include <QPointer>
#include <QVector>
#include <QWidget>

#include <memory>

class QLabel;
class QLineEdit;
class QPushButton;
class QNetworkAccessManager;
class QVBoxLayout;
struct SoftFill;

// 配置 - 软件页：推荐软件列表，按分类分组，可搜索
// 每行：图标 + 名称 + 状态列（已装版本 / 可一键装 / 需手动下载）+ 操作按钮
// 用 QScrollArea + 行布局（不用 itemWidget，避免宽度塌陷）
class SoftwarePage : public QWidget {
    Q_OBJECT
public:
    explicit SoftwarePage(QWidget* parent = nullptr);

private:
    // 一行（或一个分类表头）的重建/过滤单元
    struct Row {
        QWidget* widget = nullptr;
        QWidget* head = nullptr;   // 所属分类表头
        QString key;               // 搜索命中用：名称 + 分类 + 关键词（小写）
        bool installed = false;
        bool oneClick = false;     // 未装时能不能用 winget 一键装
    };

    void rebuild();
    void buildRowsStep();         // 分批建行控件，别把点菜单那一下堵死
    void clearRows();
    void applyFilter();
    void fillVersions();          // 版本查询丢到后台线程，回来再填状态列
    void applyCachedVersions();
    void openOfficial(const QString& url);
    void showVersions(const QString& name, const QString& keywords, const QString& url,
                      const QString& wingetId);
    void download(const QString& name, const QString& wingetId, const QString& url);
    void installVersion(const QString& name, const QString& wingetId, const QString& version);
    void requestWebIcon(QLabel* iconLabel, const QString& url); // 未安装时抓取站点图标
    void launchOrOpen(const QString& keywords, const QString& url); // 装了就直接起，没装就去官网

    QVBoxLayout* m_rowsLay = nullptr;
    QPushButton* m_refreshBtn = nullptr;
    QLineEdit* m_search = nullptr;
    QLabel* m_status = nullptr;
    QNetworkAccessManager* m_nam = nullptr;
    QVector<Row> m_rows;
    QVector<QWidget*> m_heads;
    QVector<QPointer<QLabel>> m_versionLabels;   // 与 m_versionTargets 同序
    QVector<QString> m_versionTargets;
    std::shared_ptr<SoftFill> m_fill;   // 分批建行的游标，建完就 reset
};
