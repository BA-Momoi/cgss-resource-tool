#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include <winhttp.h>
#include <stdlib.h>
#include <pathcch.h>
#include "auto_updata.h"
#include "util.h"
#include "paper.h"
#include "unpack.h"
#include "miniz/miniz.h"

static HINTERNET g_sess = NULL;
int DOWNERROR = 0; 

typedef struct Version_Num{     //存提取出来的参数
    double version;
    wchar_t down_url[256];
}Version_num;
/* 释放更新脚本到磁盘,启动它并让脚本接管后续替换流程。
   wroot: 临时更新目录的父目录
   target_dir: 当前 exe 所在目录（实际要替换的目录）
   exe_name: 主程序 exe 的文件名
   pid: 当前进程 PID，脚本只等待这个 PID 退出
   成功返回0,失败返回-1 */
static int run_update_script(const wchar_t *wroot,
                             const wchar_t *target_dir,
                             const wchar_t *exe_name,
                             DWORD pid)
{
    wchar_t bat_path[MAX_PATH];
    swprintf(bat_path, _countof(bat_path), L"%ls\\update.bat", wroot);

    FILE *f = _wfopen(bat_path, L"wb");
    if (!f) {
        fprintf(stderr, "无法创建更新脚本\n");
        return -1;
    }

    /* bat 内容,%~1 = 根目录, %~2 = 目标目录, %~3 = exe文件名, %~4 = PID */
    const char *bat_content =
        "@echo off\r\n"
        "setlocal enabledelayedexpansion\r\n"
        "chcp 65001 >nul\r\n"
        "set \"ROOT=%~1\"\r\n"
        "set \"TARGET_DIR=%~2\"\r\n"
        "set \"EXE_NAME=%~3\"\r\n"
        "set \"PARENT_PID=%~4\"\r\n"
        "set \"UPDATE_MARKER=%TARGET_DIR%\\CGSS_ResourceTool.update-in-progress\"\r\n"
        "if \"%ROOT:~-1%\"==\"\\\" set \"ROOT=%ROOT:~0,-1%\"\r\n"
        "set \"SOURCE_DIR=%ROOT%\\updata\\CGSS_ResourceTool\"\r\n"
        "set \"UPDATE_TMP=%ROOT%\\updata\"\r\n"
        "set \"ZIP_FILE=%ROOT%\\updata.zip\"\r\n"
        ":WAIT_EXIT\r\n"
        "if not defined PARENT_PID goto AFTER_WAIT\r\n"
        "tasklist /FI \"PID eq %PARENT_PID%\" /NH 2>NUL | find \"%PARENT_PID%\" >NUL\r\n"
        "if \"%ERRORLEVEL%\"==\"0\" (\r\n"
        "    timeout /t 1 /nobreak >NUL\r\n"
        "    goto WAIT_EXIT\r\n"
        ")\r\n"
        ":AFTER_WAIT\r\n"
        "timeout /t 1 /nobreak >NUL\r\n"
        "if not exist \"%SOURCE_DIR%\" (\r\n"
        "    echo 错误: 未找到新版本文件夹 %SOURCE_DIR%\r\n"
        "    del /f /q \"%UPDATE_MARKER%\" >NUL 2>NUL\r\n"
        "    pause\r\n"
        "    exit /b 1\r\n"
        ")\r\n"
        "echo 正在替换程序文件...\r\n"
        "robocopy \"%SOURCE_DIR%\" \"%TARGET_DIR%\" /E /NFL /NDL /NJH /NJS /R:3 /W:1 /XF \"master.mdb\" \"master.mdb.sync\" \"master.mdb.sync.tmp\" \"master.mdb.lz4\" \"master.mdb.update\" \"master.mdb.update.sync\" \"master.mdb.update.sync.tmp\" \"manifest_*.db*\"\r\n"
        "if %ERRORLEVEL% GEQ 8 (\r\n"
        "    echo 替换过程中出现错误, 错误码: %ERRORLEVEL%\r\n"
        "    del /f /q \"%UPDATE_MARKER%\" >NUL 2>NUL\r\n"
        "    pause\r\n"
        "    exit /b 1\r\n"
        ")\r\n"
        "if exist \"%UPDATE_TMP%\" rd /s /q \"%UPDATE_TMP%\"\r\n"
        "if exist \"%ZIP_FILE%\" del /f /q \"%ZIP_FILE%\"\r\n"
        "echo 更新完成, 正在重启程序...\r\n"
        "start \"\" \"%TARGET_DIR%\\%EXE_NAME%\"\r\n"
        "del /f /q \"%UPDATE_MARKER%\" >NUL 2>NUL\r\n"
        "(goto) 2>nul & del \"%~f0\"\r\n";

    int script_ok = fputs(bat_content, f) >= 0;
    if (fclose(f) != 0) script_ok = 0;
    if (!script_ok){
        DeleteFileW(bat_path);
        fprintf(stderr, "写入更新脚本失败\n");
        return -1;
    }

    wchar_t marker_path[MAX_PATH];
    swprintf(marker_path, _countof(marker_path),
             L"%ls\\CGSS_ResourceTool.update-in-progress", target_dir);
    HANDLE marker = CreateFileW(marker_path, GENERIC_WRITE, 0, NULL,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (marker == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "无法创建更新交接标记: %lu\n", GetLastError());
        return -1;
    }
    CloseHandle(marker);

    /* 构造 cmd.exe /c "bat路径" "根目录" "目标目录" "exe名" "PID" */
    wchar_t cmdline[MAX_PATH * 5];
    swprintf(cmdline, _countof(cmdline),
             L"cmd.exe /c \"\"%ls\" \"%ls\" \"%ls\" \"%ls\" \"%lu\"\"",
             bat_path, wroot, target_dir, exe_name, (unsigned long)pid);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    ZeroMemory(&pi, sizeof pi);
    si.cb = sizeof si;

    BOOL ok = CreateProcessW(
        NULL,           /* 应用程序名, 用命令行里的就行, 传NULL */
        cmdline,        /* 完整命令行 */
        NULL, NULL,     /* 安全属性 */
        FALSE,          /* 不继承句柄 */
        CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP,  /* 独立新窗口+新进程组 */
        NULL,           /* 环境变量, 用父进程的 */
        wroot,          /* 工作目录 */
        &si, &pi
    );

    if (!ok) {
        DWORD error = GetLastError();
        DeleteFileW(marker_path);
        fprintf(stderr, "启动更新脚本失败, 错误码: %lu\n", error);
        return -1;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}
/* 根据目录一次创建父级 返回1创建完成，返回-1为创建失败 */
static int make_parent_dirs(const char * file_path){
    char file[MAX_PATH];
    char *p;
    
    strcpy(file,file_path);

    for(p = file;*p;p++){
        if(*p == '\\'|| *p == '/'){
            char save = *p;
            *p = '\0';

            if(*file && !CreateDirectoryA(file,NULL) && GetLastError() != ERROR_ALREADY_EXISTS){    //创建文件失败为权限不足才返回错误
                *p = save;
                return -1;
            }   
            *p =save;
        }
    }
    return 1;
}

/* zip解包 完成返回1 打开失败返回-1，解包失败返回0 */
int unzip(const char *zip_path,const char *output_path){
    mz_zip_archive zip;
    mz_zip_error error;
    int results = 1;

    mz_zip_zero_struct(&zip);

    if(!mz_zip_reader_init_file(&zip,zip_path,0)){
        error = mz_zip_get_last_error(&zip);
        fprintf(stderr,"打开%s失败%s\n",zip_path,mz_zip_get_error_string(error));   
        return -1;   
    }

    mz_uint i,filecount;

    filecount = mz_zip_reader_get_num_files(&zip);  //zip内文件数量

    for(i = 0;i < filecount;i++){
        char name[1024];
        char dst_path[1024];
        char *p;

        if(mz_zip_reader_is_file_a_directory(&zip,i))
            continue;
        
        mz_zip_reader_get_filename(&zip,i,name,sizeof name);
        printf("解压%s...\n",name);

        snprintf(dst_path,sizeof dst_path,"%s\\%s",output_path,name);
        

        for(p = dst_path;*p;p++){    //miniz 内部使用的是/，需转换
            if(*p == '/')
                *p = '\\';
        }
    
        if(make_parent_dirs(dst_path) == -1){
            fprintf(stderr,"创建%s失败\n",dst_path);
            results = 0;
            continue;
        }

        if(!mz_zip_reader_extract_to_file(&zip,i,dst_path,0)){
            error = mz_zip_get_last_error(&zip);
            fprintf(stderr,"解压失败:%s %s\n",dst_path,mz_zip_get_error_string(error));
            results = 0;
            continue;
        }
        printf("解压成功: %s\n", dst_path);
    }
    mz_zip_reader_end(&zip);
    return results;
}

/* updata的WinHttp初始化 */
static void updata_init(void){
    if (g_sess) return;
    g_sess = WinHttpOpen(L"WinHTTP-GitHub-Client/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_sess) return;
    DWORD to = 60000;
    WinHttpSetTimeouts(g_sess, to, to, to, to);
#ifdef WINHTTP_OPTION_SECURE_PROTOCOLS
    DWORD prot = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 |
                 WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    prot |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(g_sess, WINHTTP_OPTION_SECURE_PROTOCOLS, &prot, sizeof(prot));
#endif
}
// tag Version_num结构体，wroot存目录的地方，PATH_SIZE wroot的长度
static int down_file(Version_num *tag,wchar_t *wroot,size_t PATH_SIZE){
    FILE *zip_file = NULL;
    BOOL bResults = FALSE;
    HINTERNET hConnect = NULL,hRequest = NULL;
    wchar_t zippath[MAX_PATH];
    swprintf(zippath,_countof(zippath),L"%ls\\updata.zip",wroot);
    zip_file = _wfopen(zippath,L"wb");
    if (!zip_file){
        fprintf(stderr,"创建更新压缩包失败: %lu\n",GetLastError());
        return -1;
    }
    
    char utf8_wroot[512];
    wide_to_utf8(wroot,utf8_wroot,PATH_SIZE);
    printf("将下载在%s...\n",utf8_wroot);
    wchar_t host[256];
    wchar_t path[2048];

    URL_COMPONENTS url;

    ZeroMemory(&url,sizeof url);
    url.dwStructSize = sizeof url; 
    url.lpszHostName = host;
    url.dwHostNameLength = ARRAYSIZE(host);

    url.lpszUrlPath = path;
    url.dwUrlPathLength = ARRAYSIZE(path);

    url.dwSchemeLength = (DWORD)-1;

    if(!WinHttpCrackUrl(tag->down_url,0,0,&url)){
        fprintf(stderr,"WinHttpCrackUrl失败%lu\n",GetLastError());
        return -1;
    }
    char utf8_host[256],utf8_path[2048];
    wide_to_utf8(host,utf8_host,_countof(host));
    printf("Host:%s\n",utf8_host);
    wide_to_utf8(path,utf8_path,_countof(utf8_path));
    printf("Path:%s\n",utf8_path);
    printf("port:%u\n",(unsigned long)url.nPort);

    if(g_sess)
        hConnect = WinHttpConnect(g_sess,host,url.nPort,0);
    else{
        fprintf(stderr,"g_sess初始化未完成\n");
        return -1;
    }
    if(hConnect)
        hRequest = WinHttpOpenRequest(hConnect,L"GET",path,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
    else{
        fprintf(stderr,"WinHttpConnect%lu错误\n",GetLastError());
        return -1;
    }
    if(hRequest)
        bResults = WinHttpSendRequest(hRequest,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    else{
        fprintf(stderr,"WinHttpOpenRequest%lu错误\n",GetLastError());
        return -1;
    }
    if(bResults)
        bResults = WinHttpReceiveResponse(hRequest,NULL);
    else{
        fprintf(stderr,"WinHttpSendRequest%lu错误",GetLastError());
        return -1;
    }
    DWORD status = 0, status_size = sizeof(status);
    if(!WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                            WINHTTP_NO_HEADER_INDEX) || status != HTTP_STATUS_OK){
        fprintf(stderr,"更新包 HTTP 状态异常: %lu\n",
                (unsigned long)(status ? status : GetLastError()));
        fclose(zip_file);
        DeleteFileW(zippath);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        return -1;
    }
    int error_count = 0;
    LONGLONG total = 0;
    DOWNERROR = 0;
    if(bResults){
        do{
            unsigned char  buf[131072]; //128k缓冲区
            DWORD avail = 0, got = 0;
            
            for(;;){
                if(!WinHttpQueryDataAvailable(hRequest,&avail)){
                    printf("ERROR %u IN WINHTTPQUERYDATAAVAILABLE.\n",
                    GetLastError());
                    DOWNERROR = 1;
                    error_count = 4;
                    break;
                }
                if(avail == 0){
                    DOWNERROR = 0;
                    break;
                }
                if(avail > sizeof buf)avail = sizeof buf;
                if(!WinHttpReadData(hRequest,buf,avail,&got) || got == 0){
                    DOWNERROR = 1;
                    error_count = 4;
                    break;
                }
                if(fwrite(buf, 1, got, zip_file) != got){
                    DOWNERROR = 1;
                    error_count = 4;
                    break;
                }
                total += got;
            }
        }while(error_count <= 3 && DOWNERROR == 1);
        printf("下载完成: %lld KB\n", (long long)(total / 1024));
    }
    int close_ok = fclose(zip_file) == 0;
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    if(DOWNERROR == 1 || total <= 0 || !close_ok){
        DeleteFileW(zippath);
        fprintf(stderr,"下载失败\n");
        return -1;
    }
    return 0;
}

double updata_latest_version(void){
    updata_init();
    if (!g_sess) return 0;
    HINTERNET conn = WinHttpConnect(g_sess, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) return 0;
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", L"/BA-Momoi/cgss-resource-tool/releases.atom",
                                      NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    double latest = 0;
    if (req){
        WinHttpSetTimeouts(req, 5000, 5000, 5000, 5000);
        DWORD status = 0, status_size = sizeof status;
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, NULL) &&
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) &&
            status == 200){
            char body[65536];
            DWORD used = 0, got;
            while (used < sizeof body - 1 &&
                   WinHttpReadData(req, body + used, sizeof body - 1 - used, &got) && got){
                used += got;
                body[used] = '\0';
                const char *title = strstr(body, "<title>v");
                if (title && strstr(title, "</title>")){
                    sscanf(title, "<title>v%lf", &latest);
                    break;
                }
            }
        }
        WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(conn);
    return latest;
}

int updata_apply_version(double version, double latest){
    if (latest <= 0){
        fprintf(stderr, "查询 GitHub 发布版本失败，请稍后重试。\n");
        return -1;
    }
    printf("GitHub 发布版本：%.2f\n", latest);
    if (version >= latest){
        printf("程序已是最新版本。\n");
        return 0;
    }

    Version_num tag = {0};
    tag.version = latest;
    snwprintf(tag.down_url, _countof(tag.down_url),
              L"https://github.com/BA-Momoi/cgss-resource-tool/releases/latest/download/CGSS_ResourceTool.zip");

    wchar_t exeFullPath[MAX_PATH];
    DWORD pathlen = GetModuleFileNameW(NULL, exeFullPath, MAX_PATH);
    if(pathlen == 0 || pathlen == MAX_PATH){
        fprintf(stderr,"路径获取失败\n");
        return -1;
    }
    wchar_t target_dir[MAX_PATH];
    wcscpy(target_dir, exeFullPath);
    HRESULT rc = PathCchRemoveFileSpec(target_dir,MAX_PATH);
    if(FAILED(rc)){
        fprintf(stderr,"执行PathCchRemoveFileSpec函数发生错误\n");
        return -1;
    }
    wchar_t wroot[MAX_PATH];
    wcscpy(wroot, target_dir);
    rc = PathCchRemoveFileSpec(wroot,MAX_PATH);
    if(FAILED(rc)){
        fprintf(stderr,"执行PathCchRemoveFileSpec函数发生错误\n");
        return -1;
    }
    int hc = 0;
    int zc = 0;

    /* 同版本不需要再次下载，否则更新后重启会再次进入更新流程。 */
    if(version < tag.version){
        versiondef choice[] ={
            {version,0},
            {tag.version,0},
            {0.0,0}
        };
        int sel = pager_pick_version("请选择版本",choice,version,0);
        if(sel == -1 || sel == 0){
            return 0;   //用户取消或选择保留当前版本
        }

        /* 清理上一次失败留下的半解压目录和旧压缩包，避免混入本次更新。 */
        wchar_t update_tmp[MAX_PATH], update_zip[MAX_PATH];
        swprintf(update_tmp, _countof(update_tmp), L"%ls\\updata", wroot);
        swprintf(update_zip, _countof(update_zip), L"%ls\\updata.zip", wroot);
        if(GetFileAttributesW(update_tmp) != INVALID_FILE_ATTRIBUTES){
            wipe_dir(update_tmp);
            RemoveDirectoryW(update_tmp);
        }
        DeleteFileW(update_zip);

        hc = down_file(&tag,wroot,MAX_PATH);
        if(hc != 0){
            return -1;  //下载失败
        }

        char utf8_wroot[MAX_PATH];
        char utf8_wroot_file[MAX_PATH];
        wide_to_utf8(wroot,utf8_wroot,MAX_PATH);
        snprintf(utf8_wroot_file,sizeof utf8_wroot_file,"%s\\updata.zip",utf8_wroot);
        char outpath[MAX_PATH];
        snprintf(outpath,sizeof outpath,"%s\\updata",utf8_wroot);
        zc = unzip(utf8_wroot_file,outpath);

        if(zc != 1){
            return -1;  //解压失败
        }
        wchar_t source_dir[MAX_PATH];
        swprintf(source_dir, _countof(source_dir),
                 L"%ls\\updata\\CGSS_ResourceTool", wroot);
        if(GetFileAttributesW(source_dir) == INVALID_FILE_ATTRIBUTES){
            fprintf(stderr,"更新包中未找到 CGSS_ResourceTool 目录\n");
            return -1;
        }
        wchar_t *exeName = wcsrchr(exeFullPath, L'\\');
        exeName = exeName ? exeName + 1 : exeFullPath;

        if(run_update_script(wroot, target_dir, exeName, GetCurrentProcessId()) == 0)
            return 2;   //脚本已启动,主程序需要立刻退出
        else
            return -1;  //脚本启动失败
    }

    return 0;   //无需更新(当前已是最新版本)
}

int updata_main(double version){
    printf("正在检查程序更新...\n");
    fflush(stdout);
    return updata_apply_version(version, updata_latest_version());
}
