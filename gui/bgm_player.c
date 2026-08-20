#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <SDL3/SDL.h>

#include "bgm_player.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define BGM_PATH_CAP 2048
#define BGM_UTF8_PATH_CAP 6144
#define BGM_STATUS_CAP 256
#define BGM_QUEUE_MIN_BYTES (256 * 1024)
#define BGM_QUEUE_MAX_BYTES (2 * 1024 * 1024)
#define BGM_FEED_CHUNK_BYTES (64 * 1024)

enum {
    BGM_DECODE_IDLE = 0,
    BGM_DECODE_RUNNING = 1,
    BGM_DECODE_SUCCEEDED = 2,
    BGM_DECODE_FAILED = -1
};

struct BgmPlayer {
    wchar_t source_path[BGM_PATH_CAP];
    wchar_t decoder_path[BGM_PATH_CAP];
    wchar_t decoded_directory[BGM_PATH_CAP];
    char wav_path[BGM_UTF8_PATH_CAP];
    SDL_AudioStream *stream;
    SDL_AudioSpec wav_spec;
    Uint8 *wav_data;
    Uint32 wav_length;
    Uint32 wav_cursor;
    int frame_size;
    int queue_target;
    HANDLE decode_thread;
    volatile LONG decode_state;
    float volume;
    bool enabled;
    bool active;
    bool audio_ready;
    bool owns_audio_subsystem;
    bool decode_attempted;
    bool playback_logged;
    char status[BGM_STATUS_CAP];
};

static void bgm_set_status(BgmPlayer *player, const char *status)
{
    if (player)
        snprintf(player->status, sizeof(player->status), "%s",
                 status ? status : "");
}

static bool bgm_utf8_to_wide(const char *input, wchar_t *output, int capacity)
{
    int count;
    int i;

    if (!input || !output || capacity <= 0)
        return false;
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1,
                                output, capacity);
    if (count <= 0)
        count = MultiByteToWideChar(CP_UTF8, 0, input, -1, output, capacity);
    if (count <= 0)
        return false;
    for (i = 0; output[i] != L'\0'; ++i) {
        if (output[i] == L'/')
            output[i] = L'\\';
    }
    return true;
}

static bool bgm_wide_to_utf8(const wchar_t *input, char *output, int capacity)
{
    return input && output && capacity > 0 &&
           WideCharToMultiByte(CP_UTF8, 0, input, -1, output, capacity,
                               NULL, NULL) > 0;
}

static void bgm_trim_trailing_slash(wchar_t *path)
{
    size_t length;

    if (!path)
        return;
    length = wcslen(path);
    while (length > 3 && (path[length - 1] == L'\\' ||
                          path[length - 1] == L'/'))
        path[--length] = L'\0';
}

static bool bgm_join_path(wchar_t *output, int capacity,
                          const wchar_t *left, const wchar_t *right)
{
    size_t left_length;
    size_t right_length;
    bool separator;
    size_t required;

    if (!output || capacity <= 0 || !left || !right)
        return false;
    left_length = wcslen(left);
    right_length = wcslen(right);
    separator = left_length > 0 && left[left_length - 1] != L'\\';
    required = left_length + (separator ? 1u : 0u) + right_length + 1u;
    if (required > (size_t)capacity)
        return false;
    memcpy(output, left, left_length * sizeof(*output));
    if (separator)
        output[left_length++] = L'\\';
    memcpy(output + left_length, right,
           (right_length + 1u) * sizeof(*output));
    return true;
}

