/* B1NIX Userspace — hello.c
 *
 * First userspace program compiled by the external cross-toolchain
 * and loaded from the VFS by the ELF loader.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	printf("Hello from B1NIX userspace!\n");
	printf("argc = %d\n", argc);
	for (int i = 0; i < argc; i++) {
		printf("  argv[%d] = %s\n", i, argv[i]);
	}
	return 0;
}
