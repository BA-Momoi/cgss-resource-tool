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

int main(int argc, char *argv[]){
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);   // 强制 UTF-8 控制台，输入输出统一
    enable_vt();
    if (!acquire_instance_lock()) return 2;
    prefer_bundled_dotnet();
    if (argc >= 2){
        if (strcmp(argv[1], "--browse") == 0 || strcmp(argv[1], "-b") == 0 ||
            strcmp(argv[1], "--download") == 0 || strcmp(argv[1], "-d") == 0){
            use_application_directory();
            if (check_update_run(NULL) != 0)
                printf("数据库同步未完成，将继续使用本地数据库。\n");
        }
        return cli_main(argc, argv);
    }

    use_application_directory();
    int rc = updata_main(CGSS_VERSION);
    if(rc == -1){
        
        printf("更新失败，请关闭程序重试\n");
        fflush(stdout);
        system("pause");
    }
    if(rc == 2){
    /* 更新脚本已经在后台/新窗口跑起来了,主程序必须立刻退出 */
    printf("正在更新,程序即将关闭...\n");
    fflush(stdout);
    return 0;
    }
    if (check_update_run(NULL) != 0)
        printf("数据库同步未完成，将继续使用本地数据库。\n");
    /* 查找和下载已整合成一个模块(browse.c), 主菜单只留一个入口 */
    def menu[] = {
        {"1.资源查找与下载", browse_main, 0},
        {"2.解包", unpack_main, 0},
        {"3.打开Spine预览(beta)", open_spine_preview, 0},
        {"4.USM/CG解包", unpack_usm, 0},
        {"5.退出", NULL, 0},
        {"END", NULL, 0}            /* 哨兵必须最后一行 */
    };

    while(1){
        int rc = pager_pick("主菜单", menu, 0);
        if(rc == -1)                /* Esc: 继续显示菜单 */
            continue;
        if(rc == 4)                 /* "5.退出" 是第 4 项(下标从 0 数) */
            break;
        /* 选中项的 func 已经在 pager 里调用过了, 这里直接循环 */
    }

    fflush(stdout);    // 先把结果全部输出，再等按键，避免重定向时和 pause 混在一起
    system("pause");   // 双击 exe 时窗口不闪退，按任意键退出
    return 0;
}
