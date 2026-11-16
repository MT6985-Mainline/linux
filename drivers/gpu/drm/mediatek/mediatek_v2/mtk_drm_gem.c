// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/vmalloc.h>
#include <drm/drm_gem.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <drm/mediatek_drm.h>
#include <linux/iommu.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_prime.h>
#include <linux/kmemleak.h>
#include <linux/dma-heap.h>
#include <uapi/linux/dma-heap.h>
#include <linux/dma-direction.h>
#include <linux/scatterlist.h>

#include "mtk_drm_drv.h"
#include "mtk_drm_gem.h"
#include "mtk_fence.h"
#include "mtk_drm_session.h"
#include "mtk_drm_mmp.h"
#include <soc/mediatek/smi.h>
#ifdef IF_ZERO
#include "mt_iommu.h"
#include "mtk_iommu_ext.h"
#endif

#include "../mml/mtk-mml.h"
#include "../mml/mtk-mml-drm-adaptor.h"
#include "../mml/mtk-mml-driver.h"
#include <linux/of_platform.h>

static int mtk_drm_gem_object_mmap(struct drm_gem_object *obj,
                                   struct vm_area_struct *vma);

static int mtk_drm_gem_object_vmap(struct drm_gem_object *obj, struct iosys_map *map)
{
    struct mtk_drm_gem_obj *gem = to_mtk_gem_obj(obj);

    if (!gem->kvaddr)
        return -ENXIO;
    iosys_map_set_vaddr(map, gem->kvaddr);
    return 0;
}

static void mtk_drm_gem_object_vunmap(struct drm_gem_object *obj, struct iosys_map *map)
{
    iosys_map_clear(map);
}

static const struct drm_gem_object_funcs mtk_drm_gem_object_funcs = {
	.free = mtk_drm_gem_free_object,
	.mmap = mtk_drm_gem_object_mmap,
	.vmap = mtk_drm_gem_object_vmap,
	.vunmap = mtk_drm_gem_object_vunmap,
	.get_sg_table = mtk_gem_prime_get_sg_table,
	.vm_ops = &drm_gem_dma_vm_ops,
};

static struct mtk_drm_gem_obj *mtk_drm_gem_init(struct drm_device *dev,
						unsigned long size)
{
	struct mtk_drm_gem_obj *mtk_gem_obj;
	int ret;

	size = round_up(size, PAGE_SIZE);

	mtk_gem_obj = kzalloc(sizeof(*mtk_gem_obj), GFP_KERNEL);
	if (!mtk_gem_obj)
		return ERR_PTR(-ENOMEM);

	mtk_gem_obj->base.funcs = &mtk_drm_gem_object_funcs;

	ret = drm_gem_object_init(dev, &mtk_gem_obj->base, size);
	if (ret != 0) {
		DDPPR_ERR("failed to initialize gem object\n");
		kfree(mtk_gem_obj);
		return ERR_PTR(ret);
	}

	return mtk_gem_obj;
}

