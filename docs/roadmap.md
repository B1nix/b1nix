# Roadmap

## M0: Boot and Diagnostics

- [x] Build freestanding kernel ELF.
- [x] Boot on x86_64 in QEMU through Multiboot2.
- [x] Add serial and VGA text output.
- [x] Add panic, assert, and early logging.
- [x] Keep architecture-specific code behind narrow interfaces.

## M1: Architecture Layer

- [x] Define `arch_init`.
- [x] Add interrupt descriptor table on x86_64 for CPU exceptions 0-31.
- [x] Add page fault diagnostics with `CR2` reporting.
- [x] Add PIT timer interrupt through PIC IRQ0.
- [x] Add AArch64 QEMU `virt` boot path.
- [x] Keep CPU halt, interrupt control, and context switch arch-local.

## M2: Memory

- [x] Parse boot memory map from Multiboot2.
- [x] Add initial physical frame allocator.
- [x] Add initial x86_64 virtual memory mapping.
- [x] Add initial frame-backed bump kernel heap.
- [x] Add page fault diagnostics.
- [x] Replace bump frame allocator with reusable bitmap/free-list allocator.
- [x] Add unmap/remap helpers.
- [x] Add higher-half kernel mapping. Initial direct-map window done.

## M3: Scheduling

- [x] Add initial kernel threads.
- [x] Add cooperative round-robin scheduler.
- [x] Add x86_64 kernel context switching.
- [x] Add blocking and wakeup queues.
- [x] Add preemptive scheduling from timer ticks.
- [x] Add task sleep/yield APIs backed by timer ticks.

## M4: Userspace

- [x] Add user address spaces. Initial process address-space objects done.
- [x] Add syscall entry. Initial syscall dispatcher ABI done.
- [x] Add initramfs.
- [x] Run `/bin/init`.
- [x] Add basic shell.

## M5: VFS and Devices

- [x] Add file descriptors.
- [x] Add VFS.
- [x] Add devfs.
- [x] Add tmpfs.
- [x] Add tarfs/initramfs.
- [x] Add virtio-blk. Initial stub device done.

## M6: Network

- [x] Add virtio-net. Initial probe/demo device done.
- [x] Add Ethernet frame parsing.
- [x] Add ARP.
- [x] Add IPv4.
- [x] Add ICMP echo.
- [x] Add UDP.
- [x] Add DHCP client.

## M7: Graphics

- [x] Add boot framebuffer path.
- [x] Add graphical console.
- [x] Add input.
- [x] Add basic compositor.
- [x] Explore VirtIO GPU.
