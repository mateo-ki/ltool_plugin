#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <QWidget>

class QCheckBox;
class QEvent;
class QLineEdit;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QSpinBox;

// 「随机数生成」这一页的**规格与生成**这一层是纯函数，页面和判据读同一份实现。
//
// 为什么要单独拿出来：区间够不够、字符池多大、不重复取不取得出来 —— 这些判断既要在界面上
// 说成人话，又要在 `--randtest` 里被逐条钉住。写在成员函数里的话，判据要么复制一遍逻辑
// （两份一漂就各自绿），要么就只能"脑内减掉"（那是假绿）。
namespace Rand {

enum class Kind { Int, Decimal, String, Uuid };

struct Spec {
    Kind kind = Kind::Int;
    qint64 lo = 1;              // 整数 / 小数的下界
    qint64 hi = 100;            // 上界
    int count = 5;              // 要几个
    int digits = 2;             // 小数保留几位
    bool unique = false;        // 整数：取出的数不重复；字符串：字符不重复
    int length = 16;            // 字符串长度
    QString sets = QStringLiteral("dlu");   // 勾选的字符集代号，见 charSets()
    bool dropLookalike = false; // 去掉 0O1lI| 这类一眼分不清的字符
};

struct CharSet {
    QString key;                // 进 Spec::sets 的那个字母
    QString label;              // 界面上那句话
    QString chars;              // 池子
    QString lookalike;          // 这一档里"长得像"的那些
};

// 字符集只在这里定义一份：界面拿它画勾选框，判据拿它验"结果里不许出现没勾的字符"
QVector<CharSet> charSets();
QString poolOf(const Spec& s);          // 勾选项并起来（去掉易混之后的最终池子）
QString kindName(Kind k);               // "整数" / "小数" / "字符串" / "UUID"

// 生成 count 行结果。返回 false 时 *err 是一句能直接摆到界面上的话（不返回半成品）。
bool generate(const Spec& s, QStringList* out, QString* err);

} // namespace Rand

// 随机数生成工具：区间取数 / 不重复抽取 / 随机字符串 / UUID，结果可直接复制。
class RandomTool : public QWidget {
    Q_OBJECT
public:
    explicit RandomTool(QWidget* parent = nullptr);

    // 把一段文本接进来（搜索框"随几个数"那种直达）：这里能用的只有数量，
    // 所以取串里第一个整数当"要几个"，接不住就说明白（返回 false，不假装收下了）
    bool feedText(const QString& text);

    QString resultText() const;         // 结果区里现在是什么（判据读这一颗，不读界面像素）
    QString statusText() const;         // 常驻那一行说了什么
    QString statusStyle() const;        // 那一行现在挂的样式表（判据问"切完模式颜色换没换"）
    Rand::Spec currentSpec() const;     // 界面现在这个状态对应的规格

protected:
    // 夜间/日间切换的传导：这一页的状态行颜色是每次现挂的，ThemeChange 来了要自己重挂一遍
    void changeEvent(QEvent* e) override;

private:
    void applyKindUi();                 // 只同步哪些参数行可见/按钮高亮，不生成
    void regenerate();
    void copyResult();
    void setStatus(const QString& text, bool ok);

    QPushButton* m_kindBtn[4] = {};     // 按 Rand::Kind 的下标摆
    QLineEdit* m_lo = nullptr;
    QLineEdit* m_hi = nullptr;
    QSpinBox* m_count = nullptr;
    QSpinBox* m_digits = nullptr;
    QSpinBox* m_length = nullptr;
    QCheckBox* m_unique = nullptr;
    QCheckBox* m_lookalike = nullptr;
    QVector<QCheckBox*> m_sets;
    QLabel* m_loLabel = nullptr;
    QLabel* m_hiLabel = nullptr;
    QLabel* m_uniqueLabel = nullptr;
    QWidget* m_rangeRow = nullptr;
    QWidget* m_digitsRow = nullptr;
    QWidget* m_stringRow = nullptr;
    QWidget* m_charsetRow = nullptr;
    QPlainTextEdit* m_out = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_copyBtn = nullptr;
    QString m_statusText;               // 状态行最后一句原文（切模式时要按它重挂一次颜色）
    bool m_statusOk = true;             // 那一句是"成了"还是"没成"
    Rand::Kind m_kind = Rand::Kind::Int;
};
