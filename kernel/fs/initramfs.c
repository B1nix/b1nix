#include <string.h>
#include <b1nix/console.h>
#include <b1nix/initramfs.h>

static const struct initramfs_file files[] = {
	{ "/bin/init", "builtin:init\n", 13, INITRAMFS_EXECUTABLE },
	{ "/bin/sh", "builtin:sh\n", 11, INITRAMFS_EXECUTABLE },
	{ "/etc/motd", "welcome to b1nix m4\n", 23, 0 },
	{ "/README", "initramfs is alive\n", 20, 0 },
};

void initramfs_init(void)
{
	console_write("initramfs: files 0x");
	console_write_hex64(initramfs_count());
	console_write("\n");
}

const struct initramfs_file *initramfs_find(const char *path)
{
	for (usize i = 0; i < initramfs_count(); i++) {
		if (strcmp(files[i].path, path) == 0) {
			return &files[i];
		}
	}

	return 0;
}

const struct initramfs_file *initramfs_get(usize index)
{
	if (index >= initramfs_count()) {
		return 0;
	}

	return &files[index];
}

usize initramfs_count(void)
{
	return sizeof(files) / sizeof(files[0]);
}
