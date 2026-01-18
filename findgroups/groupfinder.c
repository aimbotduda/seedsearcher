/*
 * groupfinder.c - Auto-optimizing structure group finder
 * 
 * Finds groups of 3 or 4 structures within a specified radius.
 * Automatically detects system memory and optimizes strategy:
 *   - High RAM (>64GB): Maximum performance, precomputed cell coords
 *   - Medium RAM (32-64GB): Balanced approach
 *   - Low RAM (<32GB): Memory-efficient, computed on-the-fly
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

/* ============================================================================
 * Configuration - Auto-tuned at runtime
 * ========================================================================== */

#define MAX_LINE_LENGTH 256
#define AVG_BYTES_PER_LINE 35

/* Memory thresholds */
#define HIGH_MEM_THRESHOLD  (64ULL * 1024 * 1024 * 1024)   /* 64 GB */
#define MED_MEM_THRESHOLD   (32ULL * 1024 * 1024 * 1024)   /* 32 GB */

/* Runtime configuration */
typedef enum {
    MODE_HIGH_PERF,     /* Lots of RAM: cell_size = radius, precomputed coords */
    MODE_BALANCED,      /* Medium RAM: cell_size = 2*radius */
    MODE_LOW_MEM        /* Low RAM: cell_size = 4*radius, compute on-the-fly */
} OptMode;

static OptMode g_mode = MODE_LOW_MEM;
static int g_cell_multiplier = 4;
static uint64_t g_system_memory = 0;

/* ============================================================================
 * Data Structures
 * ========================================================================== */

/* High-perf mode: 24 bytes with precomputed cell coords */
typedef struct {
    int32_t x;
    int32_t z;
    int64_t cellX;
    int64_t cellZ;
} StructureFast;

/* Low-mem mode: 8 bytes, compute cell coords on demand */
typedef struct {
    int32_t x;
    int32_t z;
} StructureCompact;

/* Union to handle both modes */
typedef union {
    StructureFast fast;
    StructureCompact compact;
} Structure;

/* Compact cell entry */
typedef struct {
    int64_t cellX;
    int64_t cellZ;
    uint32_t start;
    uint32_t count;
    uint32_t next;
} CellEntry;

/* Thread work */
typedef struct {
    int thread_id;
    int num_threads;
    void *structures;           /* Either StructureFast* or StructureCompact* */
    uint64_t num_structures;
    CellEntry *cells;
    uint64_t num_cells;
    uint32_t *hash_table;
    uint64_t hash_table_size;
    int64_t radius;
    int64_t radius_sq;
    int64_t cell_size;
    FILE *output;
    pthread_mutex_t *output_lock;
    uint64_t groups_found_3;
    uint64_t groups_found_4;
    uint64_t cells_processed;
    uint32_t *neighbors_buf;
    uint32_t neighbors_buf_size;
} ThreadWork;

/* Globals */
static pthread_mutex_t g_progress_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_total_cells = 0;
static uint64_t g_processed_cells = 0;
static volatile int g_done = 0;
static struct timespec g_start_time;

static void *g_structures = NULL;
static uint64_t g_structures_count = 0;
static uint64_t g_structures_capacity = 0;
static CellEntry *g_cells = NULL;
static uint64_t g_cells_count = 0;
static uint32_t *g_hash_table = NULL;
static uint64_t g_hash_table_size = 0;
static int64_t g_cell_size = 0;

/* ============================================================================
 * System Detection
 * ========================================================================== */

static uint64_t get_system_memory(void)
{
#if defined(__APPLE__)
    int mib[2] = { CTL_HW, HW_MEMSIZE };
    uint64_t mem = 0;
    size_t len = sizeof(mem);
    if (sysctl(mib, 2, &mem, &len, NULL, 0) == 0)
        return mem;
#elif defined(__linux__)
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0)
        return (uint64_t)pages * (uint64_t)page_size;
#endif
    return 8ULL * 1024 * 1024 * 1024;  /* Default 8GB if unknown */
}

