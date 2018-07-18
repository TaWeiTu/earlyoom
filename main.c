// SPDX-License-Identifier: MIT

/* Check available memory and swap in a loop and start killing
 * processes if they get too low */

#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include "kill.h"
#include "meminfo.h"

/* Arbitrary identifiers for long options that do not have a short
 * version */
enum {
    LONG_OPT_PREFER = 513,
    LONG_OPT_AVOID,
    LONG_OPT_HELP,
};

static int set_oom_score_adj(int);
static void print_mem_stats(FILE* procdir, const meminfo_t m);
static void poll_loop(const poll_loop_args_t args);

int enable_debug = 0;
long page_size = 0;

int main(int argc, char* argv[])
{
    poll_loop_args_t args = {
        .report_interval_ms = 1000,
        /* omitted fields are set to zero */
    };
    long mem_min_kib = 0, swap_min_kib = 0; /* Same thing in KiB */
    int set_my_priority = 0;
    char* prefer_cmds = NULL;
    char* avoid_cmds = NULL;
    regex_t _prefer_regex;
    regex_t _avoid_regex;
    page_size = sysconf(_SC_PAGESIZE);

    /* request line buffering for stdout - otherwise the output
     * may lag behind stderr */
    setlinebuf(stdout);

    fprintf(stderr, "earlyoom " VERSION "\n");

    if (chdir("/proc") != 0) {
        perror("Could not cd to /proc");
        exit(4);
    }

    args.procdir = opendir(".");
    if (args.procdir == NULL) {
        perror("Could not open /proc");
        exit(5);
    }

    meminfo_t m = parse_meminfo();

    int c;
    const char* short_opt = "m:s:M:S:kinN:dvr:ph";
    struct option long_opt[] = {
        { "prefer", required_argument, NULL, LONG_OPT_PREFER },
        { "avoid", required_argument, NULL, LONG_OPT_AVOID },
        { "help", no_argument, NULL, LONG_OPT_HELP },
        { 0, 0, NULL, 0 } /* end-of-array marker */
    };

    while ((c = getopt_long(argc, argv, short_opt, long_opt, NULL)) != -1) {
        float report_interval_f = 0;
        switch (c) {
        case -1: /* no more arguments */
        case 0: /* long option toggles */
            break;
        case 'm':
            args.mem_min_percent = strtol(optarg, NULL, 10);
            // Using "-m 100" makes no sense
            if (args.mem_min_percent <= 0 || args.mem_min_percent >= 100) {
                fprintf(stderr, "-m: Invalid percentage: %s\n", optarg);
                exit(15);
            }
            break;
        case 's':
            args.swap_min_percent = strtol(optarg, NULL, 10);
            // Using "-s 100" is a valid way to ignore swap usage
            if (args.swap_min_percent <= 0 || args.swap_min_percent > 100) {
                fprintf(stderr, "-s: Invalid percentage: %s\n", optarg);
                exit(16);
            }
            break;
        case 'M':
            mem_min_kib = strtol(optarg, NULL, 10);
            if (mem_min_kib <= 0) {
                fprintf(stderr, "-M: Invalid KiB value\n");
                exit(15);
            }
            break;
        case 'S':
            swap_min_kib = strtol(optarg, NULL, 10);
            if (swap_min_kib <= 0) {
                fprintf(stderr, "-S: Invalid KiB value\n");
                exit(16);
            }
            break;
        case 'k':
            fprintf(stderr, "Option -k is ignored since earlyoom v1.2\n");
            break;
        case 'i':
            args.ignore_oom_score_adj = 1;
            break;
        case 'n':
            args.notif_command = "notify-send";
            break;
        case 'N':
            args.notif_command = optarg;
            break;
        case 'd':
            enable_debug = 1;
            break;
        case 'v':
            // The version has already been printed above
            exit(0);
        case 'r':
            report_interval_f = strtof(optarg, NULL);
            if (report_interval_f < 0) {
                fprintf(stderr, "-r: Invalid interval\n");
                exit(14);
            }
            args.report_interval_ms = report_interval_f * 1000;
            break;
        case 'p':
            set_my_priority = 1;
            break;
        case LONG_OPT_PREFER:
            prefer_cmds = optarg;
            break;
        case LONG_OPT_AVOID:
            avoid_cmds = optarg;
            break;
        case 'h':
        case LONG_OPT_HELP:
            fprintf(stderr,
                "Usage: earlyoom [OPTION]...\n"
                "\n"
                "  -m PERCENT       set available memory minimum to PERCENT of total (default 10 %%)\n"
                "  -s PERCENT       set free swap minimum to PERCENT of total (default 10 %%)\n"
                "  -M SIZE          set available memory minimum to SIZE KiB\n"
                "  -S SIZE          set free swap minimum to SIZE KiB\n"
                "  -i               user-space oom killer should ignore positive oom_score_adj values\n"
                "  -n               enable notifications using \"notify-send\"\n"
                "  -N COMMAND       enable notifications using COMMAND\n"
                "  -d               enable debugging messages\n"
                "  -v               print version information and exit\n"
                "  -r INTERVAL      memory report interval in seconds (default 1), set to 0 to\n"
                "                   disable completely\n"
                "  -p               set niceness of earlyoom to -20 and oom_score_adj to -1000\n"
                "  --prefer REGEX   prefer killing processes matching REGEX\n"
                "  --avoid REGEX    avoid killing processes matching REGEX\n"
                "  -h, --help       this help text\n");
            exit(0);
        case '?':
            fprintf(stderr, "Try 'earlyoom --help' for more information.\n");
            exit(13);
        }
    }

    if (args.mem_min_percent && mem_min_kib) {
        fprintf(stderr, "Can't use -m with -M\n");
        exit(2);
    }

    if (args.swap_min_percent && swap_min_kib) {
        fprintf(stderr, "Can't use -s with -S\n");
        exit(2);
    }

    if (prefer_cmds) {
        args.prefer_regex = &_prefer_regex;
        if (regcomp(args.prefer_regex, prefer_cmds, REG_EXTENDED | REG_NOSUB) != 0) {
            fprintf(stderr, "Could not compile regexp: %s\n", prefer_cmds);
            exit(6);
        }
        fprintf(stderr, "Prefering to kill process names that match regex '%s'\n", prefer_cmds);
    }

    if (avoid_cmds) {
        args.avoid_regex = &_avoid_regex;
        if (regcomp(args.avoid_regex, avoid_cmds, REG_EXTENDED | REG_NOSUB) != 0) {
            fprintf(stderr, "Could not compile regexp: %s\n", avoid_cmds);
            exit(6);
        }
        fprintf(stderr, "Avoiding to kill process names that match regex '%s'\n", avoid_cmds);
    }

    if (mem_min_kib) {
        if (mem_min_kib >= m.MemTotalKiB) {
            fprintf(stderr,
                "-M: the value you passed (%ld kiB) is at or above total memory (%ld kiB)\n",
                mem_min_kib, m.MemTotalKiB);
            exit(15);
        }
        args.mem_min_percent = 100 * mem_min_kib / m.MemTotalKiB;
    } else {
        if (!args.mem_min_percent) {
            args.mem_min_percent = 10;
        }
    }

    if (swap_min_kib) {
        if (swap_min_kib > m.SwapTotalKiB) {
            fprintf(stderr,
                "-S: the value you passed (%ld kiB) is above total swap (%ld kiB)\n",
                swap_min_kib, m.SwapTotalKiB);
            exit(16);
        }
        args.swap_min_percent = 100 * swap_min_kib / m.SwapTotalKiB;
    } else {
        if (!args.swap_min_percent) {
            args.swap_min_percent = 10;
        }
    }

    fprintf(stderr, "mem  total: %4d MiB, min: %2d %%\n",
        m.MemTotalMiB, args.mem_min_percent);
    fprintf(stderr, "swap total: %4d MiB, min: %2d %%\n",
        m.SwapTotalMiB, args.swap_min_percent);

    if (args.notif_command)
        fprintf(stderr, "notifications enabled using command: %s\n",
            args.notif_command);

    /* Dry-run oom kill to make sure stack grows to maximum size before
     * calling mlockall()
     */
    userspace_kill(args, 0);

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        perror("Could not lock memory - continuing anyway");

    if (set_my_priority) {
        if (setpriority(PRIO_PROCESS, 0, -20) != 0)
            perror("Could not set priority - continuing anyway");

        if (set_oom_score_adj(-1000) != 0)
            perror("Could not set oom_score_adj to -1000 for earlyoom process - continuing anyway");
    }
    // Jump into main poll loop
    poll_loop(args);
    return 0;
}

