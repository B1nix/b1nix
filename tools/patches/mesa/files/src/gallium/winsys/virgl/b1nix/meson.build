# b1nix VirGL winsys — drives the gallium virgl driver over /dev/virtio-gpu
# (no libdrm). Installed into the Mesa tree by tools/build-mesa.sh.
libvirglb1nix = static_library(
  'virglb1nix',
  ['virgl_b1nix_winsys.c', 'virgl_b1nix_stubs.c'],
  include_directories : [inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium, inc_gallium_aux, inc_gallium_drivers, inc_virtio],
  dependencies : [idep_mesautil, idep_nir_headers],
  gnu_symbol_visibility : 'hidden',
)
