#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <semaphore.h>
#include <fcntl.h>

#define N 4  // Total number of processes

sem_t *sem;

int main() {
    pid_t child_pids[N];

    // Create the semaphore
    sem = sem_open("/sem_bv_broadcast", O_CREAT, 0644, 0);
    if (sem == SEM_FAILED) {
        perror("Semaphore creation failed");
        exit(1);
    }

    // Spawn child processes
    for (int i = 0; i < N; i++) {
        if ((child_pids[i] = fork()) == 0) {
            char processIdStr[10], initialValueStr[10];
            sprintf(processIdStr, "%d", i);
            if (i < 3) {
                sprintf(initialValueStr, "%d", 1);
                // Replace child process with BV-broadcast process
                execl("./bv_broadcast", "bv_broadcast", processIdStr, initialValueStr, (char *)NULL);
                perror("execl failed");
                exit(1);  // Exit if execl fails
            } else {
                sprintf(initialValueStr, "%d", 0);
                // Replace child process with byzantine BV-broadcast process
                execl("./byzantine_bv_broadcast", "byzantine_bv_broadcast", processIdStr, initialValueStr, (char *)NULL);
                perror("execl failed");
                exit(1);  // Exit if execl fails
            }
        }
    }

    // Wait until all children have set up their sockets
    sleep(2);  // This is a simple way, but you might want to have a more deterministic method

    // Signal all children to proceed
    for (int i = 0; i < N; i++) {
        sem_post(sem);
    }

    // Wait for all children to complete (optional)
    for (int i = 0; i < N; i++) {
        waitpid(child_pids[i], NULL, 0);
    }

    sem_close(sem);
    sem_unlink("/sem_bv_broadcast");  // Cleanup the semaphore
    return 0;
}
