#ifndef CGSS_RESOURCE_BACKEND_H
#define CGSS_RESOURCE_BACKEND_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compatibility capacity used by older callers.  The backend itself accepts
 * any positive search capacity and dynamically sizes its download queue. */
#define CGSS_RESOURCE_RESULTS_MAX 256

typedef enum ResourceCategory {
    RESOURCE_CATEGORY_ALL = 0,
    RESOURCE_CATEGORY_BGM,
    RESOURCE_CATEGORY_SONG,
    RESOURCE_CATEGORY_CARD,
    RESOURCE_CATEGORY_VOICE,
    RESOURCE_CATEGORY_CHART,
    RESOURCE_CATEGORY_STAGE,
    RESOURCE_CATEGORY_ACTION,
    RESOURCE_CATEGORY_MODEL,
    RESOURCE_CATEGORY_SPINE,
    RESOURCE_CATEGORY_STICKER,
    RESOURCE_CATEGORY_CG,
    RESOURCE_CATEGORY_COUNT
} ResourceCategory;

typedef struct ResourceItem {
    char name[384];
    char hash[65];
    long long size;
    bool selected;
} ResourceItem;

typedef enum ResourceJobState {
    RESOURCE_JOB_IDLE = 0,
    RESOURCE_JOB_RUNNING,
    RESOURCE_JOB_COMPLETED,
    RESOURCE_JOB_FAILED,
    RESOURCE_JOB_CANCELLED
} ResourceJobState;

typedef struct ResourceJobSnapshot {
    ResourceJobState state;
    int completed_items;
    int total_items;
    unsigned long long downloaded_bytes;
    unsigned long long current_bytes;
    unsigned long long current_total_bytes;
    char current_name[384];
    char status[512];
    char output_directory[1024];
} ResourceJobSnapshot;

typedef struct ResourceBackend ResourceBackend;

ResourceBackend *resource_backend_create(const char *exe_directory_utf8);
void resource_backend_destroy(ResourceBackend *backend);

bool resource_backend_is_ready(const ResourceBackend *backend);
const char *resource_backend_error(const ResourceBackend *backend);
const char *resource_backend_manifest_path(const ResourceBackend *backend);
const char *resource_backend_master_path(const ResourceBackend *backend);

const char *resource_category_label(ResourceCategory category);

/* Return the complete number of manifest rows matching a category/query, or
 * -1 when the query fails.  This is not limited by the caller's page size. */
int resource_backend_count(ResourceBackend *backend,
                           ResourceCategory category,
                           const char *query_utf8);

int resource_backend_search(ResourceBackend *backend,
                            ResourceCategory category,
                            const char *query_utf8,
                            ResourceItem *items,
                            int capacity);

bool resource_backend_start_download(ResourceBackend *backend,
                                     ResourceCategory category,
                                     const ResourceItem *items,
                                     int count,
                                     bool auto_unpack);
void resource_backend_cancel(ResourceBackend *backend);
void resource_backend_snapshot(ResourceBackend *backend,
                               ResourceJobSnapshot *snapshot);
bool resource_backend_is_busy(ResourceBackend *backend);

#ifdef __cplusplus
}
#endif

#endif
