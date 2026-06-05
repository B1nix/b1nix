#include <stdio.h>
#include <unistd.h>
#include <pwd.h>

int main(void) {
    uid_t uid = geteuid();
    struct passwd *pw = getpwuid(uid);
    if (pw) {
        printf("%s\n", pw->pw_name);
    } else {
        printf("unknown uid %u\n", (unsigned int)uid);
    }
    return 0;
}
