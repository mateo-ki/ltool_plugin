// 「开机启动分析」这一路的判据，分三层：
//   1) 纯函数：StartupApproved 那串字节怎么读、怎么写回去、什么算系统服务。
//      钉的是**本机真注册表里那两组字节**（ltool/utools = 02 开头、Steam/NeatDM/Cisco/Edge = 03 + FILETIME），
//      不是我自己编的形状 —— 以前这里注释和代码是反的，就是因为它钉了个猜出来的形状。
//   2) 真写：拿一个自己造的 HKCU 临时键（Software\ltool-startuptest-<pid>）走 setStartupEnabled 全程序，
//      写完读回来核对，跑完把那个键整个删掉。**一条都不碰他真实的启动项**。
//   3) 那张页：默认藏了多少条系统服务、汇总那句和列表实际少的行数对不对得上、
//      按钮的启用/禁用和「就地两段」的第一下（第一下必须一个字节都不写）。
//
// 第 3 层要点「开始分析」，那是真扫一次这台机器（读几百个文件的签名，秒级），
// 全程只读；弹窗那条判据是"点了不许有模态框"，所以他的桌上不会多出任何东西。
#include "StartupPage.h"
#include "StartupAudit.h"
#include "Utils.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QMap>
#include <QPushButton>
#include <QRegularExpression>
#include <QString>
#include <QWidget>

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

void say(const char* tag, const char* name, const QString& got, const QString& want, int* bad)
{
    const bool ok = got == want;
    std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                got.toUtf8().constData(), want.toUtf8().constData());
    if (!ok)
        ++*bad;
    std::fflush(stdout);
}

// 本机 %TEMP%\startup-approved.txt 里那两组真字节的形状（手工 reg query 抄回来的常量，
// 判据钉的是"Windows 到底怎么写这个值"，不是"我以为它怎么写"）。
QByteArray on12()      // ltool / org.yuanli.utools
{
    return QByteArray::fromHex("020000000000000000000000");
}
QByteArray off12()     // Steam
{
    return QByteArray::fromHex("030000007BD10ECF8B3BDC01");
}
QByteArray offUwp()    // 24 字节那种（打包应用），首字节同样是 03
{
    return QByteArray::fromHex("030000007BD10ECF8B3BDC01") + QByteArray(12, '\0');
}

QByteArray readBlob(const QString& path, const QString& value)
{
    HKEY root = nullptr;
    QString sub;
    if (path.startsWith(QStringLiteral("HKEY_CURRENT_USER\\"))) {
        root = HKEY_CURRENT_USER;
        sub = path.mid(int(qstrlen("HKEY_CURRENT_USER\\")));
    } else if (path.startsWith(QStringLiteral("HKEY_LOCAL_MACHINE\\"))) {
        root = HKEY_LOCAL_MACHINE;
        sub = path.mid(int(qstrlen("HKEY_LOCAL_MACHINE\\")));
    } else
        return {};
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, (LPCWSTR)sub.utf16(), 0, KEY_READ, &k) != ERROR_SUCCESS)
        return {};
    QByteArray out;
    DWORD size = 0, type = 0;
    if (RegQueryValueExW(k, (LPCWSTR)value.utf16(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS
        && type == REG_BINARY && size > 0 && size < 4096) {
        out.resize(int(size));
        RegQueryValueExW(k, (LPCWSTR)value.utf16(), nullptr, &type,
                         reinterpret_cast<BYTE*>(out.data()), &size);
    }
    RegCloseKey(k);
    return out;
}

// 整棵键的"值名 → 字节"快照。用来证明「第一下确认」真的一个字节都没写。
QMap<QString, QByteArray> snapshot(const QString& path)
{
    QMap<QString, QByteArray> out;
    HKEY root = nullptr;
    QString sub;
    if (path.startsWith(QStringLiteral("HKEY_CURRENT_USER\\"))) {
        root = HKEY_CURRENT_USER;
        sub = path.mid(int(qstrlen("HKEY_CURRENT_USER\\")));
    } else if (path.startsWith(QStringLiteral("HKEY_LOCAL_MACHINE\\"))) {
        root = HKEY_LOCAL_MACHINE;
        sub = path.mid(int(qstrlen("HKEY_LOCAL_MACHINE\\")));
    } else
        return out;
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, (LPCWSTR)sub.utf16(), 0, KEY_READ, &k) != ERROR_SUCCESS)
        return out;
    DWORD vals = 0, maxName = 0, maxData = 0;
    RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &vals, &maxName,
                    &maxData, nullptr, nullptr);
    std::wstring nm(size_t(maxName) + 2, 0);
    QByteArray data(int(maxData) + 8, 0);
    for (DWORD i = 0; i < vals; ++i) {
        DWORD nameLen = DWORD(nm.size()), type = 0, size = DWORD(data.size());
        if (RegEnumValueW(k, i, nm.data(), &nameLen, nullptr, &type, (BYTE*)data.data(), &size)
            != ERROR_SUCCESS)
            continue;
        if (type != REG_BINARY)
            continue;
        out.insert(QString::fromWCharArray(nm.data(), int(nameLen)),
                   data.left(int(size)));   // 非二进制的值不参与这一路，快照里跳过
    }
    RegCloseKey(k);
    return out;
}

