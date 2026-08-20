#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <SDL3/SDL.h>

#include "voice_player.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define VOICE_MAX_FILES 128
#define VOICE_WIDE_PATH_CAP 2048
#define VOICE_UTF8_PATH_CAP 6144
#define VOICE_STATUS_CAP 96
#define VOICE_ASCEND_LIMIT 16

#define VOICE_INITIAL_MIN_MS 3000
#define VOICE_INITIAL_MAX_MS 6000
#define VOICE_GAP_MIN_MS 8000
#define VOICE_GAP_MAX_MS 18000
#define VOICE_RETRY_MS 5000

struct VoicePlayer {
    SDL_AudioStream *stream;
    char files[VOICE_MAX_FILES][VOICE_UTF8_PATH_CAP];
    int file_count;
    int last_index;
    Uint64 next_at_ms;
    Uint64 random_state;
    float volume;
    bool enabled;
    bool playing;
    bool audio_ready;
    bool owns_audio_subsystem;
    char status[VOICE_STATUS_CAP];
};

static void set_status(VoicePlayer *player, const char *status)
{
    if (!player)
        return;
    snprintf(player->status, sizeof(player->status), "%s",
             status ? status : "idle");
}

static bool utf8_to_wide(const char *input, wchar_t *output, int capacity)
{
    int count;
    int i;

    if (!input || !output || capacity <= 0)
        return false;
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1,
                                output, capacity);
    if (count <= 0)
        return false;
    for (i = 0; output[i] != L'\0'; ++i) {
        if (output[i] == L'/')
            output[i] = L'\\';
    }
    return true;
}

static bool wide_to_utf8(const wchar_t *input, char *output, int capacity)
{
    if (!input || !output || capacity <= 0)
        return false;
    return WideCharToMultiByte(CP_UTF8, 0, input, -1, output, capacity,
                               NULL, NULL) > 0;
}

static bool join_wide(wchar_t *output, int capacity, const wchar_t *left,
                      const wchar_t *right)
{
    size_t left_length;
    size_t right_length;
    bool needs_separator;
    size_t needed;

    if (!output || capacity <= 0 || !left || !right)
        return false;
    left_length = wcslen(left);
    right_length = wcslen(right);
    needs_separator = left_length > 0 && left[left_length - 1] != L'\\';
    needed = left_length + (needs_separator ? 1u : 0u) + right_length + 1u;
    if (needed > (size_t)capacity)
        return false;

    memcpy(output, left, left_length * sizeof(wchar_t));
    if (needs_separator)
        output[left_length++] = L'\\';
    memcpy(output + left_length, right,
           (right_length + 1u) * sizeof(wchar_t));
    return true;
}

