#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>
#include <fcntl.h>

int main(int argc, char **argv) {
    if (getuid() != 0) {
        fprintf(stderr, "useradd: Only root can add users\n");
        return 1;
    }
    
    if (argc < 2) {
        fprintf(stderr, "Usage: useradd [-u uid] [-g gid] [-s shell] username\n");
        return 1;
    }

    uid_t uid = 0;
    gid_t gid = 1000;
    const char *shell = "/bin/sh";
    const char *username = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            uid = (uid_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            gid = (gid_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            shell = argv[++i];
        } else {
            username = argv[i];
        }
    }

    if (!username) {
        fprintf(stderr, "useradd: username required\n");
        return 1;
    }

    if (getpwnam(username) != NULL) {
        fprintf(stderr, "useradd: user '%s' already exists\n", username);
        return 1;
    }

    if (uid == 0) {
        uid_t max_uid = 999;
        FILE *f = fopen("/etc/passwd", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                char *colon1 = strchr(line, ':');
                if (!colon1) continue;
                char *colon2 = strchr(colon1 + 1, ':');
                if (!colon2) continue;
                char *uid_str = colon2 + 1;
                char *colon3 = strchr(uid_str, ':');
                if (colon3) *colon3 = '\0';
                uid_t curr_uid = (uid_t)atoi(uid_str);
                if (curr_uid > max_uid) {
                    max_uid = curr_uid;
                }
            }
            fclose(f);
        }
        uid = max_uid + 1;
    }

    if (getpwuid(uid) != NULL) {
        fprintf(stderr, "useradd: UID %u is already in use\n", (unsigned int)uid);
        return 1;
    }

    FILE *f = fopen("/etc/passwd", "a");
    if (!f) {
        perror("useradd: cannot open /etc/passwd");
        return 1;
    }
    fprintf(f, "%s:x:%u:%u::/home/%s:%s\n", username, (unsigned int)uid, (unsigned int)gid, username, shell);
    fclose(f);

    f = fopen("/etc/shadow", "a");
    if (!f) {
        perror("useradd: cannot open /etc/shadow");
        return 1;
    }
    fprintf(f, "%s:*:0:0:99999:7:::\n", username);
    fclose(f);

    char home_path[128];
    if (snprintf(home_path, sizeof(home_path), "/home/%s", username) >= (int)sizeof(home_path)) {
        fprintf(stderr, "useradd: username too long\n");
        return 1;
    }
    if (mkdir(home_path, 0755) == 0) {
        chown(home_path, uid, gid);
    }

    printf("useradd: User '%s' (UID %u) added successfully\n", username, (unsigned int)uid);
    return 0;
}
