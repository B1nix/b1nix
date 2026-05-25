#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "syscall.h"
#include "types.h"

#define SHM_KEY 0x12345678
#define SHM_SIZE 4096
#define IPC_CREAT 0x1000
#define IPC_RMID 0

#define WIFEXITED(status) (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)

int main(void) {
    printf("m15_shm_fork: starting test...\n");

    int shmid = (int)syscall(SYS_SHMGET, SHM_KEY, SHM_SIZE, IPC_CREAT | 0666);
    if (shmid < 0) {
        printf("m15_shm_fork: shmget failed\n");
        return 1;
    }

    char *shared = (char *)syscall(SYS_SHMAT, shmid, NULL, 0);
    if (shared == (char *)-1) {
        printf("m15_shm_fork: shmat failed in parent\n");
        return 1;
    }

    strcpy(shared, "PARENT_DATA");

    int pid = (int)syscall(SYS_FORK);
    if (pid < 0) {
        printf("m15_shm_fork: fork failed\n");
        return 1;
    }

    if (pid == 0) {
        // Child
        if (strcmp(shared, "PARENT_DATA") != 0) {
            printf("m15_shm_fork: child did not see parent data (saw '%s')\n", shared);
            syscall(SYS_SHMDT, shared);
            syscall(SYS_EXIT, 1);
        }

        strcpy(shared, "CHILD_MODIFIED");
        syscall(SYS_SHMDT, shared);
        syscall(SYS_EXIT, 0);
    } else {
        // Parent
        int status;
        syscall(SYS_WAITPID, pid, &status, 0);

        if (WEXITSTATUS(status) != 0) {
            printf("m15_shm_fork: child exited with error\n");
            syscall(SYS_SHMDT, shared);
            syscall(SYS_SHMCTL, shmid, IPC_RMID, NULL);
            return 1;
        }

        if (strcmp(shared, "CHILD_MODIFIED") != 0) {
            printf("m15_shm_fork: parent did not see child modifications (saw '%s')\n", shared);
            syscall(SYS_SHMDT, shared);
            syscall(SYS_SHMCTL, shmid, IPC_RMID, NULL);
            return 1;
        }

        printf("m15_shm_fork: SUCCESS - shared memory is coherent across fork!\n");

        syscall(SYS_SHMDT, shared);
        syscall(SYS_SHMCTL, shmid, IPC_RMID, NULL);
        return 0;
    }
    return 0;
}
