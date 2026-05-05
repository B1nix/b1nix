#ifndef B1NIX_U_DLFCN_H
#define B1NIX_U_DLFCN_H

#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_GLOBAL 4
#define RTLD_DEFAULT ((void *)0)

void *dlopen(const char *filename, int flag);
char *dlerror(void);
void *dlsym(void *handle, const char *symbol);
int dlclose(void *handle);

#endif
