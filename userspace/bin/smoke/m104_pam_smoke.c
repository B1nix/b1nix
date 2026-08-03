/* M104 OpenPAM smoke: exercises the REAL, dlopen-capable OpenPAM library
 * (build/<arch>/ports/openpam/install/lib/libpam.so.2, built by
 * tools/ports/build-openpam.sh) against b1nix's own pam_unix.so service
 * module (tools/ports/openpam-pam_unix.c), which authenticates against the
 * real /etc/shadow via musl's crypt(3) "$6$" scheme — the same check
 * BusyBox's su/passwd applets perform (M108).
 *
 * This binary links libpam.so.2 as a normal DT_NEEDED shared library (see
 * userspace/Makefile's m104_pam_smoke rule) and calls the genuine OpenPAM
 * API: pam_start() -> pam_authenticate() -> pam_acct_mgmt() -> pam_end().
 * libpam.so.2 in turn dlopen()s /lib/security/pam_unix.so at pam_start()
 * time per /etc/pam.d/m104-pam-smoke (staged by root-image) — so a pass here
 * proves three real, independently-checkable things at once:
 *   1. libpam.so.2 loads and its dynamic symbols resolve (ld-musl actually
 *      linked it, not a stub).
 *   2. libpam.so.2 successfully dlopen()s pam_unix.so per the policy file —
 *      OpenPAM's own module-loading path, not this test hardcoding a call.
 *   3. pam_unix.so's crypt(3)-against-/etc/shadow check is exercised for
 *      real: a correct password authenticates, a wrong one does not.
 *
 * No fake passes: every marker below only fires after the corresponding
 * openpam call actually returned the value the marker claims.
 */

#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>

#include <security/pam_appl.h>
#include <security/pam_constants.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

#define PAM_SERVICE  "m104-pam-smoke"
#define TEST_USER    "pamtest"
#define TEST_PASS_OK "PamSmoke123!"
#define TEST_PASS_BAD "not the password"

/* Non-interactive PAM conversation function: PAM_PROMPT_ECHO_OFF is answered
 * with the password baked into appdata_ptr by main() below — this is the
 * standard OpenPAM pattern for a scripted/non-tty caller (see
 * openpam_ttyconv.c's real interactive version, which this deliberately
 * does NOT reuse, for the same reason a test harness doesn't want a tty). */
static int
smoke_conv(int n, const struct pam_message **msg,
    struct pam_response **resp, void *data)
{
	struct pam_response *r;
	const char *password = (const char *)data;
	int i;

	if (n <= 0 || n > PAM_MAX_NUM_MSG)
		return (PAM_CONV_ERR);
	r = calloc((size_t)n, sizeof(*r));
	if (r == NULL)
		return (PAM_BUF_ERR);
	for (i = 0; i < n; i++) {
		switch (msg[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
		case PAM_PROMPT_ECHO_ON:
			r[i].resp = strdup(password);
			r[i].resp_retcode = 0;
			break;
		default:
			r[i].resp = NULL;
			r[i].resp_retcode = 0;
			break;
		}
	}
	*resp = r;
	return (PAM_SUCCESS);
}

/* Runs one full pam_start/pam_authenticate/pam_acct_mgmt/pam_end cycle for
 * `user` with `password`. Returns the PAM_* code pam_authenticate() gave. */
static int
run_pam_cycle(const char *user, const char *password, int *acct_out)
{
	pam_handle_t *pamh = NULL;
	struct pam_conv conv;
	int r, auth_r;

	conv.conv = smoke_conv;
	conv.appdata_ptr = (void *)(uintptr_t)password;

	r = pam_start(PAM_SERVICE, user, &conv, &pamh);
	if (r != PAM_SUCCESS) {
		emit("M104-PAM: fail pam_start\n");
		return (r);
	}

	auth_r = pam_authenticate(pamh, 0);
	*acct_out = pam_acct_mgmt(pamh, 0);

	pam_end(pamh, auth_r);
	return (auth_r);
}

int
main(void)
{
	int r, acct_r;

	emit("M104-PAM: start\n");

	/* Phase 1: libpam.so.2 loaded and linked at all (DT_NEEDED resolved by
	 * ld-musl) — if we got this far past _start, it did. Confirm the API
	 * itself is callable before testing behaviour. */
	if (pam_strerror(NULL, PAM_SUCCESS) == NULL) {
		emit("M104-PAM: fail pam_strerror\n");
		return (1);
	}
	emit("M104-PAM: ok libpam-linked\n");

	/* Phase 2: correct password -> pam_unix.so must authenticate the real
	 * pamtest /etc/shadow entry (SHA-512 "$6$" hash of "PamSmoke123!",
	 * staged by userspace/Makefile's install-headers-libs target). */
	r = run_pam_cycle(TEST_USER, TEST_PASS_OK, &acct_r);
	if (r != PAM_SUCCESS) {
		emit("M104-PAM: fail auth-correct-password (");
		emit(pam_strerror(NULL, r));
		emit(")\n");
		return (1);
	}
	emit("M104-PAM: ok auth-correct-password\n");

	if (acct_r != PAM_SUCCESS) {
		emit("M104-PAM: fail acct-mgmt (");
		emit(pam_strerror(NULL, acct_r));
		emit(")\n");
		return (1);
	}
	emit("M104-PAM: ok acct-mgmt\n");

	/* Phase 3: wrong password -> pam_unix.so's crypt(3) comparison MUST
	 * fail. A module that always returns PAM_SUCCESS (a "fake pass") would
	 * be caught right here. */
	r = run_pam_cycle(TEST_USER, TEST_PASS_BAD, &acct_r);
	if (r == PAM_SUCCESS) {
		emit("M104-PAM: fail auth-wrong-password-should-have-failed\n");
		return (1);
	}
	if (r != PAM_AUTH_ERR) {
		emit("M104-PAM: fail auth-wrong-password-unexpected-code (");
		emit(pam_strerror(NULL, r));
		emit(")\n");
		return (1);
	}
	emit("M104-PAM: ok auth-wrong-password-rejected\n");

	/* Phase 4: unknown user -> PAM_USER_UNKNOWN (not PAM_AUTH_ERR — checks
	 * pam_unix.so distinguishes "no such account" from "bad password", the
	 * same distinction su(1)/login make). */
	r = run_pam_cycle("no_such_user_zzz", "irrelevant", &acct_r);
	if (r != PAM_USER_UNKNOWN) {
		emit("M104-PAM: fail unknown-user-unexpected-code (");
		emit(pam_strerror(NULL, r));
		emit(")\n");
		return (1);
	}
	emit("M104-PAM: ok unknown-user-rejected\n");

	emit("M104-PAM: done\n");
	return (0);
}
