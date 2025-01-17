// SPDX-License-Identifier: GPL-2.0-or-later

#include <drm/drm_debugfs.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_ttm_helper.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/drm_mode.h>
#include <drm/drm_prime.h>
#include <drm/ttm/ttm_page_alloc.h>

static const struct drm_gem_object_funcs drm_gem_vram_object_funcs;

/**
 * DOC: overview
 *
 * This library provides a GEM buffer object that is backed by video RAM
 * (VRAM). It can be used for framebuffer devices with dedicated memory.
 *
 * The data structure &struct drm_vram_mm and its helpers implement a memory
 * manager for simple framebuffer devices with dedicated video memory. Buffer
 * objects are either placed in video RAM or evicted to system memory. The rsp.
 * buffer object is provided by &struct drm_gem_vram_object.
 */

/*
 * Buffer-objects helpers
 */

static void drm_gem_vram_cleanup(struct drm_gem_vram_object *gbo)
{
	/* We got here via ttm_bo_put(), which means that the
	 * TTM buffer object in 'bo' has already been cleaned
	 * up; only release the GEM object.
	 */

	WARN_ON(gbo->kmap_use_count);
	WARN_ON(gbo->kmap.virtual);

	drm_gem_object_release(&gbo->bo.base);
}

static void drm_gem_vram_destroy(struct drm_gem_vram_object *gbo)
{
	drm_gem_vram_cleanup(gbo);
	kfree(gbo);
}

static void ttm_buffer_object_destroy(struct ttm_buffer_object *bo)
{
	struct drm_gem_vram_object *gbo = drm_gem_vram_of_bo(bo);

	drm_gem_vram_destroy(gbo);
}

static void drm_gem_vram_placement(struct drm_gem_vram_object *gbo,
				   unsigned long pl_flag)
{
	unsigned int i;
	unsigned int c = 0;

	gbo->placement.placement = gbo->placements;
	gbo->placement.busy_placement = gbo->placements;

	if (pl_flag & TTM_PL_FLAG_VRAM)
		gbo->placements[c++].flags = TTM_PL_FLAG_WC |
					     TTM_PL_FLAG_UNCACHED |
					     TTM_PL_FLAG_VRAM;

	if (pl_flag & TTM_PL_FLAG_SYSTEM)
		gbo->placements[c++].flags = TTM_PL_MASK_CACHING |
					     TTM_PL_FLAG_SYSTEM;

	if (!c)
		gbo->placements[c++].flags = TTM_PL_MASK_CACHING |
					     TTM_PL_FLAG_SYSTEM;

	gbo->placement.num_placement = c;
	gbo->placement.num_busy_placement = c;

	for (i = 0; i < c; ++i) {
		gbo->placements[i].fpfn = 0;
		gbo->placements[i].lpfn = 0;
	}
}

static int drm_gem_vram_init(struct drm_device *dev,
			     struct ttm_bo_device *bdev,
			     struct drm_gem_vram_object *gbo,
			     size_t size, unsigned long pg_align,
			     bool interruptible)
{
	int ret;
	size_t acc_size;

	gbo->bo.base.funcs = &drm_gem_vram_object_funcs;

	ret = drm_gem_object_init(dev, &gbo->bo.base, size);
	if (ret)
		return ret;

	acc_size = ttm_bo_dma_acc_size(bdev, size, sizeof(*gbo));

	gbo->bo.bdev = bdev;
	drm_gem_vram_placement(gbo, TTM_PL_FLAG_VRAM | TTM_PL_FLAG_SYSTEM);

	ret = ttm_bo_init(bdev, &gbo->bo, size, ttm_bo_type_device,
			  &gbo->placement, pg_align, interruptible, acc_size,
			  NULL, NULL, ttm_buffer_object_destroy);
	if (ret)
		goto err_drm_gem_object_release;

	return 0;

err_drm_gem_object_release:
	drm_gem_object_release(&gbo->bo.base);
	return ret;
}

