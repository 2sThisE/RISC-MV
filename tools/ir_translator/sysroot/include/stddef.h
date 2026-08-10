#ifndef _CVM_STDDEF_H
#define _CVM_STDDEF_H

typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef int wchar_t;

#define NULL ((void *)0)
#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
