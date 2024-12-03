#include "generator.h"
#include "finders.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>

// Define a struct to hold thread arguments
typedef struct
{
    int numThread;
    int startRegionX;
    int endRegionX;
    int startRegionZ;
    int endRegionZ;
    char *tempDir;
    int64_t seed;
} ThreadArgs;

void logD(const char *msg)
{
    //log with date before message
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    printf("[%d-%02d-%02d %02d:%02d:%02d] %s\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, msg);
}

// Define a thread function
void *threadFunc(void *arg)
{
    ThreadArgs *args = (ThreadArgs *)arg;

    Pos currPos;
    int regionX, regionZ;
    int64_t seed = args->seed;
    uint64_t s48 = (uint64_t)seed;
    int mc = MC_1_20;
    Generator g;


    
    setupGenerator(&g, mc, 0);
    applySeed(&g, DIM_OVERWORLD, s48);

    // Generate file names with thread number, use temp directory
    char filename[100];
    sprintf(filename, "%s/huts_%03d.txt", args->tempDir, args->numThread);
    FILE *f = fopen(filename, "w");

    char filename2[100];
    sprintf(filename2, "%s/monuments_%03d.txt", args->tempDir, args->numThread);
    FILE *f2 = fopen(filename2, "w");

    long long numHuts = 0;
    long long numMonuments = 0;

    printf("Thread %03d: (%d, %d) to (%d, %d)\n", args->numThread, args->startRegionX, args->startRegionZ, args->endRegionX, args->endRegionZ);

    sleep(5+args->numThread); 

    float lastPrintedPercentage = -1.0f;
    float interval = 0.05f;  // Set the interval for progress updates (0.01%)

    for (regionX = args->startRegionX; regionX < args->endRegionX; regionX++)
    {
        for (regionZ = args->startRegionZ; regionZ < args->endRegionZ; regionZ++)
        {
            uint64_t totalRegions = (uint64_t)(args->endRegionX - args->startRegionX) * (args->endRegionZ - args->startRegionZ);
            uint64_t currRegion = (uint64_t)(regionX - args->startRegionX) * (args->endRegionZ - args->startRegionZ) + (regionZ - args->startRegionZ);
            float perc = (float)currRegion / totalRegions * 100;

            // Print progress only at specified intervals
            if (perc - lastPrintedPercentage >= interval) {
                printf("Thread %03d: %.2f%% - numHuts: %llu - numMonuments: %llu\n", args->numThread, perc, numHuts, numMonuments);
                lastPrintedPercentage = perc;
            }

            getStructurePos(Swamp_Hut, mc, seed, regionX, regionZ, &currPos);
            if (isViableStructurePos(Swamp_Hut, &g, currPos.x, currPos.z, 0))
            {
                numHuts++;
                fprintf(f, "hut->(%d,%d)reg(%d,%d)\n", currPos.x, currPos.z, regionX, regionZ);
            }

            getStructurePos(Monument, mc, seed, regionX, regionZ, &currPos);
            if (isViableStructurePos(Monument, &g, currPos.x, currPos.z, 0))
            {
                numMonuments++;
                fprintf(f2, "monument->(%d,%d)reg(%d,%d)\n", currPos.x, currPos.z, regionX, regionZ);
            }
        }
    }
    
    fclose(f);
    fclose(f2);

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
    scanf("%ld", &seed);


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

    // Divide the map area along the X-axis among threads
    int regionsPerThreadX = regionsAxis / numThreads;
    int startRegionX = minRegion;

    for (int i = 0; i < numThreads; i++)
    {

        threadArgs[i].numThread = i;
        threadArgs[i].tempDir = tempDir;
        threadArgs[i].seed = seed;

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

    return 0;
}