static void detect_and_configure(uint64_t estimated_structures)
{
    g_system_memory = get_system_memory();
    
    /* Estimate memory needed for high-perf mode */
    uint64_t high_perf_mem = estimated_structures * sizeof(StructureFast) +
                             estimated_structures * sizeof(CellEntry) +
                             (1ULL << 27) * sizeof(uint32_t);  /* Max hash table */
    
    /* Estimate for balanced mode */
    uint64_t balanced_mem = estimated_structures * sizeof(StructureFast) +
                            (estimated_structures / 4) * sizeof(CellEntry) +
                            (1ULL << 26) * sizeof(uint32_t);
    
    /* Estimate for low-mem mode */
    uint64_t low_mem_need = estimated_structures * sizeof(StructureCompact) +
                            (estimated_structures / 16) * sizeof(CellEntry) +
                            (1ULL << 24) * sizeof(uint32_t);
    
    /* Leave 20% headroom for OS and other processes */
    uint64_t available = (g_system_memory * 80) / 100;
    
    if (available >= high_perf_mem && g_system_memory >= HIGH_MEM_THRESHOLD) {
        g_mode = MODE_HIGH_PERF;
        g_cell_multiplier = 1;
    } else if (available >= balanced_mem && g_system_memory >= MED_MEM_THRESHOLD) {
        g_mode = MODE_BALANCED;
        g_cell_multiplier = 2;
    } else {
        g_mode = MODE_LOW_MEM;
        g_cell_multiplier = 4;
    }
    
    /* Safety check: if even low-mem won't fit, increase multiplier */
    while (low_mem_need > available && g_cell_multiplier < 16) {
        g_cell_multiplier *= 2;
        low_mem_need = estimated_structures * sizeof(StructureCompact) +
                       (estimated_structures / (g_cell_multiplier * g_cell_multiplier)) * sizeof(CellEntry) +
                       (1ULL << 22) * sizeof(uint32_t);
    }
    
    const char *mode_str = (g_mode == MODE_HIGH_PERF) ? "HIGH PERFORMANCE" :
                           (g_mode == MODE_BALANCED) ? "BALANCED" : "MEMORY EFFICIENT";
    
    fprintf(stderr, "\n=== System Auto-Configuration ===\n");
    fprintf(stderr, "  System RAM: %.1f GB\n", g_system_memory / (1024.0 * 1024.0 * 1024.0));
    fprintf(stderr, "  Available (80%%): %.1f GB\n", available / (1024.0 * 1024.0 * 1024.0));
    fprintf(stderr, "  Mode: %s\n", mode_str);
    fprintf(stderr, "  Cell size: %d× radius\n", g_cell_multiplier);
    fprintf(stderr, "  Structure size: %zu bytes\n", 
            (g_mode == MODE_HIGH_PERF || g_mode == MODE_BALANCED) ? 
            sizeof(StructureFast) : sizeof(StructureCompact));
    fprintf(stderr, "\n");
}

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

static inline int64_t coord_to_cell(int32_t coord, int64_t cell_size)
{
    if (coord >= 0)
        return coord / cell_size;
    return (coord - cell_size + 1) / cell_size;
}

static inline uint64_t hash_cell(int64_t cx, int64_t cz, uint64_t table_size)
{
    uint64_t h = 14695981039346656037ULL;
    h ^= (uint64_t)cx;
    h *= 1099511628211ULL;
    h ^= (uint64_t)cz;
    h *= 1099511628211ULL;
    return h & (table_size - 1);
}

static double elapsed_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - g_start_time.tv_sec) +
           (now.tv_nsec - g_start_time.tv_nsec) / 1e9;
}

static void print_progress(const char *phase, uint64_t current, uint64_t total)
{
    double pct = total > 0 ? (100.0 * current / total) : 0.0;
    double elapsed = elapsed_seconds();
    double rate = elapsed > 0 ? current / elapsed : 0.0;
    double eta = rate > 0 ? (total - current) / rate : 0.0;

    int eh = (int)(elapsed / 3600);
    int em = ((int)(elapsed) % 3600) / 60;
    int es = (int)(elapsed) % 60;
    int th = (int)(eta / 3600);
    int tm = ((int)(eta) % 3600) / 60;
    int ts = (int)(eta) % 60;

    fprintf(stderr, "\r%s: %6.2f%% | %.0f/s | Elapsed: %02d:%02d:%02d | ETA: %02d:%02d:%02d    ",
            phase, pct, rate, eh, em, es, th, tm, ts);
    fflush(stderr);
}

/* ============================================================================
 * Memory Management
 * ========================================================================== */

static size_t structure_size(void)
{
    return (g_mode == MODE_HIGH_PERF || g_mode == MODE_BALANCED) ? 
           sizeof(StructureFast) : sizeof(StructureCompact);
}

