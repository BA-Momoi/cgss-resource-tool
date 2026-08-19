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
        "    pause\r\n"
        "    exit /b 1\r\n"
        ")\r\n"
        "echo 正在替换程序文件...\r\n"
        "robocopy \"%SOURCE_DIR%\" \"%TARGET_DIR%\" /E /NFL /NDL /NJH /NJS /R:3 /W:1\r\n"
        "if %ERRORLEVEL% GEQ 8 (\r\n"
        "    echo 替换过程中出现错误, 错误码: %ERRORLEVEL%\r\n"
        "    pause\r\n"
        "    exit /b 1\r\n"
        ")\r\n"
        "if exist \"%UPDATE_TMP%\" rd /s /q \"%UPDATE_TMP%\"\r\n"
        "if exist \"%ZIP_FILE%\" del /f /q \"%ZIP_FILE%\"\r\n"
        "echo 更新完成, 正在重启程序...\r\n"
        "start \"\" \"%TARGET_DIR%\\%EXE_NAME%\"\r\n"
        "(goto) 2>nul & del \"%~f0\"\r\n";

    fputs(bat_content, f);
    fclose(f);

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
        fprintf(stderr, "启动更新脚本失败, 错误码: %lu\n", GetLastError());
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
    int error_count = 0;
    LONGLONG total = 0;
    if(bResults){
        do{
            unsigned char  buf[131072]; //128k缓冲区
            DWORD avail = 0, got = 0;
            
            for(;;){
                if(!WinHttpQueryDataAvailable(hRequest,&avail)){
                    printf("ERROR %u IN WINHTTPQUERYDATAAVAILABLE.\n",
                    GetLastError());
                    DOWNERROR = 1;
                    total = 0;
                    error_count++;
                    fprintf(stderr,"第%d次下载失败...\n",(error_count++) + 1);
                    break;
                }
                if(avail == 0){
                    DOWNERROR = 0;
                    break;
                }
                if(avail > sizeof buf)avail = sizeof buf;
                if(!WinHttpReadData(hRequest,buf,avail,&got) || got == 0){
                    DOWNERROR = 0;
                    break;
                }
                fwrite(buf, 1, got, zip_file);      /* 写盘 */
                total += got;
            }
        }while(error_count <= 3 && DOWNERROR == 1);
        printf("下载完成: %lld KB\n", (long long)(total / 1024));
    }
    fclose(zip_file);   //不管有没有下载完都关掉
    if(DOWNERROR == 1){
        fprintf(stderr,"下载失败\n");
        return -1;
    }
    return 0;
}

