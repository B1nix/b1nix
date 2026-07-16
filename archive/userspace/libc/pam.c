#include <security/pam_appl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <crypt.h>
#include <errno.h>

struct pam_handle {
  const char *service;
  const char *user;
  struct pam_conv conv;
  const char *tty;
  const char *rhost;
  const char *ruser;
  const char *authtok;
};

int pam_start(const char *service_name, const char *user,
              const struct pam_conv *pam_conversation, pam_handle_t **pamh) {
  if (!pamh) return PAM_SYSTEM_ERR;
  pam_handle_t *h = calloc(1, sizeof(pam_handle_t));
  if (!h) return PAM_BUF_ERR;
  
  h->service = service_name ? strdup(service_name) : NULL;
  h->user = user ? strdup(user) : NULL;
  if (pam_conversation) {
    h->conv = *pam_conversation;
  }
  *pamh = h;
  return PAM_SUCCESS;
}

int pam_end(pam_handle_t *pamh, int pam_status) {
  (void)pam_status;
  if (!pamh) return PAM_SYSTEM_ERR;
  free((void *)pamh->service);
  free((void *)pamh->user);
  free((void *)pamh->tty);
  free((void *)pamh->rhost);
  free((void *)pamh->ruser);
  free((void *)pamh->authtok);
  free(pamh);
  return PAM_SUCCESS;
}

int pam_set_item(pam_handle_t *pamh, int item_type, const void *item) {
  if (!pamh) return PAM_SYSTEM_ERR;
  switch (item_type) {
    case PAM_SERVICE:
      free((void *)pamh->service);
      pamh->service = item ? strdup((const char *)item) : NULL;
      break;
    case PAM_USER:
      free((void *)pamh->user);
      pamh->user = item ? strdup((const char *)item) : NULL;
      break;
    case PAM_TTY:
      free((void *)pamh->tty);
      pamh->tty = item ? strdup((const char *)item) : NULL;
      break;
    case PAM_RHOST:
      free((void *)pamh->rhost);
      pamh->rhost = item ? strdup((const char *)item) : NULL;
      break;
    case PAM_RUSER:
      free((void *)pamh->ruser);
      pamh->ruser = item ? strdup((const char *)item) : NULL;
      break;
    case PAM_CONV:
      if (item) pamh->conv = *(const struct pam_conv *)item;
      break;
    case PAM_AUTHTOK:
      free((void *)pamh->authtok);
      pamh->authtok = item ? strdup((const char *)item) : NULL;
      break;
    default:
      return PAM_SYMBOL_ERR;
  }
  return PAM_SUCCESS;
}

int pam_get_item(const pam_handle_t *pamh, int item_type, const void **item) {
  if (!pamh || !item) return PAM_SYSTEM_ERR;
  switch (item_type) {
    case PAM_SERVICE: *item = pamh->service; break;
    case PAM_USER: *item = pamh->user; break;
    case PAM_TTY: *item = pamh->tty; break;
    case PAM_RHOST: *item = pamh->rhost; break;
    case PAM_RUSER: *item = pamh->ruser; break;
    case PAM_CONV: *item = &pamh->conv; break;
    case PAM_AUTHTOK: *item = pamh->authtok; break;
    default: return PAM_SYMBOL_ERR;
  }
  return PAM_SUCCESS;
}

int pam_get_user(pam_handle_t *pamh, const char **user, const char *prompt) {
  if (!pamh || !user) return PAM_SYSTEM_ERR;
  if (pamh->user) {
    *user = pamh->user;
    return PAM_SUCCESS;
  }
  
  if (!pamh->conv.conv) return PAM_CONV_ERR;
  
  struct pam_message msg;
  msg.msg_style = PAM_PROMPT_ECHO_ON;
  msg.msg = prompt ? prompt : "Username: ";
  
  const struct pam_message *pmsg = &msg;
  struct pam_response *presp = NULL;
  
  int rc = pamh->conv.conv(1, &pmsg, &presp, pamh->conv.appdata_ptr);
  if (rc != PAM_SUCCESS || !presp || !presp->resp) {
    if (presp) free(presp);
    return PAM_CONV_ERR;
  }
  
  pamh->user = presp->resp;
  *user = pamh->user;
  free(presp);
  return PAM_SUCCESS;
}

