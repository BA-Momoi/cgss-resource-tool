#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "unpack.h"
#include "preview.h"
#include "paper.h"
#include "browse.h"
#include "cg.h"
#include "auto_updata.h"
#include "CLI.h"
#include "check_update.h"
#include "util.h"
#include <wchar.h>
#define BUILD_VERIANT "db"

static void prefer_bundled_dotnet(void){
    wchar_t module_path[32768];
    DWORD capacity = (DWORD)(sizeof module_path / sizeof module_path[0]);
    DWORD length = GetModuleFileNameW(NULL, module_path, capacity);
    if (length == 0 || length >= capacity) return;

    wchar_t *separator = wcsrchr(module_path, L'\\');
    if (!separator) return;
    *separator = L'\0';

    wchar_t runtime_root[32768];
    size_t root_capacity = sizeof runtime_root / sizeof runtime_root[0];
    int written = swprintf(runtime_root, root_capacity, L"%ls\\dotnet", module_path);
    if (written < 0 || (size_t)written >= root_capacity) return;

    DWORD attributes = GetFileAttributesW(runtime_root);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return;

    SetEnvironmentVariableW(L"DOTNET_ROOT", runtime_root);
    SetEnvironmentVariableW(L"DOTNET_ROOT_X64", runtime_root);
}

static void use_application_directory(void){
    wchar_t module_path[32768];
    DWORD capacity = (DWORD)(sizeof module_path / sizeof module_path[0]);
    DWORD length = GetModuleFileNameW(NULL, module_path, capacity);
    if (length == 0 || length >= capacity) return;

    wchar_t *separator = wcsrchr(module_path, L'\\');
    if (!separator) return;
    *separator = L'\0';
    SetCurrentDirectoryW(module_path);
}

static HANDLE g_instance_mutex = NULL;

static int acquire_instance_lock(void){
    wchar_t module_path[32768];
    DWORD capacity = (DWORD)(sizeof module_path / sizeof module_path[0]);
    DWORD length = GetModuleFileNameW(NULL, module_path, capacity);
    if (length == 0 || length >= capacity) return 0;

    wchar_t *separator = wcsrchr(module_path, L'\\');
    if (!separator) return 0;
    *separator = L'\0';

    wchar_t marker_path[32768];
    int written = swprintf(marker_path, _countof(marker_path),
                           L"%ls\\CGSS_ResourceTool.update-in-progress",
                           module_path);
    if (written < 0 || (size_t)written >= _countof(marker_path)) return 0;

    for (int attempt = 0; attempt < 600; attempt++){
        if (GetFileAttributesW(marker_path) == INVALID_FILE_ATTRIBUTES) break;
        if (attempt == 0)
            printf("检测到程序正在替换，请等待更新完成...\n");
        Sleep(500);
    }
    if (GetFileAttributesW(marker_path) != INVALID_FILE_ATTRIBUTES){
        fprintf(stderr, "程序更新仍在进行或交接标记未清除；确认更新进程已退出后，可删除：%ls\n",
                marker_path);
        return 0;
    }

    CharLowerBuffW(module_path, (DWORD)wcslen(module_path));
    unsigned long long hash = 14695981039346656037ULL;
    for (const wchar_t *p = module_path; *p; p++){
        unsigned short value = (unsigned short)*p;
        hash ^= value & 0xff;
        hash *= 1099511628211ULL;
        hash ^= value >> 8;
        hash *= 1099511628211ULL;
    }

    wchar_t mutex_name[96];
    written = swprintf(mutex_name, _countof(mutex_name),
                       L"Local\\CGSS_ResourceTool_%016llx", hash);
    if (written < 0 || (size_t)written >= _countof(mutex_name)) return 0;
    SetLastError(ERROR_SUCCESS);
    g_instance_mutex = CreateMutexW(NULL, FALSE, mutex_name);
    if (!g_instance_mutex){
        fprintf(stderr, "创建程序实例锁失败: %lu\n", GetLastError());
        return 0;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS){
        CloseHandle(g_instance_mutex);
        g_instance_mutex = NULL;
        fprintf(stderr, "此目录中的程序已经在运行。\n");
        return 0;
    }
    if (GetFileAttributesW(marker_path) != INVALID_FILE_ATTRIBUTES){
        CloseHandle(g_instance_mutex);
        g_instance_mutex = NULL;
        for (int attempt = 0; attempt < 600; attempt++){
            if (GetFileAttributesW(marker_path) == INVALID_FILE_ATTRIBUTES) break;
            Sleep(500);
        }
        if (GetFileAttributesW(marker_path) != INVALID_FILE_ATTRIBUTES){
            fprintf(stderr, "程序更新仍在进行或交接标记未清除；确认更新进程已退出后，可删除：%ls\n",
                    marker_path);
            return 0;
        }
        return acquire_instance_lock();
    }
    return 1;
}

