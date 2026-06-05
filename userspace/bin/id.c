#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <stdlib.h>

void print_user(const char *label, uid_t uid) {
    struct passwd *pw = getpwuid(uid);
    if (pw) {
        printf("%s=%u(%s)", label, (unsigned int)uid, pw->pw_name);
    } else {
        printf("%s=%u", label, (unsigned int)uid);
    }
}

void print_group(const char *label, gid_t gid) {
    struct group *gr = getgrgid(gid);
    if (gr) {
        if (label && label[0] != '\0') {
            printf("%s=%u(%s)", label, (unsigned int)gid, gr->gr_name);
        } else {
            printf("%u(%s)", (unsigned int)gid, gr->gr_name);
        }
    } else {
        if (label && label[0] != '\0') {
            printf("%s=%u", label, (unsigned int)gid);
        } else {
            printf("%u", (unsigned int)gid);
        }
    }
}

int main(int argc, char **argv) {
    if (argc > 1) {
        const char *username = argv[1];
        struct passwd *pw = getpwnam(username);
        if (!pw) {
            fprintf(stderr, "id: '%s': no such user\n", username);
            return 1;
        }
        
        print_user("uid", pw->pw_uid);
        printf(" ");
        print_group("gid", pw->pw_gid);
        
        printf(" groups=");
        print_group("", pw->pw_gid);
        
        FILE *f = fopen("/etc/group", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                char *name = line;
                char *colon1 = strchr(name, ':');
                if (!colon1) continue;
                *colon1 = '\0';
                
                char *passwd = colon1 + 1;
                char *colon2 = strchr(passwd, ':');
                if (!colon2) continue;
                *colon2 = '\0';
                
                char *gid_str = colon2 + 1;
                char *colon3 = strchr(gid_str, ':');
                if (!colon3) continue;
                *colon3 = '\0';
                
                gid_t gid = (gid_t)atoi(gid_str);
                char *members = colon3 + 1;
                
                size_t len = strlen(members);
                while (len > 0 && (members[len-1] == '\n' || members[len-1] == '\r')) {
                    members[len-1] = '\0';
                    len--;
                }
                
                if (gid == pw->pw_gid) continue;

                char *tok = strtok(members, ",");
                int is_member = 0;
                while (tok) {
                    if (strcmp(tok, username) == 0) {
                        is_member = 1;
                        break;
                    }
                    tok = strtok(NULL, ",");
                }
                if (is_member) {
                    printf(",");
                    printf("%u(%s)", (unsigned int)gid, name);
                }
            }
            fclose(f);
        }
        printf("\n");
    } else {
        uid_t uid = getuid();
        uid_t euid = geteuid();
        gid_t gid = getgid();
        gid_t egid = getegid();
        
        print_user("uid", uid);
        printf(" ");
        print_group("gid", gid);
        
        if (uid != euid) {
            printf(" ");
            print_user("euid", euid);
        }
        if (gid != egid) {
            printf(" ");
            print_group("egid", egid);
        }
        
        gid_t list[64];
        int n = getgroups(64, list);
        if (n >= 0) {
            printf(" groups=");
            print_group("", gid);
            for (int i = 0; i < n; i++) {
                if (list[i] != gid) {
                    printf(",");
                    print_group("", list[i]);
                }
            }
        }
        printf("\n");
    }
    return 0;
}
