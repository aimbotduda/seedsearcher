#include "generator.h"
#include "finders.h"
#include "biomes.h"
#include "util.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <string.h>
#include <inttypes.h>
#include <sys/ioctl.h>

// Define a struct to hold thread arguments
typedef struct
{
    int totalThreads;
    int numThread;
    int startRegionX;
    int endRegionX;
    int startRegionZ;
    int endRegionZ;
    char *tempDir;
    int64_t seed;
    // user-selected structures
    int selectedTypes[32];
    const char *selectedLabels[32];
    const char *selectedPrefixes[32];
    int selectedCount;
    // selected MC version
    int mcVersion;
    // per-file flush counters
    unsigned int flushCounters[32];
} ThreadArgs;

typedef struct
{
    pthread_mutex_t lock;
    uint64_t totalRegions;
    uint64_t processedRegions;
    struct timespec startTime;
    int totalThreads;
    volatile int done;
    // dynamic per-structure progress
    int selectedCount;
    const char *selectedLabels[32];
    uint64_t selectedCounts[32];
} Progress;

static Progress g_progress;

static void progress_add_multi(uint64_t processed, const int *incs, int count)
{
    pthread_mutex_lock(&g_progress.lock);
    g_progress.processedRegions += processed;
    for (int i = 0; i < count && i < 32; i++)
    {
        g_progress.selectedCounts[i] += (incs ? (uint64_t)incs[i] : 0ULL);
    }
    pthread_mutex_unlock(&g_progress.lock);
}

static void humanize_time(double s, int *h, int *m, int *sec)
{
    if (s < 0) s = 0;
    int total = (int)(s + 0.5);
    *h = total / 3600;
    total %= 3600;
    *m = total / 60;
    *sec = total % 60;
}

// Returns the current terminal width or a sensible default
static int get_terminal_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col;
    char *cols = getenv("COLUMNS");
    if (cols)
    {
        int c = atoi(cols);
        if (c > 0) return c;
    }
    return 120;
}

static void *progressThread(void *arg)
{
    (void)arg;
    static int last_len = 0;
    for (;;)
    {
        pthread_mutex_lock(&g_progress.lock);
        uint64_t done = g_progress.processedRegions;
        uint64_t total = g_progress.totalRegions;
        int scount = g_progress.selectedCount;
        const char *labels[32];
        uint64_t counts[32];
        for (int i = 0; i < scount && i < 32; i++)
        {
            labels[i] = g_progress.selectedLabels[i];
            counts[i] = g_progress.selectedCounts[i];
        }
        int finished = g_progress.done;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - g_progress.startTime.tv_sec)
            + (now.tv_nsec - g_progress.startTime.tv_nsec) / 1e9;
        pthread_mutex_unlock(&g_progress.lock);

        double perc = total ? (100.0 * (double)done / (double)total) : 0.0;
        double rps = elapsed > 0 ? (double)done / elapsed : 0.0;
        double eta = rps > 0 ? ((double)(total - done)) / rps : 0.0;
        int eh, em, es, th, tm, ts;
        humanize_time(elapsed, &eh, &em, &es);
        humanize_time(eta, &th, &tm, &ts);

        char line[1024];
        char prefix[256];
        // Start with ETA and Reg/s at the beginning, then progress
        snprintf(prefix, sizeof(prefix), "ETA: %02dh%02dm%02ds | Reg/s: %.2f | Progress: %6.2f%%",
            th, tm, ts, rps, perc);

        // Tail: show elapsed if there is space left
        char tail[128];
        snprintf(tail, sizeof(tail), " | Elapsed: %02dh%02dm%02ds", eh, em, es);

        // Build structure tokens
        char tokens[32][64];
        int token_lens[32];
        for (int i = 0; i < scount && i < 32; i++)
        {
            snprintf(tokens[i], sizeof(tokens[i]), "%s: %llu",
                labels[i] ? labels[i] : "struct",
                (unsigned long long)counts[i]);
            token_lens[i] = (int)strlen(tokens[i]);
        }

        int width = get_terminal_width();
        if (width < 40) width = 40;

        // Try to assemble line that fits into terminal width
        int used = 0;
        int hidden = 0;
        // start with prefix
        used = snprintf(line, sizeof(line), "\r%s", prefix);
        // add a space and pipes before tokens
        used += snprintf(line + used, sizeof(line) - used, " | ");

        // add as many tokens as fit
        int first = 1;
        for (int i = 0; i < scount && i < 32; i++)
        {
            int need = token_lens[i] + (first ? 0 : 2); // 2 for ", "
            if (used + need >= width - 1)
            {
                hidden = (scount - i);
                break;
            }
            used += snprintf(line + used, sizeof(line) - used, "%s%s",
                first ? "" : ", ", tokens[i]);
            first = 0;
        }

        // If some tokens hidden, try to append "+N more"
        if (hidden > 0)
        {
            char morebuf[32];
            snprintf(morebuf, sizeof(morebuf), " +%d more", hidden);
            int need = (first ? 0 : 2) + (int)strlen(morebuf);
            if (used + need < width - 1)
            {
                used += snprintf(line + used, sizeof(line) - used, "%s%s",
                    first ? "" : ", ", morebuf);
                first = 0;
            }
        }

        // Try to append tail if it fits
        if (used + (int)strlen(tail) < width - 1)
        {
            used += snprintf(line + used, sizeof(line) - used, "%s", tail);
        }

        // Clear leftovers from previous longer line
        int pad = last_len - used;
        if (pad > 0)
        {
            memset(line + used, ' ', (size_t)pad);
            used += pad;
            line[used] = '\0';
        }

        fputs(line, stdout);
        fflush(stdout);
        last_len = used;

        if (finished) break;
        usleep(200000);
    }
    fprintf(stdout, "\n");
    fflush(stdout);
    return NULL;
}