/**
 * drm_gem_vram_create() - Creates a VRAM-backed GEM object
 * @dev:		the DRM device
 * @bdev:		the TTM BO device backing the object
 * @size:		the buffer size in bytes
 * @pg_align:		the buffer's alignment in multiples of the page size
 * @interruptible:	sleep interruptible if waiting for memory
 *
 * Returns:
 * A new instance of &struct drm_gem_vram_object on success, or
 * an ERR_PTR()-encoded error code otherwise.
 */
struct drm_gem_vram_object *drm_gem_vram_create(struct drm_device *dev,
						struct ttm_bo_device *bdev,
						size_t size,
						unsigned long pg_align,
						bool interruptible)
{
	struct drm_gem_vram_object *gbo;
	int ret;

	gbo = kzalloc(sizeof(*gbo), GFP_KERNEL);
	if (!gbo)
		return ERR_PTR(-ENOMEM);

	ret = drm_gem_vram_init(dev, bdev, gbo, size, pg_align, interruptible);
	if (ret < 0)
		goto err_kfree;

	return gbo;

err_kfree:
	kfree(gbo);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(drm_gem_vram_create);

/**
 * drm_gem_vram_put() - Releases a reference to a VRAM-backed GEM object
 * @gbo:	the GEM VRAM object
 *
 * See ttm_bo_put() for more information.
 */
void drm_gem_vram_put(struct drm_gem_vram_object *gbo)
{
	ttm_bo_put(&gbo->bo);
}
EXPORT_SYMBOL(drm_gem_vram_put);

/**
 * drm_gem_vram_mmap_offset() - Returns a GEM VRAM object's mmap offset
 * @gbo:	the GEM VRAM object
 *
 * See drm_vma_node_offset_addr() for more information.
 *
 * Returns:
 * The buffer object's offset for userspace mappings on success, or
 * 0 if no offset is allocated.
 */
u64 drm_gem_vram_mmap_offset(struct drm_gem_vram_object *gbo)
{
	return drm_vma_node_offset_addr(&gbo->bo.base.vma_node);
}
EXPORT_SYMBOL(drm_gem_vram_mmap_offset);

/**
 * drm_gem_vram_offset() - \
	Returns a GEM VRAM object's offset in video memory
 * @gbo:	the GEM VRAM object
 *
 * This function returns the buffer object's offset in the device's video
 * memory. The buffer object has to be pinned to %TTM_PL_VRAM.
 *
 * Returns:
 * The buffer object's offset in video memory on success, or
 * a negative errno code otherwise.
 */
s64 drm_gem_vram_offset(struct drm_gem_vram_object *gbo)
{
	if (WARN_ON_ONCE(!gbo->pin_count))
		return (s64)-ENODEV;
	return gbo->bo.offset;
}
EXPORT_SYMBOL(drm_gem_vram_offset);

static int drm_gem_vram_pin_locked(struct drm_gem_vram_object *gbo,
				   unsigned long pl_flag)
{
	int i, ret;
	struct ttm_operation_ctx ctx = { false, false };

	if (gbo->pin_count)
		goto out;

	if (pl_flag)
		drm_gem_vram_placement(gbo, pl_flag);

	for (i = 0; i < gbo->placement.num_placement; ++i)
		gbo->placements[i].flags |= TTM_PL_FLAG_NO_EVICT;

	ret = ttm_bo_validate(&gbo->bo, &gbo->placement, &ctx);
	if (ret < 0)
		return ret;

out:
	++gbo->pin_count;

	return 0;
}

/**
 * drm_gem_vram_pin() - Pins a GEM VRAM object in a region.
 * @gbo:	the GEM VRAM object
 * @pl_flag:	a bitmask of possible memory regions
 *
 * Pinning a buffer object ensures that it is not evicted from
 * a memory region. A pinned buffer object has to be unpinned before
 * it can be pinned to another region. If the pl_flag argument is 0,
 * the buffer is pinned at its current location (video RAM or system
 * memory).
 *
 * Returns:
 * 0 on success, or
 * a negative error code otherwise.
 */
int drm_gem_vram_pin(struct drm_gem_vram_object *gbo, unsigned long pl_flag)
{
	int ret;

	ret = ttm_bo_reserve(&gbo->bo, true, false, NULL);
	if (ret)
		return ret;
	ret = drm_gem_vram_pin_locked(gbo, pl_flag);
	ttm_bo_unreserve(&gbo->bo);

	return ret;
}
EXPORT_SYMBOL(drm_gem_vram_pin);

static int drm_gem_vram_unpin_locked(struct drm_gem_vram_object *gbo)
{
	int i, ret;
	struct ttm_operation_ctx ctx = { false, false };

	if (WARN_ON_ONCE(!gbo->pin_count))
		return 0;

	--gbo->pin_count;
	if (gbo->pin_count)
		return 0;

	for (i = 0; i < gbo->placement.num_placement ; ++i)
		gbo->placements[i].flags &= ~TTM_PL_FLAG_NO_EVICT;

	ret = ttm_bo_validate(&gbo->bo, &gbo->placement, &ctx);
	if (ret < 0)
		return ret;

	return 0;
}

/**
 * drm_gem_vram_unpin() - Unpins a GEM VRAM object
 * @gbo:	the GEM VRAM object
 *
 * Returns:
 * 0 on success, or
 * a negative error code otherwise.
 */
int drm_gem_vram_unpin(struct drm_gem_vram_object *gbo)
{
	int ret;

	ret = ttm_bo_reserve(&gbo->bo, true, false, NULL);
	if (ret)
		return ret;
	ret = drm_gem_vram_unpin_locked(gbo);
	ttm_bo_unreserve(&gbo->bo);

	return ret;
}
EXPORT_SYMBOL(drm_gem_vram_unpin);

static void *drm_gem_vram_kmap_locked(struct drm_gem_vram_object *gbo,
				      bool map, bool *is_iomem)
{
	int ret;
	struct ttm_bo_kmap_obj *kmap = &gbo->kmap;

	if (gbo->kmap_use_count > 0)
		goto out;

	if (kmap->virtual || !map)
		goto out;

	ret = ttm_bo_kmap(&gbo->bo, 0, gbo->bo.num_pages, kmap);
	if (ret)
		return ERR_PTR(ret);

out:
	if (!kmap->virtual) {
		if (is_iomem)
			*is_iomem = false;
		return NULL; /* not mapped; don't increment ref */
	}
	++gbo->kmap_use_count;
	if (is_iomem)
		return ttm_kmap_obj_virtual(kmap, is_iomem);
	return kmap->virtual;
}

/**
 * drm_gem_vram_kmap() - Maps a GEM VRAM object into kernel address space
 * @gbo:	the GEM VRAM object
 * @map:	establish a mapping if necessary
 * @is_iomem:	returns true if the mapped memory is I/O memory, or false \
	otherwise; can be NULL
 *
 * This function maps the buffer object into the kernel's address space
 * or returns the current mapping. If the parameter map is false, the
 * function only queries the current mapping, but does not establish a
 * new one.
 *
 * Returns:
 * The buffers virtual address if mapped, or
 * NULL if not mapped, or
 * an ERR_PTR()-encoded error code otherwise.
 */
void *drm_gem_vram_kmap(struct drm_gem_vram_object *gbo, bool map,
			bool *is_iomem)
{
	int ret;
	void *virtual;

	ret = ttm_bo_reserve(&gbo->bo, true, false, NULL);
	if (ret)
		return ERR_PTR(ret);
	virtual = drm_gem_vram_kmap_locked(gbo, map, is_iomem);
	ttm_bo_unreserve(&gbo->bo);

	return virtual;
}
EXPORT_SYMBOL(drm_gem_vram_kmap);

static void drm_gem_vram_kunmap_locked(struct drm_gem_vram_object *gbo)
{
	if (WARN_ON_ONCE(!gbo->kmap_use_count))
		return;
	if (--gbo->kmap_use_count > 0)
		return;

	/*
	 * Permanently mapping and unmapping buffers adds overhead from
	 * updating the page tables and creates debugging output. Therefore,
	 * we delay the actual unmap operation until the BO gets evicted
	 * from memory. See drm_gem_vram_bo_driver_move_notify().
	 */
}

/**
 * drm_gem_vram_kunmap() - Unmaps a GEM VRAM object
 * @gbo:	the GEM VRAM object
 */
void drm_gem_vram_kunmap(struct drm_gem_vram_object *gbo)
{
	int ret;

	ret = ttm_bo_reserve(&gbo->bo, false, false, NULL);
	if (WARN_ONCE(ret, "ttm_bo_reserve_failed(): ret=%d\n", ret))
		return;
	drm_gem_vram_kunmap_locked(gbo);
	ttm_bo_unreserve(&gbo->bo);
}
EXPORT_SYMBOL(drm_gem_vram_kunmap);

/**
 * drm_gem_vram_fill_create_dumb() - \
	Helper for implementing &struct drm_driver.dumb_create
 * @file:		the DRM file
 * @dev:		the DRM device
 * @bdev:		the TTM BO device managing the buffer object
 * @pg_align:		the buffer's alignment in multiples of the page size
 * @interruptible:	sleep interruptible if waiting for memory
 * @args:		the arguments as provided to \
				&struct drm_driver.dumb_create
 *
 * This helper function fills &struct drm_mode_create_dumb, which is used
 * by &struct drm_driver.dumb_create. Implementations of this interface
 * should forwards their arguments to this helper, plus the driver-specific
 * parameters.
 *
 * Returns:
 * 0 on success, or
 * a negative error code otherwise.
 */
int drm_gem_vram_fill_create_dumb(struct drm_file *file,
				  struct drm_device *dev,
				  struct ttm_bo_device *bdev,
				  unsigned long pg_align,
				  bool interruptible,
				  struct drm_mode_create_dumb *args)
{
	size_t pitch, size;
	struct drm_gem_vram_object *gbo;
	int ret;
	u32 handle;

	pitch = args->width * ((args->bpp + 7) / 8);
	size = pitch * args->height;

	size = roundup(size, PAGE_SIZE);
	if (!size)
		return -EINVAL;

	gbo = drm_gem_vram_create(dev, bdev, size, pg_align, interruptible);
	if (IS_ERR(gbo))
		return PTR_ERR(gbo);

	ret = drm_gem_handle_create(file, &gbo->bo.base, &handle);
	if (ret)
		goto err_drm_gem_object_put_unlocked;

	drm_gem_object_put_unlocked(&gbo->bo.base);

	args->pitch = pitch;
	args->size = size;
	args->handle = handle;

	return 0;

err_drm_gem_object_put_unlocked:
	drm_gem_object_put_unlocked(&gbo->bo.base);
	return ret;
}
EXPORT_SYMBOL(drm_gem_vram_fill_create_dumb);

/*
 * Helpers for struct ttm_bo_driver
 */

static bool drm_is_gem_vram(struct ttm_buffer_object *bo)
{
	return (bo->destroy == ttm_buffer_object_destroy);
}

static void drm_gem_vram_bo_driver_evict_flags(struct drm_gem_vram_object *gbo,
					       struct ttm_placement *pl)
{
	drm_gem_vram_placement(gbo, TTM_PL_FLAG_SYSTEM);
	*pl = gbo->placement;
}

static int drm_gem_vram_bo_driver_verify_access(struct drm_gem_vram_object *gbo,
						struct file *filp)
{
	return drm_vma_node_verify_access(&gbo->bo.base.vma_node,
					  filp->private_data);
}

static void drm_gem_vram_bo_driver_move_notify(struct drm_gem_vram_object *gbo,
					       bool evict,
					       struct ttm_mem_reg *new_mem)
{
	struct ttm_bo_kmap_obj *kmap = &gbo->kmap;

	if (WARN_ON_ONCE(gbo->kmap_use_count))
		return;

	if (!kmap->virtual)
		return;
	ttm_bo_kunmap(kmap);
	kmap->virtual = NULL;
}

/*
 * Helpers for struct drm_gem_object_funcs
 */

/**
 * drm_gem_vram_object_free() - \
	Implements &struct drm_gem_object_funcs.free
 * @gem:       GEM object. Refers to &struct drm_gem_vram_object.gem
 */
static void drm_gem_vram_object_free(struct drm_gem_object *gem)
{
	struct drm_gem_vram_object *gbo = drm_gem_vram_of_gem(gem);

	drm_gem_vram_put(gbo);
}

/*
 * Helpers for dump buffers
 */

/**
 * drm_gem_vram_driver_create_dumb() - \
	Implements &struct drm_driver.dumb_create
 * @file:		the DRM file
 * @dev:		the DRM device
 * @args:		the arguments as provided to \
				&struct drm_driver.dumb_create
 *
 * This function requires the driver to use @drm_device.vram_mm for its
 * instance of VRAM MM.
 *
 * Returns:
 * 0 on success, or
 * a negative error code otherwise.
 */
int drm_gem_vram_driver_dumb_create(struct drm_file *file,
				    struct drm_device *dev,
				    struct drm_mode_create_dumb *args)
{
	if (WARN_ONCE(!dev->vram_mm, "VRAM MM not initialized"))
		return -EINVAL;

	return drm_gem_vram_fill_create_dumb(file, dev, &dev->vram_mm->bdev, 0,
					     false, args);
}
EXPORT_SYMBOL(drm_gem_vram_driver_dumb_create);

/**
 * drm_gem_vram_driver_dumb_mmap_offset() - \
	Implements &struct drm_driver.dumb_mmap_offset
 * @file:	DRM file pointer.
 * @dev:	DRM device.
 * @handle:	GEM handle
 * @offset:	Returns the mapping's memory offset on success
 *
 * Returns:
 * 0 on success, or
 * a negative errno code otherwise.
 */
int drm_gem_vram_driver_dumb_mmap_offset(struct drm_file *file,
					 struct drm_device *dev,
					 uint32_t handle, uint64_t *offset)
{
	struct drm_gem_object *gem;
	struct drm_gem_vram_object *gbo;

	gem = drm_gem_object_lookup(file, handle);
	if (!gem)
		return -ENOENT;

	gbo = drm_gem_vram_of_gem(gem);
	*offset = drm_gem_vram_mmap_offset(gbo);

	drm_gem_object_put_unlocked(gem);

	return 0;
}
EXPORT_SYMBOL(drm_gem_vram_driver_dumb_mmap_offset);

/*
 * PRIME helpers
 */

/**
 * drm_gem_vram_object_pin() - \
	Implements &struct drm_gem_object_funcs.pin
 * @gem:	The GEM object to pin
 *
 * Returns:
 * 0 on success, or
 * a negative errno code otherwise.
 */
static int drm_gem_vram_object_pin(struct drm_gem_object *gem)
{
	struct drm_gem_vram_object *gbo = drm_gem_vram_of_gem(gem);

	/* Fbdev console emulation is the use case of these PRIME
	 * helpers. This may involve updating a hardware buffer from
	 * a shadow FB. We pin the buffer to it's current location
	 * (either video RAM or system memory) to prevent it from
	 * being relocated during the update operation. If you require
	 * the buffer to be pinned to VRAM, implement a callback that
	 * sets the flags accordingly.
	 */
	return drm_gem_vram_pin(gbo, 0);
}

/**
 * drm_gem_vram_object_unpin() - \
	Implements &struct drm_gem_object_funcs.unpin
 * @gem:	The GEM object to unpin
 */
static void drm_gem_vram_object_unpin(struct drm_gem_object *gem)
{
	struct drm_gem_vram_object *gbo = drm_gem_vram_of_gem(gem);

	drm_gem_vram_unpin(gbo);
}

/**
 * drm_gem_vram_object_vmap() - \
	Implements &struct drm_gem_object_funcs.vmap
 * @gem:	The GEM object to map
 *
 * Returns:
 * The buffers virtual address on success, or
 * NULL otherwise.
 */
static void *drm_gem_vram_object_vmap(struct drm_gem_object *gem)
{
	struct drm_gem_vram_object *gbo = drm_gem_vram_of_gem(gem);
	int ret;
	void *base;

	ret = ttm_bo_reserve(&gbo->bo, true, false, NULL);
	if (ret)
		return ERR_PTR(ret);

	ret = drm_gem_vram_pin_locked(gbo, 0);
	if (ret)
		goto err_ttm_bo_unreserve;
	base = drm_gem_vram_kmap_locked(gbo, true, NULL);
	if (IS_ERR(base)) {
		ret = PTR_ERR(base);
		goto err_drm_gem_vram_unpin_locked;
	}

	ttm_bo_unreserve(&gbo->bo);

	return base;

err_drm_gem_vram_unpin_locked:
	drm_gem_vram_unpin_locked(gbo);
err_ttm_bo_unreserve:
	ttm_bo_unreserve(&gbo->bo);
	return ERR_PTR(ret);
}

/**
 * drm_gem_vram_object_vunmap() - \
	Implements &struct drm_gem_object_funcs.vunmap
 * @gem:	The GEM object to unmap
 * @vaddr:	The mapping's base address
 */
static void drm_gem_vram_object_vunmap(struct drm_gem_object *gem,
				       void *vaddr)
{
	struct drm_gem_vram_object *gbo = drm_gem_vram_of_gem(gem);
	int ret;

	ret = ttm_bo_reserve(&gbo->bo, false, false, NULL);
	if (WARN_ONCE(ret, "ttm_bo_reserve_failed(): ret=%d\n", ret))
		return;

	drm_gem_vram_kunmap_locked(gbo);
	drm_gem_vram_unpin_locked(gbo);

	ttm_bo_unreserve(&gbo->bo);
}

/*
 * GEM object funcs
 */

static const struct drm_gem_object_funcs drm_gem_vram_object_funcs = {
	.free	= drm_gem_vram_object_free,
	.pin	= drm_gem_vram_object_pin,
	.unpin	= drm_gem_vram_object_unpin,
	.vmap	= drm_gem_vram_object_vmap,
	.vunmap	= drm_gem_vram_object_vunmap,
	.print_info = drm_gem_ttm_print_info,
};

/*
 * VRAM memory manager
 */

/*
 * TTM TT
 */

static void backend_func_destroy(struct ttm_tt *tt)
{
	ttm_tt_fini(tt);
	kfree(tt);
}

static struct ttm_backend_func backend_func = {
	.destroy = backend_func_destroy
};

/*
 * TTM BO device
 */

static struct ttm_tt *bo_driver_ttm_tt_create(struct ttm_buffer_object *bo,
					      uint32_t page_flags)
{
	struct ttm_tt *tt;
	int ret;

	tt = kzalloc(sizeof(*tt), GFP_KERNEL);
	if (!tt)
		return NULL;

	tt->func = &backend_func;

	ret = ttm_tt_init(tt, bo, page_flags);
	if (ret < 0)
		goto err_ttm_tt_init;

	return tt;

err_ttm_tt_init:
	kfree(tt);
	return NULL;
}

static int bo_driver_init_mem_type(struct ttm_bo_device *bdev, uint32_t type,
				   struct ttm_mem_type_manager *man)
{
	switch (type) {
	case TTM_PL_SYSTEM:
		man->flags = TTM_MEMTYPE_FLAG_MAPPABLE;
		man->available_caching = TTM_PL_MASK_CACHING;
		man->default_caching = TTM_PL_FLAG_CACHED;
		break;
	case TTM_PL_VRAM:
		man->func = &ttm_bo_manager_func;
		man->flags = TTM_MEMTYPE_FLAG_FIXED |
			     TTM_MEMTYPE_FLAG_MAPPABLE;
		man->available_caching = TTM_PL_FLAG_UNCACHED |
					 TTM_PL_FLAG_WC;
		man->default_caching = TTM_PL_FLAG_WC;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void bo_driver_evict_flags(struct ttm_buffer_object *bo,
				  struct ttm_placement *placement)
{
	struct drm_gem_vram_object *gbo;

	/* TTM may pass BOs that are not GEM VRAM BOs. */
	if (!drm_is_gem_vram(bo))
		return;

	gbo = drm_gem_vram_of_bo(bo);

	drm_gem_vram_bo_driver_evict_flags(gbo, placement);
}

static int bo_driver_verify_access(struct ttm_buffer_object *bo,
				   struct file *filp)
{
	struct drm_gem_vram_object *gbo;

	/* TTM may pass BOs that are not GEM VRAM BOs. */
	if (!drm_is_gem_vram(bo))
		return -EINVAL;

	gbo = drm_gem_vram_of_bo(bo);

	return drm_gem_vram_bo_driver_verify_access(gbo, filp);
}

static void bo_driver_move_notify(struct ttm_buffer_object *bo,
				  bool evict,
				  struct ttm_mem_reg *new_mem)
{
	struct drm_gem_vram_object *gbo;

	/* TTM may pass BOs that are not GEM VRAM BOs. */
	if (!drm_is_gem_vram(bo))
		return;

	gbo = drm_gem_vram_of_bo(bo);

	drm_gem_vram_bo_driver_move_notify(gbo, evict, new_mem);
}

static int bo_driver_io_mem_reserve(struct ttm_bo_device *bdev,
				    struct ttm_mem_reg *mem)
{
	struct ttm_mem_type_manager *man = bdev->man + mem->mem_type;
	struct drm_vram_mm *vmm = drm_vram_mm_of_bdev(bdev);

	if (!(man->flags & TTM_MEMTYPE_FLAG_MAPPABLE))
		return -EINVAL;

	mem->bus.addr = NULL;
	mem->bus.size = mem->num_pages << PAGE_SHIFT;

	switch (mem->mem_type) {
	case TTM_PL_SYSTEM:	/* nothing to do */
		mem->bus.offset = 0;
		mem->bus.base = 0;
		mem->bus.is_iomem = false;
		break;
	case TTM_PL_VRAM:
		mem->bus.offset = mem->start << PAGE_SHIFT;
		mem->bus.base = vmm->vram_base;
		mem->bus.is_iomem = true;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void bo_driver_io_mem_free(struct ttm_bo_device *bdev,
				  struct ttm_mem_reg *mem)
{ }

static struct ttm_bo_driver bo_driver = {
	.ttm_tt_create = bo_driver_ttm_tt_create,
	.ttm_tt_populate = ttm_pool_populate,
	.ttm_tt_unpopulate = ttm_pool_unpopulate,
	.init_mem_type = bo_driver_init_mem_type,
	.eviction_valuable = ttm_bo_eviction_valuable,
	.evict_flags = bo_driver_evict_flags,
	.verify_access = bo_driver_verify_access,
	.move_notify = bo_driver_move_notify,
	.io_mem_reserve = bo_driver_io_mem_reserve,
	.io_mem_free = bo_driver_io_mem_free,
};

/*
 * struct drm_vram_mm
 */

#if defined(CONFIG_DEBUG_FS)
static int drm_vram_mm_debugfs(struct seq_file *m, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *) m->private;
	struct drm_vram_mm *vmm = node->minor->dev->vram_mm;
	struct drm_mm *mm = vmm->bdev.man[TTM_PL_VRAM].priv;
	struct ttm_bo_global *glob = vmm->bdev.glob;
	struct drm_printer p = drm_seq_file_printer(m);

	spin_lock(&glob->lru_lock);
	drm_mm_print(mm, &p);
	spin_unlock(&glob->lru_lock);
	return 0;
}

static const struct drm_info_list drm_vram_mm_debugfs_list[] = {
	{ "vram-mm", drm_vram_mm_debugfs, 0, NULL },
};
#endif

/**
 * drm_vram_mm_debugfs_init() - Register VRAM MM debugfs file.
 *
 * @minor: drm minor device.
 *
 * Returns:
 * 0 on success, or
 * a negative error code otherwise.
 */
int drm_vram_mm_debugfs_init(struct drm_minor *minor)
{
	int ret = 0;

#if defined(CONFIG_DEBUG_FS)
	ret = drm_debugfs_create_files(drm_vram_mm_debugfs_list,
				       ARRAY_SIZE(drm_vram_mm_debugfs_list),
				       minor->debugfs_root, minor);
#endif
	return ret;
}
EXPORT_SYMBOL(drm_vram_mm_debugfs_init);

static int drm_vram_mm_init(struct drm_vram_mm *vmm, struct drm_device *dev,
			    uint64_t vram_base, size_t vram_size)
{
	int ret;

	vmm->vram_base = vram_base;
	vmm->vram_size = vram_size;

	ret = ttm_bo_device_init(&vmm->bdev, &bo_driver,
				 dev->anon_inode->i_mapping,
				 dev->vma_offset_manager,
				 true);
	if (ret)
		return ret;

	ret = ttm_bo_init_mm(&vmm->bdev, TTM_PL_VRAM, vram_size >> PAGE_SHIFT);
	if (ret)
		return ret;

	return 0;
}

static void drm_vram_mm_cleanup(struct drm_vram_mm *vmm)
{
	ttm_bo_device_release(&vmm->bdev);
}

static int drm_vram_mm_mmap(struct file *filp, struct vm_area_struct *vma,
			    struct drm_vram_mm *vmm)
{
	return ttm_bo_mmap(filp, vma, &vmm->bdev);
}

/*
 * Helpers for integration with struct drm_device
 */

/**
 * drm_vram_helper_alloc_mm - Allocates a device's instance of \
	&struct drm_vram_mm
 * @dev:	the DRM device
 * @vram_base:	the base address of the video memory
 * @vram_size:	the size of the video memory in bytes
 *
 * Returns:
 * The new instance of &struct drm_vram_mm on success, or
 * an ERR_PTR()-encoded errno code otherwise.
 */
struct drm_vram_mm *drm_vram_helper_alloc_mm(
	struct drm_device *dev, uint64_t vram_base, size_t vram_size)
{
	int ret;

	if (WARN_ON(dev->vram_mm))
		return dev->vram_mm;

	dev->vram_mm = kzalloc(sizeof(*dev->vram_mm), GFP_KERNEL);
	if (!dev->vram_mm)
		return ERR_PTR(-ENOMEM);

	ret = drm_vram_mm_init(dev->vram_mm, dev, vram_base, vram_size);
	if (ret)
		goto err_kfree;

	return dev->vram_mm;

err_kfree:
	kfree(dev->vram_mm);
	dev->vram_mm = NULL;
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(drm_vram_helper_alloc_mm);

/**
 * drm_vram_helper_release_mm - Releases a device's instance of \
	&struct drm_vram_mm
 * @dev:	the DRM device
 */
void drm_vram_helper_release_mm(struct drm_device *dev)
{
	if (!dev->vram_mm)
		return;

	drm_vram_mm_cleanup(dev->vram_mm);
	kfree(dev->vram_mm);
	dev->vram_mm = NULL;
}
EXPORT_SYMBOL(drm_vram_helper_release_mm);

/*
 * Helpers for &struct file_operations
 */

/**
 * drm_vram_mm_file_operations_mmap() - \
	Implements &struct file_operations.mmap()
 * @filp:	the mapping's file structure
 * @vma:	the mapping's memory area
 *
 * Returns:
 * 0 on success, or
 * a negative error code otherwise.
 */
int drm_vram_mm_file_operations_mmap(
	struct file *filp, struct vm_area_struct *vma)
{
	struct drm_file *file_priv = filp->private_data;
	struct drm_device *dev = file_priv->minor->dev;

	if (WARN_ONCE(!dev->vram_mm, "VRAM MM not initialized"))
		return -EINVAL;

	return drm_vram_mm_mmap(filp, vma, dev->vram_mm);
}
EXPORT_SYMBOL(drm_vram_mm_file_operations_mmap);
