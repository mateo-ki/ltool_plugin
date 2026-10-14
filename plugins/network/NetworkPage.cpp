#include "NetworkPage.h"
#include "Utils.h"

#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QHostAddress>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <QtEndian>

#include <algorithm>

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <shellapi.h>

namespace {

struct DnsPreset {
    const char* name;
    const char* primary;
    const char* secondary;
};

const DnsPreset kPresets[] = {
    {"阿里 DNS", "223.5.5.5", "223.6.6.6"},
    {"腾讯 DNSPod", "119.29.29.29", "119.28.28.28"},
    {"114 DNS", "114.114.114.114", "114.114.115.115"},
    {"Google DNS", "8.8.8.8", "8.8.4.4"},
};

QString ipOf(const sockaddr* sa)
{
    if (!sa)
        return {};
    QHostAddress a;
    if (sa->sa_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(sa);
        a.setAddress(qFromBigEndian<quint32>(v4->sin_addr.S_un.S_addr));
        return a.toString();
    }
    if (sa->sa_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(sa);
        QHostAddress a(*reinterpret_cast<const Q_IPV6ADDR*>(&v6->sin6_addr));
        return a.toString();
    }
    return {};
}

QString ifTypeName(ULONG type)
{
    switch (type) {
    case IF_TYPE_ETHERNET_CSMACD: return QStringLiteral("以太网");
    case IF_TYPE_IEEE80211:       return QStringLiteral("WiFi");
    case IF_TYPE_TUNNEL:          return QStringLiteral("隧道 / VPN");
    case IF_TYPE_PPP:             return QStringLiteral("PPP");
    case IF_TYPE_IEEE1394:        return QStringLiteral("1394");
    case 53: /* IF_TYPE_PROP_VIRTUAL */ return QStringLiteral("虚拟接口");
    case IF_TYPE_WWANPP:
    case IF_TYPE_WWANPP2:         return QStringLiteral("蜂窝网络");
    default:                      return QString("类型 %1").arg(type);
    }
}

QString operName(ULONG s)
{
    switch (s) {
    case IfOperStatusUp:             return QStringLiteral("已连接");
    case IfOperStatusDown:           return QStringLiteral("未连接");
    case IfOperStatusTesting:        return QStringLiteral("检测中");
    case IfOperStatusDormant:        return QStringLiteral("待激活");
    case IfOperStatusNotPresent:     return QStringLiteral("设备不在位");
    case IfOperStatusLowerLayerDown: return QStringLiteral("下层未连接");
    default:                         return QStringLiteral("未知");
    }
}

QString macOf(const uchar* addr, ULONG len)
{
    QString s;
    for (ULONG i = 0; i < len; ++i) {
        if (i)
            s += '-';
        s += QString("%1").arg(addr[i], 2, 16, QChar('0'));
    }
    return s.toUpper();
}

QString rateOf(quint64 bitsPerSec)
{
    if (bitsPerSec == 0)
        return QStringLiteral("-");
    if (bitsPerSec >= 1000000000ULL)
        return QString("%1 Gbps").arg(bitsPerSec / 1000000000.0, 0, 'f', 1);
    if (bitsPerSec >= 1000000ULL)
        return QString("%1 Mbps").arg(bitsPerSec / 1000000.0, 0, 'f', 0);
    return QString("%1 Kbps").arg(bitsPerSec / 1000.0, 0, 'f', 0);
}

QLabel* makeValue(const QString& text)
{
    auto* l = new QLabel(text.isEmpty() ? QStringLiteral("-") : text);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setWordWrap(true);
    return l;
}

// 左侧网卡行自绘：状态点 + 接口名 + "类型 · 状态 · IPv4"，避免长名互相挤压
class AdapterDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& idx) const override
    {
        const bool selected = opt.state & QStyle::State_Selected;
        const bool hover = opt.state & QStyle::State_MouseOver;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect card = opt.rect.adjusted(4, 2, -6, -4);
        if (selected)
            p->fillRect(card, QColor("#3f6ae0"));
        else if (hover)
            p->fillRect(card, QColor("#2a2b31"));

        const int left = card.left() + 16;
        QFont nameFont = opt.font;
        nameFont.setPointSizeF(10.0);
        QFontMetrics nm(nameFont);
        p->setFont(nameFont);
        p->setPen(selected ? QColor("#ffffff") : QColor("#dce0e9"));
        p->drawText(QRect(left, card.top() + 5, card.right() - left - 8, nm.height()),
                    Qt::AlignLeft | Qt::AlignVCenter | Qt::ElideRight,
                    idx.data(Qt::DisplayRole).toString());

        QFont subFont = opt.font;
        subFont.setPointSizeF(8.5);
        QFontMetrics sm(subFont);
        p->setFont(subFont);
        p->setPen(selected ? QColor("#e2e8fb") : QColor("#8b93a1"));
        p->drawText(QRect(left, card.top() + 7 + nm.height(), card.right() - left - 8,
                          sm.height()),
                    Qt::AlignLeft | Qt::AlignVCenter | Qt::ElideRight,
                    idx.data(Qt::UserRole).toString());

        p->setPen(Qt::NoPen);
        p->setBrush(idx.data(Qt::UserRole + 1).toBool() ? QColor("#33b86b")
                                                        : QColor("#5a606c"));
        p->drawEllipse(QPoint(card.left() + 7, card.center().y()), 3, 3);
        p->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const override
    {
        Q_UNUSED(opt);
        Q_UNUSED(idx);
        return QSize(238, 50);
    }
};

} // namespace

