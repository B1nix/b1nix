/*
 * pam_unix.c — b1nix's pam_unix.so: a real OpenPAM service module that
 * authenticates against /etc/shadow using musl's crypt(3) (the SHA-512
 * "$6$" scheme already used by su(1)/login/dropbear). This is the same
 * shadow-line parsing and crypt() comparison su.c performs directly; the
 * point of this module is that any PAM-aware program (dropbear included)
 * gets that check via the standard pam_sm_authenticate()/pam_sm_acct_mgmt()
 * entry points dlopen'd at runtime by libpam, instead of hardcoding it.
 *
 * Exposed to OpenPAM as /lib/security/pam_unix.so (see
 * tools/ports/build-openpam.sh for install location, matching
 * OPENPAM_MODULES_DIRECTORY baked into openpam-config.h).
 */

#define PAM_SM_AUTH
#define PAM_SM_ACCOUNT
#define PAM_SM_SESSION

#include <security/pam_modules.h>
#include <security/pam_appl.h>
#include <security/openpam.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <crypt.h>
#include <unistd.h>

/* Look up the crypt(3) hash field for `username` in /etc/shadow. Mirrors
 * userspace/bin/su.c's get_shadow_hash() — same file, same format, same
 * trust boundary (both run privileged enough to read /etc/shadow). */
static int
pam_unix_shadow_hash(const char *username, char *hash_out, size_t max_len)
{
	FILE *f;
	char line[256];
	int found = 0;

	f = fopen("/etc/shadow", "r");
	if (f == NULL)
		return (-1);
	while (fgets(line, sizeof(line), f) != NULL) {
		char *colon1, *colon2, *hash;
		size_t len;

		colon1 = strchr(line, ':');
		if (colon1 == NULL)
			continue;
		*colon1 = '\0';
		if (strcmp(line, username) != 0)
			continue;
		hash = colon1 + 1;
		colon2 = strchr(hash, ':');
		if (colon2 != NULL)
			*colon2 = '\0';
		len = strlen(hash);
		while (len > 0 && (hash[len - 1] == '\n' || hash[len - 1] == '\r')) {
			hash[--len] = '\0';
		}
		strncpy(hash_out, hash, max_len - 1);
		hash_out[max_len - 1] = '\0';
		found = 1;
		break;
	}
	fclose(f);
	return (found ? 0 : -1);
}

PAM_EXTERN int
pam_sm_authenticate(pam_handle_t *pamh, int flags __attribute__((unused)),
    int argc __attribute__((unused)), const char *argv[] __attribute__((unused)))
{
	const char *user;
	const char *pass;
	char hash[256];
	char *encrypted;
	int r;

	r = pam_get_user(pamh, &user, NULL);
	if (r != PAM_SUCCESS)
		return (r);
	if (user == NULL || user[0] == '\0')
		return (PAM_USER_UNKNOWN);

	if (getpwnam(user) == NULL)
		return (PAM_USER_UNKNOWN);

	if (pam_unix_shadow_hash(user, hash, sizeof(hash)) < 0)
		return (PAM_AUTH_ERR);

	/* An empty or locked ("!", "*") hash never authenticates. */
	if (hash[0] == '\0' || hash[0] == '!' || hash[0] == '*')
		return (PAM_AUTH_ERR);

	r = pam_get_authtok(pamh, PAM_AUTHTOK, &pass, "Password: ");
	if (r != PAM_SUCCESS)
		return (r);
	if (pass == NULL)
		return (PAM_AUTH_ERR);

	encrypted = crypt(pass, hash);
	if (encrypted == NULL || strcmp(encrypted, hash) != 0)
		return (PAM_AUTH_ERR);

	return (PAM_SUCCESS);
}

PAM_EXTERN int
pam_sm_setcred(pam_handle_t *pamh __attribute__((unused)),
    int flags __attribute__((unused)), int argc __attribute__((unused)),
    const char *argv[] __attribute__((unused)))
{

	return (PAM_SUCCESS);
}

PAM_EXTERN int
pam_sm_acct_mgmt(pam_handle_t *pamh, int flags __attribute__((unused)),
    int argc __attribute__((unused)), const char *argv[] __attribute__((unused)))
{
	const char *user;
	char hash[256];
	int r;

	r = pam_get_user(pamh, &user, NULL);
	if (r != PAM_SUCCESS)
		return (r);
	if (user == NULL || getpwnam(user) == NULL)
		return (PAM_USER_UNKNOWN);

	/* Reject accounts explicitly locked in /etc/shadow ("!"/"*" hash
	 * prefix), matching the same convention pam_sm_authenticate() above
	 * enforces — an account-management pass that ignored this would let
	 * a locked account slip through session setup even though auth
	 * would have failed, which is exactly the inconsistency PAM's
	 * separate auth/account phases exist to catch. */
	if (pam_unix_shadow_hash(user, hash, sizeof(hash)) == 0 &&
	    (hash[0] == '!' || hash[0] == '*'))
		return (PAM_AUTH_ERR);

	return (PAM_SUCCESS);
}

PAM_EXTERN int
pam_sm_open_session(pam_handle_t *pamh __attribute__((unused)),
    int flags __attribute__((unused)), int argc __attribute__((unused)),
    const char *argv[] __attribute__((unused)))
{

	return (PAM_SUCCESS);
}

PAM_EXTERN int
pam_sm_close_session(pam_handle_t *pamh __attribute__((unused)),
    int flags __attribute__((unused)), int argc __attribute__((unused)),
    const char *argv[] __attribute__((unused)))
{

	return (PAM_SUCCESS);
}