static bool preallocate_structures(size_t file_size)
{
    uint64_t estimated_count = file_size / AVG_BYTES_PER_LINE;
    uint64_t cap = (estimated_count * 11) / 10;
    if (cap < 1024) cap = 1024;
    
    /* Round up to power of 2 */
    uint64_t p2 = 1;
    while (p2 < cap) p2 *= 2;
    cap = p2;
    
    size_t elem_size = structure_size();
    g_structures = malloc(cap * elem_size);
    if (!g_structures) {
        fprintf(stderr, "Error: Failed to allocate %.2f GB for structures\n",
                (cap * elem_size) / (1024.0 * 1024.0 * 1024.0));
        return false;
    }
    
    g_structures_capacity = cap;
    fprintf(stderr, "Allocated %.2f GB for ~%lu structures\n",
            (cap * elem_size) / (1024.0 * 1024.0 * 1024.0),
            (unsigned long)estimated_count);
    
    return true;
}

static bool ensure_capacity(uint64_t needed)
{
    if (needed <= g_structures_capacity)
        return true;

    uint64_t new_cap = g_structures_capacity ? g_structures_capacity : 1024;
    while (new_cap < needed)
        new_cap *= 2;

    size_t elem_size = structure_size();
    void *new_arr = realloc(g_structures, new_cap * elem_size);
    if (!new_arr)
        return false;

    g_structures = new_arr;
    g_structures_capacity = new_cap;
    return true;
}

/* ============================================================================
 * File Parsing
 * ========================================================================== */

static bool parse_line(const char *line, int32_t *x, int32_t *z)
{
    const char *arrow = strstr(line, "->");
    if (!arrow) return false;

    const char *p = arrow + 2;
    if (*p != '(') return false;
    p++;

    char *end;
    long lx = strtol(p, &end, 10);
    if (end == p || *end != ',') return false;

    p = end + 1;
    long lz = strtol(p, &end, 10);
    if (end == p || *end != ')') return false;

    *x = (int32_t)lx;
    *z = (int32_t)lz;
    return true;
}

static uint64_t parse_file(const char *filename)
{
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open input file");
        return 0;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("Failed to stat input file");
        close(fd);
        return 0;
    }

    size_t file_size = st.st_size;
    if (file_size == 0) {
        fprintf(stderr, "Input file is empty\n");
        close(fd);
        return 0;
    }

    fprintf(stderr, "Parsing file: %s (%.2f GB)\n",
            filename, file_size / (1024.0 * 1024.0 * 1024.0));

    char *data = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        perror("Failed to mmap input file");
        close(fd);
        return 0;
    }

    madvise(data, file_size, MADV_SEQUENTIAL);

    if (!preallocate_structures(file_size)) {
        munmap(data, file_size);
        close(fd);
        return 0;
    }

    clock_gettime(CLOCK_MONOTONIC, &g_start_time);

    uint64_t progress_interval = (file_size / AVG_BYTES_PER_LINE) / 100;
    if (progress_interval < 100000) progress_interval = 100000;

    char line[MAX_LINE_LENGTH];
    const char *p = data;
    const char *end = data + file_size;
    uint64_t line_count = 0;
    bool use_fast = (g_mode == MODE_HIGH_PERF || g_mode == MODE_BALANCED);

    while (p < end) {
        const char *eol = p;
        while (eol < end && *eol != '\n') eol++;

        size_t len = eol - p;
        if (len >= MAX_LINE_LENGTH) len = MAX_LINE_LENGTH - 1;
        memcpy(line, p, len);
        line[len] = '\0';

        int32_t x, z;
        if (parse_line(line, &x, &z)) {
            if (!ensure_capacity(g_structures_count + 1)) {
                munmap(data, file_size);
                close(fd);
                return 0;
            }

            if (use_fast) {
                StructureFast *arr = (StructureFast *)g_structures;
                arr[g_structures_count].x = x;
                arr[g_structures_count].z = z;
                arr[g_structures_count].cellX = 0;  /* Computed later */
                arr[g_structures_count].cellZ = 0;
            } else {
                StructureCompact *arr = (StructureCompact *)g_structures;
                arr[g_structures_count].x = x;
                arr[g_structures_count].z = z;
            }
            g_structures_count++;
        }

        line_count++;
        if ((line_count % progress_interval) == 0) {
            print_progress("Parsing", (uint64_t)(p - data), file_size);
        }

        p = eol + 1;
    }

    munmap(data, file_size);
    close(fd);

    fprintf(stderr, "\rParsing: 100.00%% complete                                        \n");
    fprintf(stderr, "Parsed %lu structures\n", (unsigned long)g_structures_count);

    return g_structures_count;
}