NetworkPage::NetworkPage(QWidget* parent)
    : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* split = new QSplitter(this);

    m_list = new QListWidget(split);
    m_list->setFixedWidth(246);
    m_list->setUniformItemSizes(true);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setItemDelegate(new AdapterDelegate(m_list));
    m_list->setToolTip("网卡接口名取自系统接口 API（宽字符），不再解析 netsh 文本，中文接口名不会乱码");

    auto* right = new QWidget(split);
    auto* rightLay = new QVBoxLayout(right);
    rightLay->setContentsMargins(12, 4, 4, 4);

    auto* infoGroup = new QGroupBox("网络信息", right);
    auto* infoLay = new QVBoxLayout(infoGroup);
    m_title = new QLabel(infoGroup);
    QFont titleFont = m_title->font();
    titleFont.setPointSizeF(12.0);
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    infoLay->addWidget(m_title);
    m_form = new QFormLayout;
    m_form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_form->setHorizontalSpacing(14);
    m_form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    infoLay->addLayout(m_form);
    infoLay->addStretch(1);

    auto* dnsGroup = new QGroupBox("DNS 快速切换（作用于左侧选中的网卡，需要 UAC 授权）", right);
    auto* dnsLay = new QHBoxLayout(dnsGroup);
    for (const DnsPreset& preset : kPresets) {
        auto* b = new QPushButton(QString("%1\n%2").arg(preset.name, preset.primary), dnsGroup);
        b->setMinimumSize(110, 46);
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, preset] {
            const QString adapter = selectedAdapter();
            if (adapter.isEmpty()) {
                QMessageBox::information(this, "ltool", "请先在左侧选择网卡");
                return;
            }
            applyNetsh(QString("interface ip set dns name=\"%1\" static %2 primary && "
                               "netsh interface ip add dns name=\"%1\" %3 index=2 && "
                               "ipconfig /flushdns")
                           .arg(adapter, preset.primary, preset.secondary));
        });
        dnsLay->addWidget(b);
    }
    auto* dhcp = new QPushButton("恢复自动获取\n(DHCP)", dnsGroup);
    dhcp->setMinimumSize(110, 46);
    dnsLay->addWidget(dhcp);
    dnsLay->addStretch(1);

    auto* bottom = new QHBoxLayout;
    auto* refreshBtn = new QPushButton("刷新网卡", right);
    refreshBtn->setObjectName("primary");
    auto* flush = new QPushButton("刷新 DNS 缓存", right);
    m_status = new QLabel(right);
    m_status->setObjectName("muted");
    bottom->addWidget(refreshBtn);
    bottom->addWidget(flush);
    bottom->addWidget(m_status, 1);

    rightLay->addWidget(infoGroup, 1);
    rightLay->addWidget(dnsGroup);
    rightLay->addLayout(bottom);

    split->addWidget(m_list);
    split->addWidget(right);
    split->setStretchFactor(1, 1);
    outer->addWidget(split);

    connect(m_list, &QListWidget::currentRowChanged, this, &NetworkPage::showDetails);
    connect(refreshBtn, &QPushButton::clicked, this, &NetworkPage::refreshAdapters);
    connect(dhcp, &QPushButton::clicked, this, &NetworkPage::restoreDhcp);
    connect(flush, &QPushButton::clicked, this, &NetworkPage::flushDns);

    refreshAdapters();
}

QString NetworkPage::selectedAdapter() const
{
    const int row = m_list->currentRow();
    return row >= 0 && row < m_adapters.size() ? m_adapters[row].name : QString();
}

