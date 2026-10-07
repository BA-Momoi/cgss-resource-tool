#include <stdio.h>
#include "stage_map.h"

#define MAX_STAGE_MAPS 4096

typedef struct {
    int live_id;
    int bg_id;
} StageMapEntry;

static StageMapEntry stage_map[MAX_STAGE_MAPS];
static int stage_map_count;
static int stage_map_state;

static void load_stage_map(void){
    if (stage_map_state != 0) return;
    stage_map_state = -1;

    FILE *fp = fopen("stage_live_map.csv", "r");
    if (!fp) return;
    stage_map_state = 1;

    char line[128];
    while (fgets(line, sizeof line, fp)){
        int live_id, bg_id;
        if (sscanf(line, "%d,%d", &live_id, &bg_id) != 2 ||
            live_id <= 0 || bg_id <= 0)
            continue;

        int existing = -1;
        for (int i = 0; i < stage_map_count; i++){
            if (stage_map[i].live_id == live_id){
                existing = i;
                break;
            }
        }
        if (existing >= 0){
            stage_map[existing].bg_id = bg_id;
        } else if (stage_map_count < MAX_STAGE_MAPS){
            stage_map[stage_map_count].live_id = live_id;
            stage_map[stage_map_count].bg_id = bg_id;
            stage_map_count++;
        }
    }
    fclose(fp);
}

int stage_map_available(void){
    load_stage_map();
    return stage_map_state == 1 && stage_map_count > 0;
}

int stage_bg_for_live(int live_id, int *bg_id){
    if (live_id <= 0 || !bg_id) return 0;
    load_stage_map();
    for (int i = 0; i < stage_map_count; i++){
        if (stage_map[i].live_id == live_id){
            *bg_id = stage_map[i].bg_id;
            return 1;
        }
    }
    return 0;
}
