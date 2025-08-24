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
} ThreadArgs;

typedef struct
{
    pthread_mutex_t lock;
    uint64_t totalRegions;
    uint64_t processedRegions;
    uint64_t totalHuts;
    uint64_t totalMonuments;
    struct timespec startTime;
    int totalThreads;
    volatile int done;
} Progress;

static Progress g_progress;

static void progress_add(uint64_t processed, uint64_t huts, uint64_t mons)
{
    pthread_mutex_lock(&g_progress.lock);
    g_progress.processedRegions += processed;
    g_progress.totalHuts += huts;
    g_progress.totalMonuments += mons;
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

static void *progressThread(void *arg)
{
    (void)arg;
    for (;;)
    {
        pthread_mutex_lock(&g_progress.lock);
        uint64_t done = g_progress.processedRegions;
        uint64_t total = g_progress.totalRegions;
        uint64_t huts = g_progress.totalHuts;
        uint64_t mons = g_progress.totalMonuments;
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

        fprintf(stdout,
            "\rProgress: %6.2f%% | Huts: %llu | Monuments: %llu | Reg/s: %.2f | ETA: %02dh%02dm%02ds | Elapsed: %02dh%02dm%02ds",
            perc, (unsigned long long)huts, (unsigned long long)mons, rps,
            th, tm, ts, eh, em, es);
        fflush(stdout);

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

// Define a thread function

static int check_and_record_structure(int type, const char *label,
    int mc, uint64_t s48, int regX, int regZ, Generator *g, FILE *out)
{
    Pos pos;
    if (!getStructurePos(type, mc, s48, regX, regZ, &pos))
        return 0;
    // Select dimension based on structure type
    int dim = DIM_OVERWORLD;
    switch (type)
    {
        case Fortress:
        case Bastion:
        case Ruined_Portal_N:
            dim = DIM_NETHER; break;
        case End_City:
            dim = DIM_END; break;
        default:
            dim = DIM_OVERWORLD; break;
    }
    applySeed(g, dim, s48);
    if (!isViableStructurePos(type, g, pos.x, pos.z, 0))
        return 0;
    fprintf(out, "%s->(%d,%d)reg(%d,%d)\n", label, pos.x, pos.z, regX, regZ);
    return 1;
}

static void scanTile(ThreadArgs *args, Generator *g, uint64_t s48,
    int minRX, int minRZ, int maxRX, int maxRZ,
    FILE **files, long long *numHuts, long long *numMons)
{
    const int mc = args->mcVersion;

    int tileW = maxRX - minRX;
    int tileH = maxRZ - minRZ;

    if (tileW == 1 && tileH == 1)
    {
        uint64_t processed = 1;
        int incHut = 0;
        int incMonTotal = 0;
        for (int i = 0; i < args->selectedCount; i++)
        {
            int type = args->selectedTypes[i];
            int inc = check_and_record_structure(type, args->selectedLabels[i], mc, s48, minRX, minRZ, g, files[i]);
            if (inc)
            {
                if (type == Swamp_Hut) { (*numHuts) += inc; incHut += inc; }
                else { (*numMons) += inc; incMonTotal += inc; }
            }
        }

        // update global progress including new finds
        progress_add(processed, (uint64_t)incHut, (uint64_t)incMonTotal);
        return;
    }

    // Subdivide
    int midRX = minRX + tileW / 2;
    int midRZ = minRZ + tileH / 2;
    if (tileW >= tileH)
    {
        // split in X
        if (midRX > minRX)
            scanTile(args, g, s48, minRX, minRZ, midRX, maxRZ, files, numHuts, numMons);
        if (maxRX > midRX)
            scanTile(args, g, s48, midRX, minRZ, maxRX, maxRZ, files, numHuts, numMons);
    }
    else
    {
        // split in Z
        if (midRZ > minRZ)
            scanTile(args, g, s48, minRX, minRZ, maxRX, midRZ, files, numHuts, numMons);
        if (maxRZ > midRZ)
            scanTile(args, g, s48, minRX, midRZ, maxRX, maxRZ, files, numHuts, numMons);
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
    applySeed(&g, DIM_OVERWORLD, s48);

    // Generate file names with thread number, use temp directory
    FILE *files[32] = {0};
    for (int i = 0; i < args->selectedCount; i++)
    {
        char filename[128];
        sprintf(filename, "%s/%s_%03d.txt", args->tempDir, args->selectedPrefixes[i], args->numThread);
        files[i] = fopen(filename, "w");
        setvbuf(files[i], NULL, _IOFBF, 1<<20);
    }

    long long numHuts = 0;
    long long numMonuments = 0;

    // Recursive, coarse-to-fine scanning with biome prefilters.
    scanTile(args, &g, s48, args->startRegionX, args->startRegionZ, args->endRegionX, args->endRegionZ, files, &numHuts, &numMonuments);

    for (int i = 0; i < args->selectedCount; i++)
    {
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

    int64_t seed;
    printf("Enter seed: ");
    scanf("%" SCNd64, &seed);

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
    int mcVersion = MC_NEWEST;
    {
        int tmpIdx = 0;
        // consume newline from previous scanf
        getchar();
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
        char *tok = strtok(line, " \t\n");
        while (tok && chosenCount < 32)
        {
            int idx = atoi(tok);
            if (1 <= idx && idx <= supportedCount)
                chosenIdx[chosenCount++] = idx-1;
            tok = strtok(NULL, " \t\n");
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


    pthread_t threads[numThreads];
    ThreadArgs threadArgs[numThreads];

    // remove old temp directories
    system("rm -rf tmp*");

    // create new temp directory with date
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    char tempDir[20];
    sprintf(tempDir, "tmp_%d%02d%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    mkdir(tempDir, 0777);
    printf("Created tmp directory: %s\n", tempDir);

    // Initialize global progress and threads

    // Initialize global progress
    memset(&g_progress, 0, sizeof(g_progress));
    pthread_mutex_init(&g_progress.lock, NULL);
    g_progress.totalRegions = (uint64_t)regionsAxis * (uint64_t)regionsAxis;
    g_progress.totalThreads = numThreads;
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

    return 0;
}