int pam_authenticate(pam_handle_t *pamh, int flags) {
  (void)flags;
  if (!pamh) return PAM_SYSTEM_ERR;
  
  const char *username = NULL;
  int rc = pam_get_user(pamh, &username, "Login: ");
  if (rc != PAM_SUCCESS || !username) return PAM_USER_UNKNOWN;
  
  char *password = NULL;
  if (pamh->authtok) {
    password = (char *)pamh->authtok;
  } else {
    if (!pamh->conv.conv) return PAM_CONV_ERR;
    
    struct pam_message msg;
    msg.msg_style = PAM_PROMPT_ECHO_OFF;
    msg.msg = "Password: ";
    
    const struct pam_message *pmsg = &msg;
    struct pam_response *presp = NULL;
    
    int rc = pamh->conv.conv(1, &pmsg, &presp, pamh->conv.appdata_ptr);
    if (rc != PAM_SUCCESS || !presp || !presp->resp) {
      if (presp) free(presp);
      return PAM_CONV_ERR;
    }
    password = presp->resp;
  }
  
  struct passwd *pw = getpwnam(username);
  const char *hash = pw ? pw->pw_passwd : NULL;
  
  if (!hash) {
    if (!pamh->authtok && password) free(password);
    return PAM_USER_UNKNOWN;
  }
  
  const char *encrypted = crypt(password, hash);
  if (!pamh->authtok && password) free(password);
  
  if (encrypted && strcmp(encrypted, hash) == 0) {
    return PAM_SUCCESS;
  }
  return PAM_AUTH_ERR;
}

int pam_acct_mgmt(pam_handle_t *pamh, int flags) {
  (void)pamh; (void)flags;
  return PAM_SUCCESS;
}

int pam_setcred(pam_handle_t *pamh, int flags) {
  (void)pamh; (void)flags;
  return PAM_SUCCESS;
}

int pam_open_session(pam_handle_t *pamh, int flags) {
  (void)pamh; (void)flags;
  return PAM_SUCCESS;
}

int pam_close_session(pam_handle_t *pamh, int flags) {
  (void)pamh; (void)flags;
  return PAM_SUCCESS;
}

int pam_chauthtok(pam_handle_t *pamh, int flags) {
  (void)pamh; (void)flags;
  return PAM_SUCCESS;
}

const char *pam_strerror(pam_handle_t *pamh, int errnum) {
  (void)pamh;
  switch (errnum) {
    case PAM_SUCCESS: return "Success";
    case PAM_OPEN_ERR: return "Failed to load module";
    case PAM_SYMBOL_ERR: return "Symbol not found";
    case PAM_SERVICE_ERR: return "Error in service module";
    case PAM_SYSTEM_ERR: return "System error";
    case PAM_BUF_ERR: return "Memory buffer error";
    case PAM_CONV_ERR: return "Conversation failure";
    case PAM_PERM_DENIED: return "Permission denied";
    case PAM_AUTH_ERR: return "Authentication failure";
    case PAM_NEW_AUTHTOK_REQD: return "Authentication token expired";
    case PAM_CRED_INSUFFICIENT: return "Insufficient credentials";
    case PAM_AUTHINFO_UNAVAIL: return "Authentication info unavailable";
    case PAM_USER_UNKNOWN: return "User unknown";
    case PAM_MAXTRIES: return "Maximum number of tries exceeded";
    case PAM_ACCT_EXPIRED: return "Account expired";
    case PAM_SESSION_ERR: return "Session error";
    default: return "Unknown PAM error";
  }
}