static bool is_directory_w(const wchar_t *path)
{
    DWORD attributes;

    if (!path || path[0] == L'\0')
        return false;
    attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static const wchar_t *base_name_w(const wchar_t *path)
{
    const wchar_t *slash;

    if (!path)
        return L"";
    slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

static void trim_trailing_slashes(wchar_t *path)
{
    size_t length;

    if (!path)
        return;
    length = wcslen(path);
    while (length > 3 && path[length - 1] == L'\\')
        path[--length] = L'\0';
}

static bool parent_directory_w(wchar_t *path)
{
    wchar_t *slash;

    if (!path)
        return false;
    trim_trailing_slashes(path);
    slash = wcsrchr(path, L'\\');
    if (!slash)
        return false;
    if (slash == path + 2 && path[1] == L':') {
        if (path[3] == L'\0')
            return false;
        slash[1] = L'\0';
        return true;
    }
    if (slash == path)
        return false;
    *slash = L'\0';
    return true;
}

static bool directory_has_voice_area(const wchar_t *directory)
{
    wchar_t candidate[VOICE_WIDE_PATH_CAP];

    if (join_wide(candidate, VOICE_WIDE_PATH_CAP, directory,
                  L"\x97F3\x9891") &&
        is_directory_w(candidate))
        return true;
    return join_wide(candidate, VOICE_WIDE_PATH_CAP, directory,
                     L"\x8BED\x97F3") &&
           is_directory_w(candidate);
}

static bool directory_has_acb_area(const wchar_t *directory)
{
    wchar_t candidate[VOICE_WIDE_PATH_CAP];

    return join_wide(candidate, VOICE_WIDE_PATH_CAP, directory,
                     L"acb\x6587\x4EF6") &&
           is_directory_w(candidate);
}

static bool locate_card_root(const char *scene_directory_utf8,
                             wchar_t *card_root, int capacity)
{
    wchar_t cursor[VOICE_WIDE_PATH_CAP];
    int depth;

    if (!scene_directory_utf8 || !card_root || capacity <= 0 ||
        !utf8_to_wide(scene_directory_utf8, cursor, VOICE_WIDE_PATH_CAP))
        return false;
    trim_trailing_slashes(cursor);

    for (depth = 0; depth < VOICE_ASCEND_LIMIT; ++depth) {
        wchar_t parent[VOICE_WIDE_PATH_CAP];

        if (directory_has_voice_area(cursor) || directory_has_acb_area(cursor)) {
            if (wcslen(cursor) + 1u > (size_t)capacity)
                return false;
            wcscpy(card_root, cursor);
            return true;
        }

        wcscpy(parent, cursor);
        if (!parent_directory_w(parent))
            break;
        if (_wcsicmp(base_name_w(parent), L"CGSS_DOWN") == 0) {
            if (wcslen(cursor) + 1u > (size_t)capacity)
                return false;
            wcscpy(card_root, cursor);
            return true;
        }
        if (_wcsicmp(base_name_w(cursor), L"CGSS_DOWN") == 0)
            break;
        wcscpy(cursor, parent);
    }
    return false;
}

static bool parse_card_id(const char *scene_name, char *card_id,
                          int capacity)
{
    const char *digits;
    int length = 0;

    if (!scene_name || !card_id || capacity <= 1 ||
        strncmp(scene_name, "SP3S", 4) != 0)
        return false;
    digits = scene_name + 4;
    if (*digits == '\0')
        return false;
    while (digits[length] != '\0') {
        if (digits[length] < '0' || digits[length] > '9' ||
            length + 1 >= capacity)
            return false;
        card_id[length] = digits[length];
        ++length;
    }
    card_id[length] = '\0';
    return true;
}

static bool ends_with_wav(const wchar_t *name)
{
    size_t length;

    if (!name)
        return false;
    length = wcslen(name);
    return length >= 4 && _wcsicmp(name + length - 4, L".wav") == 0;
}

static bool matches_card_wav(const wchar_t *name, const wchar_t *card_id)
{
    wchar_t prefix[64];
    size_t prefix_length;

    if (!name || !card_id ||
        swprintf(prefix, sizeof(prefix) / sizeof(prefix[0]), L"card_%ls",
                 card_id) < 0)
        return false;
    prefix_length = wcslen(prefix);
    if (_wcsnicmp(name, prefix, prefix_length) != 0)
        return false;
    if (name[prefix_length] == L'.')
        return _wcsicmp(name + prefix_length, L".wav") == 0;
    return name[prefix_length] == L'_' && ends_with_wav(name);
}

static bool add_voice_file(VoicePlayer *player, const wchar_t *directory,
                           const wchar_t *name)
{
    wchar_t path[VOICE_WIDE_PATH_CAP];

    if (!player || player->file_count >= VOICE_MAX_FILES ||
        !join_wide(path, VOICE_WIDE_PATH_CAP, directory, name) ||
        !wide_to_utf8(path, player->files[player->file_count],
                      VOICE_UTF8_PATH_CAP))
        return false;
    ++player->file_count;
    return true;
}

static void scan_wav_directory(VoicePlayer *player, const wchar_t *directory,
                               const wchar_t *card_id, bool filter_card_id)
{
    wchar_t pattern[VOICE_WIDE_PATH_CAP];
    WIN32_FIND_DATAW data;
    HANDLE handle;

    if (!player || !directory ||
        !join_wide(pattern, VOICE_WIDE_PATH_CAP, directory, L"*"))
        return;
    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    do {
        if (player->file_count >= VOICE_MAX_FILES)
            break;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            data.cFileName[0] == L'.')
            continue;
        if (!ends_with_wav(data.cFileName))
            continue;
        if (filter_card_id && !matches_card_wav(data.cFileName, card_id))
            continue;
        add_voice_file(player, directory, data.cFileName);
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
}

static int compare_voice_paths(const void *left, const void *right)
{
    return strcmp((const char *)left, (const char *)right);
}

static void scan_card_files(VoicePlayer *player, const wchar_t *card_root,
                            const char *card_id_utf8)
{
    wchar_t card_id[48];
    wchar_t directory[VOICE_WIDE_PATH_CAP];

    if (!player || !card_root || !card_id_utf8 ||
        !utf8_to_wide(card_id_utf8, card_id,
                      (int)(sizeof(card_id) / sizeof(card_id[0]))))
        return;

    if (join_wide(directory, VOICE_WIDE_PATH_CAP, card_root,
                  L"\x97F3\x9891"))
        scan_wav_directory(player, directory, card_id, true);

    if (player->file_count == 0) {
        wchar_t voice_directory[VOICE_WIDE_PATH_CAP];
        wchar_t decoded_directory[VOICE_WIDE_PATH_CAP];
        wchar_t acb_folder[96];

        if (swprintf(acb_folder,
                     (int)(sizeof(acb_folder) / sizeof(acb_folder[0])),
                     L"_acb_card_%ls.acb", card_id) >= 0 &&
            join_wide(voice_directory, VOICE_WIDE_PATH_CAP, card_root,
                      L"\x8BED\x97F3") &&
            join_wide(directory, VOICE_WIDE_PATH_CAP, voice_directory,
                      acb_folder) &&
            join_wide(decoded_directory, VOICE_WIDE_PATH_CAP, directory,
                      L"internal")) {
            scan_wav_directory(player, decoded_directory, card_id, false);
        }

        /* acb.c may leave decoded WAV files below the downloaded ACB dir. */
        if (player->file_count == 0 &&
            swprintf(acb_folder,
                     (int)(sizeof(acb_folder) / sizeof(acb_folder[0])),
                     L"_acb_card_%ls.acb", card_id) >= 0 &&
            join_wide(voice_directory, VOICE_WIDE_PATH_CAP, card_root,
                      L"acb\x6587\x4EF6") &&
            join_wide(directory, VOICE_WIDE_PATH_CAP, voice_directory,
                      acb_folder) &&
            join_wide(decoded_directory, VOICE_WIDE_PATH_CAP, directory,
                      L"internal")) {
            scan_wav_directory(player, decoded_directory, card_id, false);
        }
    }

    if (player->file_count > 1) {
        qsort(player->files, (size_t)player->file_count,
              sizeof(player->files[0]), compare_voice_paths);
    }
}

static Uint64 random_delay(VoicePlayer *player, int minimum, int maximum)
{
    int span;

    if (!player || maximum <= minimum)
        return (Uint64)minimum;
    span = maximum - minimum + 1;
    return (Uint64)(minimum + SDL_rand_r(&player->random_state, span));
}

static void schedule_initial(VoicePlayer *player, Uint64 now_ms)
{
    player->next_at_ms = now_ms +
        random_delay(player, VOICE_INITIAL_MIN_MS, VOICE_INITIAL_MAX_MS);
}

static void schedule_gap(VoicePlayer *player, Uint64 now_ms)
{
    player->next_at_ms = now_ms +
        random_delay(player, VOICE_GAP_MIN_MS, VOICE_GAP_MAX_MS);
}

static void clear_playback(VoicePlayer *player)
{
    if (!player)
        return;
    if (player->stream)
        SDL_ClearAudioStream(player->stream);
    player->playing = false;
    player->next_at_ms = 0;
}

static void refresh_idle_status(VoicePlayer *player)
{
    if (!player)
        return;
    if (!player->enabled)
        set_status(player, "disabled");
    else if (player->file_count <= 0)
        set_status(player, "idle: no matching voice WAV");
    else if (!player->audio_ready)
        set_status(player, "idle: audio unavailable");
    else
        set_status(player, "ready");
}

static int choose_file_index(VoicePlayer *player)
{
    int index;

    if (!player || player->file_count <= 0)
        return -1;
    index = SDL_rand_r(&player->random_state, player->file_count);
    if (player->file_count > 1 && index == player->last_index) {
        int offset = 1 + SDL_rand_r(&player->random_state,
                                    player->file_count - 1);
        index = (index + offset) % player->file_count;
    }
    return index;
}

static void remove_file(VoicePlayer *player, int index)
{
    int i;

    if (!player || index < 0 || index >= player->file_count)
        return;
    for (i = index; i + 1 < player->file_count; ++i)
        memcpy(player->files[i], player->files[i + 1],
               sizeof(player->files[i]));
    --player->file_count;
    if (player->last_index == index)
        player->last_index = -1;
    else if (player->last_index > index)
        --player->last_index;
}

static bool prepare_stream(VoicePlayer *player, const SDL_AudioSpec *spec,
                           bool *new_stream)
{
    if (!player || !spec || !new_stream)
        return false;
    *new_stream = false;

    if (player->stream &&
        !SDL_SetAudioStreamFormat(player->stream, spec, NULL)) {
        SDL_DestroyAudioStream(player->stream);
        player->stream = NULL;
    }
    if (!player->stream) {
        player->stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, spec, NULL, NULL);
        if (!player->stream)
            return false;
        *new_stream = true;
    }
    if (!SDL_SetAudioStreamGain(player->stream, player->volume)) {
        SDL_DestroyAudioStream(player->stream);
        player->stream = NULL;
        *new_stream = false;
        return false;
    }
    return true;
}

VoicePlayer *voice_player_init(void)
{
    VoicePlayer *player = (VoicePlayer *)calloc(1, sizeof(*player));
    SDL_InitFlags initialized;

    if (!player)
        return NULL;
    player->enabled = true;
    player->volume = 0.75f;
    player->last_index = -1;
    player->random_state = SDL_GetPerformanceCounter() ^
                           (Uint64)(uintptr_t)player;
    if (player->random_state == 0)
        player->random_state = UINT64_C(0x9e3779b97f4a7c15);

    initialized = SDL_WasInit(SDL_INIT_AUDIO);
    if ((initialized & SDL_INIT_AUDIO) != 0) {
        player->audio_ready = true;
    } else if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        player->audio_ready = true;
        player->owns_audio_subsystem = true;
    }
    refresh_idle_status(player);
    return player;
}

