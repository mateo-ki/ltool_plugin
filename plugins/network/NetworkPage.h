#pragma once

#include <QStringList>
#include <QVector>
#include <QWidget>

class QFormLayout;
class QLabel;
class QListWidget;

// 配置 - 网络页：左侧网卡列表（IP Helper 取宽字符接口名）+ 右侧该网卡的网络信息，
// 以及 DNS 一键切换（UAC 提权 netsh）
class NetworkPage : public QWidget {
    Q_OBJECT
public:
    explicit NetworkPage(QWidget* parent = nullptr);

private slots:
    void refreshAdapters();
    void restoreDhcp();
    void flushDns();

private:
    struct Adapter {
        QString name;        // FriendlyName，netsh 用它定位接口
        QString description;
        QString type;
        QString status;
        QString mac;
        QStringList ipv4;
        QStringList ipv6;
        QStringList gateways;
        QStringList dns;
        bool connected = false;
        bool dhcp = false;
        quint32 index = 0;
        quint64 rx = 0;      // bits/s
        quint64 tx = 0;
    };

    void showDetails(int row);
    void applyNetsh(const QString& dnsCommand);
    QString selectedAdapter() const;

    QListWidget* m_list = nullptr;
    QVector<Adapter> m_adapters;
    QLabel* m_title = nullptr;
    QFormLayout* m_form = nullptr;
    QLabel* m_status = nullptr;
};