/* Print a status line like
 *   mem avail: 5259 MiB (67 %), swap free: 0 MiB (0 %)"
 * to the fd passed in out_fd.
 */
static void print_mem_stats(FILE* out_fd, const meminfo_t m)
{
    fprintf(out_fd,
        "mem avail: %4d of %4d MiB (%2d %%), swap free: %4d of %4d MiB (%2d %%)\n",
        m.MemAvailableMiB,
        m.MemTotalMiB,
        m.MemAvailablePercent,
        m.SwapFreeMiB,
        m.SwapTotalMiB,
        m.SwapFreePercent);
}

static int set_oom_score_adj(int oom_score_adj)
{
    char buf[256];
    pid_t pid = getpid();

    snprintf(buf, sizeof(buf), "%d/oom_score_adj", pid);
    FILE* f = fopen(buf, "w");
    if (f == NULL) {
        return -1;
    }

    fprintf(f, "%d", oom_score_adj);
    fclose(f);

    return 0;
}

/* Calculate the time we should sleep based upon how far away from the memory and swap
 * limits we are (headroom). Returns a millisecond value between 100 and 1000 (inclusive).
 * The idea is simple: if memory and swap can only fill up so fast, we know how long we can sleep
 * without risking to miss a low memory event.
 */
static int sleep_time_ms(const poll_loop_args_t* args, const meminfo_t* m)
{
    // Maximum expected memory/swap fill rate. In kiB per millisecond ==~ MiB per second.
    const int mem_fill_rate = 6000; // 6000MiB/s seen with "stress -m 4 --vm-bytes 4G"
    const int swap_fill_rate = 800; //  800MiB/s seen with membomb on ZRAM
    // Clamp calculated value to this range (milliseconds)
    const int min_sleep = 100;
    const int max_sleep = 1000;

    int mem_headroom_kib = (m->MemAvailablePercent - args->mem_min_percent) * 10 * m->MemTotalMiB;
    if (mem_headroom_kib < 0) {
        mem_headroom_kib = 0;
    }
    int swap_headroom_kib = (m->SwapFreePercent - args->swap_min_percent) * 10 * m->SwapTotalMiB;
    if (swap_headroom_kib < 0) {
        swap_headroom_kib = 0;
    }
    int ms = mem_headroom_kib / mem_fill_rate + swap_headroom_kib / swap_fill_rate;
    if (ms < min_sleep) {
        return min_sleep;
    }
    if (ms > max_sleep) {
        return max_sleep;
    }
    return ms;
}

