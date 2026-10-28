/*
 * Problem 1: Multithreaded Student Report Card Generator
 *
 * Phase 1: one worker thread per student aggregates a column of marks.
 * Phase 2: k threads sort contiguous segments, parent does a k-way merge.
 * Phase 3: interactive menu, single threaded.
 *
 * No mutexes / semaphores / condition variables are used. Every shared
 * location has exactly one writer at a time (see comments near each phase).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define MAX_SUBJECTS 32
#define MAX_STUDENTS 128

typedef struct {
    int student_id;
    double average;
    int minimum;
    int maximum;
    int subject_marks[MAX_SUBJECTS];
} student_stats;

struct thread_params {
    int thread_id;      /* logical thread id */
    int start_index;    /* starting index in student_stats array */
    int segment_size;   /* number of elements to process */
};

static int num_subjects, num_students;
static int marks[MAX_SUBJECTS][MAX_STUDENTS];   /* read-only after load */
static student_stats students[MAX_STUDENTS];    /* indexed by id at first */
static student_stats merged[MAX_STUDENTS];      /* final sorted order */
static int sort_key;                            /* -3 max, -2 min, -1 avg, >=0 subject */

/* ---------- Phase 1 ---------- */

/*
 * Worker i reads column i of the (read-only) marks matrix and writes only
 * students[i]. Different workers therefore touch disjoint array elements.
 * The argument points into the persistent array worker_ids, so it stays
 * valid for the lifetime of the thread.
 */
static void *aggregate_worker(void *arg)
{
    int i = *(int *)arg;
    student_stats *s = &students[i];
    int sum = 0, r;

    s->student_id = i;
    s->minimum = marks[0][i];
    s->maximum = marks[0][i];
    for (r = 0; r < num_subjects; r++) {
        int m = marks[r][i];
        s->subject_marks[r] = m;
        sum += m;
        if (m < s->minimum) s->minimum = m;
        if (m > s->maximum) s->maximum = m;
    }
    /* integer sum / count: equal sums give bit-identical doubles */
    s->average = (double)sum / (double)num_subjects;

    pthread_exit(NULL);
}

/* ---------- Phase 2 ---------- */

static double key_value(const student_stats *s)
{
    if (sort_key == -3) return s->maximum;
    if (sort_key == -2) return s->minimum;
    if (sort_key == -1) return s->average;
    return s->subject_marks[sort_key];
}

/* returns 1 if a must come before b (descending key, then lower id) */
static int comes_before(const student_stats *a, const student_stats *b)
{
    double va = key_value(a), vb = key_value(b);
    if (va != vb) return va > vb;
    return a->student_id < b->student_id;
}

/*
 * Sorts only students[start .. start+size-1] with insertion sort.
 * Segments are disjoint, so no two threads touch the same element.
 */
static void *sort_worker(void *arg)
{
    struct thread_params *p = arg;
    student_stats *seg = &students[p->start_index];
    int i, j;

    for (i = 1; i < p->segment_size; i++) {
        student_stats tmp = seg[i];
        j = i - 1;
        while (j >= 0 && comes_before(&tmp, &seg[j])) {
            seg[j + 1] = seg[j];
            j--;
        }
        seg[j + 1] = tmp;
    }

    pthread_exit(NULL);
}

/* k-way merge of k sorted segments (each of size seg) into merged[] */
static void kway_merge(int k, int seg)
{
    int pos[MAX_STUDENTS];  /* next unread offset inside each segment */
    int out, t;

    for (t = 0; t < k; t++) pos[t] = 0;
    for (out = 0; out < num_students; out++) {
        int best = -1;
        for (t = 0; t < k; t++) {
            if (pos[t] >= seg) continue;
            if (best < 0 ||
                comes_before(&students[t * seg + pos[t]],
                             &students[best * seg + pos[best]]))
                best = t;
        }
        merged[out] = students[best * seg + pos[best]];
        pos[best]++;
    }
}

/* ---------- helpers ---------- */

/* returns 1 ok, 0 not an integer, -1 EOF */
static int read_line_int(const char *prompt, int *out)
{
    char line[128];
    char extra;
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) return -1;
    if (sscanf(line, " %d %c", out, &extra) != 1) return 0;
    return 1;
}