/* ============================================================================
 * Sorting
 * ========================================================================== */

/* Fast mode: precomputed cell coords */
static int compare_fast(const void *a, const void *b)
{
    const StructureFast *sa = (const StructureFast *)a;
    const StructureFast *sb = (const StructureFast *)b;

    if (sa->cellX < sb->cellX) return -1;
    if (sa->cellX > sb->cellX) return 1;
    if (sa->cellZ < sb->cellZ) return -1;
    if (sa->cellZ > sb->cellZ) return 1;
    return 0;
}

/* Compact mode: compute on-the-fly */
static int compare_compact(const void *a, const void *b)
{
    const StructureCompact *sa = (const StructureCompact *)a;
    const StructureCompact *sb = (const StructureCompact *)b;
    
    int64_t cellXa = coord_to_cell(sa->x, g_cell_size);
    int64_t cellXb = coord_to_cell(sb->x, g_cell_size);
    int64_t cellZa = coord_to_cell(sa->z, g_cell_size);
    int64_t cellZb = coord_to_cell(sb->z, g_cell_size);

    if (cellXa < cellXb) return -1;
    if (cellXa > cellXb) return 1;
    if (cellZa < cellZb) return -1;
    if (cellZa > cellZb) return 1;
    return 0;
}

/* ============================================================================
 * Spatial Index Building
 * ========================================================================== */

