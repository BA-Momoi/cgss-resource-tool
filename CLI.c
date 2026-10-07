#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "CLI.h"
#include "auto_updata.h"
#include "browse.h"
#include "cg.h"
#include "unpack.h"
#include "preview.h"

static void print_usage(const char *program) {
    printf("CGSS Resource Tool %.2f\n", CGSS_VERSION);
    printf("用法: %s <命令> [参数]\n", program);
    printf("  --help, -h                 显示帮助\n");
    printf("  --version, -v              显示版本\n");
    printf("  --unpack, -u <目录>        解包目录下角色资源中的所有未处理文件\n");
    printf("  --browse, -b               打开资源查找与下载菜单\n");
    printf("  --download, -d             打开资源下载菜单\n");
    printf("  --preview, -p              打开 Spine 预览\n");
    printf("  --unpack-usm, -uu           打开 USM/CG 解包菜单\n");
    printf("  --cg, -c                   打开 USM/CG 解包菜单\n");
    printf("  --auto-update, -au         检查并安装更新\n");
}

static int run_noarg_command(int argc, const char *option, int (*action)(void)) {
    if (argc != 2) {
        fprintf(stderr, "参数错误: %s 不接受额外参数\n", option);
        return 2;
    }
    return action() < 0 ? 1 : 0;
}

int cli_main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }

    const char *option = argv[1];
    if (strcmp(option, "--help") == 0 || strcmp(option, "-h") == 0) {
        if (argc != 2) {
            fprintf(stderr, "参数错误: --help 不接受额外参数\n");
            return 2;
        }
        print_usage(argv[0]);
        return 0;
    }
    if (strcmp(option, "--version") == 0 || strcmp(option, "-v") == 0) {
        if (argc != 2) {
            fprintf(stderr, "参数错误: --version 不接受额外参数\n");
            return 2;
        }
        printf("%.2f\n", CGSS_VERSION);
        return 0;
    }
    if (strcmp(option, "--unpack") == 0 || strcmp(option, "-u") == 0) {
        if (argc != 3) {
            fprintf(stderr, "用法: %s %s <资源目录>\n", argv[0], option);
            return 2;
        }
        int wide_len = MultiByteToWideChar(CP_ACP, 0, argv[2], -1, NULL, 0);
        if (wide_len <= 0) {
            fprintf(stderr, "无法解析资源目录路径\n");
            return 2;
        }
        wchar_t *path = (wchar_t *)malloc((size_t)wide_len * sizeof *path);
        if (!path) {
            fprintf(stderr, "内存不足\n");
            return 1;
        }
        if (!MultiByteToWideChar(CP_ACP, 0, argv[2], -1, path, wide_len)) {
            free(path);
            fprintf(stderr, "无法解析资源目录路径\n");
            return 2;
        }
        int rc = unpack_resources_path(path, 0);
        free(path);
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(option, "--browse") == 0 || strcmp(option, "-b") == 0 ||
        strcmp(option, "--download") == 0 || strcmp(option, "-d") == 0)
        return run_noarg_command(argc, option, browse_main);
    if (strcmp(option, "--preview") == 0 || strcmp(option, "-p") == 0)
        return run_noarg_command(argc, option, open_spine_preview);
    if (strcmp(option, "--unpack-usm") == 0 || strcmp(option, "-uu") == 0 ||
        strcmp(option, "--cg") == 0 || strcmp(option, "-c") == 0)
        return run_noarg_command(argc, option, unpack_usm);
    if (strcmp(option, "--auto-update") == 0 || strcmp(option, "-au") == 0) {
        if (argc != 2) {
            fprintf(stderr, "参数错误: %s 不接受额外参数\n", option);
            return 2;
        }
        int rc = updata_main(CGSS_VERSION);
        return rc == -1 ? 1 : 0;
    }

    fprintf(stderr, "未知命令: %s\n", option);
    print_usage(argv[0]);
    return 2;
}
