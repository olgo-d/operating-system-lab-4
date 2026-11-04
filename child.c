 #define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <semaphore.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>

#define BUFFER 4096

int wait_reader(sem_t *reader, pid_t parent) {
    while (1) {
        struct timespec timeout;

        if (clock_gettime(CLOCK_REALTIME, &timeout) == -1) {
            return -1;
        }
        timeout.tv_sec++;

        if (sem_timedwait(reader, &timeout) == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != ETIMEDOUT) {
            return -1;
        }
        if (getppid() != parent) {
            errno = EPIPE;
            return -1;
        }
    }
}

int main(int argc, char *argv[]) {
    printf(
        "Child PID: %ld, parent PID: %ld\n",
        (long)getpid(),
        (long)getppid()
    );

    if (argc != 3) {
        fprintf(stderr, "Usage: child OUTPUT_FILE MAPPING_FILE\n");
        return 1;
    }

    pid_t parent = getppid();
    size_t map_size = 2 * sizeof(sem_t) + 1 + BUFFER;

    int fd = open(argv[2], O_RDWR);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    void *memory = mmap(
        NULL, map_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED, fd, 0
    );

    if (memory == MAP_FAILED) {
        perror("mmap");
        if (close(fd) == -1) {
            perror("close");
        }
        return 1;
    }

    if (close(fd) == -1) {
        perror("close");
        if (munmap(memory, map_size) == -1) {
            perror("munmap");
        }
        return 1;
    }

    sem_t *writer = (sem_t *)memory;
    sem_t *reader = writer + 1;
    char *flag = (char *)(reader + 1);
    char *shared_line = flag + 1;

    FILE *file = fopen(argv[1], "w");
    if (file == NULL) {
        perror("fopen");
        if (munmap(memory, map_size) == -1) {
            perror("munmap");
        }
        return 1;
    }

    char *line = NULL;
    size_t size = 0;
    size_t used = 0;
    int error = 0;

    while (1) {
        if (wait_reader(reader, parent) == -1) {
            perror("wait_reader");
            error = 1;
            break;
        }

        char type = *flag;

        if (type == 'E') {
            if (used != 0) {
                fprintf(stderr, "Incomplete input line\n");
                error = 1;
            }

            if (sem_post(writer) == -1) {
                perror("sem_post");
                error = 1;
            }
            break;
        }

        if (type != 'C' && type != 'L') {
            fprintf(stderr, "Invalid message type\n");
            error = 1;
            break;
        }

        size_t count = strnlen(shared_line, BUFFER);

        if (count == BUFFER || count > SIZE_MAX - used - 1) {
            fprintf(stderr, "Invalid message length\n");
            error = 1;
            break;
        }

        size_t needed = used + count + 1;

        if (needed > size) {
            size_t new_size = size ? size : BUFFER;

            while (new_size < needed) {
                if (new_size > SIZE_MAX / 2) {
                    new_size = needed;
                    break;
                }
                new_size *= 2;
            }

            char *new_line = realloc(line, new_size);

            if (new_line == NULL) {
                perror("realloc");
                error = 1;
                break;
            }

            line = new_line;
            size = new_size;
        }

        memcpy(line + used, shared_line, count);
        used += count;
        line[used] = '\0';

        if (sem_post(writer) == -1) {
            perror("sem_post");
            error = 1;
            break;
        }

        if (type == 'C') {
            continue;
        }

        float sum = 0;
        char *ptr = line;

        while (*ptr != '\0') {
            char *end;
            errno = 0;
            float value = strtof(ptr, &end);

            if (ptr == end) {
                break;
            }

            if (errno == ERANGE) {
                perror("strtof");
                error = 1;
                break;
            }

            sum += value;
            ptr = end;
        }

        if (error) {
            break;
        }

        if (fprintf(file, "%f\n", sum) < 0) {
            perror("fprintf");
            error = 1;
            break;
        }

        used = 0;
    }

    free(line);

    if (fclose(file) == EOF) {
        perror("fclose");
        error = 1;
    }

    if (munmap(memory, map_size) == -1) {
        perror("munmap");
        error = 1;
    }

    return error;
}