static bool build_spatial_index(int64_t radius)
{
    int64_t cell_size = radius * g_cell_multiplier;
    g_cell_size = cell_size;
    
    fprintf(stderr, "Building spatial index (cell size: %ld = %d× radius)...\n",
            (long)cell_size, g_cell_multiplier);

    if (g_structures_count == 0) {
        fprintf(stderr, "No structures to index\n");
        return false;
    }

    clock_gettime(CLOCK_MONOTONIC, &g_start_time);
    bool use_fast = (g_mode == MODE_HIGH_PERF || g_mode == MODE_BALANCED);

    /* Precompute cell coords for fast mode */
    if (use_fast) {
        fprintf(stderr, "  Precomputing cell coordinates...\n");
        StructureFast *arr = (StructureFast *)g_structures;
        for (uint64_t i = 0; i < g_structures_count; i++) {
            arr[i].cellX = coord_to_cell(arr[i].x, cell_size);
            arr[i].cellZ = coord_to_cell(arr[i].z, cell_size);
        }
    }

    /* Sort */
    fprintf(stderr, "  Sorting %lu structures...\n", (unsigned long)g_structures_count);
    if (use_fast) {
        qsort(g_structures, g_structures_count, sizeof(StructureFast), compare_fast);
    } else {
        qsort(g_structures, g_structures_count, sizeof(StructureCompact), compare_compact);
    }
    fprintf(stderr, "  Sort complete\n");

    /* Count unique cells */
    fprintf(stderr, "  Counting cells...\n");
    uint64_t num_cells = 1;
    
    if (use_fast) {
        StructureFast *arr = (StructureFast *)g_structures;
        for (uint64_t i = 1; i < g_structures_count; i++) {
            if (arr[i].cellX != arr[i-1].cellX || arr[i].cellZ != arr[i-1].cellZ)
                num_cells++;
        }
    } else {
        StructureCompact *arr = (StructureCompact *)g_structures;
        int64_t prev_cx = coord_to_cell(arr[0].x, cell_size);
        int64_t prev_cz = coord_to_cell(arr[0].z, cell_size);
        for (uint64_t i = 1; i < g_structures_count; i++) {
            int64_t cx = coord_to_cell(arr[i].x, cell_size);
            int64_t cz = coord_to_cell(arr[i].z, cell_size);
            if (cx != prev_cx || cz != prev_cz) {
                num_cells++;
                prev_cx = cx;
                prev_cz = cz;
            }
        }
    }
    
    fprintf(stderr, "  Found %lu cells (avg %.1f structures/cell)\n",
            (unsigned long)num_cells, (double)g_structures_count / num_cells);

    /* Allocate cells */
    g_cells = malloc(num_cells * sizeof(CellEntry));
    if (!g_cells) {
        fprintf(stderr, "Failed to allocate cells\n");
        return false;
    }
    g_cells_count = num_cells;

    /* Build cell entries */
    fprintf(stderr, "  Building cell index...\n");
    uint64_t cell_idx = 0;
    uint64_t cell_start = 0;
    
    if (use_fast) {
        StructureFast *arr = (StructureFast *)g_structures;
        for (uint64_t i = 1; i <= g_structures_count; i++) {
            bool is_new = (i == g_structures_count) ||
                          (arr[i].cellX != arr[i-1].cellX) ||
                          (arr[i].cellZ != arr[i-1].cellZ);
            if (is_new) {
                g_cells[cell_idx].cellX = arr[cell_start].cellX;
                g_cells[cell_idx].cellZ = arr[cell_start].cellZ;
                g_cells[cell_idx].start = (uint32_t)cell_start;
                g_cells[cell_idx].count = (uint32_t)(i - cell_start);
                g_cells[cell_idx].next = 0;
                cell_idx++;
                cell_start = i;
            }
        }
    } else {
        StructureCompact *arr = (StructureCompact *)g_structures;
        int64_t prev_cx = coord_to_cell(arr[0].x, cell_size);
        int64_t prev_cz = coord_to_cell(arr[0].z, cell_size);
        
        for (uint64_t i = 1; i <= g_structures_count; i++) {
            bool is_new = (i == g_structures_count);
            if (!is_new) {
                int64_t cx = coord_to_cell(arr[i].x, cell_size);
                int64_t cz = coord_to_cell(arr[i].z, cell_size);
                is_new = (cx != prev_cx || cz != prev_cz);
                if (is_new) {
                    prev_cx = cx;
                    prev_cz = cz;
                }
            }
            if (is_new) {
                g_cells[cell_idx].cellX = coord_to_cell(arr[cell_start].x, cell_size);
                g_cells[cell_idx].cellZ = coord_to_cell(arr[cell_start].z, cell_size);
                g_cells[cell_idx].start = (uint32_t)cell_start;
                g_cells[cell_idx].count = (uint32_t)(i - cell_start);
                g_cells[cell_idx].next = 0;
                cell_idx++;
                cell_start = i;
            }
        }
    }

    /* Build hash table - size based on available memory */
    uint64_t max_hash_bits = (g_mode == MODE_HIGH_PERF) ? 27 : 
                             (g_mode == MODE_BALANCED) ? 26 : 24;
    g_hash_table_size = 1ULL << 20;
    while (g_hash_table_size < num_cells * 2 && g_hash_table_size < (1ULL << max_hash_bits))
        g_hash_table_size *= 2;
    
    fprintf(stderr, "  Hash table: %lu buckets (%.2f MB)\n",
            (unsigned long)g_hash_table_size,
            (g_hash_table_size * sizeof(uint32_t)) / (1024.0 * 1024.0));
    
    g_hash_table = calloc(g_hash_table_size, sizeof(uint32_t));
    if (!g_hash_table) {
        fprintf(stderr, "Failed to allocate hash table\n");
        free(g_cells);
        return false;
    }
    
    for (uint64_t i = 0; i < num_cells; i++) {
        uint64_t h = hash_cell(g_cells[i].cellX, g_cells[i].cellZ, g_hash_table_size);
        g_cells[i].next = g_hash_table[h];
        g_hash_table[h] = (uint32_t)(i + 1);
    }

    g_total_cells = num_cells;
    
    double total_mem = (g_structures_count * structure_size() + 
                       num_cells * sizeof(CellEntry) + 
                       g_hash_table_size * sizeof(uint32_t)) / (1024.0 * 1024.0 * 1024.0);
    fprintf(stderr, "  Total memory used: %.2f GB\n", total_mem);

    return true;
}

static CellEntry *find_cell(int64_t cx, int64_t cz)
{
    uint64_t h = hash_cell(cx, cz, g_hash_table_size);
    uint32_t idx = g_hash_table[h];
    
    while (idx != 0) {
        CellEntry *cell = &g_cells[idx - 1];
        if (cell->cellX == cx && cell->cellZ == cz)
            return cell;
        idx = cell->next;
    }
    return NULL;
}

/* ============================================================================
 * Group Finding - Templated for both modes
 * ========================================================================== */

