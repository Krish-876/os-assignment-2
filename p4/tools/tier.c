/*
 * tier - userspace control for fork_guard tiers.
 *
 *   tier set <user> <gold|silver|bronze>   (root)
 *   tier reset                             (root)
 *   tier show
 *
 * Build:   gcc -Wall -Wextra -O2 -o tier tier.c
 * Install: sudo install -m 755 tier /usr/local/bin/tier
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>

#define TIER_FILE  "/proc/sched_tier"
#define STATS_FILE "/proc/fork_guard"

static int write_cmd(const char *cmd)
{
    int fd = open(TIER_FILE, O_WRONLY);
    ssize_t n;

    if (fd < 0) {
        if (errno == ENOENT)
            fprintf(stderr, "tier: %s missing (is the fork_guard kernel running?)\n", TIER_FILE);
        else if (errno == EACCES || errno == EPERM)
            fprintf(stderr, "tier: permission denied, only root can assign tiers\n");
        else
            fprintf(stderr, "tier: open %s: %s\n", TIER_FILE, strerror(errno));
        return -1;
    }
    n = write(fd, cmd, strlen(cmd));
    if (n < 0) {
        if (errno == EPERM)
            fprintf(stderr, "tier: permission denied, only root can assign tiers\n");
        else
            fprintf(stderr, "tier: write failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int cmd_set(const char *user, const char *tier)
{
    struct passwd *pw;
    char buf[64];

    if (strcmp(tier, "gold") && strcmp(tier, "silver") && strcmp(tier, "bronze")) {
        fprintf(stderr, "tier: unknown tier '%s' (use gold, silver or bronze)\n", tier);
        return 1;
    }
    pw = getpwnam(user);
    if (!pw) {
        fprintf(stderr, "tier: unknown user '%s'\n", user);
        return 1;
    }
    snprintf(buf, sizeof(buf), "set %u %s\n", (unsigned)pw->pw_uid, tier);
    if (write_cmd(buf) != 0)
        return 1;
    printf("user %s (uid %u) -> %s\n", user, (unsigned)pw->pw_uid, tier);
    return 0;
}

static int cmd_show(void)
{
    FILE *fp = fopen(TIER_FILE, "r");
    char line[256];
    int rows = 0;
    FILE *st;

    if (!fp) {
        fprintf(stderr, "tier: cannot open %s: %s\n", TIER_FILE, strerror(errno));
        return 1;
    }
    /* first line is the header, the rest are "uid tier nice" */
    if (fgets(line, sizeof(line), fp)) {
        unsigned uid;
        char tier[16];
        int nice;

        while (fgets(line, sizeof(line), fp)) {
            struct passwd *pw;

            if (sscanf(line, "%u %15s %d", &uid, tier, &nice) != 3)
                continue;
            if (rows == 0)
                printf("%-8s %-12s %-8s %s\n", "UID", "USER", "TIER", "NICE");
            pw = getpwuid((uid_t)uid);
            printf("%-8u %-12s %-8s %d\n", uid, pw ? pw->pw_name : "?", tier, nice);
            rows++;
        }
    }
    fclose(fp);
    if (rows == 0)
        printf("(no user is tiered; everyone is SILVER)\n");

    st = fopen(STATS_FILE, "r");
    if (st) {
        printf("\n--- %s ---\n", STATS_FILE);
        while (fgets(line, sizeof(line), st))
            fputs(line, stdout);
        fclose(st);
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: tier set <user> <gold|silver|bronze>\n"
            "       tier reset\n"
            "       tier show\n");
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "set"))
        return cmd_set(argv[2], argv[3]);
    if (argc == 2 && !strcmp(argv[1], "reset")) {
        if (write_cmd("reset\n") != 0)
            return 1;
        printf("all users reset to SILVER\n");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "show"))
        return cmd_show();
    usage();
    return 2;
}
