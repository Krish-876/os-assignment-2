/*
 * Problem 3: Sudoku Conflict Analyzer and Repair Advisor
 *
 * Usage: ./sudoku_analyzer <gridfile>
 * Build: gcc -Wall -Wextra -O2 -pthread -o sudoku_analyzer sudoku_analyzer.c
 *
 * Phase 1: 27 validation threads (9 rows, 9 columns, 9 sub-grids).
 * Phase 2: score, 9 heatmap threads (one per 3x3 block), hotspots, repair report.
 *
 * No mutex / semaphore / condition variable is used. Why every shared
 * object is race-free:
 *
 *   grid[9][9]     written only by main() while loading, before any thread
 *                  exists. Read-only for all threads afterwards.
 *   validity[27]   thread with unit_id u writes only validity[u]: exactly
 *                  one writer per element, nobody reads it until after
 *                  pthread_join() (join gives the happens-before edge).
 *   results        each worker malloc()s its own struct and hands the
 *                  pointer back through pthread_exit(); the parent only
 *                  touches it after joining that thread.
 *   dup_masks[27]  filled by the parent (single thread) after all joins and
 *                  before the heatmap threads start; read-only during
 *                  Phase 2, so concurrent reads are safe.
 *   heatmap[9][9]  block thread b writes only the 9 cells of its own 3x3
 *                  block; blocks are disjoint, and the parent reads the
 *                  heatmap only after joining all 9 threads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>

#define NUM_UNITS 27
/* 
 * 0-8      Rows
 * 9-17     Columns
 * 18-26    Blocks
*/

struct unit_params {
    int unit_id;                /* 0-26 */
};

struct unit_result {
    int unit_id;
    int valid;                  /* 1 = VALID, 0 = INVALID */
    int missing_mask;           /* bit d set => digit d never appears */
    int dup_mask;               /* bit d set => digit d appears twice or more */
    int blank_count;            /* number of empty cells in this unit */
};

struct block_params {
    int block_id;               /* 0-8, row-major */
};

static int grid[9][9];                  /* 0 = empty */
static int validity[NUM_UNITS];         /* Stores whether each unit is valid. valid = 1, invalid = 0 */
static int missing_masks[NUM_UNITS];    /* Stores the missing-digit bitmask for each unit. */
static int dup_masks[NUM_UNITS];        /* Stores the duplicated-digit bitmask for each unit. */
static int blank_counts[NUM_UNITS];     /* Stores the number of empty cells in each unit. */
static int heatmap[9][9];               /* Stores the conflict score of every Sudoku cell. */

/* converts Unit ID(u) & Position in Unit(i) into actual Row(r) and Column(c) in the grid */
static void unit_cell(int u, int i, int *r, int *c)
{
    if (u < 9) {
        *r = u;
        *c = i;
    } else if (u < 18) {
        *r = i;
        *c = u - 9;
    } else {
        int b = u - 18;
        *r = (b / 3) * 3 + i / 3;
        *c = (b % 3) * 3 + i % 3;
    }
}

/* ---------- Phase 1 ---------- */

// Thread Worker to validate each unit, called 27 times
static void *validate_unit(void *arg)
{
    const struct unit_params *p = arg;
    int count[10] = {0};
    int blanks = 0, i, d;
    int missing = 0, dup = 0;
    struct unit_result *res;

    for (i = 0; i < 9; i++) {
        int r, c, v;
        unit_cell(p->unit_id, i, &r, &c);
        v = grid[r][c];
        if (v == 0)
            blanks++;
        else
            count[v]++;
    }
    for (d = 1; d <= 9; d++) {
        if (count[d] == 0) missing |= 1 << d;
        if (count[d] >= 2) dup |= 1 << d;
    }

    res = malloc(sizeof(*res));
    if (!res)
        pthread_exit(NULL);
    res->unit_id = p->unit_id;
    res->missing_mask = missing;
    res->dup_mask = dup;
    res->blank_count = blanks;
    res->valid = (missing == 0 && dup == 0 && blanks == 0);

    validity[p->unit_id] = res->valid;      /* only our own index */
    pthread_exit(res);
}

/* ---------- Phase 2 ---------- */

// Thread Worker to fill each block of the heatmap grid 9 times
static void *heatmap_block(void *arg)
{
    const struct block_params *p = arg;
    int r0 = (p->block_id / 3) * 3;
    int c0 = (p->block_id % 3) * 3;
    int r, c;

    for (r = r0; r < r0 + 3; r++) {
        for (c = c0; c < c0 + 3; c++) {
            int v = grid[r][c];
            if (v == 0) {
                heatmap[r][c] = 3;
                continue;
            }
            heatmap[r][c] = ((dup_masks[r] >> v) & 1) +                         /* Row Conflict */
                            ((dup_masks[9 + c] >> v) & 1) +                     /* Column Conflict */
                            ((dup_masks[18 + (r / 3) * 3 + c / 3] >> v) & 1);   /* Block Conflict */
        }
    }
    return NULL;
}