/* Boot framebuffer RAM is supplied by the bootloader, not dma_alloc_attrs(). */
static struct sg_table *mtk_gem_vmap_pa(struct mtk_drm_gem_obj *gem,
				      phys_addr_t pa, struct device *dma_dev,
				      dma_addr_t *dma_addr)
{
	struct page **pages;
	struct sg_table *sgt;
	phys_addr_t end;
	size_t size = gem->base.size;
	unsigned int npages, i;
	void *vaddr;
	int ret;

	if (!size || !IS_ALIGNED(pa, PAGE_SIZE) ||
	    check_add_overflow(pa, (phys_addr_t)size, &end))
		return ERR_PTR(-EINVAL);
	npages = size >> PAGE_SHIFT;
	if (!npages || npages != size >> PAGE_SHIFT)
		return ERR_PTR(-E2BIG);
	pages = kmalloc_array(npages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return ERR_PTR(-ENOMEM);
	for (i = 0; i < npages; i++) {
		unsigned long pfn = PHYS_PFN(pa) + i;

		if (!pfn_valid(pfn)) {
			ret = -EINVAL;
			goto free_pages;
		}
		pages[i] = pfn_to_page(pfn);
	}
	vaddr = vmap(pages, npages, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	if (!vaddr) {
		ret = -ENOMEM;
		goto free_pages;
	}
	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto unmap_vaddr;
	}
	ret = sg_alloc_table_from_pages(sgt, pages, npages, 0, size, GFP_KERNEL);
	if (ret)
		goto free_sgt;
	ret = dma_map_sgtable(dma_dev, sgt, DMA_BIDIRECTIONAL, DMA_ATTR_SKIP_CPU_SYNC);
	if (ret)
		goto free_table;
	if (drm_prime_get_contiguous_size(sgt) < size) {
		ret = -EINVAL;
		goto unmap_dma;
	}
	*dma_addr = sg_dma_address(sgt->sgl);
	gem->cookie = vaddr;
	gem->kvaddr = vaddr;
	kfree(pages);
	return sgt;

unmap_dma:
	dma_unmap_sgtable(dma_dev, sgt, DMA_BIDIRECTIONAL, DMA_ATTR_SKIP_CPU_SYNC);
free_table:
	sg_free_table(sgt);
free_sgt:
	kfree(sgt);
unmap_vaddr:
	vunmap(vaddr);
free_pages:
	kfree(pages);
	return ERR_PTR(ret);
}

int mtk_drm_boot_fb_free_pages(phys_addr_t base, size_t size)
{
	phys_addr_t end, pa;

	if (!size || !IS_ALIGNED(base, PAGE_SIZE) || !IS_ALIGNED(size, PAGE_SIZE) ||
	    check_add_overflow(base, (phys_addr_t)size, &end))
		return -EINVAL;
	/* Validate the complete region before releasing any reserved page. */
	for (pa = base; pa < end; pa += PAGE_SIZE)
		if (!pfn_valid(PHYS_PFN(pa)) || !PageReserved(pfn_to_page(PHYS_PFN(pa))))
			return -EINVAL;
	for (pa = base; pa < end; pa += PAGE_SIZE)
		free_reserved_page(pfn_to_page(PHYS_PFN(pa)));
	return 0;
}

static inline void *mtk_gem_dma_alloc(struct device *dev, size_t size,
				       dma_addr_t *dma_handle, gfp_t flag,
				       unsigned long attrs, const char *name,
				       int line)
{
	void *va;

	DDPINFO("D_ALLOC:%s[%d], dma:0x%p, size:%ld\n", name, line,
			dma_handle, size);
	DRM_MMP_EVENT_START(dma_alloc, line, 0);
	va = dma_alloc_attrs(dev, size, dma_handle,
					flag, attrs);

	DRM_MMP_EVENT_END(dma_alloc, (unsigned long)dma_handle,
			(unsigned long)size);
	return va;
}

static inline void mtk_gem_dma_free(struct device *dev, size_t size,
				     void *cpu_addr, dma_addr_t dma_handle,
				     unsigned long attrs, const char *name,
				     int line)
{
	DDPINFO("D_FREE:%s[%d], dma:0x%llx, size:%ld\n", name, line,
			dma_handle, size);
	DRM_MMP_EVENT_START(dma_free, (unsigned long)dma_handle,
			(unsigned long)size);
	dma_free_attrs(dev, size, cpu_addr, dma_handle,
					attrs);

	DRM_MMP_EVENT_END(dma_free, line, 0);
}

struct mtk_drm_gem_obj *mtk_drm_fb_gem_insert(struct drm_device *dev,
					      size_t size, phys_addr_t fb_base,
					      unsigned int vramsize)
{
	struct mtk_drm_private *private = dev->dev_private;
	struct mtk_drm_gem_obj *gem;
	struct device *dma_dev;
	struct sg_table *sgt;
	dma_addr_t fb_dma;
	size_t bytes = vramsize ? vramsize : size;
	int ret;

	if (!bytes || !fb_base || (vramsize && size > vramsize))
		return ERR_PTR(-EINVAL);
	if (!private->dma_dev)
		return ERR_PTR(-EPROBE_DEFER);
	dma_dev = get_device(private->dma_dev);
	gem = mtk_drm_gem_init(dev, bytes);
	if (IS_ERR(gem)) {
		put_device(dma_dev);
		return gem;
	}
	gem->size = gem->base.size;
	sgt = mtk_gem_vmap_pa(gem, fb_base, dma_dev, &fb_dma);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto release_gem;
	}
	gem->boot_fb = true;
	gem->boot_dma_dev = dma_dev;
	gem->boot_fb_base = fb_base;
	gem->sg = sgt;
	gem->dma_attrs = DMA_ATTR_WRITE_COMBINE;
	gem->dma_addr = mtk_drm_helper_get_opt(private->helper_opt, MTK_DRM_OPT_USE_M4U)
			? fb_dma : fb_base;
	DDPINFO("%s: pa=%pa dma=%pad size=%zu\n", __func__, &fb_base,
		&gem->dma_addr, gem->size);
	return gem;

release_gem:
	drm_gem_object_release(&gem->base);
	kfree(gem);
	put_device(dma_dev);
	return ERR_PTR(ret);
}

void mtk_drm_fb_gem_release(struct drm_device *dev)
{
	struct mtk_drm_private *priv = dev->dev_private;
	struct mtk_drm_gem_obj *gem = priv->fb_info.fb_gem;

	priv->fb_info.fb_gem = NULL;
	if (!IS_ERR_OR_NULL(gem))
		drm_gem_object_put(&gem->base);
}

