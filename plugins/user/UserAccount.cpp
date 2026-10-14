#include "UserAccount.h"

#include "Utils.h"

#include <QDir>
#include <QCheckBox>
#include <QFile>
#include <QMessageBox>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QFormLayout>
#include <QLabel>

#include <windows.h>
#include <shellapi.h>

namespace {

// 一次 powershell 把该读的都读回来。全是只读命令。
// 输出统一 UTF-8：PS5.1 默认按控制台代码页出，中文显示名会变乱码，所以第一行先改 OutputEncoding。
const char* kReadScript =
    "[Console]::OutputEncoding=[Text.Encoding]::UTF8\n"
    "$n=$env:USERNAME; $m=$env:COMPUTERNAME\n"
    "\"MACHINE=$m\"; \"NAME=$n\"\n"
    "$u=$null; try{$u=Get-LocalUser -Name $n -ErrorAction Stop}catch{}\n"
    "if($u){\"LOCAL=1\"; \"SID=\"+$u.SID.Value}else{\"LOCAL=0\"; \"SID=\"}\n"
    "$f=''\n"
    "try{$f=([adsi]\"WinNT://$m/$n,user\").FullName}catch{}\n"
    "\"FULL=$f\"\n"
    "$p=''\n"
    "if($u){try{$p=(Get-ItemProperty \"HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\$($u.SID.Value)\" "
    "-Name ProfileImagePath -ErrorAction Stop).ProfileImagePath}catch{}}\n"
    "\"PROFILE=$p\"\n"
    // 微软账户的痕迹：这个键只有登录过 MSA 的机器上才有。只拿来出提示，不据此禁用按钮 ——
    // 它不是权威判据，禁错了就把功能整个锁死在本地账户上。
    "$msa='0'\n"
    "if(Test-Path 'HKCU:\\Software\\Microsoft\\IdentityCRL\\NumberedSignin'){$msa='1'}\n"
    "if(Test-Path 'HKCU:\\Software\\Microsoft\\IdentityCRL\\UserExtendedProperties'){$msa='1'}\n"
    "\"MSA=$msa\"\n";

QString trimmed(const QString& s)
{
    QString t = s;
    while (t.endsWith(QLatin1Char('\r')) || t.endsWith(QLatin1Char('\n')))
        t.chop(1);
    return t.trimmed();
}

} // namespace

namespace UserAccount {

Info read()
{
    Info out;
    QProcess p;
    p.start(QStringLiteral("powershell"),
            { QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
              QStringLiteral("-Command"), QString::fromUtf8(kReadScript) });
    if (!p.waitForFinished(15000)) {
        out.error = QStringLiteral("powershell 没在 15 秒内回话");
        return out;
    }
    const QString text = QString::fromUtf8(p.readAllStandardOutput());
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString k = line.left(eq).trimmed();
        const QString v = trimmed(line.mid(eq + 1));
        if (k == QLatin1String("MACHINE")) out.machine = v;
        else if (k == QLatin1String("NAME")) out.name = v;
        else if (k == QLatin1String("LOCAL")) out.local = (v == QLatin1String("1"));
        else if (k == QLatin1String("SID")) out.sid = v;
        else if (k == QLatin1String("FULL")) out.fullName = v;
        else if (k == QLatin1String("PROFILE")) out.profileDir = v;
        else if (k == QLatin1String("MSA")) out.msaHint = (v == QLatin1String("1"));
    }
    if (out.name.isEmpty()) {
        out.error = QStringLiteral("没读回账户名：%1").arg(trimmed(p.readAllStandardError()));
        return out;
    }
    out.readable = true;
    return out;
}

QString psQuote(const QString& s)
{
    // PowerShell 的单引号串里唯一的转义就是把 ' 写成 ''，别的字符都是字面量 ——
    // 这段是要拿去提权执行的，不能指望调用方记得引号
    QString out = QStringLiteral("'");
    for (const QChar& c : s) {
        if (c == QLatin1Char('\''))
            out += QLatin1String("''");
        else
            out += c;
    }
    return out + QLatin1Char('\'');
}