static void poll_loop(const poll_loop_args_t args)
{
    meminfo_t m = {0};
    int report_countdown_ms = 0;
    // extra time to sleep after a kill
    const int cooldown_ms = 200;

    while (1) {
        m = parse_meminfo();

        if (m.MemAvailablePercent <= args.mem_min_percent && m.SwapFreePercent <= args.swap_min_percent) {
            fprintf(stderr,
                "Low memory! mem avail: %d of %d MiB (%d) %% <= min %d %%, swap free: %d of %d MiB (%d %%) <= min %d %%\n",
                m.MemAvailableMiB,
                m.MemTotalMiB,
                m.MemAvailablePercent,
                args.mem_min_percent,
                m.SwapFreeMiB,
                m.SwapTotalMiB,
                m.SwapFreePercent,
                args.swap_min_percent);
            userspace_kill(args, 9);
            // With swap enabled, the kernel seems to need more than 100ms to free the memory
            // of the killed process. This means that earlyoom would immediately kill another
            // process. Sleep a little extra to give the kernel time to free the memory.
            // (Yes, this will sleep even if the kill has failed. Does no harm and keeps the
            // code simple.)
            if (m.SwapTotalMiB > 0) {
                usleep(cooldown_ms*1000);
                report_countdown_ms -= cooldown_ms;
            }
        } else if (args.report_interval_ms && report_countdown_ms <= 0) {
            print_mem_stats(stdout, m);
            report_countdown_ms = args.report_interval_ms;
        }
        int sleep_ms = sleep_time_ms(&args, &m);
        if (enable_debug) {
            printf("adaptive sleep time: %d ms\n", sleep_ms);
        }
        usleep(sleep_ms*1000);
        report_countdown_ms -= sleep_ms;
    }
}