const QString cuRun = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run");
const QString cuSf = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder");
const QString lmRun = QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run");

QString yes(bool b)
{
    return b ? QStringLiteral("yes") : QStringLiteral("no");
}

int selectableRows(QListWidget* l)
{
    int n = 0;
    for (int i = 0; i < l->count(); ++i)
        if (l->item(i)->flags() & Qt::ItemIsSelectable)
            ++n;
    return n;
}

// 页面上那两条状态行：一行是汇总（一共几条），一行是选中详情。
// objectName 已经被样式表占了（QLabel#muted 上色），所以判据按 testId 这个动态属性找。
QLabel* label(QWidget* page, const char* testId)
{
    for (QLabel* l : page->findChildren<QLabel*>())
        if (l->property("testId").toByteArray() == testId)
            return l;
    return nullptr;
}

} // namespace

extern "C" Q_DECL_EXPORT int ltoolPluginSelftest()
{
    int bad = 0;

    // ---------- 1) 纯函数：读 ----------
    say("a", "approved_02_is_on", yes(StartupAudit::approvedState(on12()) == Approved::On), "yes", &bad);
    say("a", "approved_03_is_off", yes(StartupAudit::approvedState(off12()) == Approved::Off), "yes", &bad);
    say("a", "approved_03_uwp24_is_off",
        yes(StartupAudit::approvedState(offUwp()) == Approved::Off), "yes", &bad);
    say("a", "approved_empty_is_on",
        yes(StartupAudit::approvedState(QByteArray()) == Approved::On), "yes", &bad);
    // 反面对照：这条以前是红的根源就是把 03 当成"启用"。
    say("a", "approved_03_is_not_on", yes(StartupAudit::approvedState(off12()) == Approved::On), "no", &bad);

    // ---------- 1) 纯函数：写回去的那份 ----------
    const QByteArray flipped = StartupAudit::approvedBlobFor(Approved::Off, on12());
    say("a", "blob_len_kept_12", QString::number(flipped.size()), "12", &bad);
    say("a", "blob_first_byte_03", QString::number(uchar(flipped.at(0))), "3", &bad);
    const QByteArray backOn = StartupAudit::approvedBlobFor(Approved::On, off12());
    say("a", "blob_back_on_first_02", QString::number(uchar(backOn.at(0))), "2", &bad);
    // 关掉时那个 FILETIME（第 5 字节起的 8 字节）原样留着：Explorer 用它记"什么时候关的"，
    // 抹成 0 不会坏，但那是把别人写的东西改了，判据要钉住"只改首字节"。
    say("a", "blob_keeps_filetime",
        backOn.right(8) == off12().right(8) ? QStringLiteral("yes") : QStringLiteral("no"), "yes", &bad);
    const QByteArray fresh = StartupAudit::approvedBlobFor(Approved::Off, QByteArray());
    say("a", "blob_from_empty_len", QString::number(fresh.size()), "12", &bad);
    const QByteArray uwpOff = StartupAudit::approvedBlobFor(Approved::On, offUwp());
    say("a", "blob_keeps_len_24", QString::number(uwpOff.size()), "24", &bad);

    // ---------- 1) 纯函数：什么算系统服务 ----------
    const QString root = QStringLiteral("C:\\Windows");
    say("a", "svc_system32", yes(StartupAudit::isSystemService(QStringLiteral("C:\\Windows\\System32\\svchost.exe"), root)), "yes", &bad);
    say("a", "svc_syswow64", yes(StartupAudit::isSystemService(QStringLiteral("c:\\windows\\syswow64\\thing.exe"), root)), "yes", &bad);
    say("a", "svc_unexpanded_var", yes(StartupAudit::isSystemService(QStringLiteral("%SystemRoot%\\system32\\svc.exe"), root)), "yes", &bad);
    say("a", "svc_forward_slash", yes(StartupAudit::isSystemService(QStringLiteral("C:/Windows/System32/notepad.exe"), root)), "yes", &bad);
    say("a", "svc_steam_not", yes(StartupAudit::isSystemService(QStringLiteral("D:\\Games\\Steam\\bin\\steam.exe"), root)), "no", &bad);
    say("a", "svc_programfiles_not",
        yes(StartupAudit::isSystemService(QStringLiteral("C:\\Program Files\\Tencent\\WeChat\\WeChat.exe"), root)), "no", &bad);
    say("a", "svc_empty_not", yes(StartupAudit::isSystemService(QString(), root)), "no", &bad);
    // 拿不到 SystemRoot（异常环境）时，\system32\ 那一段还是认得出系统服务
    say("a", "svc_no_root_still_matches_32",
        yes(StartupAudit::isSystemService(QStringLiteral("X:\\Windows\\System32\\a.exe"), QString())), "yes", &bad);

    // ---------- 2) 这一页不许动的东西：拒绝 ----------
    StartupItem svc;
    svc.source = StartupItem::Source::Service;
    svc.name = QStringLiteral("SelfTestSvc");
    const QString refuse = StartupAudit::setStartupEnabled(svc, false);
    say("a", "refuse_service_words",
        refuse.startsWith(QStringLiteral("这条不是应用启动项")) ? QStringLiteral("yes") : refuse, "yes", &bad);
    StartupItem noValue = svc;
    noValue.approvedPath = cuRun;   // 有路径没值名 = 同样不能动
    say("a", "refuse_no_value_name",
        StartupAudit::setStartupEnabled(noValue, false).startsWith(QStringLiteral("这条不是应用启动项"))
            ? QStringLiteral("yes") : QStringLiteral("no"), "yes", &bad);
    StartupItem oddHive = svc;
    oddHive.approvedPath = QStringLiteral("FOO\\Software\\Whatever");
    oddHive.approvedValue = QStringLiteral("x");
    say("a", "refuse_unknown_hive",
        StartupAudit::setStartupEnabled(oddHive, false).startsWith(QStringLiteral("认不出这个注册表根"))
            ? QStringLiteral("yes") : QStringLiteral("no"), "yes", &bad);

    // ---------- 2) 真写：自己造的临时键，跑完删掉 ----------
    const QString scratchSub = QStringLiteral("Software\\ltool-startuptest-%1")
                                   .arg(QCoreApplication::applicationPid());
    const QString scratchPath = QStringLiteral("HKEY_CURRENT_USER\\") + scratchSub;
    LPCWSTR scratchW = (LPCWSTR)scratchSub.utf16();
    (void)RegDeleteTreeW(HKEY_CURRENT_USER, scratchW);   // 上一次被强杀留下的残骸先清掉
    StartupItem it;
    it.source = StartupItem::Source::RegistryRun;
    it.name = QStringLiteral("自测那一条");
    it.approvedPath = scratchPath;
    it.approvedValue = QStringLiteral("selftestA");
    const QString v = it.approvedValue;

    say("a", "write_off_returns_ok", StartupAudit::setStartupEnabled(it, false), QString(), &bad);
    QByteArray got = readBlob(scratchPath, v);
    say("a", "write_off_landed_03", QString::number(got.isEmpty() ? -1 : uchar(got.at(0))), "3", &bad);
    say("a", "write_off_len", QString::number(got.size()), "12", &bad);
    say("a", "state_reads_back_off",
        yes(StartupAudit::approvedState(readBlob(scratchPath, v)) == Approved::Off), "yes", &bad);
    say("a", "write_on_returns_ok", StartupAudit::setStartupEnabled(it, true), QString(), &bad);
    got = readBlob(scratchPath, v);
    say("a", "write_on_landed_02", QString::number(got.isEmpty() ? -1 : uchar(got.at(0))), "2", &bad);
    say("a", "twice_off_is_not_an_error", StartupAudit::setStartupEnabled(it, false), QString(), &bad);
    // 原来已经有一份 24 字节的（打包应用那种）：长度必须照原来那份写回去
    {
        HKEY k = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, scratchW, 0, nullptr, 0, KEY_READ | KEY_WRITE,
                            nullptr, &k, nullptr) == ERROR_SUCCESS) {
            const QByteArray seeded = StartupAudit::approvedBlobFor(Approved::On, offUwp());
            QByteArray marked = seeded;
            marked[23] = char(0xA5);        // 尾巴上放个记号，看它有没有被截掉
            RegSetValueExW(k, (LPCWSTR)v.utf16(), 0, REG_BINARY,
                           reinterpret_cast<const BYTE*>(marked.constData()), DWORD(marked.size()));
            RegCloseKey(k);
        }
        say("a", "write_24_returns_ok", StartupAudit::setStartupEnabled(it, false), QString(), &bad);
        got = readBlob(scratchPath, v);
        say("a", "write_24_kept_len", QString::number(got.size()), "24", &bad);
        say("a", "write_24_kept_tail_marker",
            got.size() == 24 && uchar(got.at(23)) == 0xA5 ? QStringLiteral("yes") : QStringLiteral("no"),
            "yes", &bad);
    }
    // HKLM 那一条在普通权限下应当**给出一句人能看懂的失败原因**，不是"改了"也不是闷着返回空
    {
        StartupItem lm = it;
        lm.approvedPath = QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\ltool-startuptest-%1")
                              .arg(QCoreApplication::applicationPid());
        const QString why = StartupAudit::setStartupEnabled(lm, false);
        if (why.isEmpty()) {   // 提权跑的：真写进去了，那就把它删干净再点名这次不算
            (void)RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                                 (LPCWSTR)lm.approvedPath.mid(int(qstrlen("HKEY_LOCAL_MACHINE\\"))).utf16());
            std::printf("SKIP hklm_refusal_reason —— 这次是提权跑的，写成功了（正常权限下才有得测）\n");
        } else
            say("a", "hklm_refusal_reason", why.isEmpty() ? QStringLiteral("no") : QStringLiteral("yes"),
                "yes", &bad);
    }
    // 那把"逐字节快照"的尺子自己得先能量出真变化 —— 不然
    // first_click_writes_nothing 会在一份永远为空的快照上白绿。
    {
        const QMap<QString, QByteArray> s1 = snapshot(scratchPath);
        StartupAudit::setStartupEnabled(it, true);
        const QMap<QString, QByteArray> s2 = snapshot(scratchPath);
        say("a", "snapshot_tool_detects_a_real_write",
            s1 == s2 ? QStringLiteral("no") : QStringLiteral("yes"), "yes", &bad);
    }
    (void)RegDeleteTreeW(HKEY_CURRENT_USER, scratchW);
    say("a", "scratch_key_cleaned",
        readBlob(scratchPath, v).isEmpty() ? QStringLiteral("yes") : QStringLiteral("no"), "yes", &bad);

    // ---------- 3) 那张页 ----------
    StartupPage page;
    page.setAttribute(Qt::WA_DontShowOnScreen, true);
    auto* list = page.findChild<QListWidget*>();
    if (list)
        list->setSelectionBehavior(QAbstractItemView::SelectRows);   // 不然下面逐行选中按"项"来，行为不确定
    auto* toggle = page.findChild<QPushButton*>("startupToggle");
    auto* sysSvc = page.findChild<QCheckBox*>("startupShowSysSvc");
    QLabel* summary = label(&page, "startupSummary");
    QLabel* status = label(&page, "startupStatus");
    say("a", "page_has_list", yes(list != nullptr), "yes", &bad);
    say("a", "page_has_toggle_btn", yes(toggle != nullptr), "yes", &bad);
    say("a", "page_has_showSysSvc", yes(sysSvc != nullptr), "yes", &bad);
    say("a", "toggle_starts_disabled", yes(toggle && !toggle->isEnabled()), "yes", &bad);
    say("a", "sysSvc_off_by_default", yes(sysSvc && !sysSvc->isChecked()), "yes", &bad);
    // 那张页的说明行不能再写"只读"：它现在能关东西，界面必须说实话
    QString tipText;
    for (QLabel* l : page.findChildren<QLabel*>())
        if (l->objectName() == QLatin1String("tip"))
            tipText = l->text();
    say("a", "tip_no_longer_claims_readonly", tipText.contains(QStringLiteral("只读")) ? QStringLiteral("no") : QStringLiteral("yes"), "yes", &bad);
    say("a", "tip_says_how_to_disable",
        tipText.contains(QStringLiteral("禁用这一项")) ? QStringLiteral("yes") : QStringLiteral("no"), "yes", &bad);
    // 那一排从三颗按钮变四颗（多了「禁用这一项」）：量一下 900 宽的页里它会不会被挤到看不见。
    // 隐藏着的控件不跑布局，所以先 show()（WA_DontShowOnScreen 已经设了，不会出现在他桌上）再 resize。
    page.show();
    page.resize(900, 620);
    page.layout()->activate();
    QCoreApplication::processEvents();
    if (toggle && sysSvc) {
        const bool inPage = toggle->x() >= 0 && toggle->x() + toggle->width() <= page.width()
            && toggle->width() >= toggle->minimumSizeHint().width();
        say("a", "toggle_btn_visible_at_900", yes(inPage), "yes", &bad);
        say("a", "showsys_cb_visible_at_900",
            yes(sysSvc->x() >= 0 && sysSvc->x() + sysSvc->width() <= page.width()), "yes", &bad);
        std::printf("INFO startup_toggle_geo=%d+%d of page %d\n", toggle->x(), toggle->width(),
                    page.width());
        std::fflush(stdout);
    }

    // 真扫一次（只读），等后台那趟回来
    QPushButton* scanBtn = nullptr;
    for (QPushButton* b : page.findChildren<QPushButton*>())
        if (b->text() == QStringLiteral("开始分析"))
            scanBtn = b;
    say("a", "page_has_scan_btn", yes(scanBtn != nullptr), "yes", &bad);
    if (scanBtn && list && summary) {
        QElapsedTimer et;
        et.start();
        scanBtn->click();
        // 扫完的标志：列表里有可选的行。注意 QEventLoop::isRunning() 在没 exec() 之前是
        // false —— 拿它当循环条件等于一次都不等（第一次跑就栽在这，120 秒"超时"其实是 0 毫秒）。
        const int deadlineMs = 120000;
        while (selectableRows(list) == 0 && et.elapsed() < deadlineMs)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        const qint64 ms = et.elapsed();
        std::printf("INFO startup_scan_rows=%d ms=%lld\n", selectableRows(list),
                    static_cast<long long>(ms));
        std::fflush(stdout);
        if (selectableRows(list) == 0)
            std::printf("SKIP 整页扫描 —— 120 秒没出结果（这台机器上扫不出来或被卡住），后面的页面判据不算数\n");
        else {
            const QString sum = summary->text();
            QRegularExpression re(QStringLiteral("另有 (\\d+) 条系统服务没列出"));
            const auto m = re.match(sum);
            const int hidden = m.hasMatch() ? m.captured(1).toInt() : -1;
            say("a", "summary_reports_hidden_count", hidden >= 0 ? QStringLiteral("yes") : sum, "yes", &bad);
            const int before = selectableRows(list);
            // 汇总那句"一共 N 项"必须就是列表里现在能选的行数（以前它是 m_items.size()，
            // 藏了一批之后那句会报"一共 120 项"而列表只有 40 行 —— 界面不说真话）
            const QRegularExpression totalRe(QStringLiteral("一共 (\\d+) 项"));
            const int totalClaim = totalRe.match(sum).captured(1).toInt();
            say("a", "summary_total_equals_listed_rows",
                QString::number(totalClaim) + QStringLiteral("/%1").arg(before),
                QString("%1/%1").arg(before), &bad);
            // 分组标题：藏着这一档的时候必须写"系统服务除外"，勾上之后那句要收回去。
            // （以前这条是把标题在**勾上之后**读一遍再要它含"除外" —— 永远红，红的是判据自己）
            auto svcHeaderText = [&] {
                QString t;
                for (int i = 0; i < list->count(); ++i)
                    if (list->item(i)->text().contains(QStringLiteral("后台服务")))
                        t = list->item(i)->text();
                return t;
            };
            const bool headerHonestHidden = hidden == 0
                || svcHeaderText().contains(QStringLiteral("系统服务除外"));
            // 勾上「显示系统服务」：多出来的行数必须正好等于汇总说的那个"没列出"的数（不许悄悄少一批）
            sysSvc->setChecked(true);
            const int after = selectableRows(list);
            const bool headerHonestShown = hidden == 0
                || !svcHeaderText().contains(QStringLiteral("系统服务除外"));
            sysSvc->setChecked(false);
            say("a", "checkbox_reveals_exactly_hidden",
                hidden == 0 ? QStringLiteral("skip") : QString::number(after - before),
                hidden == 0 ? QStringLiteral("skip") : QString::number(hidden), &bad);
            say("a", "group_header_says_except_system",
                hidden == 0 ? QStringLiteral("skip")
                            : (headerHonestHidden && headerHonestShown ? QStringLiteral("yes")
                                                                       : QStringLiteral("no")),
                hidden == 0 ? QStringLiteral("skip") : QStringLiteral("yes"), &bad);

            // 下拉里点名只看「后台服务」而这一档又被默认藏起来：那种"选了怎么一片空"必须有话接着
            if (auto* combo = page.findChild<QComboBox*>()) {
                int svcIdx = -1;
                for (int i = 0; i < combo->count(); ++i)
                    if (combo->itemData(i).toInt() == int(StartupItem::Source::Service))
                        svcIdx = i;
                const int keep = combo->currentIndex();
                combo->setCurrentIndex(svcIdx);
                QCoreApplication::processEvents();
                const QString st = status ? status->text() : QString();
                say("a", "service_filter_explains_the_hidden_batch",
                    (svcIdx < 0 || hidden == 0)
                        ? QStringLiteral("skip")
                        : (st.contains(QStringLiteral("系统服务默认是藏起来的")) ? QStringLiteral("yes")
                                                                                : QStringLiteral("no")),
                    (svcIdx < 0 || hidden == 0) ? QStringLiteral("skip") : QStringLiteral("yes"), &bad);
                combo->setCurrentIndex(keep);
                QCoreApplication::processEvents();
            }

            // 选中每一行：按钮的启用状态必须和状态行那句"这一页不动它"对得上
            int toggleable = 0, mismatched = 0;
            for (int i = 0; i < list->count(); ++i) {
                if (!(list->item(i)->flags() & Qt::ItemIsSelectable))
                    continue;
                list->setCurrentRow(i);
                QCoreApplication::processEvents();
                const QString st = status ? status->text() : QString();
                const bool saysCant = st.contains(QStringLiteral("这一页不动它"));
                if (toggle->isEnabled() == saysCant)
                    ++mismatched;
                if (toggle->isEnabled())
                    ++toggleable;
            }
            say("a", "btn_state_matches_status_line",
                QString::number(mismatched) + QStringLiteral("/%1行").arg(selectableRows(list)),
                QStringLiteral("0/%1行").arg(selectableRows(list)), &bad);
            if (toggleable == 0)
                std::printf("SKIP 两段确认 —— 这台机器上一条能由这页开关的启动项都没扫出来\n");
            else {
                // 挑第一条可关的，量"第一下必须一个字节都不写"
                int target = -1;
                for (int i = 0; i < list->count(); ++i) {
                    if (!(list->item(i)->flags() & Qt::ItemIsSelectable))
                        continue;
                    list->setCurrentRow(i);
                    QCoreApplication::processEvents();
                    if (toggle->isEnabled()) {
                        target = i;
                        break;
                    }
                }
                QMap<QString, QByteArray> b1, b2, b3;
                b1 = snapshot(cuRun);
                b2 = snapshot(cuSf);
                b3 = snapshot(lmRun);
                toggle->click();          // 第一下 = 只确认
                QCoreApplication::processEvents();
                QMap<QString, QByteArray> a1, a2, a3;
                a1 = snapshot(cuRun);
                a2 = snapshot(cuSf);
                a3 = snapshot(lmRun);
                say("a", "first_click_writes_nothing",
                    (a1 == b1 && a2 == b2 && a3 == b3) ? QStringLiteral("yes") : QStringLiteral("no"), "yes", &bad);
                say("a", "first_click_arms_button",
                    toggle->text().startsWith(QStringLiteral("再点一次确认")) ? QStringLiteral("yes") : toggle->text(),
                    "yes", &bad);
                say("a", "first_click_no_modal",
                    QApplication::activeModalWidget() == nullptr ? QStringLiteral("yes") : QStringLiteral("no"),
                    "yes", &bad);
                // 换一行 = 收回那句确认（不然是"看着没关掉、按下去关了别的"）
                int other = -1;
                for (int i = 0; i < list->count(); ++i) {
                    if (i != target && (list->item(i)->flags() & Qt::ItemIsSelectable)) {
                        other = i;
                        break;
                    }
                }
                if (other >= 0) {
                    list->setCurrentRow(other);
                    QCoreApplication::processEvents();
                    say("a", "changing_selection_disarms",
                        toggle->text().startsWith(QStringLiteral("再点一次确认")) ? QStringLiteral("no")
                                                                                 : QStringLiteral("yes"),
                        "yes", &bad);
                } else
                    std::printf("SKIP 换选中收回确认 —— 只有一行可关，没法换\n");
                std::printf("INFO startup_first_toggleable_row=%d of %d rows\n", target, selectableRows(list));
                std::fflush(stdout);
                // 注意：这里**绝不点第二下**。第二下会真改他机器上那条启动项，
                // 那条路径由上面那棵临时键覆盖（同一个 setStartupEnabled）。
            }
        }
    }
    std::printf("STARTUPTEST: 失败 %d 项\n", bad);
    std::fflush(stdout);
    return bad;
}
