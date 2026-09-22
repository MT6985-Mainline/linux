// SPDX-License-Identifier: GPL-2.0
/*
 * corot fbscan: locate the live LK scanout inside the reserved framebuffer.
 *
 * The videolfb tag base (0xfd91f000) is the reserved-region start, not
 * necessarily the address LK is still scanning out (same trap as xaga where
 * the live layer sat at 0xff5c3000 inside 0xfe14f000+0x1eb0000). Scan the
 * whole reserved range once at late_initcall and print a per-MB nonzero
 * density map plus the first words; the MB that holds the LK logo is the
 * real scanout. Runs after the consoles are up, so the report lands in
 * kmsg and is mirrored into the cust boot-log by the initramfs.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>

#define FB_BASE	0xfd91f000UL
#define FB_SIZE	0x026e0000UL

static int __init corot_fb_scan_init(void)
{
	void __iomem *base;
	unsigned long off;

	pr_info("fbscan: scanning %08lx+%08lx\n", FB_BASE, FB_SIZE);
	base = ioremap(FB_BASE, FB_SIZE);
	if (!base) {
		pr_info("fbscan: ioremap failed\n");
		return 0;
	}
	for (off = 0; off < FB_SIZE; off += SZ_1M) {
		u32 __iomem *p = (u32 __iomem *)(base + off);
		unsigned long nz = 0, i;

		for (i = 0; i < SZ_1M / 4; i++)
			if (readl(&p[i]))
				nz++;
		pr_info("fbscan 0x%08lx nz=%lu d0=%08x d1=%08x\n",
			FB_BASE + off, nz, readl(base + off),
			readl(base + off + 4));
	}
	iounmap(base);
	pr_info("fbscan: done\n");
	return 0;
}
late_initcall(corot_fb_scan_init);
