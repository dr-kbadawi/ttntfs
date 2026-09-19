/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
#ifndef _LINUX_MODULE_H
#define _LINUX_MODULE_H
#include <linux/types.h>
#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_VERSION(x)
#define MODULE_ALIAS_FS(x)
#define MODULE_ALIAS(x)
#define MODULE_PARM_DESC(a, b)
#define module_param(a, b, c)
/* The kernel entry points are static functions in super.c. The macros emit
 * exported wrappers so the port can call them without editing the source:
 * ntfsport_module_init() creates the caches/workqueue, ntfsport_module_exit()
 * tears them down. */
int ntfsport_module_init(void);
void ntfsport_module_exit(void);
#define module_init(x) int ntfsport_module_init(void) { return x(); }
#define module_exit(x) void ntfsport_module_exit(void) { x(); }
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define THIS_MODULE ((void *)0)
#endif
