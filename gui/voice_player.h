#ifndef CGSS_VOICE_PLAYER_H
#define CGSS_VOICE_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VoicePlayer VoicePlayer;

/* Short lifecycle names are aliases for the explicit init/shutdown names. */
VoicePlayer *voice_player_create(void);
void voice_player_destroy(VoicePlayer *player);

/*
 * Initializes the optional audio subsystem and returns an inert player even
 * when no playback device is available. NULL only means allocation failed.
 */
VoicePlayer *voice_player_init(void);

/*
 * Stops the previous card voice and binds WAV files belonging to the current
 * SP3S<ID> scene. Returns the number of matching playable candidates found.
 */
int voice_player_rescan(VoicePlayer *player, const char *scene_name,
                        const char *scene_directory_utf8);
int voice_player_refresh(VoicePlayer *player, const char *scene_name,
                         const char *scene_directory_utf8);

/* Call once per GUI frame with SDL_GetTicks(). */
void voice_player_update(VoicePlayer *player, uint64_t now_ms);

void voice_player_set_enabled(VoicePlayer *player, bool enabled);
bool voice_player_is_enabled(const VoicePlayer *player);

/* Linear stream gain, clamped to 0.0 through 1.0. */
void voice_player_set_volume(VoicePlayer *player, float volume);
float voice_player_volume(const VoicePlayer *player);

const char *voice_player_status(const VoicePlayer *player);
const char *voice_player_error(const VoicePlayer *player);
int voice_player_file_count(const VoicePlayer *player);
int voice_player_count(const VoicePlayer *player);

void voice_player_shutdown(VoicePlayer *player);

#ifdef __cplusplus
}
#endif

#endif /* CGSS_VOICE_PLAYER_H */
