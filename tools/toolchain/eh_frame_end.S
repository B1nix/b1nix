/* A single zero word terminating a shared object's .eh_frame.
 *
 * libgcc's classic DWARF unwinder (the only FDE lookup path this toolchain's
 * libgcc_s.so was built with) finds frames through __register_frame, which scans
 * CIE/FDE records starting at the .eh_frame begin until it hits a record whose
 * 4-byte length field is zero. __b1nix_run_dso_init registers every loaded shared
 * object's .eh_frame this way so C++ exceptions can unwind across the
 * libstdc++.so.6 / executable boundary. Normally crtendS.o's __FRAME_END__ supplies
 * this terminator, but the b1nix shared libstdc++ is linked without the crt files
 * (this newlib target ships no crti.o/crtn.o). Linked LAST, this object's zero word
 * lands after every real CIE/FDE so the scan stops cleanly. */
	.section .eh_frame,"a",@progbits
	.long 0
