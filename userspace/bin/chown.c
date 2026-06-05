#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: chown <owner>[:<group>] <file>\n");
        return 1;
    }
    
    char *owner_grp = argv[1];
    const char *file_path = argv[2];
    
    uid_t owner_uid = (uid_t)-1;
    gid_t group_gid = (gid_t)-1;
    
    char *colon = strchr(owner_grp, ':');
    if (colon) {
        *colon = '\0';
        char *grp_name = colon + 1;
        
        if (owner_grp[0] != '\0') {
            struct passwd *pw = getpwnam(owner_grp);
            if (pw) {
                owner_uid = pw->pw_uid;
            } else {
                owner_uid = (uid_t)atoi(owner_grp);
            }
        }
        
        if (grp_name[0] != '\0') {
            struct group *gr = getgrnam(grp_name);
            if (gr) {
                group_gid = gr->gr_gid;
            } else {
                group_gid = (gid_t)atoi(grp_name);
            }
        }
    } else {
        struct passwd *pw = getpwnam(owner_grp);
        if (pw) {
            owner_uid = pw->pw_uid;
        } else {
            owner_uid = (uid_t)atoi(owner_grp);
        }
    }
    
    if (chown(file_path, owner_uid, group_gid) < 0) {
        perror("chown failed");
        return 1;
    }
    
    return 0;
}
