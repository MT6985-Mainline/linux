/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_COROT_MARKER_H
#define _LINUX_COROT_MARKER_H

#include <linux/stdarg.h>
#include <linux/types.h>

#ifdef CONFIG_COROT_MARKER_WRITER
void corot_marker_early_init(void);
void corot_marker_stage(u32 stage);
void corot_marker_early_printk(const char *fmt, va_list args);
void corot_marker_put(const char *fmt, ...) __printf(1, 2);
#else
static inline void corot_marker_early_init(void) { }
static inline void corot_marker_stage(u32 stage) { }
static inline void corot_marker_early_printk(const char *fmt, va_list args) { }
static inline void corot_marker_put(const char *fmt, ...) { }
#endif

#endif /* _LINUX_COROT_MARKER_H */