struct mtk_drm_gem_obj *mtk_drm_gem_create(struct drm_device *dev, size_t size,
					   bool alloc_kmap)
{
	struct mtk_drm_private *priv = dev->dev_private;
	struct mtk_drm_gem_obj *mtk_gem;
	struct drm_gem_object *obj;
	int ret;

	mtk_gem = mtk_drm_gem_init(dev, size);
	if (IS_ERR(mtk_gem))
		return ERR_CAST(mtk_gem);

	obj = &mtk_gem->base;

	mtk_gem->dma_attrs = DMA_ATTR_WRITE_COMBINE;

	if (!alloc_kmap)
		mtk_gem->dma_attrs |= DMA_ATTR_NO_KERNEL_MAPPING;
	//	mtk_gem->dma_attrs |= DMA_ATTR_NO_WARN;
	mtk_gem->cookie =
		mtk_gem_dma_alloc(priv->dma_dev, obj->size, &mtk_gem->dma_addr,
				GFP_KERNEL, mtk_gem->dma_attrs, __func__,
				__LINE__);
	if (!mtk_gem->cookie) {
		DDPPR_ERR("failed to allocate %zx byte dma buffer", obj->size);
		ret = -ENOMEM;
		goto err_gem_free;
	}
	mtk_gem->size = obj->size;

	if (alloc_kmap)
		mtk_gem->kvaddr = mtk_gem->cookie;

	pr_debug("cookie = %p dma_addr = %pad size = %zu\n",
			 mtk_gem->cookie, &mtk_gem->dma_addr, mtk_gem->size);

	return mtk_gem;

err_gem_free:
	drm_gem_object_release(obj);
	kfree(mtk_gem);
	return ERR_PTR(ret);
}

struct mtk_drm_gem_obj *mtk_drm_gem_create_from_heap(struct drm_device *dev,
                                       const char *heap, size_t size)
{
    struct mtk_drm_private *priv = dev->dev_private;
    struct mtk_drm_gem_obj *gem;
    struct dma_buf *buf;
    struct dma_buf_attachment *attach;
    struct sg_table *sg;
    struct iosys_map map = IOSYS_MAP_INIT_VADDR(NULL);
    int ret;

    gem = mtk_drm_gem_init(dev, size);
    if (IS_ERR(gem))
        return gem;
    buf = dma_heap_buffer_alloc_by_name(heap, size, O_RDWR | O_CLOEXEC, 0);
    if (IS_ERR(buf)) {
        ret = PTR_ERR(buf);
        goto release_gem;
    }
    attach = dma_buf_attach(buf, priv->dma_dev);
    if (IS_ERR(attach)) {
        ret = PTR_ERR(attach);
        goto put_buf;
    }
    sg = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
    if (IS_ERR(sg)) {
        ret = PTR_ERR(sg);
        goto detach;
    }
    ret = dma_buf_vmap_unlocked(buf, &map);
    if (ret)
        iosys_map_clear(&map); /* Secure heaps can be inaccessible to the CPU. */
    gem->heap_buf = buf;
    gem->heap_attach = attach;
    gem->heap_map = map;
    gem->sg = sg;
    gem->dma_addr = sg_dma_address(sg->sgl);
    gem->size = buf->size;
    gem->kvaddr = map.is_iomem ? NULL : map.vaddr;
    return gem;

detach:
    dma_buf_detach(buf, attach);
put_buf:
    dma_buf_put(buf);
release_gem:
    drm_gem_object_release(&gem->base);
    kfree(gem);
    return ERR_PTR(ret);
}

void mtk_drm_gem_free_object(struct drm_gem_object *obj)
{
	struct mtk_drm_gem_obj *mtk_gem = to_mtk_gem_obj(obj);
	struct mtk_drm_private *priv = obj->dev->dev_private;
	DRM_MMP_MARK(ion_import_free,
		(unsigned long)mtk_gem->dma_addr,
		(unsigned long)((unsigned long long)(mtk_gem->dma_addr >> 4 & 0x030000000ul)
		| mtk_gem->size));

	if (mtk_gem->boot_fb) {
		int ret;

		dma_unmap_sgtable(mtk_gem->boot_dma_dev, mtk_gem->sg,
				DMA_BIDIRECTIONAL, DMA_ATTR_SKIP_CPU_SYNC);
		sg_free_table(mtk_gem->sg);
		kfree(mtk_gem->sg);
		vunmap(mtk_gem->cookie);
		if (mtk_gem->boot_reclaim) {
			ret = mtk_drm_boot_fb_free_pages(mtk_gem->boot_fb_base, obj->size);
			if (ret)
				dev_warn(obj->dev->dev, "Boot framebuffer RAM could not be reclaimed: %d\n", ret);
		}
		put_device(mtk_gem->boot_dma_dev);
	} else if (mtk_gem->heap_buf) {
        if (!iosys_map_is_null(&mtk_gem->heap_map))
            dma_buf_vunmap_unlocked(mtk_gem->heap_buf, &mtk_gem->heap_map);
        dma_buf_unmap_attachment_unlocked(mtk_gem->heap_attach, mtk_gem->sg,
                                         DMA_BIDIRECTIONAL);
        dma_buf_detach(mtk_gem->heap_buf, mtk_gem->heap_attach);
        dma_buf_put(mtk_gem->heap_buf);
    } else if (mtk_gem->sg)
        drm_prime_gem_destroy(obj, mtk_gem->sg);
	else if (!mtk_gem->sec)
		mtk_gem_dma_free(priv->dma_dev, obj->size, mtk_gem->cookie,
			       mtk_gem->dma_addr, mtk_gem->dma_attrs,
			       __func__, __LINE__);

	/* release file pointer to gem object. */
	drm_gem_object_release(obj);

	kfree(mtk_gem);
}