VoicePlayer *voice_player_create(void)
{
    return voice_player_init();
}

int voice_player_rescan(VoicePlayer *player, const char *scene_name,
                        const char *scene_directory_utf8)
{
    char card_id[48];
    wchar_t card_root[VOICE_WIDE_PATH_CAP];

    if (!player)
        return 0;
    clear_playback(player);
    player->file_count = 0;
    player->last_index = -1;

    if (parse_card_id(scene_name, card_id, (int)sizeof(card_id)) &&
        locate_card_root(scene_directory_utf8, card_root,
                         VOICE_WIDE_PATH_CAP)) {
        scan_card_files(player, card_root, card_id);
    }

    if (player->enabled && player->audio_ready && player->file_count > 0)
        schedule_initial(player, SDL_GetTicks());
    refresh_idle_status(player);
    return player->file_count;
}

int voice_player_refresh(VoicePlayer *player, const char *scene_name,
                         const char *scene_directory_utf8)
{
    return voice_player_rescan(player, scene_name, scene_directory_utf8);
}

void voice_player_update(VoicePlayer *player, uint64_t now_ms_value)
{
    Uint64 now_ms = (Uint64)now_ms_value;
    int attempts;

    if (!player || !player->enabled || !player->audio_ready ||
        player->file_count <= 0)
        return;

    if (player->playing) {
        int queued = player->stream
                   ? SDL_GetAudioStreamQueued(player->stream) : -1;
        if (queued > 0)
            return;
        player->playing = false;
        if (queued < 0 && player->stream) {
            SDL_DestroyAudioStream(player->stream);
            player->stream = NULL;
            player->next_at_ms = now_ms + VOICE_RETRY_MS;
            set_status(player, "idle: audio device unavailable");
            return;
        }
        schedule_gap(player, now_ms);
        refresh_idle_status(player);
        return;
    }

    if (now_ms < player->next_at_ms)
        return;

    attempts = player->file_count;
    while (attempts-- > 0 && player->file_count > 0) {
        SDL_AudioSpec spec;
        Uint8 *wav_data = NULL;
        Uint32 wav_length = 0;
        bool new_stream = false;
        bool queued;
        int index = choose_file_index(player);

        if (index < 0)
            break;
        if (!SDL_LoadWAV(player->files[index], &spec, &wav_data,
                         &wav_length) || !wav_data || wav_length == 0 ||
            wav_length > (Uint32)INT_MAX) {
            if (wav_data)
                SDL_free(wav_data);
            remove_file(player, index);
            continue;
        }

        if (!prepare_stream(player, &spec, &new_stream)) {
            SDL_free(wav_data);
            player->next_at_ms = now_ms + VOICE_RETRY_MS;
            set_status(player, "idle: audio device unavailable");
            return;
        }

        queued = SDL_PutAudioStreamData(player->stream, wav_data,
                                        (int)wav_length) &&
                 SDL_FlushAudioStream(player->stream);
        SDL_free(wav_data);
        if (!queued ||
            (new_stream &&
             !SDL_ResumeAudioStreamDevice(player->stream))) {
            SDL_DestroyAudioStream(player->stream);
            player->stream = NULL;
            player->next_at_ms = now_ms + VOICE_RETRY_MS;
            set_status(player, "idle: audio device unavailable");
            return;
        }

        player->last_index = index;
        player->playing = true;
        player->next_at_ms = 0;
        set_status(player, "playing");
        return;
    }

    refresh_idle_status(player);
}