static void print_student(const student_stats *s)
{
    int i;
    printf("Student ID   : %d\n", s->student_id);
    printf("Average      : %.2f\n", s->average);
    printf("Minimum      : %d\n", s->minimum);
    printf("Maximum      : %d\n", s->maximum);
    printf("Subject Marks: [");
    for (i = 0; i < num_subjects; i++)
        printf("%s%d", i ? ", " : "", s->subject_marks[i]);
    printf("]\n");
}

static int load_input(const char *path)
{
    FILE *fp = fopen(path, "r");
    int i, j;

    if (!fp) {
        perror(path);
        return -1;
    }
    if (fscanf(fp, "%d %d", &num_subjects, &num_students) != 2 ||
        num_subjects < 1 || num_subjects > MAX_SUBJECTS ||
        num_students < 1 || num_students > MAX_STUDENTS) {
        fprintf(stderr, "%s: bad header (subjects 1..%d, students 1..%d)\n",
                path, MAX_SUBJECTS, MAX_STUDENTS);
        fclose(fp);
        return -1;
    }
    for (i = 0; i < num_subjects; i++)
        for (j = 0; j < num_students; j++)
            if (fscanf(fp, "%d", &marks[i][j]) != 1) {
                fprintf(stderr, "%s: not enough marks (row %d, col %d)\n", path, i, j);
                fclose(fp);
                return -1;
            }
    fclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : (access("input.txt", R_OK) == 0 ? "input.txt" : "p1/input.txt");
    pthread_t tid[MAX_STUDENTS];
    int worker_ids[MAX_STUDENTS];            /* persistent thread arguments */
    struct thread_params params[MAX_STUDENTS];
    int i, k = 0, seg, r;

    if (load_input(path) != 0) return 1;

    /* Phase 1: one worker per student, parent joins them all */
    for (i = 0; i < num_students; i++) {
        worker_ids[i] = i;
        if (pthread_create(&tid[i], NULL, aggregate_worker, &worker_ids[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    for (i = 0; i < num_students; i++)
        pthread_join(tid[i], NULL);

    /* Phase 2: read and validate sort key and k */
    char prompt[128];
    snprintf(prompt, sizeof(prompt),
             "Enter sort key (-3=max, -2=min, -1=avg, 0..%d=subject index): ",
             num_subjects - 1);
    for (;;) {
        r = read_line_int(prompt, &sort_key);
        if (r < 0) return 1;
        if (r == 1 && sort_key >= -3 && sort_key < num_subjects) break;
        printf("Invalid sort key. Valid: -3, -2, -1, or 0..%d\n", num_subjects - 1);
    }
    for (;;) {
        r = read_line_int("Enter number of sorting threads (k): ", &k);
        if (r < 0) return 1;
        if (r == 1 && k >= 1 && k <= num_students && num_students % k == 0) break;
        printf("Invalid k. Must be between 1 and %d and divide %d.\n",
               num_students, num_students);
    }

    seg = num_students / k;
    for (i = 0; i < k; i++) {
        params[i].thread_id = i;
        params[i].start_index = i * seg;
        params[i].segment_size = seg;
        if (pthread_create(&tid[i], NULL, sort_worker, &params[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    for (i = 0; i < k; i++)
        pthread_join(tid[i], NULL);

    kway_merge(k, seg);

    /* Phase 3: menu loop, no threads */
    printf("--- Report Card Menu ---\n");
    for (;;) {
        char line[128];
        int cmd, id = 0, n;

        printf("Enter command (1 <id> = view student, 2 = view all, 0 = exit): ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;   /* EOF acts like exit */

        n = sscanf(line, " %d %d", &cmd, &id);
        if (n < 1) {
            printf("Invalid command.\n");
            continue;
        }
        if (cmd == 0) break;
        if (cmd == 1) {
            const student_stats *found = NULL;
            if (n < 2) {
                printf("Usage: 1 <student_id>\n");
                continue;
            }
            /* look up by student_id: sorting moved the entries around */
            for (i = 0; i < num_students; i++)
                if (merged[i].student_id == id) {
                    found = &merged[i];
                    break;
                }
            if (!found)
                printf("No such student id: %d (valid 0..%d)\n", id, num_students - 1);
            else
                print_student(found);
        } else if (cmd == 2) {
            for (i = 0; i < num_students; i++) {
                if (sort_key == -1)
                    printf("s%d %.2f\n", merged[i].student_id, merged[i].average);
                else
                    printf("s%d %d\n", merged[i].student_id, (int)key_value(&merged[i]));
            }
        } else {
            printf("Invalid command.\n");
        }
    }
    return 0;
}