QStringList reject(const QString& proposed, bool forAccountName)
{
    QStringList why;
    const QString s = proposed.trimmed();
    if (s.isEmpty())
        why << QStringLiteral("名字是空的");
    if (s.contains(QRegularExpression(QStringLiteral("[\\r\\n\\0]"))))
        why << QStringLiteral("名字里有换行");
    // 账户名和显示名都不能带这些：它们是 Windows 的对象名限制，不是我个人偏好
    static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|]"));
    if (s.contains(bad))
        why << QStringLiteral("不能含这些字符：\\\\ / : * ? \" < > |");
    if (forAccountName) {
        if (s.size() > 20)
            why << QStringLiteral("账户名最长 20 个字符（显示名可以长一点）");
        if (s.startsWith(QLatin1Char(' ')) || s.endsWith(QLatin1Char(' ')))
            why << QStringLiteral("账户名首尾不能是空格");
    } else if (s.size() > 64) {
        why << QStringLiteral("显示名别超过 64 个字符");
    }
    return why;
}

QString fullNameScript(const QString& machine, const QString& name, const QString& newFull)
{
    // 显示名 = SAM 里那个 FullName。登录界面、开始菜单头像、锁屏上显示的都是它。
    // 不动账户名，更不动用户目录。
    // ADSI 那串路径是**拼**出来的："WinNT://" + '机' + '/' + '号' + ',user'。
    // 整串塞进双引号里再套一层单引号，单引号就成了名字的一部分
    //（例：WinNT://<机器名>/'<账户名>' 这么拼出来找不到对象 —— 这里刻意不写真机名真账号）
    return QStringLiteral(
               "$ErrorActionPreference='Stop'\n"
               "try {\n"
               "  $u = [adsi](\"WinNT://\" + %1 + '/' + %2 + ',user')\n"
               "  $u.FullName = %3\n"
               "  $u.SetInfo()\n"
               "  exit 0\n"
               "} catch {\n"
               "  exit 1\n"
               "}\n")
        .arg(psQuote(machine), psQuote(name), psQuote(newFull.trimmed()));
}

QString renameScript(const QString& machine, const QString& name, const QString& newName)
{
    Q_UNUSED(machine);
    // 改的是账户名本身。Rename-LocalUser 不会去动 C:\\Users\\<旧名>，界面上也照样显示旧目录 ——
    // 这条必须在按钮前头跟人说清楚，不能等他自己发现。
    return QStringLiteral(
               "$ErrorActionPreference='Stop'\n"
               "try {\n"
               "  Rename-LocalUser -Name %1 -NewName %2 -ErrorAction Stop\n"
               "  exit 0\n"
               "} catch {\n"
               "  exit 1\n"
               "}\n")
        .arg(psQuote(name), psQuote(newName.trimmed()));
}

QString newUserScript(const QString& name, const QString& fullName, const QString& password,
                      bool asAdmin)
{
    // 管理员组按 SID 找（S-1-5-32-544），不写死组名：中文系统上那个组的本地化名不一定
    // 长得和英文一致，Translate 一次拿到的才是这台机器上的真名。
    // 不勾管理员就整段不加（不是留一个 if ($false)）—— 脚本里少一行就少一种可能。
    const QString addAdmin = asAdmin
        ? QStringLiteral(
              "  $adm = (New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544'))"
              ".Translate([Security.Principal.NTAccount]).Value.Split('\')[-1]\n"
              "  Add-LocalGroupMember -Group $adm -Member %1 -ErrorAction Stop\n")
              .arg(psQuote(name.trimmed()))
        : QString();
    return QStringLiteral(
               "$ErrorActionPreference='Stop'\n"
               "try {\n"
               "  $sp = ConvertTo-SecureString %1 -AsPlainText -Force\n"
               "  New-LocalUser -Name %2 -Password $sp -FullName %3 "
               "-Description 'created by ltool' -PasswordNeverExpires -AccountNeverExpires -ErrorAction Stop\n"
               "%4"
               "  exit 0\n"
               "} catch {\n"
               "  exit 1\n"
               "}\n")
        .arg(psQuote(password), psQuote(name.trimmed()),
             psQuote(fullName.trimmed().isEmpty() ? name.trimmed() : fullName.trimmed()),
             addAdmin);
}