void voice_player_set_enabled(VoicePlayer *player, bool enabled)
{
    if (!player || player->enabled == enabled)
        return;
    player->enabled = enabled;
    clear_playback(player);
    if (enabled && player->audio_ready && player->file_count > 0)
        schedule_initial(player, SDL_GetTicks());
    refresh_idle_status(player);
}

bool voice_player_is_enabled(const VoicePlayer *player)
{
    return player && player->enabled;
}

void voice_player_set_volume(VoicePlayer *player, float volume)
{
    if (!player)
        return;
    if (!(volume >= 0.0f))
        volume = 0.0f;
    else if (volume > 1.0f)
        volume = 1.0f;
    player->volume = volume;
    if (player->stream)
        SDL_SetAudioStreamGain(player->stream, volume);
}

float voice_player_volume(const VoicePlayer *player)
{
    return player ? player->volume : 0.0f;
}

const char *voice_player_status(const VoicePlayer *player)
{
    return player ? player->status : "uninitialized";
}

const char *voice_player_error(const VoicePlayer *player)
{
    return voice_player_status(player);
}

int voice_player_file_count(const VoicePlayer *player)
{
    return player ? player->file_count : 0;
}

int voice_player_count(const VoicePlayer *player)
{
    return voice_player_file_count(player);
}

void voice_player_shutdown(VoicePlayer *player)
{
    if (!player)
        return;
    if (player->stream)
        SDL_DestroyAudioStream(player->stream);
    if (player->owns_audio_subsystem)
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    free(player);
}

void voice_player_destroy(VoicePlayer *player)
{
    voice_player_shutdown(player);
}