int mtk_drm_gem_dumb_create(struct drm_file *file_priv, struct drm_device *dev,
			    struct drm_mode_create_dumb *args)
{
	struct mtk_drm_gem_obj *mtk_gem;
	int ret;

	args->pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	args->size = (__u64)args->pitch * (__u64)args->height;

	mtk_gem = mtk_drm_gem_create(dev, args->size, true);
	if (IS_ERR(mtk_gem))
		return PTR_ERR(mtk_gem);

	/*
	 * allocate a id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file_priv, &mtk_gem->base, &args->handle);
	if (ret)
		goto err_handle_create;

	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put(&mtk_gem->base);

	mtk_gem->is_dumb = 1;

	return 0;

err_handle_create:
	mtk_drm_gem_free_object(&mtk_gem->base);
	return ret;
}

int mtk_drm_gem_dumb_map_offset(struct drm_file *file_priv,
				struct drm_device *dev, uint32_t handle,
				uint64_t *offset)
{
	struct drm_gem_object *obj;
	int ret;

	obj = drm_gem_object_lookup(file_priv, handle);
	if (!obj) {
		DDPPR_ERR("failed to lookup gem object.\n");
		return -EINVAL;
	}

	ret = drm_gem_create_mmap_offset(obj);
	if (ret)
		goto out;

	*offset = drm_vma_node_offset_addr(&obj->vma_node);
	pr_debug("offset = 0x%llx\n", *offset);

out:
	drm_gem_object_put(obj);
	return ret;
}

static int mtk_drm_gem_object_mmap(struct drm_gem_object *obj,
				   struct vm_area_struct *vma)

{
	int ret;
	struct mtk_drm_gem_obj *mtk_gem = to_mtk_gem_obj(obj);
	struct mtk_drm_private *priv = obj->dev->dev_private;

	/*
	 * dma_alloc_attrs() allocated a struct page table for mtk_gem, so clear
	 * VM_PFNMAP flag that was set by drm_gem_mmap_obj()/drm_gem_mmap().
	 */
	vm_flags_clear(vma, VM_PFNMAP);
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_pgoff = 0;

	if (mtk_gem->boot_fb) {
		vm_flags_set(vma, VM_IO | VM_PFNMAP);
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		return remap_pfn_range(vma, vma->vm_start, PHYS_PFN(mtk_gem->boot_fb_base),
				       vma->vm_end - vma->vm_start, vma->vm_page_prot);
	}
	if (mtk_gem->heap_buf) {
        ret = dma_buf_mmap(mtk_gem->heap_buf, vma, 0);
        /* A successful dma-buf mapping owns its file, not this GEM object. */
        if (!ret)
            drm_gem_object_put(obj);
        return ret;
    }
	ret = dma_mmap_attrs(priv->dma_dev, vma, mtk_gem->cookie,
			     mtk_gem->dma_addr, obj->size, mtk_gem->dma_attrs);
	/* drm_gem_mmap_obj() releases its mapping reference on failure. */
	return ret;
}

int mtk_drm_gem_mmap_buf(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	int ret;

	ret = drm_gem_mmap_obj(obj, obj->size, vma);
	if (ret)
		return ret;

	return 0;
}

int mtk_drm_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_gem_object *obj;
	int ret;

	ret = drm_gem_mmap(filp, vma);
	if (ret)
		return ret;

	obj = vma->vm_private_data;

	return 0;
}

void mtk_drm_gem_ion_free_handle(struct dma_buf *buf_hnd,
				const char *name, int line)
{
	if (!buf_hnd) {
		DDPPR_ERR("invalid buf_hnd handle!\n");

		return;
	}

	dma_buf_put(buf_hnd);

	DDPDBG("free dma_buf handle 0x%p\n", buf_hnd);
}

/**
 * Import ion handle and configure this buffer
 * @client
 * @fd ion shared fd
 * @return ion handle
 */
struct dma_buf *mtk_drm_gem_ion_import_handle(int fd)
{
	struct dma_buf *buf_hnd = NULL;

	buf_hnd = dma_buf_get(fd);

	if (IS_ERR(buf_hnd)) {
		DDPPR_ERR("%s:%d error! hnd:0x%p, fd:%d\n",
					__func__, __LINE__, buf_hnd, fd);
		return NULL;
	}

	DDPDBG("import dma_buf handle fd=%d,hnd=0x%p\n", fd, buf_hnd);

	return buf_hnd;
}

/*
 * Allocate a sg_table for this GEM object.
 * Note: Both the table's contents, and the sg_table itself must be freed by
 *       the caller.
 * Returns a pointer to the newly allocated sg_table, or an ERR_PTR() error.
 */