Probe probe(const QString& name)
{
    Probe out;
    QProcess p;
    const QString script = QStringLiteral(
                               "[Console]::OutputEncoding=[Text.Encoding]::UTF8\n"
                               "$n=%1\n"
                               "$u=$null; try{$u=Get-LocalUser -Name $n -ErrorAction Stop}catch{}\n"
                               "if($u){\"EXISTS=1\"; \"FULL=\"+$u.FullName}else{\"EXISTS=0\"}\n"
                               "$adm='0'\n"
                               "if($u){try{$g=(New-Object Security.Principal.SecurityIdentifier("
                               "'S-1-5-32-544')).Translate([Security.Principal.NTAccount]).Value"
                               ".Split('\\')[-1]; if(Get-LocalGroupMember -Group $g -Name $n -ErrorAction "
                               "SilentlyContinue){$adm='1'}}catch{}\n"
                               "}\n"
                               "\"ADMIN=$adm\"\n")
                               .arg(psQuote(name));
    p.start(QStringLiteral("powershell"),
            { QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
              QStringLiteral("-Command"), script });
    if (!p.waitForFinished(15000)) {
        out.error = QStringLiteral("powershell 没回话");
        return out;
    }
    for (const QString& line : QString::fromUtf8(p.readAllStandardOutput()).split(QLatin1Char('\n'))) {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString k = line.left(eq).trimmed();
        const QString v = trimmed(line.mid(eq + 1));
        if (k == QLatin1String("EXISTS")) out.exists = (v == QLatin1String("1"));
        else if (k == QLatin1String("ADMIN")) out.admin = (v == QLatin1String("1"));
        else if (k == QLatin1String("FULL")) out.fullName = v;
    }
    out.readable = true;
    return out;
}

bool runElevatedScript(const QString& script, QString* err)
{
    // 落成临时 .ps1 再执行，不走 cmd.exe 拼串：中文和引号在 cmd 那一层会被剥/转义出各种花样，
    // 而这段是要提权跑的，宁可多写一个文件也不要在引号上赌。
    QTemporaryFile tf(QDir::tempPath() + "/ltool-user-XXXXXX.ps1");
    tf.setAutoRemove(false);
    if (!tf.open()) {
        if (err)
            *err = QStringLiteral("临时脚本写不出来");
        return false;
    }
    const QString path = tf.fileName();
    tf.close();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err)
            *err = QStringLiteral("临时脚本打不开写：%1").arg(path);
        QFile::remove(path);
        return false;
    }
    f.write("\xEF\xBB\xBF");            // PS5.1 认 BOM，没它整段中文会变乱码（实测踩过）
    f.write(script.toUtf8());
    f.close();

    const QString params = QStringLiteral("-NoProfile -ExecutionPolicy Bypass -File \"%1\"").arg(path);
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = reinterpret_cast<LPCWSTR>(params.utf16());
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (!ShellExecuteExW(&sei)) {
        const DWORD e = GetLastError();
        if (err)
            *err = e == ERROR_CANCELLED
                ? QStringLiteral("没给管理员授权，什么都没改")
                : QStringLiteral("提权没起来（错误码 %1）").arg(e);
        QFile::remove(path);
        return false;
    }
    bool ok = false;
    DWORD code = 1;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 30000);
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        ok = (code == 0);
    }
    QFile::remove(path);
    if (!ok && err)
        *err = QStringLiteral("脚本跑完但退出码不是 0（%1）—— 多半是没权限或名字被系统拒了").arg(code);
    return ok;
}

} // namespace UserAccount

