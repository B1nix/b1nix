#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    uname(&uts);

    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd))) {
        strcpy(cwd, "/");
    }

    /* Read uptime from /proc/uptime */
    double uptime_sec = 0;
    FILE *fup = fopen("/proc/uptime", "r");
    if (fup) {
        fscanf(fup, "%lf", &uptime_sec);
        fclose(fup);
    }
    unsigned long ut = (unsigned long)uptime_sec;
    unsigned long up_days = ut / 86400;
    unsigned long up_hours = (ut % 86400) / 3600;
    unsigned long up_mins = (ut % 3600) / 60;
    unsigned long up_secs = ut % 60;

    /* Read memory from /proc/meminfo */
    unsigned long mem_total_kb = 0, mem_free_kb = 0;
    FILE *fmem = fopen("/proc/meminfo", "r");
    if (fmem) {
        char line[128];
        while (fgets(line, sizeof(line), fmem)) {
            if (strncmp(line, "MemTotal:", 9) == 0) {
                sscanf(line + 9, "%lu", &mem_total_kb);
            } else if (strncmp(line, "MemFree:", 8) == 0) {
                sscanf(line + 8, "%lu", &mem_free_kb);
            }
        }
        fclose(fmem);
    }
    unsigned long mem_used_mb = (mem_total_kb > mem_free_kb ? mem_total_kb - mem_free_kb : 0) / 1024;
    unsigned long mem_total_mb = mem_total_kb / 1024;

    /* Read cpu name from /proc/cpuinfo */
    char cpu_name[128] = "b1nix Virtual CPU";
    FILE *fcpu = fopen("/proc/cpuinfo", "r");
    if (fcpu) {
        char line[128];
        while (fgets(line, sizeof(line), fcpu)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *colon = strchr(line, ':');
                if (colon) {
                    strncpy(cpu_name, colon + 2, sizeof(cpu_name) - 1);
                    char *nl = strchr(cpu_name, '\n');
                    if (nl) *nl = '\0';
                }
                break;
            }
        }
        fclose(fcpu);
    }

    printf("      _     user@b1nix (Ring 3)\n");
    printf("  ___| |_   os: %s %s\n", uts.sysname[0] ? uts.sysname : "b1nix", uts.release);
    printf(" / _ \\ __|  kernel: %s\n", uts.version[0] ? uts.version : "0.92.4");
    printf("|  __/ |_   cpu: %s\n", cpu_name);
    printf(" \\___|\\__|  arch: %s\n", uts.machine[0] ? uts.machine : "x86_64");
    printf("           shell: /bin/sh\n");
    printf("           cwd: %s\n", cwd);
    if (up_days > 0) {
        printf("           uptime: %lud %02lu:%02lu:%02lu\n", up_days, up_hours, up_mins, up_secs);
    } else {
        printf("           uptime: %02lu:%02lu:%02lu\n", up_hours, up_mins, up_secs);
    }
    printf("           memory: %lu/%lu MB\n", mem_used_mb, mem_total_mb);
    return 0;
}
