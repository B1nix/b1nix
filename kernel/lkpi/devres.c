/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * M101 linuxkpi: the device model's managed allocations.
 *
 * Every devm_ allocation is one kheap block: a header followed by the caller's
 * memory. The caller only ever sees the memory, so devm_kfree steps back one
 * header to find the node — which is why the header must be the last thing
 * before the payload and why nothing may be inserted between them.
 *
 * The list is newest-first and released in that order. Reverse order is not a
 * detail: a driver allocates a ring, then a context that points into it, and
 * releasing the ring first would hand the context's release a dangling pointer.
 */

#include <linux/errno.h>
#include <linux/device.h>

struct devres_action {
	void (*action)(void *);
	void *data;
};

static void *devres_payload(struct devres_node *node)
{
	return (void *)(node + 1);
}

static struct devres_node *devres_node_of(void *payload)
{
	return (struct devres_node *)payload - 1;
}

void device_set_name(struct device *dev, const char *name)
{
	if (dev)
		dev->init_name = name;
}

void device_initialize(struct device *dev)
{
	if (!dev)
		return;
	const char *name = dev->init_name;
	dev->parent = 0;
	dev->driver_data = 0;
	dev->devres = 0;
	lkpi_mutex_init(&dev->devres_lock);
	lkpi_device_init(&dev->lk, name, 0);
}

static void *devres_alloc(struct device *dev, usize size, gfp_t flags,
                          devres_release_t release)
{
	if (!dev || size == 0)
		return 0;
	struct devres_node *node = (struct devres_node *)lkpi_kmalloc(
		sizeof(struct devres_node) + size, flags);
	if (!node)
		return 0;
	node->release = release;
	node->size = size;

	lkpi_mutex_lock(&dev->devres_lock);
	node->next = dev->devres;
	dev->devres = node;
	lkpi_mutex_unlock(&dev->devres_lock);

	return devres_payload(node);
}

void *devm_kmalloc(struct device *dev, usize size, gfp_t flags)
{
	return devres_alloc(dev, size, flags, 0);
}

void *devm_kzalloc(struct device *dev, usize size, gfp_t flags)
{
	void *p = devres_alloc(dev, size, flags | __GFP_ZERO, 0);
	if (p) {
		/* lkpi_kmalloc only honours __GFP_ZERO for the whole block, and the
		 * block here starts with our header, so zero the payload explicitly
		 * rather than assume. */
		char *c = (char *)p;
		for (usize i = 0; i < size; i++)
			c[i] = 0;
	}
	return p;
}

void *devm_kcalloc(struct device *dev, usize n, usize size, gfp_t flags)
{
	if (n == 0 || size == 0)
		return 0;
	/* The same overflow check kcalloc makes: a count computed from userspace
	 * must not wrap this into a small allocation. */
	if (n > (usize)-1 / size)
		return 0;
	return devm_kzalloc(dev, n * size, flags);
}

void devm_kfree(struct device *dev, void *ptr)
{
	if (!dev || !ptr)
		return;
	struct devres_node *target = devres_node_of(ptr);

	lkpi_mutex_lock(&dev->devres_lock);
	struct devres_node **link = &dev->devres;
	while (*link && *link != target)
		link = &(*link)->next;
	if (*link == target)
		*link = target->next;
	else
		target = 0; /* not ours: refuse rather than free a stranger's memory */
	lkpi_mutex_unlock(&dev->devres_lock);

	if (target) {
		if (target->release)
			target->release(dev, devres_payload(target));
		lkpi_kfree(target);
	}
}

static void devres_action_release(struct device *dev, void *res)
{
	struct devres_action *a = (struct devres_action *)res;
	(void)dev;
	if (a->action)
		a->action(a->data);
}

int devm_add_action(struct device *dev, void (*action)(void *), void *data)
{
	if (!dev || !action)
		return -EINVAL;
	struct devres_action *a = (struct devres_action *)devres_alloc(
		dev, sizeof(*a), GFP_KERNEL, devres_action_release);
	if (!a)
		return -ENOMEM;
	a->action = action;
	a->data = data;
	return 0;
}

int devm_add_action_or_reset(struct device *dev, void (*action)(void *),
                             void *data)
{
	int err = devm_add_action(dev, action, data);
	if (err && action) {
		/* Could not register the cleanup, so run it now: the caller is about
		 * to fail out believing the resource is accounted for. */
		action(data);
	}
	return err;
}

void devres_release_all(struct device *dev)
{
	if (!dev)
		return;

	lkpi_mutex_lock(&dev->devres_lock);
	struct devres_node *node = dev->devres;
	dev->devres = 0;
	lkpi_mutex_unlock(&dev->devres_lock);

	/* Newest first: a later allocation may point into an earlier one. */
	while (node) {
		struct devres_node *next = node->next;
		if (node->release)
			node->release(dev, devres_payload(node));
		lkpi_kfree(node);
		node = next;
	}
}

usize devres_count(struct device *dev)
{
	if (!dev)
		return 0;
	lkpi_mutex_lock(&dev->devres_lock);
	usize n = 0;
	for (struct devres_node *node = dev->devres; node; node = node->next)
		n++;
	lkpi_mutex_unlock(&dev->devres_lock);
	return n;
}