namespace {

QString oneLiner(const UserAccount::Info& i)
{
    return QStringLiteral(
               "账户名：%1\n机器名：%2\n显示名：%3\n类型：%4\n用户目录：%5\nSID：%6")
        .arg(i.name, i.machine,
             i.fullName.isEmpty() ? QStringLiteral("（没设，界面显示的就是账户名）") : i.fullName,
             i.local ? (i.msaHint ? QStringLiteral("本地账户（另外还读到微软账户的登录痕迹）")
                                   : QStringLiteral("本地账户"))
                     : QStringLiteral("SAM 里查不到这个名（多半是微软账户）"),
             i.profileDir, i.sid);
}

} // namespace

UserAccountPage::UserAccountPage(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(8);

    m_state = new QLabel(this);
    m_state->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(m_state);

    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("muted"));
    m_hint->setWordWrap(true);
    lay->addWidget(m_hint);

    auto* box = new QFormLayout;
    box->setSpacing(6);
    m_full = new QLineEdit(this);
    m_full->setPlaceholderText(QStringLiteral("显示名：登录界面和开始菜单里那个名字"));
    m_applyFull = new QPushButton(QStringLiteral("改显示名"), this);
    m_applyFull->setObjectName(QStringLiteral("applyFull"));
    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(QStringLiteral("账户名：最长 20 个字符，改了不会动 C:\\Users 目录"));
    m_applyName = new QPushButton(QStringLiteral("改账户名"), this);
    m_applyName->setObjectName(QStringLiteral("applyName"));
    box->addRow(QStringLiteral("显示名"), m_full);
    box->addRow(QString(), m_applyFull);
    box->addRow(QStringLiteral("账户名"), m_name);
    box->addRow(QString(), m_applyName);
    lay->addLayout(box);

    // ---- 新建用户这一段：和上面两个是不同性质的事（那是改自己，这是造一个新账户），
    //      所以单独一组、单独一颗按钮，别混在同一排里 ----
    auto* createBox = new QFormLayout;
    createBox->setSpacing(6);
    m_newName = new QLineEdit(this);
    m_newName->setObjectName(QStringLiteral("newName"));
    m_newName->setPlaceholderText(QStringLiteral("新账户的登录名（最长 20 个字符）"));
    m_newFull = new QLineEdit(this);
    m_newFull->setObjectName(QStringLiteral("newFull"));
    m_newFull->setPlaceholderText(QStringLiteral("显示名（可留空，留空就用登录名）"));
    m_newPass = new QLineEdit(this);
    m_newPass->setObjectName(QStringLiteral("newPass"));
    m_newPass->setEchoMode(QLineEdit::Password);
    m_newPass->setPlaceholderText(QStringLiteral("密码（必填）"));
    m_newAdmin = new QCheckBox(QStringLiteral("赋予管理员权限（加入 Administrators 组）"), this);
    m_newAdmin->setObjectName(QStringLiteral("newAdmin"));
    m_newAdmin->setChecked(true);
    m_create = new QPushButton(QStringLiteral("新建用户"), this);
    m_create->setObjectName(QStringLiteral("applyCreate"));
    createBox->addRow(QStringLiteral("新账户名"), m_newName);
    createBox->addRow(QStringLiteral("新显示名"), m_newFull);
    createBox->addRow(QStringLiteral("密码"), m_newPass);
    createBox->addRow(QString(), m_newAdmin);
    createBox->addRow(QString(), m_create);
    lay->addLayout(createBox);

    auto* reload = new QPushButton(QStringLiteral("重新读一遍"), this);
    lay->addWidget(reload);
    lay->addStretch(1);

    connect(m_applyFull, &QPushButton::clicked, this, &UserAccountPage::applyFullName);
    connect(m_applyName, &QPushButton::clicked, this, &UserAccountPage::applyAccountName);
    connect(m_create, &QPushButton::clicked, this, &UserAccountPage::createNewUser);
    connect(reload, &QPushButton::clicked, this, &UserAccountPage::refresh);
    refresh();
}