struct sg_table *mtk_gem_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct mtk_drm_gem_obj *mtk_gem = to_mtk_gem_obj(obj);
	struct mtk_drm_private *priv = obj->dev->dev_private;
	struct sg_table *sgt;
	int ret;

	DDPDBG("%s:%d obj:0x%p +\n",
		   __func__, __LINE__,
		   obj);

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		DDPPR_ERR("%s:%d sgt alloc error\n",
			   __func__, __LINE__);
		return ERR_PTR(-ENOMEM);
	}

	if (mtk_gem->boot_fb) {
		struct scatterlist *src, *dst;
		unsigned int i;

		ret = sg_alloc_table(sgt, mtk_gem->sg->orig_nents, GFP_KERNEL);
		if (ret) {
			kfree(sgt);
			return ERR_PTR(ret);
		}
		dst = sgt->sgl;
		for_each_sg(mtk_gem->sg->sgl, src, mtk_gem->sg->orig_nents, i) {
			sg_set_page(dst, sg_page(src), src->length, src->offset);
			dst = sg_next(dst);
		}
		return sgt;
	}

	ret = dma_get_sgtable_attrs(priv->dma_dev, sgt, mtk_gem->cookie,
				    mtk_gem->dma_addr, obj->size,
				    mtk_gem->dma_attrs);
	if (ret) {
		DDPPR_ERR("failed to allocate sgt, %d\n", ret);
		kfree(sgt);
		return ERR_PTR(ret);
	}

	DDPDBG("%s:%d sgt:0x%p ret:%d -\n",
		   __func__, __LINE__,
		   sgt,
		   ret);

	return sgt;
}

struct drm_gem_object *
mtk_gem_prime_import_sg_table(struct drm_device *dev,
			      struct dma_buf_attachment *attach,
			      struct sg_table *sg)
{
	struct mtk_drm_gem_obj *mtk_gem = NULL;
#ifdef IF_ZERO
	int ret;
	struct scatterlist *s;
	unsigned int i, last_len = 0, error_cnt = 0;
	dma_addr_t expected, last_iova = 0;
#endif

	DDPDBG("%s:%d dev:0x%p, attach:0x%p, sg:0x%p +\n",
			__func__, __LINE__,
			dev,
			attach,
			sg);
	if (attach && attach->dmabuf)
		mtk_gem = mtk_drm_gem_init(dev, attach->dmabuf->size);

	if (IS_ERR(mtk_gem)) {
		DDPPR_ERR("%s:%d mtk_gem error:0x%p\n",
				__func__, __LINE__,
				mtk_gem);
		return ERR_PTR(PTR_ERR(mtk_gem));
	} else if (!mtk_gem) {
		DDPPR_ERR("%s:%d mtk_gem error:0x%p\n",
				__func__, __LINE__,
				mtk_gem);
		return ERR_PTR(-ENOMEM);
	}

#ifdef IF_ZERO
	expected = sg_dma_address(sg->sgl);
	for_each_sg(sg->sgl, s, sg->nents, i) {
		if (sg_dma_address(s) != expected) {
			if (!error_cnt)
				DDPPR_ERR("sg_table is not contiguous %u\n",
					  sg->nents);
			DDPPR_ERR("exp:0x%llx,cur:0x%llx/%u,last:0x%llx+0x%x\n",
				  expected, sg_dma_address(s),
				  i, last_iova, last_len);
			if (error_cnt++ > 5)
				break;
		}
		last_iova = sg_dma_address(s);
		last_len = sg_dma_len(s);
		expected = sg_dma_address(s) + sg_dma_len(s);
	}
	if (error_cnt) {
		ret = -EINVAL;
		goto err_gem_free;
	}
#endif
	if (disp_sec_cb.cb != NULL && attach->dmabuf)
		mtk_gem->sec = disp_sec_cb.cb(DISP_SEC_CHECK, NULL, 0, attach->dmabuf);
	else
		mtk_gem->sec = false;
	mtk_gem->dma_addr = sg_dma_address(sg->sgl);
	mtk_gem->size = attach->dmabuf->size;
	mtk_gem->sg = sg;
	DRM_MMP_MARK(ion_import_dma,
		(unsigned long)mtk_gem->dma_addr,
		(unsigned long)((unsigned long long)(mtk_gem->dma_addr >> 4 & 0x030000000ul)
		| mtk_gem->size));

	DDPDBG("%s:%d sec:%d, addr:0x%llx, size:%ld, sg:0x%p -\n",
			__func__, __LINE__,
			mtk_gem->sec,
			mtk_gem->dma_addr,
			mtk_gem->size,
			mtk_gem->sg);

	return &mtk_gem->base;

#ifdef IF_ZERO
err_gem_free:
	kfree(mtk_gem);
	return ERR_PTR(ret);
#endif
}

int mtk_gem_map_offset_ioctl(struct drm_device *drm, void *data,
			     struct drm_file *file_priv)
{
	struct drm_mtk_gem_map_off *args = data;

	return mtk_drm_gem_dumb_map_offset(file_priv, drm, args->handle,
					   &args->offset);
}

