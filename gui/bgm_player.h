#ifndef CGSS_BGM_PLAYER_H
#define CGSS_BGM_PLAYER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BgmPlayer BgmPlayer;

/*
 * The player watches CGSS_DOWN/BGM/bgm_studio_night.acb. When it appears,
 * acb2wavs.exe is run on a worker thread and the decoded WAV is looped by
 * SDL3. Decoding therefore never blocks the GUI thread.
 */
BgmPlayer *bgm_player_create(const char *exe_directory_utf8);
void bgm_player_destroy(BgmPlayer *player);

/* Call once per GUI frame. active=false pauses without losing position. */
void bgm_player_update(BgmPlayer *player, bool active);

void bgm_player_set_enabled(BgmPlayer *player, bool enabled);
bool bgm_player_is_enabled(const BgmPlayer *player);
void bgm_player_set_volume(BgmPlayer *player, float volume);
float bgm_player_volume(const BgmPlayer *player);

bool bgm_player_source_exists(const BgmPlayer *player);
bool bgm_player_is_ready(const BgmPlayer *player);
const char *bgm_player_status(const BgmPlayer *player);

/* Retry decoding after acb2wavs.exe returned an error or produced no WAV. */
void bgm_player_retry(BgmPlayer *player);

#ifdef __cplusplus
}
#endif

#endif /* CGSS_BGM_PLAYER_H */