int updata_main(double version){
    printf("请确保自己的网络环境可以正常连接Github\n");
    printf("正在检查更新...\n");
    fflush(stdout);
    DWORD dwSize = 0;
    DWORD dwDownloaded = 0;
    LPSTR pszOutBuffer;
    HINTERNET hConnect = NULL,hRequest = NULL;
    BOOL bResults = FALSE;
    Version_num tag = {0};
    snwprintf(tag.down_url,_countof(tag.down_url),L"https://github.com/BA-Momoi/cgss-resource-tool/releases/latest/download/CGSS_ResourceTool.zip");
    DWORD len = 0;       /* 已存字节数 */
    char *tmp = NULL;   //存数据的指针
    updata_init();
    if(g_sess)
        hConnect = WinHttpConnect(g_sess,L"github.com",INTERNET_DEFAULT_HTTPS_PORT,0);
    else{
        fprintf(stderr,"WinHttpOpen%lu失败\n",GetLastError());
        return -1;
    }
    if(hConnect)
        hRequest = WinHttpOpenRequest(hConnect,L"GET",L"/BA-Momoi/cgss-resource-tool/releases.atom",   //访问的虚拟地址
        NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
    else{
        fprintf(stderr,"WinHttpConnect%lu失败\n",GetLastError());
        return -1;
    }
    if(hRequest)
        bResults = WinHttpSendRequest(hRequest,WINHTTP_NO_ADDITIONAL_HEADERS,0,
        WINHTTP_NO_REQUEST_DATA,0,0,0);
    if(bResults)
        bResults = WinHttpReceiveResponse(hRequest,NULL);
    else
        fprintf(stderr,"WinHttpSendRequest%lu失败\n",GetLastError());
    int error_count = 0;
    if(bResults){
        do{
            free(tmp);
            tmp = NULL;
            len = 0;
            size_t ram_size = 0;
            DWORD cap = 65536;   /* 容量 */
            do{
                dwSize = 0;
                if(!WinHttpQueryDataAvailable(hRequest,&dwSize)){
                    printf("ERROR %u IN WINHTTPQUERYDATAAVAILABLE.\n",
                    GetLastError());
                    break;
                }
                    pszOutBuffer = malloc((dwSize+1)*sizeof(char));
                if(!pszOutBuffer){
                    printf("Out of memory\n");
                    dwSize = 0;
                    break;
                }
                else{
                    ZeroMemory(pszOutBuffer,dwSize+1);
                    if(!WinHttpReadData(hRequest,(LPVOID)pszOutBuffer,dwSize,&dwDownloaded)){
                        printf("ERROR %u IN WINHTTPREADDATA.\n",GetLastError());
                        DOWNERROR = 1;
                        free(pszOutBuffer);
                        fprintf(stderr,"第%d次下载失败...\n",(error_count++) + 1);
                        break;  //下载错误退出并重新下载，最多三次
                    }
                    else{  
                        /*if(strstr(pszOutBuffer,"\"tag_name\"") != NULL){
                            char *start_ptr = strstr(pszOutBuffer,"\"tag_name\"");
                            sscanf(start_ptr,"\"tag_name\":\"v%lf\"",&tag.version); //将版本号提取至变量
                        */
                        if (tmp == NULL){
                            char *np = (char*)malloc(sizeof(char)*cap);
                            if (!np){ 
                                DOWNERROR = 1;
                                tmp = NULL;
                                free(pszOutBuffer);
                                fprintf(stderr,"内存分派失败\n");
                                break;
                            }
                            tmp = np;
                        }
                        if (len + dwDownloaded > cap){
                            while (len + dwDownloaded > cap) cap *= 2;
                            char *np = (char*)realloc(tmp, cap);
                            if (!np){ 
                                free(tmp);
                                DOWNERROR = 1;
                                tmp = NULL;
                                free(pszOutBuffer);
                                fprintf(stderr,"内存分派失败\n");
                                break;
                            }
                            tmp = np;
                        }
                        memcpy(tmp + len, pszOutBuffer, dwDownloaded);   /* 拷进去 */
                        len += dwDownloaded;  
                    }
                }
                DOWNERROR = 0;
                free(pszOutBuffer);
            }while(dwSize > 0);
        }while(DOWNERROR == 1 && error_count <= 3);
    }
    if (tmp){
        tmp[len] = 0;
    /*printf("收到 %lu 字节\n", (unsigned long)len);
    if (tmp)
        printf("开头: %.200s\n", tmp);   先确认收到的是 JSON */
        char *np = strstr(tmp,"<title>v");
        if(np){   
            sscanf(np,"<title>v%lf</title>",&tag.version);
            if(tag.down_url != NULL && tag.version != 0.0){
                printf("Version:%.2lf\n",tag.version);
                wprintf(L"Url:%ls\n",tag.down_url);
            }else
                fprintf(stderr,"获取版本号和链接失败\n");
        }
        else
            fprintf(stderr,"访问%s后并无有效信息\n","github.com/BA-Momoi/cgss-resource-tool/releases.atom");
        
    }
    if(!bResults)
        fprintf(stderr,"ERROR %d has occurred.\n",GetLastError());
    if(hRequest) WinHttpCloseHandle(hRequest);
    if(hConnect) WinHttpCloseHandle(hConnect);
    
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
