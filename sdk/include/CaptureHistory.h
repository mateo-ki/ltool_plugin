#pragma once

#include <QPixmap>
#include <QString>
#include <QVector>

// 截图历史：内存里留着最近若干张，同时落盘到存放目录（默认在软件目录下，见 Utils::capturesDir），
// 所以重启 ltool 之后置顶窗左侧的往期截图还在。
// 每条给一个 key（就是它落盘的文件名）：置顶窗编辑完要写回"自己那一条"，
// 用下标写会被后来的截图把下标挤掉，写错条目。
//
// 为什么它在 core 而截图那三个窗口在 plugins/capture：core 的配置页要改存放目录
// （改完 reload()、还要在界面上写"往期共 N 张"），而 core 不能反向依赖插件 dll。
// 于是这份"往期列表"留在共享的那一份里 —— 界面关掉时两份也不会各存一份内存状态。
class CaptureHistory {
public:
    struct Item {
        QPixmap pm;
        QString key;
    };

    static QString add(const QPixmap& pm);                // 入列并存盘，返回 key
    // 编辑结果写回同一条。write=false 只更新内存：马赛克是逐个鼠标事件在改图，
    // 每改一下都 PNG 编码落盘会把手感拖死，所以只在松手时写一次。
    static void replace(const QString& key, const QPixmap& pm, bool write = true);
    static int count();
    static Item at(int index);                            // 越界返回空条目
    // 右键缩略图删除用：去掉内存里第 index 张，并删掉它落盘的那个文件，返回它的 key。
    // 只按 key 删不行：存盘失败过的条目 key 是空的，也得能从列表里拿掉。
    static QString removeAt(int index);
    static void clear();                                  // 清空内存 + 删掉 captures 里的文件
    // 配置页改了存放目录之后调这个：丢掉内存里的旧目录条目，按新目录重读一遍
    // （不删文件 —— 新目录里可能本来就有你自己的截图）
    static void reload();

private:
    struct Entry {
        QPixmap pm;
        QString key;    // captures 目录里的文件名；存不下时为空
    };
    static QVector<Entry>& items();
    static void load(bool trim = true);   // 首次访问时才扫盘读取；trim=false 不清理超额文件
    static QString dir();
};

// F1 浮层开没开 —— core 侧那一个状态位。
//
// 浮层本体在 plugins/capture，而 core 的搜索框要在浮层举着的时候先别收起
// （窗口一 hide，浮层的"窗口吸附"就找不到搜索框了）。插件→core 单向调用可以，
// 反过来不行，所以把这个状态放在 core：浮层开合时由插件写、core 读。
// 只放这一个 bool，不搞事件总线。
namespace CaptureState {

void setActive(bool on);
bool isActive();

} // namespace CaptureState
