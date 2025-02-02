// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2016, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#include <linux/dma-buf.h>
#include <linux/rbtree.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <asm/barrier.h>

struct msm_iommu_meta {
	struct rb_node node;
	struct list_head maps;
	atomic_t refcount;
	rwlock_t lock;
	void *buffer;
};

struct msm_iommu_map {
	struct device *dev;
	struct msm_iommu_meta *meta;
	struct list_head lnode;
	struct scatterlist sgl;
	enum dma_data_direction dir;
	unsigned int nents;
	atomic_t refcount;
};

static struct rb_root iommu_root;
static DEFINE_RWLOCK(rb_tree_lock);

static void msm_iommu_meta_add(struct msm_iommu_meta *new_meta)
{
	struct rb_root *root = &iommu_root;
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct msm_iommu_meta *meta;

	write_lock(&rb_tree_lock);
	while (*p) {
		parent = *p;
		meta = rb_entry(parent, typeof(*meta), node);
		if (new_meta->buffer < meta->buffer)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}
	rb_link_node(&new_meta->node, parent, p);
	rb_insert_color(&new_meta->node, root);
	write_unlock(&rb_tree_lock);
}

static struct msm_iommu_meta *msm_iommu_meta_lookup(void *buffer)
{
	struct rb_root *root = &iommu_root;
	struct rb_node **p = &root->rb_node;
	struct msm_iommu_meta *meta;

	read_lock(&rb_tree_lock);
	while (*p) {
		meta = rb_entry(*p, typeof(*meta), node);
		if (buffer < meta->buffer) {
			p = &(*p)->rb_left;
		} else if (buffer > meta->buffer) {
			p = &(*p)->rb_right;
		} else {
			read_unlock(&rb_tree_lock);
			return meta;
		}
	}
	read_unlock(&rb_tree_lock);

	return NULL;
}

static void msm_iommu_map_add(struct msm_iommu_meta *meta,
			      struct msm_iommu_map *map)
{
	write_lock(&meta->lock);
	list_add(&map->lnode, &meta->maps);
	write_unlock(&meta->lock);
}

static struct msm_iommu_map *msm_iommu_map_lookup(struct msm_iommu_meta *meta,
						  struct device *dev)
{
	struct msm_iommu_map *map;

	list_for_each_entry(map, &meta->maps, lnode) {
		if (map->dev == dev)
			return map;
	}

	return NULL;
}

static void msm_iommu_meta_put(struct msm_iommu_meta *meta)
{
	struct rb_root *root = &iommu_root;

	if (atomic_dec_return(&meta->refcount))
		return;

	write_lock(&rb_tree_lock);
	rb_erase(&meta->node, root);
	write_unlock(&rb_tree_lock);

	kfree(meta);
}

int msm_dma_map_sg_attrs(struct device *dev, struct scatterlist *sg, int nents,
			 enum dma_data_direction dir, struct dma_buf *dma_buf,
			 struct dma_attrs *attrs)
{
	int not_lazy = dma_get_attr(DMA_ATTR_NO_DELAYED_UNMAP, attrs);
	struct msm_iommu_meta *meta;
	struct msm_iommu_map *map;

	meta = msm_iommu_meta_lookup(dma_buf->priv);
	if (meta) {
		atomic_inc(&meta->refcount);
		read_lock(&meta->lock);
		map = msm_iommu_map_lookup(meta, dev);
		if (map)
			atomic_inc(&map->refcount);
		read_unlock(&meta->lock);
	} else {
		while (!(meta = kmalloc(sizeof(*meta), GFP_KERNEL)));

		*meta = (typeof(*meta)){
			.buffer = dma_buf->priv,
			.refcount = ATOMIC_INIT(2 - not_lazy),
			.lock = __RW_LOCK_UNLOCKED(&meta->lock),
			.maps = LIST_HEAD_INIT(meta->maps)
		};

		msm_iommu_meta_add(meta);
		map = NULL;
	}

