/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
#ifndef _LINUX_SMP_H
#define _LINUX_SMP_H
#include <linux/types.h>
#define smp_processor_id() 0
#define num_online_cpus() 1
#endif
