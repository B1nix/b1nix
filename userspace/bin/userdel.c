#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>

int delete_from_passwd(const char *username) {
    FILE *fin = fopen("/etc/passwd", "r");
    if (!fin) return -1;
    unsigned int old_umask = umask(0022);
    FILE *fout = fopen("/etc/passwd.tmp", "w");
    umask(old_umask);
    if (!fout) {
        fclose(fin);
        return -1;
    }
    
    char line[256];
    int deleted = 0;
    while (fgets(line, sizeof(line), fin)) {
        char line_copy[256];
        strcpy(line_copy, line);
        char *colon = strchr(line_copy, ':');
        if (colon) *colon = '\0';
        if (strcmp(line_copy, username) == 0) {
            deleted = 1;
        } else {
            fputs(line, fout);
        }
    }
    fclose(fin);
    fclose(fout);
    
    if (!deleted) {
        unlink("/etc/passwd.tmp");
        return -1;
    }
    chmod("/etc/passwd.tmp", 0644);
    chown("/etc/passwd.tmp", 0, 0);
    if (rename("/etc/passwd.tmp", "/etc/passwd") < 0) {
        return -1;
    }
    return 0;
}

int delete_from_shadow(const char *username) {
    FILE *fin = fopen("/etc/shadow", "r");
    if (!fin) return -1;
    unsigned int old_umask = umask(0077);
    FILE *fout = fopen("/etc/shadow.tmp", "w");
    umask(old_umask);
    if (!fout) {
        fclose(fin);
        return -1;
    }
    
    char line[256];
    int deleted = 0;
    while (fgets(line, sizeof(line), fin)) {
        char line_copy[256];
        strcpy(line_copy, line);
        char *colon = strchr(line_copy, ':');
        if (colon) *colon = '\0';
        if (strcmp(line_copy, username) == 0) {
            deleted = 1;
        } else {
            fputs(line, fout);
        }
    }
    fclose(fin);
    fclose(fout);
    
    if (!deleted) {
        unlink("/etc/shadow.tmp");
        return -1;
    }
    chmod("/etc/shadow.tmp", 0600);
    chown("/etc/shadow.tmp", 0, 0);
    if (rename("/etc/shadow.tmp", "/etc/shadow") < 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (getuid() != 0) {
        fprintf(stderr, "userdel: Only root can delete users\n");
        return 1;
    }
    
    if (argc < 2) {
        fprintf(stderr, "Usage: userdel username\n");
        return 1;
    }
    
    const char *username = argv[1];
    
    struct passwd *pw = getpwnam(username);
    if (!pw) {
        fprintf(stderr, "userdel: user '%s' does not exist\n", username);
        return 1;
    }
    
    if (delete_from_passwd(username) < 0) {
        fprintf(stderr, "userdel: error removing from /etc/passwd\n");
        return 1;
    }
    
    delete_from_shadow(username);
    
    printf("userdel: User '%s' deleted successfully\n", username);
    return 0;
}