/* Get structure coordinates */
static inline void get_coords(uint32_t idx, int32_t *x, int32_t *z)
{
    if (g_mode == MODE_HIGH_PERF || g_mode == MODE_BALANCED) {
        StructureFast *arr = (StructureFast *)g_structures;
        *x = arr[idx].x;
        *z = arr[idx].z;
    } else {
        StructureCompact *arr = (StructureCompact *)g_structures;
        *x = arr[idx].x;
        *z = arr[idx].z;
    }
}

static inline int64_t dist_sq_idx(uint32_t a, uint32_t b)
{
    int32_t ax, az, bx, bz;
    get_coords(a, &ax, &az);
    get_coords(b, &bx, &bz);
    int64_t dx = (int64_t)ax - (int64_t)bx;
    int64_t dz = (int64_t)az - (int64_t)bz;
    return dx * dx + dz * dz;
}

static void output_group(FILE *out, pthread_mutex_t *lock, uint32_t *group, int count)
{
    pthread_mutex_lock(lock);

    fprintf(out, "Group of %d:\n", count);
    double cx = 0, cz = 0;
    for (int i = 0; i < count; i++) {
        int32_t x, z;
        get_coords(group[i], &x, &z);
        fprintf(out, "  (%d, %d)\n", x, z);
        cx += x;
        cz += z;
    }
    cx /= count;
    cz /= count;

    double max_dist = 0;
    for (int i = 0; i < count; i++) {
        int32_t x, z;
        get_coords(group[i], &x, &z);
        double dx = x - cx;
        double dz = z - cz;
        double dist = sqrt(dx * dx + dz * dz);
        if (dist > max_dist) max_dist = dist;
    }

    fprintf(out, "  Center: (%.1f, %.1f)\n", cx, cz);
    fprintf(out, "  Max distance from center: %.1f blocks\n", max_dist);
    fprintf(out, "  Distance from spawn: %.1f blocks\n\n", sqrt(cx * cx + cz * cz));

    pthread_mutex_unlock(lock);
}

static bool is_valid_group(uint32_t *group, int count, int64_t radius_sq)
{
    double cx = 0, cz = 0;
    for (int i = 0; i < count; i++) {
        int32_t x, z;
        get_coords(group[i], &x, &z);
        cx += x;
        cz += z;
    }
    cx /= count;
    cz /= count;

    for (int i = 0; i < count; i++) {
        int32_t x, z;
        get_coords(group[i], &x, &z);
        double dx = x - cx;
        double dz = z - cz;
        if (dx * dx + dz * dz > (double)radius_sq)
            return false;
    }
    return true;
}

static void find_groups_in_cell(CellEntry *cell, ThreadWork *work)
{
    uint32_t *neighbors = work->neighbors_buf;
    uint32_t max_neighbors = work->neighbors_buf_size;
    int64_t radius_sq = work->radius_sq;
    
    /* Search range depends on cell multiplier */
    int search_range = (g_cell_multiplier + 1) / 2 + 1;
    
    /* Collect neighbors */
    uint32_t num_neighbors = 0;
    for (int dx = -search_range; dx <= search_range; dx++) {
        for (int dz = -search_range; dz <= search_range; dz++) {
            CellEntry *nc = find_cell(cell->cellX + dx, cell->cellZ + dz);
            if (!nc) continue;
            for (uint32_t i = 0; i < nc->count && num_neighbors < max_neighbors; i++) {
                neighbors[num_neighbors++] = nc->start + i;
            }
        }
    }

    if (num_neighbors < 3) return;

    int64_t max_pair_dist_sq = 4 * radius_sq;
    
    for (uint32_t ci = 0; ci < cell->count; ci++) {
        uint32_t base_idx = cell->start + ci;
        
        /* Build candidates */
        uint32_t candidates[4096];
        uint32_t num_cand = 0;
        
        for (uint32_t ni = 0; ni < num_neighbors && num_cand < 4096; ni++) {
            uint32_t idx = neighbors[ni];
            if (idx <= base_idx) continue;
            if (dist_sq_idx(base_idx, idx) <= max_pair_dist_sq) {
                candidates[num_cand++] = idx;
            }
        }

        if (num_cand < 2) continue;

        /* Groups of 4 */
        if (num_cand >= 3) {
            for (uint32_t i = 0; i < num_cand - 2; i++) {
                for (uint32_t j = i + 1; j < num_cand - 1; j++) {
                    if (dist_sq_idx(candidates[i], candidates[j]) > max_pair_dist_sq)
                        continue;

                    for (uint32_t k = j + 1; k < num_cand; k++) {
                        if (dist_sq_idx(candidates[i], candidates[k]) > max_pair_dist_sq)
                            continue;
                        if (dist_sq_idx(candidates[j], candidates[k]) > max_pair_dist_sq)
                            continue;

                        uint32_t group[4] = { base_idx, candidates[i], candidates[j], candidates[k] };
                        if (is_valid_group(group, 4, radius_sq)) {
                            output_group(work->output, work->output_lock, group, 4);
                            work->groups_found_4++;
                        }
                    }
                }
            }
        }

        /* Groups of 3 */
        for (uint32_t i = 0; i < num_cand - 1; i++) {
            for (uint32_t j = i + 1; j < num_cand; j++) {
                if (dist_sq_idx(candidates[i], candidates[j]) > max_pair_dist_sq)
                    continue;

                uint32_t group[3] = { base_idx, candidates[i], candidates[j] };
                if (is_valid_group(group, 3, radius_sq)) {
                    output_group(work->output, work->output_lock, group, 3);
                    work->groups_found_3++;
                }
            }
        }
    }
}