int mtk_gem_create_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file_priv)
{
//avoid security issue
#ifdef IF_ZERO
	struct mtk_drm_gem_obj *mtk_gem;
	struct drm_mtk_gem_create *args = data;
	int ret;

	DDPDBG("%s(), args->size %llu\n", __func__, args->size);
	if (args->size == 0) {
		DDPPR_ERR("%s(), invalid args->size %llu\n",
			  __func__, args->size);
		return 0;
	}

	mtk_gem = mtk_drm_gem_create(dev, args->size, true);
	if (IS_ERR(mtk_gem))
		return PTR_ERR(mtk_gem);

	/*
	 * allocate a id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file_priv, &mtk_gem->base, &args->handle);
	if (ret)
		goto err_handle_create;

	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put(&mtk_gem->base);

	return 0;

err_handle_create:
	mtk_drm_gem_free_object(&mtk_gem->base);
	return ret;
#endif
	DDPPR_ERR("not supported!\n");
	return 0;
}

static void prepare_output_buffer(struct drm_device *dev,
				  struct drm_mtk_gem_submit *buf,
				  struct mtk_fence_buf_info *output_buf)
{

	if (!(mtk_drm_session_mode_is_decouple_mode(dev) &&
	      mtk_drm_session_mode_is_mirror_mode(dev))) {
		buf->interface_fence_fd = MTK_INVALID_FENCE_FD;
		buf->interface_index = 0;
		return;
	}

	/* create second fence for WDMA when decouple mirror mode */
	buf->layer_id = mtk_fence_get_interface_timeline_id();
	output_buf = mtk_fence_prepare_buf(dev, buf, false, NULL);
	if (output_buf) {
		buf->interface_fence_fd = output_buf->fence;
		buf->interface_index = output_buf->idx;
	} else {
		DDPPR_ERR("P+ FAIL /%s%d/L%d/e%d/fd%d/id%d/ffd%d\n",
			  mtk_fence_session_mode_spy(buf->session_id),
			  MTK_SESSION_DEV(buf->session_id), buf->layer_id,
			  buf->layer_en, buf->index, buf->fb_id, buf->fence_fd);
		buf->interface_fence_fd = MTK_INVALID_FENCE_FD;
		buf->interface_index = 0;
	}
}

int mtk_gem_submit_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file_priv)
{
	int ret = 0;
	struct drm_mtk_gem_submit *args = data;
	struct mtk_fence_buf_info *buf, *buf2 = NULL;

	if (args->type == MTK_SUBMIT_OUT_FENCE)
		args->layer_id = mtk_fence_get_output_timeline_id();

	if (args->layer_en) {
		buf = mtk_fence_prepare_buf(dev, args, false, NULL);
		if (buf != NULL) {
			args->fence_fd = buf->fence;
			args->index = buf->idx;
		} else {
			DDPPR_ERR("P+ FAIL /%s%d/L%d/e%d/fd%d/id%d/ffd%d\n",
				  mtk_fence_session_mode_spy(args->session_id),
				  MTK_SESSION_DEV(args->session_id),
				  args->layer_id, args->layer_en, args->fb_id,
				  args->index, args->fence_fd);
			args->fence_fd = MTK_INVALID_FENCE_FD; /* invalid fd */
			args->index = 0;
		}
		if (args->type == MTK_SUBMIT_OUT_FENCE)
			prepare_output_buffer(dev, args, buf2);
	} else {
		DDPPR_ERR("P+ FAIL /%s%d/l%d/e%d/fd%d/id%d/ffd%d\n",
			  mtk_fence_session_mode_spy(args->session_id),
			  MTK_SESSION_DEV(args->session_id), args->layer_id,
			  args->layer_en, args->fb_id, args->index,
			  args->fence_fd);
		args->fence_fd = MTK_INVALID_FENCE_FD; /* invalid fd */
		args->index = 0;
	}

	return ret;
}

void print_mml_frame_buffer(struct mml_frame_buffer frame_buf)
{
	unsigned int i = 0;

	DDPMSG("====  frame buffer s ====\n");
	DDPMSG("src buf\n");
	DDPMSG("[0]:fd:%d, size:%d, [1]:fd:%d, size:%d, [2]:fd:%d, size:%d,",
			frame_buf.src.fd[0], frame_buf.src.size[0],
			frame_buf.src.fd[1], frame_buf.src.size[1],
			frame_buf.src.fd[2], frame_buf.src.size[2]);
	DDPMSG("cnt:%d, fence:%d, flush:%d, invalid:%d\n",
			frame_buf.src.cnt, frame_buf.src.fence,
			frame_buf.src.flush, frame_buf.src.invalid);
	DDPMSG("dest:(cnt:%d)\n", frame_buf.dest_cnt);
	for (; i < MML_MAX_OUTPUTS; ++i) {
		DDPMSG("dest buf[%d]\n", i);
		DDPMSG("[0]:fd:%d, size:%d, [1]:fd:%d, size:%d, [2]:fd:%d, size:%d,",
			frame_buf.dest[i].fd[0], frame_buf.dest[i].size[0],
			frame_buf.dest[i].fd[1], frame_buf.dest[i].size[1],
			frame_buf.dest[i].fd[2], frame_buf.dest[i].size[2]);

		DDPMSG("cnt:%d, fence:%d, flush:%d, invalid:%d\n",
			frame_buf.dest[i].cnt, frame_buf.dest[i].fence,
			frame_buf.dest[i].flush, frame_buf.dest[i].invalid);
	}
	DDPMSG("==== frame buffer e ====\n");
}

