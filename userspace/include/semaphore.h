#ifndef B1NIX_U_SEMAPHORE_H
#define B1NIX_U_SEMAPHORE_H

typedef int sem_t;

int sem_init(sem_t *sem, int pshared, unsigned int value);
int sem_wait(sem_t *sem);
int sem_post(sem_t *sem);
int sem_destroy(sem_t *sem);

#endif
