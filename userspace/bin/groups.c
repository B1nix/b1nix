#include <stdio.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#include <string.h>
#include <stdlib.h>

void print_group_name(gid_t gid) {
    struct group *gr = getgrgid(gid);
    if (gr) {
        printf("%s", gr->gr_name);
    } else {
        printf("%u", (unsigned int)gid);
    }
}

int main(int argc, char **argv) {
    if (argc > 1) {
        const char *username = argv[1];
        struct passwd *pw = getpwnam(username);
        if (!pw) {
            fprintf(stderr, "groups: '%s': no such user\n", username);
            return 1;
        }
        print_group_name(pw->pw_gid);

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
                    printf(" %s", name);
                }
            }
            fclose(f);
        }
        printf("\n");
    } else {
        gid_t list[64];
        int n = getgroups(64, list);
        if (n < 0) {
            perror("getgroups");
            return 1;
        }
        
        gid_t egid = getegid();
        print_group_name(egid);
        
        for (int i = 0; i < n; i++) {
            if (list[i] != egid) {
                printf(" ");
                print_group_name(list[i]);
            }
        }
        printf("\n");
    }
    return 0;
}
