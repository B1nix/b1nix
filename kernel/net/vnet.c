/* The parts of the virtual-device model that belong to no single kind (M109):
 * destroying a device, and moving one under a master. Both have to dispatch on
 * what the device is, and neither belongs inside the bridge or the bond.
 */

#include <b1nix/vnet.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/errno.h>

int vnet_destroy(struct netdev *nd)
{
	if (!nd)
		return -EINVAL;
	if (!netdev_is_virtual(nd) || !nd->destroy)
		return -EOPNOTSUPP;
	if (nd == netdev_active())
		return -EBUSY;
	/* Leave the bridge or bond first: unregistering only clears the departing
	 * device's own pointers, and a master left holding a pointer to a
	 * destroyed port would forward frames into freed state. */
	if (nd->master)
		(void)vnet_set_master(nd, 0);
	nd->destroy(nd);
	return 0;
}

int vnet_set_master(struct netdev *dev, struct netdev *master)
{
	if (!dev)
		return -EINVAL;
	/* Releasing: whoever owns it takes it back. */
	if (!master) {
		struct netdev *old = dev->master;
		if (!old)
			return 0;
		if (old->kind == NETDEV_KIND_BRIDGE)
			return bridge_del_port(old, dev);
		if (old->kind == NETDEV_KIND_BOND)
			return bond_release(old, dev);
		return -EOPNOTSUPP;
	}
	/* Enslaving the interface that holds the address would take the stack's
	 * L3 configuration with it, silently. An operator who means to do that
	 * takes the interface down first. */
	if (dev == netdev_active())
		return -EBUSY;
	if (master->kind == NETDEV_KIND_BRIDGE)
		return bridge_add_port(master, dev);
	if (master->kind == NETDEV_KIND_BOND)
		return bond_enslave(master, dev);
	return -EOPNOTSUPP;
}