void UserAccountPage::refresh()
{
    // 读一次要走 powershell（半秒到一秒），所以放在后台线程；页面先给一句"正在读"，
    // 不能让人对着一个空白页猜它在不在干活
    m_state->setText(QStringLiteral("正在读账户信息…"));
    m_applyFull->setEnabled(false);
    m_applyName->setEnabled(false);
    if (!m_watcher) {
        m_watcher = new QFutureWatcher<UserAccount::Info>(this);
        connect(m_watcher, &QFutureWatcherBase::finished, this, [this] {
            applyInfo(static_cast<QFutureWatcher<UserAccount::Info>*>(m_watcher)->result());
        });
    }
    static_cast<QFutureWatcher<UserAccount::Info>*>(m_watcher)
        ->setFuture(QtConcurrent::run(UserAccount::read));
}

void UserAccountPage::applyInfo(const UserAccount::Info& got)
{
    m_info = got;
    if (!m_info.readable) {
        m_state->setText(QStringLiteral("读不到账户信息：%1").arg(m_info.error));
        m_applyFull->setEnabled(false);
        m_applyName->setEnabled(false);
        return;
    }
    m_state->setText(oneLiner(m_info));
    m_full->setText(m_info.fullName);
    m_name->setText(m_info.name);
    m_applyFull->setEnabled(true);
    m_applyName->setEnabled(m_info.local);
    m_hint->setText(QStringLiteral(
        "改显示名只动登录界面/开始菜单显示的那个名字，最安全。"
        "改账户名影响的是登录用的名字本身：C:\Users 下面那个目录不会跟着改名，"
        "已经写死了旧路径的程序不会自动跟上，改完要注销再登才全生效。"
        "如果这是微软账户，显示名是服务器同步下来的，本地改了也会被覆盖，"
        "得去 account.microsoft.com 改。三个动作都要管理员授权（会弹 UAC）。"));
}

void UserAccountPage::applyFullName()
{
    const QString want = m_full->text();
    if (want.trimmed() == m_info.fullName) {
        m_hint->setText(QStringLiteral("显示名没变，什么都没做。"));
        return;
    }
    const QStringList why = UserAccount::reject(want, false);
    if (!why.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("改显示名"), why.join(QLatin1Char('\n')));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("改显示名"),
                              QStringLiteral("把登录界面显示的名字从「%1」改成「%2」？\n"
                                             "不动账户名，也不动 C:\\Users 目录。")
                                  .arg(m_info.fullName.isEmpty() ? m_info.name : m_info.fullName,
                                       want.trimmed()))
        != QMessageBox::Yes)
        return;
    QString err;
    const bool ran = UserAccount::runElevatedScript(
        UserAccount::fullNameScript(m_info.machine, m_info.name, want), &err);
    refresh();                                    // 回读：读到什么就说什么，不拿"跑了"当"改了"
    const bool now = (m_info.fullName == want.trimmed());
    m_hint->setText(now ? QStringLiteral("显示名已改成「%1」（回读确认）").arg(m_info.fullName)
                        : QStringLiteral("没改成：回读到的还是「%1」。%2")
                              .arg(m_info.fullName.isEmpty() ? m_info.name : m_info.fullName,
                                   ran ? QStringLiteral("脚本本身跑成功了，但值没变 —— "
                                                        "如果这是微软账户，请去 account.microsoft.com 改")
                                       : err));
    Utils::log(QStringLiteral("username: 改显示名 ok=%1 now=%2 err=%3")
                   .arg(now ? 1 : 0).arg(m_info.fullName, ran ? QStringLiteral("ok") : err));
}

