#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <signal.h>
#include <semaphore.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/mman.h>

#define BUFFER 4096

int wait_child(pid_t *id){
    int status;
    pid_t result;

    do {
        result = waitpid(*id, &status, 0);
    } while (result == -1 && errno == EINTR);

    if (result == -1) {
        perror("waitpid");
        return -1;
    }

    *id = -1;

    if (!WIFEXITED(status)) {
        fprintf(stderr, "Child process terminated abnormally\n");
        return -1;
    }

    if (WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Child process failed with code %d\n",
                WEXITSTATUS(status));
        return -1;
    }

    return 0;
}

int wait_empty(sem_t *empty, pid_t *id) {
    while (1) {
        struct timespec timeout;

        if (clock_gettime(CLOCK_REALTIME, &timeout) == -1) {
            return -1;
        }
        timeout.tv_sec++;

        if (sem_timedwait(empty, &timeout) == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != ETIMEDOUT) {
            return -1;
        }

        pid_t result = waitpid(*id, NULL, WNOHANG);

        if (result == *id) {
            *id = -1;
            errno = EPIPE;
            return -1;
        }
        if (result == -1 && errno != EINTR) {
            return -1;
        }
    }
}

int send_part(void *memory, pid_t *id, const char *data,
              size_t length, char type) {
    sem_t *writer = memory;
    sem_t *reader = writer + 1;
    char *flag = (char *)(reader + 1);
    char *shared_line = flag + 1;

    if (length >= BUFFER) {
        errno = EINVAL;
        return -1;
    }

    if (wait_empty(writer, id) == -1) {
        return -1;
    }

    if (length > 0) {
        memcpy(shared_line, data, length);
    }

    shared_line[length] = '\0';
    *flag = type;

    return sem_post(reader);
}

int main(){
    char filename[256];
    if (fgets(filename, sizeof(filename), stdin) == NULL){
        perror("fgets");
        return 1;
    }
    filename[strcspn(filename, "\n")] = '\0';

    char template[] = "/tmp/fileXXXXXX";
    int fd;

    fd = mkstemp(template);
    if (fd == -1){
        perror("mkstemp");
        return 1;
    }

    size_t size = 2 * sizeof(sem_t) + 1 + BUFFER;

    if (ftruncate(fd, size) != 0){
        perror("ftruncate");
        close(fd);
        unlink(template);
        return 1;
    }

    void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (memory == MAP_FAILED){
        perror("mmap");
        close(fd);
        unlink(template);
        return 1;
    }

    if (close(fd) == -1) {
        perror("close");
        munmap(memory, size);
        unlink(template);
        return 1;
    }

    sem_t *writer = (sem_t *)memory;
    sem_t *reader = writer + 1;

    if (sem_init(writer, 1, 1) == -1){
        perror("sem_init");
        munmap(memory, size);
        unlink(template);
        return 1;
    }

    if (sem_init(reader, 1, 0) == -1){
        perror("sem_init");
        sem_destroy(writer);
        munmap(memory, size);
        unlink(template);
        return 1;
    }

    pid_t id = fork();

    if (id < 0){
        perror("fork");
        sem_destroy(writer);
        sem_destroy(reader);
        unlink(template);
        munmap(memory, size);
        return 1;
    }

    if (id == 0){
        execl("./child", "child", filename, template, (char *)NULL);
        perror("execl");
        _exit(1);
    }

    printf(
        "Parent PID: %ld, child PID: %ld\n",
        (long)getpid(),
        (long)id
    );

    char *line = NULL;
    size_t capacity = 0;
    ssize_t len;
    int error = 0;

    while ((len = getline(&line, &capacity, stdin)) != -1) {
        for (size_t pos = 0; pos < (size_t)len;) {
            size_t count = (size_t)len - pos;

            if (count >= BUFFER) {
                count = BUFFER - 1;
            }

            char type = pos + count == (size_t)len ? 'L' : 'C';

            if (send_part(memory, &id, line + pos, count, type) == -1) {
                perror("send_part");
                error = 1;
                break;
            }

            pos += count;
        }

        if (error) {
            break;
        }
    }

    if (ferror(stdin)){
        perror("getline");
        error = 1;
    }

    if (!error) {
        if (send_part(memory, &id, NULL, 0, 'E') == -1) {
            perror("send_part");
            error = 1;
        }
    }

    if (error) {
        if (id > 0) {
            if (kill(id, SIGTERM) == -1 && errno != ESRCH) {
                perror("kill");
            }
        }
    }

    if (id > 0) {
        if (error) {
            pid_t waited;

            do {
                waited = waitpid(id, NULL, 0);
            } while (waited == -1 && errno == EINTR);

            if (waited == -1) {
                perror("waitpid");
            }
        }
        else if (wait_child(&id) == -1) {
            error = 1;
        }
    }

    free(line);

    if (sem_destroy(writer) == -1) {
        perror("sem_destroy writer");
        error = 1;
    }

    if (sem_destroy(reader) == -1) {
        perror("sem_destroy reader");
        error = 1;
    }

    if (munmap(memory, size) == -1) {
        perror("munmap");
        error = 1;
    }

    if (unlink(template) == -1) {
        perror("unlink");
        error = 1;
    }

    return error;
}