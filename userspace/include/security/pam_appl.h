#ifndef B1NIX_U_SECURITY_PAM_APPL_H
#define B1NIX_U_SECURITY_PAM_APPL_H

#ifdef __cplusplus
extern "C" {
#endif

#define PAM_SUCCESS          0
#define PAM_OPEN_ERR         1
#define PAM_SYMBOL_ERR       2
#define PAM_SERVICE_ERR      3
#define PAM_SYSTEM_ERR       4
#define PAM_BUF_ERR          5
#define PAM_CONV_ERR         6
#define PAM_PERM_DENIED      7
#define PAM_AUTH_ERR         8
#define PAM_NEW_AUTHTOK_REQD 9
#define PAM_CRED_INSUFFICIENT 10
#define PAM_AUTHINFO_UNAVAIL 11
#define PAM_USER_UNKNOWN     12
#define PAM_MAXTRIES         13
#define PAM_ACCT_EXPIRED     14
#define PAM_SESSION_ERR      15
#define PAM_CRED_UNAVAIL     16
#define PAM_CRED_EXPIRED     17
#define PAM_CRED_ERR         18
#define PAM_NO_MODULE_DATA   19

#define PAM_SILENT           0x8000

#define PAM_SERVICE          1
#define PAM_USER             2
#define PAM_TTY              3
#define PAM_REQUEST          4
#define PAM_CONV             5
#define PAM_AUTHTOK          6
#define PAM_OLDAUTHTOK       7
#define PAM_RHOST            8
#define PAM_RUSER            9
#define PAM_USER_PROMPT      10
#define PAM_FAIL_DELAY       11

struct pam_message {
  int msg_style;
  const char *msg;
};

#define PAM_PROMPT_ECHO_OFF 1
#define PAM_PROMPT_ECHO_ON  2
#define PAM_ERROR_MSG       3
#define PAM_TEXT_INFO       4

struct pam_response {
  char *resp;
  int resp_retcode;
};

struct pam_conv {
  int (*conv)(int num_msg, const struct pam_message **msg,
              struct pam_response **resp, void *appdata_ptr);
  void *appdata_ptr;
};

typedef struct pam_handle pam_handle_t;

int pam_start(const char *service_name, const char *user,
              const struct pam_conv *pam_conversation, pam_handle_t **pamh);
int pam_end(pam_handle_t *pamh, int pam_status);
int pam_authenticate(pam_handle_t *pamh, int flags);
int pam_acct_mgmt(pam_handle_t *pamh, int flags);
int pam_setcred(pam_handle_t *pamh, int flags);
int pam_open_session(pam_handle_t *pamh, int flags);
int pam_close_session(pam_handle_t *pamh, int flags);
int pam_chauthtok(pam_handle_t *pamh, int flags);
int pam_set_item(pam_handle_t *pamh, int item_type, const void *item);
int pam_get_item(const pam_handle_t *pamh, int item_type, const void **item);
const char *pam_strerror(pam_handle_t *pamh, int errnum);
int pam_get_user(pam_handle_t *pamh, const char **user, const char *prompt);

#ifdef __cplusplus
}
#endif

#endif
