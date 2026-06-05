#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <grp.h>

int main(int argc, char **argv) {
    if (getuid() != 0) {
        fprintf(stderr, "groupadd: Only root can add groups\n");
        return 1;
    }
    
    if (argc < 2) {
        fprintf(stderr, "Usage: groupadd [-g gid] groupname\n");
        return 1;
    }

    gid_t gid = 0;
    const char *groupname = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            gid = (gid_t)atoi(argv[++i]);
        } else {
            groupname = argv[i];
        }
    }

    if (!groupname) {
        fprintf(stderr, "groupadd: groupname required\n");
        return 1;
    }

    if (getgrnam(groupname) != NULL) {
        fprintf(stderr, "groupadd: group '%s' already exists\n", groupname);
        return 1;
    }

    if (gid == 0) {
        gid_t max_gid = 999;
        FILE *f = fopen("/etc/group", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                char *colon1 = strchr(line, ':');
                if (!colon1) continue;
                char *colon2 = strchr(colon1 + 1, ':');
                if (!colon2) continue;
                char *gid_str = colon2 + 1;
                char *colon3 = strchr(gid_str, ':');
                if (colon3) *colon3 = '\0';
                gid_t curr_gid = (gid_t)atoi(gid_str);
                if (curr_gid > max_gid) {
                    max_gid = curr_gid;
                }
            }
            fclose(f);
        }
        gid = max_gid + 1;
    }

    if (getgrgid(gid) != NULL) {
        fprintf(stderr, "groupadd: GID %u is already in use\n", (unsigned int)gid);
        return 1;
    }

    FILE *f = fopen("/etc/group", "a");
    if (!f) {
        perror("groupadd: cannot open /etc/group");
        return 1;
    }
    fprintf(f, "%s:x:%u:\n", groupname, (unsigned int)gid);
    fclose(f);

    printf("groupadd: Group '%s' (GID %u) added successfully\n", groupname, (unsigned int)gid);
    return 0;
}
