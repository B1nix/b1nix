/*
 * SPDX-License-Identifier: MIT
 *
 * M99 linuxkpi: request_firmware over the VFS.
 * See kernel/include/lkpi/firmware.h.
 */

#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/vfs.h>
#include <lkpi/firmware.h>
#include <string.h>

/* Refuse anything larger than this. amdgpu's biggest single blob is a few MiB;
 * the bound stops a corrupt or hostile file from exhausting the kernel heap. */
#define LKPI_FW_MAX_BYTES (32u * 1024 * 1024)

struct lkpi_firmware {
	struct firmware pub; /* must be first: released via the public pointer */
	u8 *bytes;
	usize size;
};

static int fw_try_path(const char *path, struct lkpi_firmware **out)
{
	struct b1nix_stat st;
	if (vfs_stat(path, &st) < 0)
		return -ENOENT;
	if (st.st_size == 0 || st.st_size > (u64)LKPI_FW_MAX_BYTES)
		return -EINVAL;

	int fd = vfs_open_flags(path, B1NIX_O_RDONLY);
	if (fd < 0)
		return -ENOENT;

	usize size = (usize)st.st_size;
	u8 *bytes = kmalloc(size);
	if (!bytes) {
		vfs_close(fd);
		return -ENOMEM;
	}

	usize got = 0;
	while (got < size) {
		isize n = vfs_read(fd, (char *)bytes + got, size - got);
		if (n <= 0)
			break;
		got += (usize)n;
	}
	vfs_close(fd);

	if (got != size) {
		kfree(bytes);
		return -EIO;
	}

	struct lkpi_firmware *fw = kzalloc(sizeof(*fw));
	if (!fw) {
		kfree(bytes);
		return -ENOMEM;
	}
	fw->bytes = bytes;
	fw->size = size;
	fw->pub.data = bytes;
	fw->pub.size = size;
	*out = fw;
	return 0;
}

static int fw_request(const struct firmware **out_fw, const char *name, int warn)
{
	if (!out_fw || !name || !name[0])
		return -EINVAL;
	*out_fw = 0;

	struct lkpi_firmware *fw = 0;
	int rc;

	if (name[0] == '/') {
		rc = fw_try_path(name, &fw);
	} else {
		static const char *dirs[] = {LKPI_FIRMWARE_PATH_0, LKPI_FIRMWARE_PATH_1};
		char path[256];
		rc = -ENOENT;
		for (usize i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
			usize dl = strlen(dirs[i]);
			usize nl = strlen(name);
			if (dl + nl + 1 > sizeof(path))
				continue;
			memcpy(path, dirs[i], dl);
			memcpy(path + dl, name, nl);
			path[dl + nl] = '\0';
			rc = fw_try_path(path, &fw);
			if (rc == 0)
				break;
		}
	}

	if (rc < 0) {
		if (warn) {
			console_write("firmware: cannot load '");
			console_write(name);
			console_write("'\n");
		}
		return rc;
	}

	*out_fw = &fw->pub;
	return 0;
}

int request_firmware(const struct firmware **fw, const char *name)
{
	return fw_request(fw, name, 1);
}

int firmware_request_nowarn(const struct firmware **fw, const char *name)
{
	return fw_request(fw, name, 0);
}

void release_firmware(const struct firmware *fw)
{
	if (!fw)
		return;
	/* struct firmware is the first member of struct lkpi_firmware, so the
	 * public pointer is the private one. */
	struct lkpi_firmware *priv = (struct lkpi_firmware *)(void *)(usize)fw;
	if (priv->bytes)
		kfree(priv->bytes);
	kfree(priv);
}
