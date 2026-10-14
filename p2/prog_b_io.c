/*
 * Problem 2, Program B (I/O-bound): fork 8 children, each runs io_task().
 * Usage: ./prog_b_io ["label line, e.g. Time Quantum: 50ms"]
 *
 * Timing: the parent takes one reference timestamp before forking. Each
 * child stamps its start (first action after fork) and end (before exit)
 * and sends {id, start, end} to the parent through a pipe made before fork.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include "workload_io.h"

#define NUM_CHILDREN 8
#define PROG_TITLE "Program-B (I/O-Bound)"

struct record {                 /* fixed size, well below PIPE_BUF: atomic write */
    int child_id;
    struct timespec start;
    struct timespec end;
};

static double ms_between(const struct timespec *a, const struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000.0 + (b->tv_nsec - a->tv_nsec) / 1e6;
}

int main(int argc, char **argv)
{
    int fds[2];
    struct timespec ref, done;
    struct record rec;
    pid_t pids[NUM_CHILDREN];
    double sum_resp = 0, max_resp = 0, sum_turn = 0, max_turn = 0;
    int i, got = 0;

    if (pipe(fds) != 0) {
        perror("pipe");
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &ref);   /* the one reference timestamp */

    for (i = 0; i < NUM_CHILDREN; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            struct record r;
            clock_gettime(CLOCK_MONOTONIC, &r.start);   /* first action */
            close(fds[0]);
            io_task(i);
            r.child_id = i;
            clock_gettime(CLOCK_MONOTONIC, &r.end);
            if (write(fds[1], &r, sizeof(r)) != (ssize_t)sizeof(r))
                _exit(2);
            _exit(0);
        }
        pids[i] = pid;
    }
    close(fds[1]);      /* parent keeps only the read end */

    while (got < NUM_CHILDREN) {
        ssize_t n = read(fds[0], &rec, sizeof(rec));
        double resp, turn;
        if (n != (ssize_t)sizeof(rec))
            break;      /* EOF: some child died without reporting */
        resp = ms_between(&ref, &rec.start);
        turn = ms_between(&ref, &rec.end);
        sum_resp += resp;
        sum_turn += turn;
        if (resp > max_resp) max_resp = resp;
        if (turn > max_turn) max_turn = turn;
        got++;
    }
    for (i = 0; i < NUM_CHILDREN; i++)
        waitpid(pids[i], NULL, 0);
    clock_gettime(CLOCK_MONOTONIC, &done);
    close(fds[0]);

    if (got != NUM_CHILDREN) {
        fprintf(stderr, "only %d of %d children reported\n", got, NUM_CHILDREN);
        return 1;
    }

    {
        struct rusage ru;
        double elapsed = ms_between(&ref, &done) / 1000.0;
        getrusage(RUSAGE_CHILDREN, &ru);

        printf("%s\n", PROG_TITLE);
        printf("--------------------------------------------------\n");
        if (argc > 1)
            printf("%s\n", argv[1]);
        printf("Involuntary Context Switches: %ld\n", ru.ru_nivcsw);
        printf("Voluntary Context Switches: %ld\n", ru.ru_nvcsw);
        printf("Average Response Time:      %.2fms\n", sum_resp / NUM_CHILDREN);
        printf("Maximum Response Time:      %.2fms\n", max_resp);
        printf("Average Turnaround Time:    %.2fms\n", sum_turn / NUM_CHILDREN);
        printf("Maximum Turnaround Time:    %.2fms\n", max_turn);
        printf("Throughput:                 %.2f children/s\n", NUM_CHILDREN / elapsed);
        printf("Total Elapsed Time:         %.2fs\n", elapsed);
    }
    return 0;
}
