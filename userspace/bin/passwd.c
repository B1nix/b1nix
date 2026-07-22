#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <crypt.h>
#include <fcntl.h>
#include <sys/stat.h>

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

void gen_salt(char *salt_out) {
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./";
    static int seeded = 0;
    if (!seeded) {
        int fd = open("/dev/urandom", O_RDONLY);
        unsigned int seed = 0;
        if (fd >= 0) {
            read(fd, &seed, sizeof(seed));
            close(fd);
        }
        srand(seed);
        seeded = 1;
    }
    strcpy(salt_out, "$b1$");
    for (int i = 4; i < 12; i++) {
        salt_out[i] = charset[rand() % 64];
    }
    salt_out[12] = '$';
    salt_out[13] = '\0';
}

int update_shadow(const char *username, const char *new_hash) {
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
    int updated = 0;
    while (fgets(line, sizeof(line), fin)) {
        char line_copy[256];
        strcpy(line_copy, line);
        char *colon1 = strchr(line_copy, ':');
        if (!colon1) {
            fputs(line, fout);
            continue;
        }
        *colon1 = '\0';
        
        if (strcmp(line_copy, username) == 0) {
            char *fields = colon1 + 1;
            char *colon2 = strchr(fields, ':');
            if (colon2) {
                fprintf(fout, "%s:%s:%s", username, new_hash, colon2 + 1);
            } else {
                fprintf(fout, "%s:%s:0:0:99999:7:::\n", username, new_hash);
            }
            updated = 1;
        } else {
            fputs(line, fout);
        }
    }
    fclose(fin);
    fclose(fout);
    
    if (!updated) {
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
    uid_t my_uid = getuid();
    const char *target_user = NULL;
    
    if (argc > 1) {
        target_user = argv[1];
    } else {
        struct passwd *pw = getpwuid(my_uid);
        if (!pw) {
            fprintf(stderr, "passwd: unknown user\n");
            return 1;
        }
        target_user = pw->pw_name;
    }
    
    if (my_uid != 0) {
        struct passwd *pw = getpwuid(my_uid);
        if (!pw || strcmp(pw->pw_name, target_user) != 0) {
            fprintf(stderr, "passwd: Only root can change other users' passwords\n");
            return 1;
        }
    }

    struct passwd *pw = getpwnam(target_user);
    if (!pw) {
        fprintf(stderr, "passwd: User '%s' does not exist\n", target_user);
        return 1;
    }

    if (my_uid != 0) {
        char old_hash[256];
        if (get_shadow_hash(target_user, old_hash, sizeof(old_hash)) == 0 && old_hash[0] != '\0') {
            char *old_pass = getpass("Changing password for user. Old password: ");
            if (!old_pass) return 1;
            char *encrypted = crypt(old_pass, old_hash);
            if (!encrypted || strcmp(encrypted, old_hash) != 0) {
                fprintf(stderr, "passwd: Incorrect password\n");
                return 1;
            }
        }
    }

    char *new_pass = getpass("Enter new password: ");
    if (!new_pass || new_pass[0] == '\0') {
        fprintf(stderr, "passwd: password unchanged\n");
        return 1;
    }
    char pass1[128];
    strncpy(pass1, new_pass, sizeof(pass1));
    pass1[sizeof(pass1) - 1] = '\0';

    char *new_pass2 = getpass("Retype new password: ");
    if (!new_pass2 || strcmp(pass1, new_pass2) != 0) {
        fprintf(stderr, "passwd: passwords do not match\n");
        return 1;
    }

    char salt[32];
    gen_salt(salt);
    char *new_hash = crypt(pass1, salt);
    if (!new_hash) {
        fprintf(stderr, "passwd: hash generation failed\n");
        return 1;
    }

    if (update_shadow(target_user, new_hash) < 0) {
        fprintf(stderr, "passwd: error updating shadow file\n");
        return 1;
    }

    printf("passwd: password updated successfully\n");
    return 0;
}