void UserAccountPage::applyAccountName()
{
    const QString want = m_name->text();
    if (want.trimmed() == m_info.name) {
        m_hint->setText(QStringLiteral("账户名没变，什么都没做。"));
        return;
    }
    const QStringList why = UserAccount::reject(want, true);
    if (!why.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("改账户名"), why.join(QLatin1Char('\n')));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("改账户名"),
                              QStringLiteral("要把登录用的账户名从「%1」改成「%2」。\n\n"
                                             "· C:\\Users\\%1 这个目录**不会**跟着改名\n"
                                             "· 写死了旧路径的程序/脚本不会自动跟上\n"
                                             "· 改完要注销重新登录\n\n确认继续吗？")
                                  .arg(m_info.name, want.trimmed()))
        != QMessageBox::Yes)
        return;
    QString err;
    const bool ran = UserAccount::runElevatedScript(
        UserAccount::renameScript(m_info.machine, m_info.name, want), &err);
    const QString before = m_info.name;
    refresh();
    const bool now = (m_info.name == want.trimmed());
    m_hint->setText(now ? QStringLiteral("账户名已改成「%1」（回读确认）。注销再登录后全面生效；"
                                          "用户目录还是 %2。")
                              .arg(m_info.name, before)
                        : QStringLiteral("没改成：回读到的还是「%1」。%2")
                              .arg(m_info.name, ran ? QStringLiteral("脚本跑成功了但名字没变") : err));
    Utils::log(QStringLiteral("username: 改账户名 %1→%2 ok=%3 err=%4")
                   .arg(before, m_info.name).arg(now ? 1 : 0).arg(ran ? QStringLiteral("ok") : err));
}


void UserAccountPage::createNewUser()
{
    const QString name = m_newName->text().trimmed();
    const QString full = m_newFull->text().trimmed();
    const QString pass = m_newPass->text();
    QStringList why = UserAccount::reject(name, true);
    if (pass.isEmpty())
        why << QStringLiteral("密码不能空着 —— 空密码的本地账户根本登不进去，还不如不建");
    if (why.isEmpty() && UserAccount::probe(name).exists)
        why << QStringLiteral("账户 %1 已经存在了").arg(name);
    if (!why.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("新建用户"), why.join(QStringLiteral("\n")));
        return;
    }
    const bool admin = m_newAdmin->isChecked();
    const QString ask = QStringLiteral("在本机（%1）上创建本地账户「%2」%3。")
        .arg(m_info.machine, name,
             admin ? QStringLiteral("，并加入 Administrators 组（那是完整的管理员权限）")
                   : QStringLiteral("（普通用户，不给管理员）"));
    const QString lines = QStringLiteral(
        "新用户第一次登录时 Windows 会给它建一个全新的 C:\\Users 目录。"
        "密码设为永久有效，不会被到期策略拦住。"
        "密码会临时写进一个脚本文件、跑完立刻删 —— 但那一瞬间它是在盘上的。"
        "需要管理员授权（会弹 UAC）。确认创建吗？");
    if (QMessageBox::question(this, QStringLiteral("新建用户"), ask + QStringLiteral("\n") + lines) != QMessageBox::Yes)
        return;
    QString err;
    const bool ran = UserAccount::runElevatedScript(
        UserAccount::newUserScript(name, full, pass, admin), &err);
    const UserAccount::Probe after = UserAccount::probe(name);   // 回读，不拿"跑了"当"建了"
    m_hint->setText(after.exists
        ? QStringLiteral("已建好「%1」：显示名=%2，管理员=%3。注销一下就能在登录列表里看到它。")
              .arg(name, after.fullName.isEmpty() ? name : after.fullName,
                   after.admin ? QStringLiteral("是") : QStringLiteral("否"))
        : QStringLiteral("没建成：%1").arg(ran ? QStringLiteral("脚本说成功了，但回读不到这个账户") : err));
    m_newPass->clear();
    Utils::log(QStringLiteral("username: 新建用户 %1 admin=%2 ok=%3 err=%4")
                   .arg(name).arg(admin ? 1 : 0).arg(after.exists ? 1 : 0)
                   .arg(ran ? QStringLiteral("ok") : err));
}