void logD(const char *msg)
{
    // log with date before message
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    printf("[%d-%02d-%02d %02d:%02d:%02d] %s\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, msg);
}

static int get_structure_dim(int type)
{
    switch (type)
    {
        case Fortress:
        case Bastion:
        case Ruined_Portal_N:
            return DIM_NETHER;
        case End_City:
            return DIM_END;
        default:
            return DIM_OVERWORLD;
    }
}

void *threadFunc(void *arg)
{
    ThreadArgs *args = (ThreadArgs *)arg;

    int64_t seed = args->seed;
    uint64_t s48 = (uint64_t)seed & MASK48;
    int mc = args->mcVersion;
    Generator g;

    setupGenerator(&g, mc, 0);

    FILE *files[32] = {0};
    for (int i = 0; i < args->selectedCount; i++)
    {
        char filename[256];
        snprintf(filename, sizeof(filename), "%s/%s_%03d.txt",
            args->tempDir, args->selectedPrefixes[i], args->numThread);
        files[i] = fopen(filename, "w");
        if (files[i])
            setvbuf(files[i], NULL, _IOFBF, 1 << 20);
        args->flushCounters[i] = 0u;
    }

    // Pre-group selected structures by dimension so applySeed is called
    // at most once per dimension per region instead of once per structure.
    static const int dimOrder[3] = { DIM_OVERWORLD, DIM_NETHER, DIM_END };
    int dimStructIdx[3][32];
    int dimStructCount[3] = {0, 0, 0};
    for (int i = 0; i < args->selectedCount; i++)
    {
        int dim = get_structure_dim(args->selectedTypes[i]);
        for (int d = 0; d < 3; d++)
        {
            if (dimOrder[d] == dim)
            {
                dimStructIdx[d][dimStructCount[d]++] = i;
                break;
            }
        }
    }

    // Thread-local accumulators to avoid locking the global mutex every region
    uint64_t localProcessed = 0;
    int localIncs[32] = {0};

    // Flat nested loop replaces the recursive scanTile (which had no
    // intermediate filtering and only added call overhead).
    for (int rx = args->startRegionX; rx < args->endRegionX; rx++)
    {
        for (int rz = args->startRegionZ; rz < args->endRegionZ; rz++)
        {
            for (int d = 0; d < 3; d++)
            {
                if (dimStructCount[d] == 0)
                    continue;
                int applied = 0;
                for (int k = 0; k < dimStructCount[d]; k++)
                {
                    int i = dimStructIdx[d][k];
                    int type = args->selectedTypes[i];

                    // Fast math-only rejection before expensive biome check
                    Pos pos;
                    if (!getStructurePos(type, mc, s48, rx, rz, &pos))
                        continue;

                    // Lazy applySeed: only when at least one structure
                    // passes the position check in this dimension group
                    if (!applied)
                    {
                        applySeed(&g, dimOrder[d], s48);
                        applied = 1;
                    }
                    if (!isViableStructurePos(type, &g, pos.x, pos.z, 0))
                        continue;

                    fprintf(files[i], "%s->(%d,%d)reg(%d,%d)\n",
                        args->selectedLabels[i], pos.x, pos.z, rx, rz);
                    args->flushCounters[i]++;
                    if ((args->flushCounters[i] & 2047u) == 0u)
                        fflush(files[i]);
                    localIncs[i]++;
                }
            }

            localProcessed++;
            if ((localProcessed & 4095u) == 0u)
            {
                progress_add_multi(localProcessed, localIncs,
                    args->selectedCount);
                localProcessed = 0;
                memset(localIncs, 0, sizeof(localIncs));
            }
        }
    }

    // Flush remaining accumulated progress
    if (localProcessed > 0)
        progress_add_multi(localProcessed, localIncs, args->selectedCount);

    for (int i = 0; i < args->selectedCount; i++)
    {
        if (files[i]) fflush(files[i]);
        if (files[i]) fclose(files[i]);
    }

    return NULL;
}

int main()
{

    int regionsAxis = 117188;
    int maxRegion = 58594;
    int minRegion = -maxRegion;

    // Input for number of threads
    int numThreads;
    printf("Enter the number of threads: ");
    scanf("%d", &numThreads);
    // Drain leftover newline from scanf
    {
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF) {}
    }

    int64_t seed;
    printf("Enter seed (number or string): ");
    char seedInput[256];
    if (fgets(seedInput, sizeof(seedInput), stdin))
    {
        // Remove trailing newline
        size_t len = strlen(seedInput);
        if (len > 0 && seedInput[len - 1] == '\n')
            seedInput[--len] = '\0';
        
        // Check if input is purely numeric (with optional leading minus)
        int isNumeric = 1;
        char *p = seedInput;
        if (*p == '-') p++;  // allow negative sign
        if (*p == '\0') isNumeric = 0;  // empty or just "-"
        while (*p)
        {
            if (*p < '0' || *p > '9')
            {
                isNumeric = 0;
                break;
            }
            p++;
        }
        
        if (isNumeric)
        {
            // Parse as number directly
            seed = strtoll(seedInput, NULL, 10);
        }
        else
        {
            // Convert string to seed using Java's String.hashCode()
            int32_t hash = 0;
            for (size_t i = 0; i < len; i++)
            {
                hash = hash * 31 + (int32_t)(unsigned char)seedInput[i];
            }
            seed = (int64_t)hash;
            printf("String '%s' converted to seed: %" PRId64 "\n", seedInput, seed);
        }
    }
    else
    {
        seed = 0;
    }

    // Select Minecraft version
    printf("Select Minecraft version (enter one index):\n");
    int versionsList[] = {
        MC_B1_7, MC_B1_8,
        MC_1_0, MC_1_1, MC_1_2, MC_1_3, MC_1_4, MC_1_5, MC_1_6, MC_1_7, MC_1_8,
        MC_1_9, MC_1_10, MC_1_11, MC_1_12, MC_1_13, MC_1_14, MC_1_15,
        MC_1_16_1, MC_1_16,
        MC_1_17, MC_1_18,
        MC_1_19_2, MC_1_19,
        MC_1_20,
        MC_1_21_1, MC_1_21_3, MC_1_21_WD
    };
    const int versionsCount = (int)(sizeof(versionsList)/sizeof(versionsList[0]));
    for (int i = 0; i < versionsCount; i++)
    {
        printf("  %d) %s\n", i+1, mc2str(versionsList[i]));
    }
    printf("Your choice (default latest): ");
    fflush(stdout);
    int mcVersion = MC_NEWEST;
    {
        int tmpIdx = 0;
        char vbuf[64];
        if (fgets(vbuf, sizeof(vbuf), stdin))
        {
            tmpIdx = atoi(vbuf);
            if (tmpIdx >= 1 && tmpIdx <= versionsCount)
                mcVersion = versionsList[tmpIdx-1];
        }
    }

    // Present supported structures and read user selection as numbers
    struct { int type; const char *label; const char *prefix; } supported[] = {
        { Desert_Pyramid,    "desert_pyramid",   "desert_pyramids" },
        { Jungle_Temple,     "jungle_temple",    "jungle_temples" },
        { Swamp_Hut,         "hut",              "huts" },
        { Igloo,             "igloo",            "igloos" },
        { Village,           "village",          "villages" },
        { Ocean_Ruin,        "ocean_ruin",       "ocean_ruins" },
        { Shipwreck,         "shipwreck",        "shipwrecks" },
        { Monument,          "monument",         "monuments" },
        { Mansion,           "mansion",          "mansions" },
        { Outpost,           "outpost",          "outposts" },
        { Ruined_Portal,     "ruined_portal",    "ruined_portals" },
        { Ruined_Portal_N,   "ruined_portal_n",  "ruined_portals_nether" },
        { Ancient_City,      "ancient_city",     "ancient_cities" },
        { Treasure,          "treasure",         "treasures" },
        { Fortress,          "fortress",         "fortresses" },
        { Bastion,           "bastion",          "bastions" },
        { End_City,          "end_city",         "end_cities" },
        { Trail_Ruins,       "trail_ruins",      "trail_ruins" },
        { Trial_Chambers,    "trial_chambers",   "trial_chambers" },
    };
    const int supportedCount = (int)(sizeof(supported)/sizeof(supported[0]));

    printf("Select structures to scan (space-separated indices):\n");
    for (int i = 0; i < supportedCount; i++)
    {
        printf("  %d) %s\n", i+1, supported[i].label);
    }
    printf("Your choice (e.g., 1 2 4): ");
    int chosenIdx[32];
    int chosenCount = 0;
    // Read a full line and parse ints
    char line[256];
    if (fgets(line, sizeof(line), stdin))
    {
        char *ctx = NULL;
        char *tok = strtok_r(line, " \t\n", &ctx);
        while (tok && chosenCount < 32)
        {
            int idx = atoi(tok);
            if (1 <= idx && idx <= supportedCount)
                chosenIdx[chosenCount++] = idx-1;
            tok = strtok_r(NULL, " \t\n", &ctx);
        }
    }
    if (chosenCount == 0)
    {
        // default: huts and monuments
        int idxHut = -1, idxMon = -1;
        for (int i = 0; i < supportedCount; i++)
        {
            if (supported[i].type == Swamp_Hut) idxHut = i;
            if (supported[i].type == Monument) idxMon = i;
        }
        if (idxHut >= 0) chosenIdx[chosenCount++] = idxHut;
        if (idxMon >= 0) chosenIdx[chosenCount++] = idxMon;
        if (chosenCount == 0)
        {
            // Fallback: first two entries
            chosenIdx[chosenCount++] = 0;
            if (supportedCount > 1) chosenIdx[chosenCount++] = 1;
        }
    }

    // Ask whether to merge output files when done (recommended for groupfinder)
    int mergeFiles = 1;
    {
        printf("Merge all output files into one when done? (recommended for groupfinder) [Y/n]: ");
        fflush(stdout);
        char mbuf[64];
        if (fgets(mbuf, sizeof(mbuf), stdin))
        {
            if (mbuf[0] == 'n' || mbuf[0] == 'N')
                mergeFiles = 0;
        }
    }

    pthread_t threads[numThreads];
    ThreadArgs threadArgs[numThreads];

    // remove old temp directories
    system("rm -rf tmp*");

    // create new temp directory with date
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    char tempDir[64];
    snprintf(tempDir, sizeof(tempDir), "tmp_%d%02d%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    mkdir(tempDir, 0777);
    printf("Created tmp directory: %s\n", tempDir);

    // Initialize global progress and threads

    // Initialize global progress
    memset(&g_progress, 0, sizeof(g_progress));
    pthread_mutex_init(&g_progress.lock, NULL);
    g_progress.totalRegions = (uint64_t)regionsAxis * (uint64_t)regionsAxis;
    g_progress.totalThreads = numThreads;
    // set selected structures for progress display
    g_progress.selectedCount = (chosenCount <= 32) ? chosenCount : 32;
    for (int i = 0; i < g_progress.selectedCount; i++)
    {
        int sidx = chosenIdx[i];
        g_progress.selectedLabels[i] = supported[sidx].label;
        g_progress.selectedCounts[i] = 0ULL;
    }
    clock_gettime(CLOCK_MONOTONIC, &g_progress.startTime);

    pthread_t progThread;
    pthread_create(&progThread, NULL, progressThread, NULL);

    // Divide the map area along the X-axis among threads
    int regionsPerThreadX = regionsAxis / numThreads;
    int startRegionX = minRegion;

    for (int i = 0; i < numThreads; i++)
    {

        threadArgs[i].numThread = i;
        threadArgs[i].tempDir = tempDir;
        threadArgs[i].seed = seed;
        threadArgs[i].totalThreads = numThreads;
        // selected structures
        threadArgs[i].selectedCount = chosenCount;
        for (int k = 0; k < chosenCount; k++)
        {
            int sidx = chosenIdx[k];
            threadArgs[i].selectedTypes[k] = supported[sidx].type;
            threadArgs[i].selectedLabels[k] = supported[sidx].label;
            threadArgs[i].selectedPrefixes[k] = supported[sidx].prefix;
        }

        // Set chosen MC version
        threadArgs[i].mcVersion = mcVersion;

        // Calculate end region for X-axis
        int endRegionX = startRegionX + regionsPerThreadX;
        if (i == numThreads - 1) // Last thread takes the remaining regions
            endRegionX = maxRegion;

        threadArgs[i].startRegionX = startRegionX;
        threadArgs[i].endRegionX = endRegionX;
        threadArgs[i].startRegionZ = minRegion; // Constant Z-axis
        threadArgs[i].endRegionZ = maxRegion;   // Constant Z-axis

        // Update start region for next thread
        startRegionX = endRegionX;

        // Create thread
        pthread_create(&threads[i], NULL, threadFunc, (void *)&threadArgs[i]);
    }

    // Wait for all threads to finish
    for (int i = 0; i < numThreads; i++)
    {
        pthread_join(threads[i], NULL);
    }

    // Signal progress thread to finish and join
    pthread_mutex_lock(&g_progress.lock);
    g_progress.done = 1;
    pthread_mutex_unlock(&g_progress.lock);
    pthread_join(progThread, NULL);

    // Merge all per-thread output files into one file per structure type,
    // then combine everything into a single file for groupfinder
    if (mergeFiles)
    {
        char mergedPath[128];
        snprintf(mergedPath, sizeof(mergedPath), "%s/all_structures.txt", tempDir);
        FILE *merged = fopen(mergedPath, "w");
        if (merged)
        {
            setvbuf(merged, NULL, _IOFBF, 1 << 20);
            uint64_t totalLines = 0;
            for (int k = 0; k < chosenCount; k++)
            {
                int sidx = chosenIdx[k];
                for (int thr = 0; thr < numThreads; thr++)
                {
                    char fname[256];
                    snprintf(fname, sizeof(fname), "%s/%s_%03d.txt",
                        tempDir, supported[sidx].prefix, thr);
                    FILE *in = fopen(fname, "r");
                    if (!in) continue;
                    char buf[8192];
                    size_t n;
                    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
                    {
                        fwrite(buf, 1, n, merged);
                        for (size_t b = 0; b < n; b++)
                            if (buf[b] == '\n') totalLines++;
                    }
                    fclose(in);
                }
            }
            fclose(merged);
            printf("Merged %llu structures into: %s\n",
                (unsigned long long)totalLines, mergedPath);
        }
        else
        {
            fprintf(stderr, "Warning: could not create merged file %s\n", mergedPath);
        }
    }

    return 0;
}