static HANDLE app_version_thread, resource_version_thread;
static double latest_app_version;
static long long latest_resource_version;
static int resource_sync_attempted, exit_for_update;

static DWORD WINAPI fetch_app_version(LPVOID arg){
    (void)arg;
    latest_app_version = updata_latest_version();
    return 0;
}

static DWORD WINAPI fetch_resource_version(LPVOID arg){
    (void)arg;
    latest_resource_version = check_update_latest_version(1);
    return 0;
}

static int browse_with_current_data(void){
    if (!resource_sync_attempted){
        const int have_local = find_manifest() && GetFileAttributesW(L"master.mdb") != INVALID_FILE_ATTRIBUTES;
        DWORD ready = resource_version_thread ?
            WaitForSingleObject(resource_version_thread, have_local ? 0 : INFINITE) : WAIT_OBJECT_0;
        if (ready == WAIT_OBJECT_0){
            resource_sync_attempted = 1;
            int result = check_update_run_with_version(NULL, latest_resource_version);
            if (result != 0) printf("数据库同步未完成，将继续使用本地数据库。\n");
        }
    }
    return browse_main();
}

static int apply_app_update(void){
    printf("正在检查程序更新...\n");
    fflush(stdout);
    if (app_version_thread){
        WaitForSingleObject(app_version_thread, INFINITE);
        CloseHandle(app_version_thread);
        app_version_thread = NULL;
    }
    if (latest_app_version <= 0) latest_app_version = updata_latest_version();
    int result = updata_apply_version(CGSS_VERSION, latest_app_version);
    if (result == 2) exit_for_update = 1;
    else system("pause");
    return result;
}

int main(int argc, char *argv[]){
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);   // 强制 UTF-8 控制台，输入输出统一
    enable_vt();
    if (!acquire_instance_lock()) return 2;
    prefer_bundled_dotnet();
    if (argc >= 2){
        if (strcmp(argv[1], "--preview") == 0 || strcmp(argv[1], "-p") == 0)
            use_application_directory();
        if (strcmp(argv[1], "--browse") == 0 || strcmp(argv[1], "-b") == 0 ||
            strcmp(argv[1], "--download") == 0 || strcmp(argv[1], "-d") == 0){
            use_application_directory();
            if (check_update_run(NULL) != 0)
                printf("数据库同步未完成，将继续使用本地数据库。\n");
        }
        return cli_main(argc, argv);
    }

    use_application_directory();
    app_version_thread = CreateThread(NULL, 0, fetch_app_version, NULL, 0, NULL);
    resource_version_thread = CreateThread(NULL, 0, fetch_resource_version, NULL, 0, NULL);
    /* 查找和下载已整合成一个模块(browse.c), 主菜单只留一个入口 */
    def menu[] = {
        {"1.资源查找与下载", browse_with_current_data, 0},
        {"2.打开 Spine 资源库", open_spine_preview, 0},
        {"3.USM/CG 手动解包", unpack_usm, 0},
        {"4.检查程序更新", apply_app_update, 0},
        {"5.退出", NULL, 0},
        {"END", NULL, 0}            /* 哨兵必须最后一行 */
    };

    while(!exit_for_update){
        if (app_version_thread && WaitForSingleObject(app_version_thread, 0) == WAIT_OBJECT_0 &&
            latest_app_version > CGSS_VERSION)
            snprintf(menu[3].name, sizeof menu[3].name, "4.检查程序更新（有新版）");
        int rc = pager_pick("主菜单", menu, 0);
        if(rc == -1)                /* Esc: 继续显示菜单 */
            continue;
        if(rc == 4)                 /* "5.退出" 是第 4 项(下标从 0 数) */
            break;
        /* 选中项的 func 已经在 pager 里调用过了, 这里直接循环 */
    }

    if (app_version_thread) CloseHandle(app_version_thread);
    if (resource_version_thread) CloseHandle(resource_version_thread);
    if (exit_for_update){
        printf("正在更新，程序即将关闭...\n");
        return 0;
    }
    fflush(stdout);    // 先把结果全部输出，再等按键，避免重定向时和 pause 混在一起
    system("pause");   // 双击 exe 时窗口不闪退，按任意键退出
    return 0;
}
