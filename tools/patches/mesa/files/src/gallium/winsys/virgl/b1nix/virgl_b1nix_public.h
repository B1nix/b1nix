/* b1nix VirGL winsys public entry. */
#ifndef VIRGL_B1NIX_PUBLIC_H
#define VIRGL_B1NIX_PUBLIC_H

struct pipe_screen;

/* Open /dev/virtio-gpu and return a gallium pipe_screen backed by the host GPU
 * via the b1nix VirGL transport, or NULL if no virgl host device is present. */
struct pipe_screen *virgl_b1nix_screen_create(void);

#endif
