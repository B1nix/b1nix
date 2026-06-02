#include <b1nix/arch.h>
#include <b1nix/arch_x86_64.h>
#include <b1nix/console.h>
#include <b1nix/serial.h>

/* Read the current RBP register */
static u64 read_rbp(void) {
  u64 val;
  __asm__ volatile("movq %%rbp, %0" : "=r"(val));
  return val;
}

/* Read the current RIP by using the return address of this function */
/* __builtin_return_address gives us the caller's return address.
   Used as a fallback when no interrupt frame is available.        */

void panic(const char *message)
{
	console_write("\nKERNEL PANIC: ");
	console_write(message);
	console_write("\n");

	serial_write("\nKERNEL PANIC: ");
	serial_write(message);
	serial_write("\n");

	arch_backtrace(read_rbp(), (u64)__builtin_return_address(0));

	arch_halt();
}