static bool bgm_file_exists_w(const wchar_t *path)
{
    DWORD attributes = path ? GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES;
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool bgm_find_decoded_wav(BgmPlayer *player)
{
    wchar_t pattern[BGM_PATH_CAP];
    wchar_t best_path[BGM_PATH_CAP];
    WIN32_FIND_DATAW data;
    HANDLE handle;
    unsigned long long best_size = 0;

    if (!player || !bgm_join_path(pattern, BGM_PATH_CAP,
                                  player->decoded_directory, L"*.wav"))
        return false;
    best_path[0] = L'\0';
    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    do {
        unsigned long long size;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            continue;
        size = ((unsigned long long)data.nFileSizeHigh << 32) |
               (unsigned long long)data.nFileSizeLow;
        if (size > best_size &&
            bgm_join_path(best_path, BGM_PATH_CAP,
                          player->decoded_directory, data.cFileName))
            best_size = size;
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
    return best_path[0] != L'\0' &&
           bgm_wide_to_utf8(best_path, player->wav_path,
                            (int)sizeof(player->wav_path));
}

static DWORD WINAPI bgm_decode_worker(void *argument)
{
    BgmPlayer *player = (BgmPlayer *)argument;
    wchar_t command[BGM_PATH_CAP * 2 + 16];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD exit_code = (DWORD)-1;
    bool ok = false;

    if (!player)
        return 1;
    if (swprintf(command, (int)(sizeof(command) / sizeof(command[0])),
                 L"\"%ls\" \"%ls\"", player->decoder_path,
                 player->source_path) < 0) {
        InterlockedExchange(&player->decode_state, BGM_DECODE_FAILED);
        return 1;
    }
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    if (CreateProcessW(player->decoder_path, command, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        WaitForSingleObject(process.hProcess, INFINITE);
        ok = GetExitCodeProcess(process.hProcess, &exit_code) && exit_code == 0;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    InterlockedExchange(&player->decode_state,
                        ok ? BGM_DECODE_SUCCEEDED : BGM_DECODE_FAILED);
    return ok ? 0 : 1;
}

static void bgm_close_finished_thread(BgmPlayer *player)
{
    if (!player || !player->decode_thread ||
        InterlockedCompareExchange(&player->decode_state, 0, 0) ==
            BGM_DECODE_RUNNING)
        return;
    WaitForSingleObject(player->decode_thread, INFINITE);
    CloseHandle(player->decode_thread);
    player->decode_thread = NULL;
}

static bool bgm_start_decode(BgmPlayer *player)
{
    if (!player || player->decode_thread || player->decode_attempted ||
        !bgm_file_exists_w(player->source_path) ||
        !bgm_file_exists_w(player->decoder_path))
        return false;
    player->decode_attempted = true;
    InterlockedExchange(&player->decode_state, BGM_DECODE_RUNNING);
    player->decode_thread = CreateThread(NULL, 0, bgm_decode_worker,
                                         player, 0, NULL);
    if (!player->decode_thread) {
        InterlockedExchange(&player->decode_state, BGM_DECODE_FAILED);
        bgm_set_status(player, "无法启动 BGM 解码线程");
        return false;
    }
    bgm_set_status(player, "正在解码 bgm_studio_night...");
    return true;
}

static bool bgm_load_wav(BgmPlayer *player)
{
    Uint8 *data = NULL;
    Uint32 length = 0;
    SDL_AudioSpec spec;
    long long bytes_per_second;

    if (!player || player->wav_path[0] == '\0')
        return false;
    if (!SDL_LoadWAV(player->wav_path, &spec, &data, &length) || !data ||
        length == 0 || length > (Uint32)INT_MAX) {
        if (data)
            SDL_free(data);
        bgm_set_status(player, "读取解码后的 BGM WAV 失败");
        return false;
    }
    player->frame_size = SDL_AUDIO_BYTESIZE(spec.format) * spec.channels;
    if (player->frame_size <= 0) {
        SDL_free(data);
        bgm_set_status(player, "BGM WAV 格式无效");
        return false;
    }
    length -= length % (Uint32)player->frame_size;
    if (length == 0) {
        SDL_free(data);
        bgm_set_status(player, "BGM WAV 没有可播放的采样");
        return false;
    }
    bytes_per_second = (long long)spec.freq * player->frame_size;
    if (bytes_per_second < BGM_QUEUE_MIN_BYTES)
        player->queue_target = BGM_QUEUE_MIN_BYTES;
    else if (bytes_per_second > BGM_QUEUE_MAX_BYTES)
        player->queue_target = BGM_QUEUE_MAX_BYTES;
    else
        player->queue_target = (int)bytes_per_second;
    player->wav_spec = spec;
    player->wav_data = data;
    player->wav_length = length;
    player->wav_cursor = 0;
    fprintf(stderr, "[BGM] 已载入 %s（%u 字节）\n",
            player->wav_path, (unsigned int)length);
    bgm_set_status(player, "BGM 已就绪");
    return true;
}

static bool bgm_open_stream(BgmPlayer *player)
{
    if (!player || !player->wav_data || !player->audio_ready)
        return false;
    if (player->stream)
        return true;
    player->stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &player->wav_spec, NULL, NULL);
    if (!player->stream) {
        bgm_set_status(player, "无法打开 BGM 音频设备");
        return false;
    }
    if (!SDL_SetAudioStreamGain(player->stream, player->volume)) {
        SDL_DestroyAudioStream(player->stream);
        player->stream = NULL;
        bgm_set_status(player, "无法设置 BGM 音量");
        return false;
    }
    return true;
}

static bool bgm_fill_queue(BgmPlayer *player)
{
    int queued;

    if (!player || !player->stream || !player->wav_data ||
        player->wav_length == 0)
        return false;
    queued = SDL_GetAudioStreamQueued(player->stream);
    if (queued < 0)
        return false;
    while (queued < player->queue_target) {
        Uint32 remaining = player->wav_length - player->wav_cursor;
        Uint32 chunk = remaining;
        if (chunk > BGM_FEED_CHUNK_BYTES)
            chunk = BGM_FEED_CHUNK_BYTES;
        chunk -= chunk % (Uint32)player->frame_size;
        if (chunk == 0) {
            player->wav_cursor = 0;
            continue;
        }
        if (!SDL_PutAudioStreamData(player->stream,
                                    player->wav_data + player->wav_cursor,
                                    (int)chunk))
            return false;
        player->wav_cursor += chunk;
        if (player->wav_cursor >= player->wav_length)
            player->wav_cursor = 0;
        queued += (int)chunk;
    }
    return true;
}

BgmPlayer *bgm_player_create(const char *exe_directory_utf8)
{
    BgmPlayer *player = (BgmPlayer *)calloc(1, sizeof(*player));
    wchar_t root[BGM_PATH_CAP];
    wchar_t bgm_directory[BGM_PATH_CAP];
    wchar_t decoded_root[BGM_PATH_CAP];
    SDL_InitFlags initialized;

    if (!player)
        return NULL;
    player->enabled = true;
    player->volume = 0.42f;
    if (!bgm_utf8_to_wide(exe_directory_utf8 ? exe_directory_utf8 : ".",
                          root, BGM_PATH_CAP)) {
        bgm_set_status(player, "程序目录无效");
        return player;
    }
    bgm_trim_trailing_slash(root);
    if (!bgm_join_path(bgm_directory, BGM_PATH_CAP, root,
                       L"CGSS_DOWN\\BGM") ||
        !bgm_join_path(player->source_path, BGM_PATH_CAP, bgm_directory,
                       L"bgm_studio_night.acb") ||
        !bgm_join_path(player->decoder_path, BGM_PATH_CAP, root,
                       L"acb2wavs.exe") ||
        !bgm_join_path(decoded_root, BGM_PATH_CAP, bgm_directory,
                       L"_acb_bgm_studio_night.acb") ||
        !bgm_join_path(player->decoded_directory, BGM_PATH_CAP, decoded_root,
                       L"internal")) {
        bgm_set_status(player, "BGM 路径过长");
        return player;
    }

    initialized = SDL_WasInit(SDL_INIT_AUDIO);
    if ((initialized & SDL_INIT_AUDIO) != 0) {
        player->audio_ready = true;
    } else if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        player->audio_ready = true;
        player->owns_audio_subsystem = true;
    }
    bgm_set_status(player, "等待下载 bgm_studio_night");
    return player;
}

void bgm_player_update(BgmPlayer *player, bool active)
{
    LONG decode_state;

    if (!player)
        return;
    player->active = active;
    bgm_close_finished_thread(player);

    if (!player->wav_data) {
        decode_state = InterlockedCompareExchange(&player->decode_state, 0, 0);
        /* acb2wavs creates the WAV before it has finished writing PCM. */
        if (decode_state == BGM_DECODE_RUNNING) {
            bgm_set_status(player, "正在解码 bgm_studio_night...");
            return;
        }
        if (bgm_find_decoded_wav(player)) {
            if (!bgm_load_wav(player))
                return;
        } else {
            if (decode_state == BGM_DECODE_SUCCEEDED ||
                decode_state == BGM_DECODE_FAILED) {
                bgm_set_status(player, decode_state == BGM_DECODE_SUCCEEDED ?
                    "解码完成，但没有找到 BGM WAV" :
                    "acb2wavs 解码 BGM 失败");
                return;
            }
            if (!bgm_file_exists_w(player->source_path)) {
                bgm_set_status(player, "等待下载 bgm_studio_night");
                return;
            }
            if (!bgm_file_exists_w(player->decoder_path)) {
                bgm_set_status(player, "缺少 acb2wavs.exe");
                return;
            }
            if (bgm_start_decode(player))
                return;
        }
    }

    if (!player->audio_ready) {
        bgm_set_status(player, "BGM 已解码，但音频设备不可用");
        return;
    }
    if (!player->enabled || !player->active) {
        if (player->stream && !SDL_AudioStreamDevicePaused(player->stream))
            SDL_PauseAudioStreamDevice(player->stream);
        bgm_set_status(player, !player->enabled ? "BGM 已关闭" :
                                                  "BGM 已暂停");
        return;
    }
    if (!bgm_open_stream(player) || !bgm_fill_queue(player)) {
        if (player->stream) {
            SDL_DestroyAudioStream(player->stream);
            player->stream = NULL;
        }
        bgm_set_status(player, "BGM 播放设备发生错误");
        return;
    }
    if (SDL_AudioStreamDevicePaused(player->stream) &&
        !SDL_ResumeAudioStreamDevice(player->stream)) {
        bgm_set_status(player, "无法开始播放 BGM");
        return;
    }
    if (!player->playback_logged) {
        fprintf(stderr, "[BGM] 已开始循环播放 bgm_studio_night\n");
        player->playback_logged = true;
    }
    bgm_set_status(player, "正在循环播放 bgm_studio_night");
}

void bgm_player_set_enabled(BgmPlayer *player, bool enabled)
{
    if (player)
        player->enabled = enabled;
}

bool bgm_player_is_enabled(const BgmPlayer *player)
{
    return player && player->enabled;
}

void bgm_player_set_volume(BgmPlayer *player, float volume)
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

float bgm_player_volume(const BgmPlayer *player)
{
    return player ? player->volume : 0.0f;
}

bool bgm_player_source_exists(const BgmPlayer *player)
{
    return player && bgm_file_exists_w(player->source_path);
}

bool bgm_player_is_ready(const BgmPlayer *player)
{
    return player && player->wav_data != NULL;
}

const char *bgm_player_status(const BgmPlayer *player)
{
    return player ? player->status : "BGM 播放器未创建";
}

void bgm_player_retry(BgmPlayer *player)
{
    if (!player || player->decode_thread)
        return;
    player->decode_attempted = false;
    InterlockedExchange(&player->decode_state, BGM_DECODE_IDLE);
    player->wav_path[0] = '\0';
    bgm_set_status(player, "准备重新解码 BGM");
}

void bgm_player_destroy(BgmPlayer *player)
{
    if (!player)
        return;
    if (player->decode_thread) {
        WaitForSingleObject(player->decode_thread, INFINITE);
        CloseHandle(player->decode_thread);
    }
    if (player->stream)
        SDL_DestroyAudioStream(player->stream);
    if (player->wav_data)
        SDL_free(player->wav_data);
    if (player->owns_audio_subsystem)
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    free(player);
}