//To-Do: need to be remove
void print_mml_frame_info(struct mml_frame_info info)
{
	unsigned int i = 0;

	DDPMSG("====  frame_info s ====\n");
	DDPMSG("src cfg:\n");
	DDPMSG("w:%d, h:%d, y_s:%d, uv_s:%d, vert_s:%d, f:%d, pro:%d,",
		info.src.width, info.src.height,
		info.src.y_stride, info.src.uv_stride,
		info.src.vert_stride, info.src.format, info.src.profile);
	DDPMSG("plane_offset[0]:%d, plane_offset[1]:%d, plane_offset[2]:%d, p_c:%d, sec:%d\n",
		info.src.plane_offset[0], info.src.plane_offset[1], info.src.plane_offset[2],
		info.src.plane_cnt, info.src.secure);
	DDPMSG("dest_cnt:%d\n", info.dest_cnt);
	for (; i < MML_MAX_OUTPUTS; ++i) {
		DDPMSG("dest cfg[%d]:\n", i);
		DDPMSG("w:%d, h:%d, y_s:%d, uv_s:%d, vert_s:%d, f:%d, pro:%d,",
			info.dest[i].data.width, info.dest[i].data.height,
			info.dest[i].data.y_stride, info.dest[i].data.uv_stride,
			info.dest[i].data.vert_stride, info.dest[i].data.format,
			info.dest[i].data.profile);
		DDPMSG("plane_offset[0]:%d, plane_offset[1]:%d, plane_offset[2]:%d,",
			info.dest[i].data.plane_offset[0],
			info.dest[i].data.plane_offset[1],
			info.dest[i].data.plane_offset[2]);
		DDPMSG("p_c:%d, sec:%d\n",
			info.dest[i].data.plane_cnt, info.dest[i].data.secure);
		DDPMSG("x_sub_px:%d, y_sub_px:%d, w_sub_px:%d, h_sub_px:%d\n",
			info.dest[i].crop.x_sub_px, info.dest[i].crop.y_sub_px,
			info.dest[i].crop.w_sub_px, info.dest[i].crop.h_sub_px);
		DDPMSG("l:%d, t:%d, w:%d, h:%d\n",
			info.dest[i].compose.left, info.dest[i].compose.top,
			info.dest[i].compose.width, info.dest[i].compose.height);
		DDPMSG("[MMLPQParamParser] en_sharp[%d], en_ur[%d], en_dc[%d],",
			info.dest[i].pq_config.en_sharp, info.dest[i].pq_config.en_ur,
			info.dest[i].pq_config.en_dc);
		DDPMSG("en_color[%d], en_hdr[%d], en_ccorr[%d], en_dre[%d]",
			info.dest[i].pq_config.en_color, info.dest[i].pq_config.en_hdr,
			info.dest[i].pq_config.en_ccorr, info.dest[i].pq_config.en_dre);
		DDPMSG("rotate:%d, flip:%d, pq_config.en:%d\n",
			info.dest[i].rotate, info.dest[i].flip, info.dest[i].pq_config);
	}
	DDPMSG("mode:%d, layer_id:%d\n", info.mode, info.layer_id);
	DDPMSG("====  frame_info e ====\n");
}

//To-Do: need to be remove
void print_mml_submit(struct mml_submit *args)
{
	int i = 0;

	if (!args)
		return;

	DDPMSG("====  %s s ====\n", __func__);
	if (args->job)
		DDPMSG("jobid:%d, fence:%d\n", args->job->jobid, args->job->fence);

	print_mml_frame_info(args->info);
	print_mml_frame_buffer(args->buffer);

	for (i = 0; i < MML_MAX_OUTPUTS; i++) {
		if (args->pq_param[i]) {
			DDPMSG("pq_param[%d]\n", i);
			DDPMSG("e:%d, t:%d, pq_s:%d, l_id:%d, d_id:%d, s_ga:%d, d_ga:%d,",
				args->pq_param[i]->enable, args->pq_param[i]->time_stamp,
				args->pq_param[i]->scenario, args->pq_param[i]->layer_id,
				args->pq_param[i]->disp_id, args->pq_param[i]->src_gamut,
				args->pq_param[i]->dst_gamut);
			DDPMSG("src_hdr_video_mode:%d, video_id:%d, time_stamp:%d,",
				args->pq_param[i]->src_hdr_video_mode,
				args->pq_param[i]->video_param.video_id,
				args->pq_param[i]->video_param.time_stamp);
			DDPMSG("ishdr2sdr:%d, param_table:%d, pq_user_info:%d\n",
				args->pq_param[i]->video_param.ishdr2sdr,
				args->pq_param[i]->video_param.param_table,
				args->pq_param[i]->user_info);
		}
	}
	DDPMSG("sec:%lld, usec:%lld, update:%d\n",
		args->end.sec, args->end.nsec, args->update);
	DDPMSG("====  %s e ====\n", __func__);
}

