/*
 * b1nix stubs for the few Mesa symbols the gallium virgl driver references but
 * that the minimal b1nix Mesa build does not provide: the video layer (vl_*,
 * b1nix has no GPU video accel) and DRI config (driconf). The clear/draw render
 * path never calls these — they exist only to satisfy the static link.
 *
 * The linker matches by name, not signature; these no-op definitions provide
 * the symbols. Local prototypes keep -Werror=missing-prototypes happy without
 * pulling the real (struct-heavy) headers.
 */

void driParseConfigFiles(void);
unsigned char driQueryOptionb(void);
int driQueryOptioni(void);
void *vl_video_buffer_create(void);
void vl_video_buffer_destroy(void);
void *vl_video_buffer_get_associated_data(void);
void vl_video_buffer_set_associated_data(void);
int vl_video_buffer_is_format_supported(void);

void driParseConfigFiles(void) {}
unsigned char driQueryOptionb(void) { return 0; }
int driQueryOptioni(void) { return 0; }
void *vl_video_buffer_create(void) { return (void *)0; }
void vl_video_buffer_destroy(void) {}
void *vl_video_buffer_get_associated_data(void) { return (void *)0; }
void vl_video_buffer_set_associated_data(void) {}
int vl_video_buffer_is_format_supported(void) { return 0; }
