/*
 * Sound mixer layer — M79: Audio Stack
 *
 * A registry of registered sound devices plus the generic OSS-style mixer
 * ioctl dispatcher. Drivers may expose hardware volume hooks through
 * struct sound_device.set_volume/get_volume; drivers without a hardware
 * volume still track a software mixer state in the struct, so the ioctl
 * interface behaves identically across all sound devices.
 *
 * The ioctl numbers are the classic OSS SOUND_MIXER_READ_* / WRITE_* family
 * (type 'M', Linux _IOC layout), served through the /dev/dspN inode ioctl
 * hook (vfs_ioctl -> inode->ioctl_cb).
 */
#include <b1nix/sound.h>
#include <b1nix/mixer.h>
#include <b1nix/syscall.h>
#include <b1nix/errno.h>

#define SOUND_MAX_DEVICES 8

static struct sound_device *sound_devices[SOUND_MAX_DEVICES];
static int sound_device_count;

void sound_register(struct sound_device *dev) {
	if (sound_device_count < SOUND_MAX_DEVICES)
		sound_devices[sound_device_count++] = dev;
}

void sound_unregister(struct sound_device *dev) {
	for (int i = 0; i < sound_device_count; i++) {
		if (sound_devices[i] != dev)
			continue;
		for (int j = i; j + 1 < sound_device_count; j++)
			sound_devices[j] = sound_devices[j + 1];
		sound_devices[--sound_device_count] = 0;
		return;
	}
}

/* ── Loadable sound-driver hooks (M95) ──────────────────────────────────
 * The HDA controller driver is a module (hda.ko). The two call sites that
 * used to name it directly — vfs_repopulate_after_root_mount(), which has to
 * re-create /dev/dsp on the real root, and the test-mode self-test driver in
 * kernel_main() — now go through this registry, so a kernel booted without
 * hda.ko simply does neither. */
static const struct sound_driver_hooks *sound_hooks[SOUND_MAX_DEVICES];
static int sound_hook_count;

void sound_register_hooks(const struct sound_driver_hooks *hooks) {
	if (!hooks || sound_hook_count >= SOUND_MAX_DEVICES)
		return;
	for (int i = 0; i < sound_hook_count; i++)
		if (sound_hooks[i] == hooks)
			return;
	sound_hooks[sound_hook_count++] = hooks;
}

void sound_unregister_hooks(const struct sound_driver_hooks *hooks) {
	for (int i = 0; i < sound_hook_count; i++) {
		if (sound_hooks[i] != hooks)
			continue;
		for (int j = i; j + 1 < sound_hook_count; j++)
			sound_hooks[j] = sound_hooks[j + 1];
		sound_hooks[--sound_hook_count] = 0;
		return;
	}
}

void sound_module_dev_init(void) {
	for (int i = 0; i < sound_hook_count; i++)
		if (sound_hooks[i]->dev_init)
			sound_hooks[i]->dev_init();
}

void sound_module_selftest(void) {
	for (int i = 0; i < sound_hook_count; i++)
		if (sound_hooks[i]->selftest)
			sound_hooks[i]->selftest();
}

struct sound_device *sound_get_default(void) {
	if (sound_device_count > 0)
		return sound_devices[0];
	return 0;
}

/* OSS ioctl direction bits (Linux _IOC). */
#define MIXER_IOC_READ  2u
#define MIXER_IOC_WRITE 1u

int sound_mixer_ioctl(struct sound_device *dev, u64 request, void *arg) {
	if (((request >> 8) & 0xFF) != 'M')
		return -ENOTTY;

	if (!dev)
		dev = sound_get_default();
	if (!dev || !dev->ready || !dev->ready(dev))
		return -ENXIO;

	u32 nr = (u32)(request & 0xFF);
	u32 dir = (u32)(request >> 30);

	switch (nr) {
	case SOUND_MIXER_DEVMASK:
	case SOUND_MIXER_RECMASK:
	case SOUND_MIXER_RECSRC:
	case SOUND_MIXER_STEREODEVS: {
		u32 val;
		switch (nr) {
		case SOUND_MIXER_DEVMASK:
			val = (1u << SOUND_MIXER_VOLUME) | (1u << SOUND_MIXER_PCM) |
			      (1u << SOUND_MIXER_MUTE);
			break;
		case SOUND_MIXER_RECMASK:
		case SOUND_MIXER_RECSRC:
			val = 0; /* no capture sources */
			break;
		default:
			val = (1u << SOUND_MIXER_VOLUME) | (1u << SOUND_MIXER_PCM);
			break;
		}
		if (!arg || syscall_copyout(arg, &val, sizeof(val)) < 0)
			return -EFAULT;
		return 0;
	}

	case SOUND_MIXER_VOLUME:
	case SOUND_MIXER_PCM:
	case SOUND_MIXER_MUTE: {
		if (dir == MIXER_IOC_READ) {
			int l, r, m;
			if (dev->get_volume) {
				int rc = dev->get_volume(dev, &l, &r, &m);
				if (rc < 0)
					return rc;
			} else {
				l = dev->vol_left;
				r = dev->vol_right;
				m = dev->muted;
			}
			int val;
			if (nr == SOUND_MIXER_MUTE)
				val = m ? 1 : 0;
			else
				val = (l & 0xFF) | ((r & 0xFF) << 8);
			if (!arg || syscall_copyout(arg, &val, sizeof(val)) < 0)
				return -EFAULT;
			return 0;
		}

		/* Write (SOUND_MIXER_WRITE_*) */
		if (!arg)
			return -EINVAL;
		int val;
		if (syscall_copyin(&val, arg, sizeof(val)) < 0)
			return -EFAULT;

		if (nr == SOUND_MIXER_MUTE) {
			dev->muted = val & 1;
			if (dev->set_volume)
				return dev->set_volume(dev, -1, -1, dev->muted);
			return 0;
		}

		int l = val & 0xFF;
		int r = (val >> 8) & 0xFF;
		if (l > 100) l = 100;
		if (r > 100) r = 100;
		dev->vol_left = l;
		dev->vol_right = r;
		if (dev->set_volume) {
			int rc = dev->set_volume(dev, l, r, dev->muted);
			if (rc < 0)
				return rc;
		}
		/* OSS convention: echo the accepted value back to the caller. */
		val = (l & 0xFF) | ((r & 0xFF) << 8);
		if (syscall_copyout(arg, &val, sizeof(val)) < 0)
			return -EFAULT;
		return 0;
	}

	default:
		return -ENOTTY;
	}
}
