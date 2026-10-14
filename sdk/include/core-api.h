#pragma once

#include <QtGlobal>

// ltoolcore.dll 的边界标记。
//
// CMake 的 WINDOWS_EXPORT_ALL_SYMBOLS 能自动导出**函数**，导不出**数据** ——
// QObject 子类的 staticMetaObject 和 vtable 都是数据，跨模块 qobject_cast、
// 按成员函数指针 connect、new 一个 core 里的控件，引用的就是它们
// （第一次拆 core 时四个 LNK2019 全是 staticMetaObject，函数一个没缺）。
//
// 所以只给"会被 core 之外碰到"的 Qt 类贴这个宏，其余靠自动导出。
// 新加类时怎么判断要不要贴：链接器报 LNK2019 且符号名里带 staticMetaObject /
// vftable / typeinfo 才贴，别预防性地把所有头文件刷一遍。
#if defined(LTOOL_CORE_BUILD)
#  define LTOOL_CORE_API Q_DECL_EXPORT
#else
#  define LTOOL_CORE_API Q_DECL_IMPORT
#endif