static void *progress_thread(void *arg)
{
    (void)arg;
    while (!g_done) {
        pthread_mutex_lock(&g_progress_lock);
        uint64_t processed = g_processed_cells;
        uint64_t total = g_total_cells;
        pthread_mutex_unlock(&g_progress_lock);
        print_progress("Finding groups", processed, total);
        usleep(500000);
    }
    print_progress("Finding groups", g_total_cells, g_total_cells);
    fprintf(stderr, "\n");
    return NULL;
}

static void *worker_thread(void *arg)
{
    ThreadWork *work = (ThreadWork *)arg;

    for (uint64_t i = work->thread_id; i < work->num_cells; i += work->num_threads) {
        find_groups_in_cell(&work->cells[i], work);

        pthread_mutex_lock(&g_progress_lock);
        g_processed_cells++;
        pthread_mutex_unlock(&g_progress_lock);

        work->cells_processed++;
    }

    return NULL;
}

/* ============================================================================
 * Cleanup & Main
 * ========================================================================== */

static void cleanup(void)
{
    free(g_structures); g_structures = NULL;
    free(g_cells); g_cells = NULL;
    free(g_hash_table); g_hash_table = NULL;
}

static char *read_line(char *buf, size_t size)
{
    if (!fgets(buf, (int)size, stdin)) return NULL;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    return buf;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    char input_file[512];
    int64_t radius;
    int num_threads;
    int available_cores = (int)sysconf(_SC_NPROCESSORS_ONLN);

    printf("=== Structure Group Finder (Auto-Optimizing) ===\n\n");
    printf("Automatically detects system resources and optimizes performance.\n\n");

    printf("Enter input file path: ");
    fflush(stdout);
    if (!read_line(input_file, sizeof(input_file)) || input_file[0] == '\0') {
        fprintf(stderr, "Error: No input file specified\n");
        return 1;
    }

    struct stat st;
    if (stat(input_file, &st) < 0) {
        fprintf(stderr, "Error: Cannot access '%s': %s\n", input_file, strerror(errno));
        return 1;
    }
    
    size_t file_size = st.st_size;
    uint64_t estimated_structures = file_size / AVG_BYTES_PER_LINE;
    printf("  File size: %.2f GB (~%lu structures)\n\n", 
           file_size / (1024.0 * 1024.0 * 1024.0), (unsigned long)estimated_structures);

    /* Auto-configure based on system memory */
    detect_and_configure(estimated_structures);

    printf("Enter radius (max distance from center in blocks): ");
    fflush(stdout);
    char radius_buf[64];
    if (!read_line(radius_buf, sizeof(radius_buf))) {
        fprintf(stderr, "Error: No radius specified\n");
        return 1;
    }
    radius = atoll(radius_buf);
    if (radius <= 0) {
        fprintf(stderr, "Error: Radius must be positive\n");
        return 1;
    }

    printf("\nUse multithreading? [Y/n] (detected %d cores): ", available_cores);
    fflush(stdout);
    char mt_buf[64];
    int use_mt = 1;
    if (read_line(mt_buf, sizeof(mt_buf)) && (mt_buf[0] == 'n' || mt_buf[0] == 'N'))
        use_mt = 0;

    if (use_mt) {
        printf("Enter number of threads (default %d): ", available_cores);
        fflush(stdout);
        char threads_buf[64];
        if (read_line(threads_buf, sizeof(threads_buf)) && threads_buf[0] != '\0') {
            int t = atoi(threads_buf);
            num_threads = (t > 0) ? t : available_cores;
        } else {
            num_threads = available_cores;
        }
        if (num_threads > 256) num_threads = 256;
    } else {
        num_threads = 1;
    }

    printf("\n=== Final Configuration ===\n");
    printf("  Input: %s\n", input_file);
    printf("  Radius: %ld blocks\n", (long)radius);
    printf("  Cell size: %ld blocks\n", (long)(radius * g_cell_multiplier));
    printf("  Threads: %d\n", num_threads);
    printf("\n");

    struct timespec total_start;
    clock_gettime(CLOCK_MONOTONIC, &total_start);

    uint64_t count = parse_file(input_file);
    if (count == 0) {
        cleanup();
        return 1;
    }

    if (!build_spatial_index(radius)) {
        cleanup();
        return 1;
    }

    char output_filename[256];
    snprintf(output_filename, sizeof(output_filename), "groups_%ld.txt", (long)radius);
    FILE *output = fopen(output_filename, "w");
    if (!output) {
        perror("Failed to open output file");
        cleanup();
        return 1;
    }

    fprintf(output, "Structure groups within %ld block radius\n", (long)radius);
    fprintf(output, "Input: %s\n", input_file);
    fprintf(output, "Structures: %lu\n\n", (unsigned long)count);

    printf("Searching for groups...\n");

    pthread_t *threads = malloc((size_t)num_threads * sizeof(pthread_t));
    ThreadWork *work = calloc((size_t)num_threads, sizeof(ThreadWork));
    pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;

    if (!threads || !work) {
        fclose(output);
        cleanup();
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &g_start_time);

    pthread_t progress_tid;
    pthread_create(&progress_tid, NULL, progress_thread, NULL);

    /* Buffer size scales with available memory */
    uint32_t buf_size = (g_mode == MODE_HIGH_PERF) ? 262144 : 
                        (g_mode == MODE_BALANCED) ? 131072 : 65536;

    for (int i = 0; i < num_threads; i++) {
        work[i].thread_id = i;
        work[i].num_threads = num_threads;
        work[i].structures = g_structures;
        work[i].num_structures = g_structures_count;
        work[i].cells = g_cells;
        work[i].num_cells = g_cells_count;
        work[i].hash_table = g_hash_table;
        work[i].hash_table_size = g_hash_table_size;
        work[i].radius = radius;
        work[i].radius_sq = radius * radius;
        work[i].cell_size = radius * g_cell_multiplier;
        work[i].output = output;
        work[i].output_lock = &output_lock;
        work[i].neighbors_buf = malloc(buf_size * sizeof(uint32_t));
        work[i].neighbors_buf_size = buf_size;

        if (!work[i].neighbors_buf) {
            for (int j = 0; j < i; j++) free(work[j].neighbors_buf);
            fclose(output);
            cleanup();
            return 1;
        }

        pthread_create(&threads[i], NULL, worker_thread, &work[i]);
    }

    uint64_t total_3 = 0, total_4 = 0;
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        total_3 += work[i].groups_found_3;
        total_4 += work[i].groups_found_4;
        free(work[i].neighbors_buf);
    }

    g_done = 1;
    pthread_join(progress_tid, NULL);

    fprintf(output, "\n=== Summary ===\n");
    fprintf(output, "Groups of 3: %lu\n", (unsigned long)total_3);
    fprintf(output, "Groups of 4: %lu\n", (unsigned long)total_4);

    struct timespec total_end;
    clock_gettime(CLOCK_MONOTONIC, &total_end);
    double elapsed = (total_end.tv_sec - total_start.tv_sec) +
                     (total_end.tv_nsec - total_start.tv_nsec) / 1e9;

    printf("\n=== Results ===\n");
    printf("Groups of 3: %lu\n", (unsigned long)total_3);
    printf("Groups of 4: %lu\n", (unsigned long)total_4);
    printf("Output: %s\n", output_filename);
    printf("Time: %02d:%02d:%02d (%.1fs)\n",
           (int)(elapsed / 3600), ((int)elapsed % 3600) / 60, (int)elapsed % 60, elapsed);

    fclose(output);
    free(threads);
    free(work);
    cleanup();

    return 0;
}
