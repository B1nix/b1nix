#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <crypt.h>
#include <grp.h>

extern char **environ;

int get_shadow_hash(const char *username, char *hash_out, size_t max_len) {
    FILE *f = fopen("/etc/shadow", "r");
    if (!f) return -1;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char *colon1 = strchr(line, ':');
        if (!colon1) continue;
        *colon1 = '\0';
        if (strcmp(line, username) == 0) {
            char *hash = colon1 + 1;
            char *colon2 = strchr(hash, ':');
            if (colon2) *colon2 = '\0';
            size_t len = strlen(hash);
            while (len > 0 && (hash[len-1] == '\n' || hash[len-1] == '\r')) {
                hash[len-1] = '\0';
                len--;
            }
            strncpy(hash_out, hash, max_len);
            hash_out[max_len - 1] = '\0';
            found = 1;
            break;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

int main(int argc, char **argv) {
    int is_login = 0;
    const char *target_user = "root";
    
    int arg_idx = 1;
    if (arg_idx < argc && strcmp(argv[arg_idx], "-") == 0) {
        is_login = 1;
        arg_idx++;
    }
    if (arg_idx < argc) {
        target_user = argv[arg_idx];
    }

    struct passwd *pw = getpwnam(target_user);
    if (!pw) {
        fprintf(stderr, "su: user '%s' does not exist\n", target_user);
        return 1;
    }

    if (strlen(pw->pw_name) >= 32 || strlen(pw->pw_dir) >= 256 || strlen(pw->pw_shell) >= 256) {
        fprintf(stderr, "su: user fields are too long\n");
        return 1;
    }

    // Authenticate if caller is not root
    if (getuid() != 0) {
        char hash[256];
        if (get_shadow_hash(target_user, hash, sizeof(hash)) < 0) {
            fprintf(stderr, "su: shadow hash lookup failed\n");
            return 1;
        }
        
        char *pass = getpass("Password: ");
        if (!pass) {
            fprintf(stderr, "su: authentication failed\n");
            return 1;
        }
        
        char *encrypted = crypt(pass, hash);
        if (!encrypted || strcmp(encrypted, hash) != 0) {
            fprintf(stderr, "su: Incorrect password\n");
            return 1;
        }
    }

    // Set GID, supplementary groups, and UID
    if (setgid(pw->pw_gid) < 0) {
        perror("su: setgid failed");
        return 1;
    }
    if (initgroups(pw->pw_name, pw->pw_gid) < 0) {
        perror("su: initgroups failed");
        return 1;
    }
    if (setuid(pw->pw_uid) < 0) {
        perror("su: setuid failed");
        return 1;
    }

    // Set environment
    if (is_login) {
        char *new_env[5];
        static char home_env[512];
        static char user_env[256];
        static char shell_env[256];
        if (snprintf(home_env, sizeof(home_env), "HOME=%s", pw->pw_dir) >= (int)sizeof(home_env) ||
            snprintf(user_env, sizeof(user_env), "USER=%s", pw->pw_name) >= (int)sizeof(user_env) ||
            snprintf(shell_env, sizeof(shell_env), "SHELL=%s", pw->pw_shell[0] ? pw->pw_shell : "/bin/sh") >= (int)sizeof(shell_env)) {
            fprintf(stderr, "su: environment variables too long\n");
            return 1;
        }
        new_env[0] = home_env;
        new_env[1] = user_env;
        new_env[2] = shell_env;
        new_env[3] = "PATH=/bin:/usr/bin";
        new_env[4] = NULL;
        environ = new_env;
        
        if (chdir(pw->pw_dir) < 0) {
            chdir("/");
        }
    } else {
        setenv("USER", pw->pw_name, 1);
        setenv("HOME", pw->pw_dir, 1);
    }

    const char *shell = pw->pw_shell[0] ? pw->pw_shell : "/bin/sh";
    char *args[2];
    static char arg0[256];
    if (is_login) {
        const char *slash = strrchr(shell, '/');
        if (snprintf(arg0, sizeof(arg0), "-%s", slash ? slash + 1 : shell) >= (int)sizeof(arg0)) {
            fprintf(stderr, "su: shell name too long\n");
            return 1;
        }
        args[0] = arg0;
    } else {
        args[0] = (char *)shell;
    }
    args[1] = NULL;

    execve(shell, args, environ);
    perror("su: execve failed");
    return 1;
}
