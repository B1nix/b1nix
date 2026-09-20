/* M108: the login shell of the m108user account. It exists so the su smoke can
 * tell "su ran THE ACCOUNT'S shell" from "su ran whatever shell it inherited":
 * only this binary prints the line below, and only /etc/passwd points at it.
 *
 * It is an ELF rather than a #!/bin/sh script on purpose. A script's $0 is the
 * script's own path — the shebang exec replaces the caller's argv[0] with
 * [interpreter, script, args...], on b1nix exactly as on Linux — so a script
 * could never show the login '-' prefix su puts in argv[0]. An ELF keeps the
 * argv[0] its caller chose, which is the second half of the proof.
 */
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	printf("m108shell-argv0=%s\n", argc > 0 && argv[0] ? argv[0] : "");
	fflush(stdout);

	/* Hand the rest of the command line to a real shell, so `su -c CMD` still
	 * runs CMD. argv[0] becomes the shell's own name; everything after it (the
	 * "-c", "CMD" su appended) is passed through untouched. */
	if (argc > 0)
		argv[0] = (char *)"/bin/sh";
	execv("/bin/sh", argv);
	perror("m108shell: exec /bin/sh");
	return 127;
}