/* ---------- input ---------- */

// Takes input into the grid, and throws error upon invalid grid
// returns 0 on success, -1 with a diagnostic on failure
static int load_grid(const char *path)
{
    FILE *fp = fopen(path, "r");
    int ch, n = 0;

    if (!fp) {
        perror(path);
        return -1;
    }
    while ((ch = fgetc(fp)) != EOF) {
        int v;
        if (isspace(ch))
            continue;
        if (ch == '.' || ch == '0')
            v = 0;
        else if (ch >= '1' && ch <= '9')
            v = ch - '0';
        else {
            fprintf(stderr, "Error: invalid character '%c' (0x%02x) in %s\n",
                    isprint(ch) ? ch : '?', ch, path);
            fclose(fp);
            return -1;
        }
        if (n >= 81) {
            fprintf(stderr, "Error: more than 81 cells in %s\n", path);
            fclose(fp);
            return -1;
        }
        grid[n / 9][n % 9] = v;
        n++;
    }
    fclose(fp);
    if (n != 81) {
        fprintf(stderr, "Error: expected 81 cells, found %d in %s\n", n, path);
        return -1;
    }
    return 0;
}

static void print_digit_set(int mask)
{
    int d, first = 1;
    printf("{");
    for (d = 1; d <= 9; d++)
        if (mask & (1 << d)) {
            printf("%s%d", first ? "" : ", ", d);
            first = 0;
        }
    printf("}");
}

int main(int argc, char **argv)
{
    pthread_t tid[NUM_UNITS];
    struct unit_params uparams[NUM_UNITS];
    struct unit_result *results[NUM_UNITS];
    struct block_params bparams[9];
    int i, r, c, valid_units = 0, hotspots = 0;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <gridfile>\n", argv[0]);
        return 1;
    }
    if (load_grid(argv[1]) != 0)
        return 1;

    /* Phase 1 */
    for (i = 0; i < NUM_UNITS; i++) {
        uparams[i].unit_id = i;
        if (pthread_create(&tid[i], NULL, validate_unit, &uparams[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            return 1;
        }
    }
    for (i = 0; i < NUM_UNITS; i++) {
        void *ret = NULL;
        pthread_join(tid[i], &ret);
        results[i] = ret;
        if (!results[i]) {
            fprintf(stderr, "worker %d failed (out of memory)\n", i);
            return 1;
        }
    }
    /* copy what Phase 2 needs, then release every result struct */
    for (i = 0; i < NUM_UNITS; i++) {
        missing_masks[i] = results[i]->missing_mask;
        dup_masks[i] = results[i]->dup_mask;
        blank_counts[i] = results[i]->blank_count;
        if (results[i]->valid)
            valid_units++;
        free(results[i]);
        results[i] = NULL;
    }

    printf("Input Grid:\n");
    for (r = 0; r < 9; r++) {
        for (c = 0; c < 9; c++) {
            if (grid[r][c] == 0)
                printf("%s.", c ? " " : "");
            else
                printf("%s%d", c ? " " : "", grid[r][c]);
        }
        printf("\n");
    }
    printf("\n");

    if (valid_units == NUM_UNITS) {
        printf("VALID\n");
        return 0;
    }
    printf("INVALID\n");

    /* (a) validity score */
    printf("Validity Score: %.2f%%\n\n", valid_units * 100.0 / NUM_UNITS);

    /* (b) heatmap: 9 threads, one per block */
    for (i = 0; i < 9; i++) {
        bparams[i].block_id = i;
        if (pthread_create(&tid[i], NULL, heatmap_block, &bparams[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            return 1;
        }
    }
    for (i = 0; i < 9; i++)
        pthread_join(tid[i], NULL);

    printf("Conflict Density Heatmap:\n");
    for (r = 0; r < 9; r++) {
        for (c = 0; c < 9; c++)
            printf("%s%d", c ? " " : "", heatmap[r][c]);
        printf("\n");
    }
    printf("\n");

    /* (c) hotspots, row-major */
    printf("Hotspots (score 3):\n");
    for (r = 0; r < 9; r++)
        for (c = 0; c < 9; c++)
            if (heatmap[r][c] == 3) {
                if (grid[r][c] == 0)
                    printf("  (%d,%d) value=0 (empty)\n", r, c);
                else
                    printf("  (%d,%d) value=%d\n", r, c, grid[r][c]);
                hotspots++;
            }
    printf("Total Hotspots: %d\n\n", hotspots);

    /* (d) missing digit report: one line per invalid unit */
    printf("Missing Digit Report:\n");
    for (i = 0; i < NUM_UNITS; i++) {
        const char *kind = i < 9 ? "Row" : (i < 18 ? "Column" : "Sub-grid");
        if (validity[i])
            continue;
        printf("  %-8s %d : missing ", kind, i % 9);
        print_digit_set(missing_masks[i]);
        printf("  blanks=%d\n", blank_counts[i]);
    }
    return 0;
}