void NetworkPage::refreshAdapters()
{
    const QString keep = m_list->currentItem()
        ? m_list->currentItem()->text()
        : QString();
    m_adapters.clear();
    m_list->clear();

    ULONG bufLen = 40 * 1024;
    QByteArray buf(int(bufLen), Qt::Uninitialized);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &bufLen);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(int(bufLen));
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &bufLen);
    }
    if (rc != NO_ERROR) {
        m_status->setText(QString("枚举网卡失败（错误码 %1）").arg(rc));
        Utils::log(QString("GetAdaptersAddresses failed rc=%1").arg(rc));
        return;
    }

    for (const auto* a = addrs; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        Adapter ad;
        ad.name = QString::fromWCharArray(a->FriendlyName);
        ad.description = QString::fromWCharArray(a->Description);
        ad.type = ifTypeName(a->IfType);
        ad.status = operName(a->OperStatus);
        ad.connected = a->OperStatus == IfOperStatusUp;
        ad.index = a->IfIndex;
        ad.mac = macOf(a->PhysicalAddress, a->PhysicalAddressLength);
        ad.dhcp = (a->Flags & IP_ADAPTER_DHCP_ENABLED) != 0;
        ad.rx = a->ReceiveLinkSpeed;
        ad.tx = a->TransmitLinkSpeed;
        for (const auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const sockaddr* sa = u->Address.lpSockaddr;
            const QString ip = ipOf(sa);
            if (ip.isEmpty())
                continue;
            const QString text = u->OnLinkPrefixLength
                ? QString("%1/%2").arg(ip).arg(u->OnLinkPrefixLength)
                : ip;
            if (sa->sa_family == AF_INET) {
                if (!ip.startsWith("169.254."))
                    ad.ipv4 << text;
            } else if (!ip.startsWith("fe80")) {
                ad.ipv6 << text;
            }
        }
        for (const auto* g = a->FirstGatewayAddress; g; g = g->Next) {
            const QString ip = ipOf(g->Address.lpSockaddr);
            if (!ip.isEmpty())
                ad.gateways << ip;
        }
        for (const auto* d = a->FirstDnsServerAddress; d; d = d->Next) {
            const QString ip = ipOf(d->Address.lpSockaddr);
            if (!ip.isEmpty())
                ad.dns << ip;
        }
        m_adapters.append(ad);
    }
    // 已连接的网卡排前面
    std::stable_sort(m_adapters.begin(), m_adapters.end(),
                     [](const Adapter& x, const Adapter& y) {
                         return x.connected && !y.connected;
                     });
    for (const Adapter& ad : m_adapters) {
        QString sub = QString("%1 · %2").arg(ad.type, ad.status);
        if (!ad.ipv4.isEmpty())
            sub += QString(" · %1").arg(ad.ipv4.first().section('/', 0, 0));
        auto* it = new QListWidgetItem(ad.name, m_list);
        it->setData(Qt::UserRole, sub);
        it->setData(Qt::UserRole + 1, ad.connected);
        it->setToolTip(QString("%1\n%2").arg(ad.name, ad.description));
    }

    int row = 0;
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->text() == keep)
            row = i;
    }
    if (m_list->count() > 0)
        m_list->setCurrentRow(row);
    m_status->setText(QString("共 %1 个网卡接口").arg(m_adapters.size()));
}

void NetworkPage::showDetails(int row)
{
    while (m_form->rowCount() > 0)
        m_form->removeRow(0);
    if (row < 0 || row >= m_adapters.size()) {
        m_title->clear();
        m_form->addRow("状态", makeValue("没有可用的网卡接口"));
        return;
    }
    const Adapter& ad = m_adapters[row];
    m_title->setText(ad.name);
    m_form->addRow("驱动描述", makeValue(ad.description));
    m_form->addRow("类型", makeValue(ad.type));
    m_form->addRow("状态", makeValue(ad.status));
    m_form->addRow("IP / DNS 获取", makeValue(ad.dhcp ? QStringLiteral("DHCP（自动）")
                                                      : QStringLiteral("手动 / 静态")));
    m_form->addRow("接口索引", makeValue(QString::number(ad.index)));
    m_form->addRow("MAC 地址", makeValue(ad.mac));
    m_form->addRow("速率（收/发）",
                   makeValue(QString("%1 / %2").arg(rateOf(ad.rx), rateOf(ad.tx))));
    m_form->addRow("IPv4", makeValue(ad.ipv4.join('\n')));
    m_form->addRow("IPv6", makeValue(ad.ipv6.join('\n')));
    m_form->addRow("网关", makeValue(ad.gateways.join('\n')));
    m_form->addRow("DNS 服务器", makeValue(ad.dns.join('\n')));
}

void NetworkPage::applyNetsh(const QString& dnsCommand)
{
    // netsh 修改 DNS 需要管理员：ShellExecute runas 弹 UAC
    const QString params = QString("/c %1").arg(dnsCommand);
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = reinterpret_cast<LPCWSTR>(params.utf16());
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (!ShellExecuteExW(&sei)) {
        m_status->setText("未获得管理员授权，未做修改");
        return;
    }
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 20000);
        CloseHandle(sei.hProcess);
    }
    m_status->setText("DNS 已更新并刷新缓存");
    Utils::log(QString("dns changed: %1").arg(dnsCommand));
    refreshAdapters();
}

void NetworkPage::restoreDhcp()
{
    const QString adapter = selectedAdapter();
    if (adapter.isEmpty()) {
        QMessageBox::information(this, "ltool", "请先在左侧选择网卡");
        return;
    }
    applyNetsh(QString("interface ip set dns name=\"%1\" dhcp && ipconfig /flushdns")
                   .arg(adapter));
}

void NetworkPage::flushDns()
{
    QProcess::startDetached("ipconfig", {"/flushdns"});
    m_status->setText("DNS 缓存已刷新");
}
