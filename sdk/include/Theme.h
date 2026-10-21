#pragma once

#include <QColor>
#include <QString>

class QApplication;
class QWidget;

// 一套颜色、两个模式（夜间 / 日间）。
//
// 为什么要有这一层：整个程序原来把颜色写死在 28 个文件、271 处字面量里（`#1b1c20`、
// `#e8ebf2`、`rgba(255,255,255,0.08)` 那一族；2026-10-20 这一轮之前 git grep 量的），
// 其中 src/ 占 135 处 / 9 个文件。2026-10-20 他要"配置页能切夜间/日间"，
// 一处一处改是改不完的 —— 所以颜色从这儿出，别处只写**名字**。
// 剩下那 136 处在 plugins/ 里，这一轮没动（改它们=重发全部产物，见 docs/notes.md 末尾）。
//
// 两套值都在 Theme.cpp 那张表里，是"同一套角色、两种取值"，不是两套设计：
// 角色名（window/text/muted/field/border/accent…）说的是它在界面上干什么，
// 日间那一份按 WCAG 对过正文对比度（见 .cpp 里那行注释）。
//
// 样式表怎么用它：页面里那份 QSS 写成带 `@token` 的形式，交给 Theme::expand() 换值。
// 这样 QSS 还是能整段读，又不用把二十个 `%1..%20` 按位置数（数错一位就全花）。
namespace Theme {

enum class Mode { Dark, Light };

// 读 `ui/theme`（没配过 = 夜间，和这一版之前的行为一模一样）
Mode current();
// 落盘 + 当场生效：重刷 qApp 调色板、qApp 那两份弹层样式表，并给所有顶层窗口发一遍
// QEvent::ThemeChange（各页在那儿把自己那份样式表重算一遍 —— 样式表是挂在窗口上的，
// 改 qApp 的不会自动传导下去）
void setMode(Mode m);
// 只读配置、不动界面（启动那一下用）
void applyToApp(QApplication& app);

// 把 QSS 里的 `@role` 换成当前模式的值后挂到 w 上，并接住之后每次 setMode() 发出的
// ThemeChange 自动重挂。为什么要这一层：样式表挂在各个控件自己身上，改 qApp 传导不下去；
// 每个页各写一份 event() 分支又会漏（漏的那半在切完模式后还是老颜色，不说真话）。
void bind(QWidget* w, const char* qssWithTokens);
// 菜单专用：Qt6 的 windows11/windowsvista 风格画弹层用的是系统浅底、不读调色板，
// 所以菜单的样式表必须挂在菜单自己身上；挂在启动时一次、活一整天的菜单（托盘那个）
// 光靠这一句拿不到新底色，得走 bind 才能跟着模式重挂。
void bindMenu(QWidget* menu);

const char* name(Mode m);           // "夜间" / "日间"
QString token(const char* role);    // 一个角色的颜色字面量，给拼 QSS 用
QColor color(const char* role);     // 同一个值的 QColor 形式
// 自绘代码用这一颗拿半透明：`hairline` 那类"跟着模式换底色的描边/填充"在两个模式下
// 需要换的是**色相**（夜间压白、日间压黑），alpha 反而一样，所以 alpha 留在调用点。
// 只对 `#rrggbb` 形式的角色有效（rgba() 那几颗 QColor 解析不了）。
QColor color(const char* role, int alpha);

// 把 QSS 里的 `@role` 换成当前模式的值（不认识的名字原样留着，好排查）
QString expand(const char* qssWithTokens);
QString expand(const QString& qssWithTokens);

// WCAG 2.1 那套算法，写在这里只有一份：判据要拿它问"这两个角色放在一起看不看得见"，
// 而各个页自己抄一份系数就会各自算出不同数（`--cfgcheck` 原来那份局部 lambda 已搬过来）。
// 半透明的颜色不许直接比 —— 先 `over()` 合成到它真正压着的那层底上（卡片底压在列表底上）。
double relativeLuminance(QRgb c);
double contrast(QRgb a, QRgb b);
QColor over(const QColor& top, const QColor& bottom);   // 按 top 自己的 alpha 合成，结果不带 alpha

// 弹层那两份（菜单 / 悬停说明）。Qt6 的 windows11/windowsvista 风格画弹层用的是系统的
// 浅色底，调色板它不读 —— 只能靠样式表，所以这两份必须跟着模式走。
QString menuStyle();
QString tipStyle();

} // namespace Theme
