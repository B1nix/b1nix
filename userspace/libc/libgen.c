#include <libgen.h>
#include <string.h>

char *dirname(char *path) {
    static char dot[] = ".";
    static char slash[] = "/";
    if (!path || *path == '\0') {
        return dot;
    }

    size_t len = strlen(path);
    // Remove trailing slashes (except if path is "/")
    while (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }

    if (len == 1 && path[0] == '/') {
        return slash;
    }

    // Find the last slash
    char *last_slash = strrchr(path, '/');
    if (!last_slash) {
        return dot;
    }

    if (last_slash == path) {
        // Path is "/something"
        path[1] = '\0';
        return path;
    }

    // Path is "dir/something"
    *last_slash = '\0';
    // Remove any trailing slashes from the directory component
    len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }
    return path;
}

char *basename(char *path) {
    static char dot[] = ".";
    static char slash[] = "/";
    if (!path || *path == '\0') {
        return dot;
    }

    size_t len = strlen(path);
    // Remove trailing slashes (except if path is "/")
    while (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }

    if (len == 1 && path[0] == '/') {
        return slash;
    }

    char *last_slash = strrchr(path, '/');
    if (!last_slash) {
        return path;
    }

    return last_slash + 1;
}