int mtk_drm_ioctl_mml_gem_submit(struct drm_device *dev, void *data,
			 struct drm_file *file_priv)
{
	int ret = 0;
	int i = 0;
	struct mml_submit *submit_user = (struct mml_submit *)data;
	struct mtk_drm_private *priv = dev->dev_private;
	struct mml_drm_ctx *mml_ctx = NULL;
	struct mml_submit *submit_kernel;
	struct drm_crtc *crtc;
	struct mtk_drm_crtc *mtk_crtc;

	DDPINFO("%s:%d +\n", __func__, __LINE__);

	if (!mtk_drm_helper_get_opt(priv->helper_opt, MTK_DRM_OPT_MML_PRIMARY)) {
		DDPINFO("%s:%d MTK_DRM_OPT_MML_PRIMARY is not support\n", __func__, __LINE__);
		return -EINVAL;
	}

	if (!submit_user)
		return -EFAULT;

	submit_kernel = kzalloc(sizeof(struct mml_submit), GFP_KERNEL);
	if (!submit_kernel) {
		DDPMSG("[%s][%d][%d] kzalloc fail\n", __func__, __LINE__, ret);
		return -EFAULT;
	}

	memcpy(submit_kernel, submit_user, sizeof(struct mml_submit));
	submit_kernel->job = kzalloc(sizeof(struct mml_job), GFP_KERNEL);
	if (!submit_kernel->job) {
		ret = -EFAULT;
		DDPMSG("[%s][%d][%d] kzalloc fail\n", __func__, __LINE__, ret);
		goto err_handle_create;
	}

	if (submit_user->job) {
		ret = copy_from_user(submit_kernel->job, submit_user->job, sizeof(struct mml_job));
		if (ret) {
			DDPMSG("[%s][%d][%d] copy_from_user fail\n", __func__, __LINE__, ret);
			goto err_handle_create;
		}
	} else
		DDPMSG("[%s] submit_user->job is null\n", __func__);

	for (i = 0; i < MML_MAX_OUTPUTS; i++) {
		if (submit_user->pq_param[i]) {
			submit_kernel->pq_param[i] =
				kzalloc(sizeof(struct mml_pq_param), GFP_KERNEL);
			if (!submit_kernel->pq_param[i]) {
				ret = -EFAULT;
				DDPMSG("[%s][%d][%d] kzalloc fail\n", __func__, __LINE__, ret);
				goto err_handle_create;
			}

			ret = copy_from_user(submit_kernel->pq_param[i], submit_user->pq_param[i],
				sizeof(struct mml_pq_param));
			if (ret) {
				DDPMSG("[%s][%d][%d] copy_from_user fail\n",
				__func__, __LINE__, ret);
				goto err_handle_create;
			}
			//copy_from_user(submit_kernel->pq_param[i]->gralloc_extra_handle,
			//	submit_user->pq_param[i]->gralloc_extra_handle, sizeof(void *));
		} else {
			DDPMSG("%s submit_user->pq_param[i] is null\n", __func__);
		}
	}

	crtc = list_first_entry(&(dev)->mode_config.crtc_list,
		typeof(*crtc), head);
	mtk_crtc = to_mtk_crtc(crtc);
	mml_ctx = mtk_drm_get_mml_drm_ctx(dev, crtc);

	if (mtk_crtc->mml_debug & DISP_MML_DBG_LOG) {
		DDPINFO("%s:%d\n", __func__, __LINE__);
		print_mml_submit(submit_kernel);
		DDPINFO("%s:%d\n", __func__, __LINE__);
	}

	if (mml_ctx > 0) {
		DDPINFO("%s:%d mml_drm_submit +\n", __func__, __LINE__);
		ret = mml_drm_submit(mml_ctx, submit_kernel, NULL);
		DDPINFO("%s:%d mml_drm_submit - ret:%d, job(id,fence):(%d,%d)\n",
			__func__, __LINE__, ret,
			submit_kernel->job->jobid, submit_kernel->job->fence);
		if (ret)
			DDPMSG("submit failed: %d\n", ret);
	}

	if (submit_user->job) {
		ret = copy_to_user(submit_user->job, submit_kernel->job, sizeof(struct mml_job));
		if (ret)
			DDPMSG("[%s][%d][%d] copy_to_user fail\n", __func__, __LINE__, ret);
	}

err_handle_create:
	for (i = 0; i < MML_MAX_OUTPUTS; i++) {
		if (submit_kernel->pq_param[i])
			kfree(submit_kernel->pq_param[i]);
	}
	kfree(submit_kernel->job);
	kfree(submit_kernel);

	DDPINFO("%s:%d -\n", __func__, __LINE__);

	return ret;
}