	if (map) {
		sg->dma_address = map->sgl.dma_address;
		sg->dma_length = map->sgl.dma_length;
		if (is_device_dma_coherent(dev))
			dmb(ish);
	} else {
		while (!(map = kmalloc(sizeof(*map), GFP_KERNEL)));
		while (!dma_map_sg_attrs(dev, sg, nents, dir, attrs));

		*map = (typeof(*map)){
			.dev = dev,
			.dir = dir,
			.meta = meta,
			.nents = nents,
			.lnode = LIST_HEAD_INIT(map->lnode),
			.refcount = ATOMIC_INIT(2 - not_lazy),
			.sgl = {
				.dma_address = sg->dma_address,
				.dma_length = sg->dma_length,
				.page_link = sg->page_link,
				.offset = sg->offset,
				.length = sg->length
			}
		};

		msm_iommu_map_add(meta, map);
	}

	return nents;
}

void msm_dma_unmap_sg(struct device *dev, struct scatterlist *sgl, int nents,
		      enum dma_data_direction dir, struct dma_buf *dma_buf)
{
	struct msm_iommu_meta *meta;
	struct msm_iommu_map *map;
	bool free_map;

	meta = msm_iommu_meta_lookup(dma_buf->priv);
	if (!meta)
		return;

	write_lock(&meta->lock);
	map = msm_iommu_map_lookup(meta, dev);
	if (!map) {
		write_unlock(&meta->lock);
		return;
	}

	free_map = atomic_dec_and_test(&map->refcount);
	if (free_map)
		list_del(&map->lnode);
	write_unlock(&meta->lock);

	if (free_map) {
		dma_unmap_sg(map->dev, &map->sgl, map->nents, map->dir);
		kfree(map);
	}

	msm_iommu_meta_put(meta);
}

int msm_dma_unmap_all_for_dev(struct device *dev)
{
	struct msm_iommu_map *map, *map_next;
	struct rb_root *root = &iommu_root;
	struct msm_iommu_meta *meta;
	struct rb_node *meta_node;
	LIST_HEAD(unmap_list);
	int ret = 0;

	read_lock(&rb_tree_lock);
	meta_node = rb_first(root);
	while (meta_node) {
		meta = rb_entry(meta_node, typeof(*meta), node);
		write_lock(&meta->lock);
		list_for_each_entry_safe(map, map_next, &meta->maps, lnode) {
			if (map->dev != dev)
				continue;

			/* Do the actual unmapping outside of the locks */
			if (atomic_dec_and_test(&map->refcount))
				list_move_tail(&map->lnode, &unmap_list);
			else
				ret = -EINVAL;
		}
		write_unlock(&meta->lock);
		meta_node = rb_next(meta_node);
	}
	read_unlock(&rb_tree_lock);

	list_for_each_entry_safe(map, map_next, &unmap_list, lnode) {
		dma_unmap_sg(map->dev, &map->sgl, map->nents, map->dir);
		kfree(map);
	}

	return ret;
}

/* Only to be called by ION code when a buffer is freed */
void msm_dma_buf_freed(void *buffer)
{
	struct msm_iommu_map *map, *map_next;
	struct msm_iommu_meta *meta;
	LIST_HEAD(unmap_list);

	meta = msm_iommu_meta_lookup(buffer);
	if (!meta)
		return;

	write_lock(&meta->lock);
	list_for_each_entry_safe(map, map_next, &meta->maps, lnode) {
		/* Do the actual unmapping outside of the lock */
		if (atomic_dec_and_test(&map->refcount))
			list_move_tail(&map->lnode, &unmap_list);
		else
			list_del_init(&map->lnode);
	}
	write_unlock(&meta->lock);

	list_for_each_entry_safe(map, map_next, &unmap_list, lnode) {
		dma_unmap_sg(map->dev, &map->sgl, map->nents, map->dir);
		kfree(map);
	}

	msm_iommu_meta_put(meta);
}
