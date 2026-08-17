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
#include "miniz/miniz.h"

static HINTERNET g_sess = NULL;
int DOWNERROR = 0; 

typedef struct Version_Num{     //存提取出来的参数
    double version;
    wchar_t down_url[256];
}Version_num;

/* zip解包 */
int unzip(const char *zip_path,const char *output_path){
    mz_zip_archive zip;
    mz_zip_error error;

    mz_zip_zero_struct(&zip);

    if(!mz_zip_reader_init_file(&zip,zip_path,0)){
        error = mz_zip_get_last_error(&zip);
        fprintf(stderr,"打开%s失败%s\n",zip_path,mz_zip_get_error_string(error));
        return -1;   
    }

    mz_uint i,filecount;

    filecount = mz_zip_reader_get_num_files(&zip);

    for(i = 0;i < filecount;i++){
        char name[1024];

        if(mz_zip_reader_is_file_a_directory(&zip,i))
            continue;
        
        mz_zip_reader_get_filename(&zip,i,name,sizeof name);
        printf("解压%s...\n",name);

        mz_zip_reader_extract_to_file(&zip,i,name,0);

    }
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
    wide_to_utf8(wroot,utf8_wroot,_countof(wroot));
    printf("将下载在%ls...\n",utf8_wroot);
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
        fprintf(stderr,"WinHttpCrackUrl失败\n");
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
        fprintf(stderr,"WinHttpConnect%s错误\n",GetLastError());
        return -1;
    }
    if(hRequest)
        bResults = WinHttpSendRequest(hRequest,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    else{
        fprintf(stderr,"WinHttpOpenRequest%s错误\n",GetLastError());
        return -1;
    }
    if(bResults)
        bResults = WinHttpReceiveResponse(hRequest,NULL);
    else{
        fprintf(stderr,"WinHttpSendRequest%s错误",GetLastError());
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
        fprintf(stderr,"WinHttpOpen%u失败\n",GetLastError());
        return -1;
    }
    if(hConnect)
        hRequest = WinHttpOpenRequest(hConnect,L"GET",L"/BA-Momoi/cgss-resource-tool/releases.atom",   //访问的虚拟地址
        NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
    else{
        fprintf(stderr,"WinHttpConnect%u失败\n",GetLastError());
        return -1;
    }
    if(hRequest)
        bResults = WinHttpSendRequest(hRequest,WINHTTP_NO_ADDITIONAL_HEADERS,0,
        WINHTTP_NO_REQUEST_DATA,0,0,0);
    if(bResults)
        bResults = WinHttpReceiveResponse(hRequest,NULL);
    else
        fprintf(stderr,"WinHttpSendRequest%u失败\n",GetLastError());
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
    
    wchar_t wroot[MAX_PATH];
    DWORD pathlen = GetModuleFileNameW(NULL,wroot,MAX_PATH);
    if(pathlen == 0 || pathlen == MAX_PATH){
        fprintf(stderr,"路径获取失败\n");
        return -1;
    }
    HRESULT rc = PathCchRemoveFileSpec(wroot,MAX_PATH);
    if(FAILED(rc)){
        fprintf(stderr,"执行PathCchRemoveFileSpec函数发生错误\n");
        return -1;
    }
    rc = PathCchRemoveFileSpec(wroot,MAX_PATH);
    if(FAILED(rc)){
        fprintf(stderr,"执行PathCchRemoveFileSpec函数发生错误\n");
        return -1;
    }
    int hc = 0;
    /*if(version < tag.version){
        hc = down_file(&tag,wroot,MAX_PATH);
        if(hc == 0)
            return 1;   //更新完成，返回1需重启
        else
            return -1;  //更新失败
    }else{
        printf("暂无新版本\n");
    }*/
    if(version == tag.version){
        versiondef choice[] ={
            {version,0},
            {tag.version,0},
            {0.0,0}
        };
        int sel = pager_pick_version("请选择版本",choice,version,0);
        if(sel == -1 || sel == 0){
            return 0;
        }else{
            hc = down_file(&tag,wroot,MAX_PATH);
            if(hc == 0){
                char utf8_wroot[MAX_PATH];
                wide_to_utf8(wroot,utf8_wroot,MAX_PATH);
                snprintf(utf8_wroot,sizeof utf8_wroot,"%s\\updata.zip",utf8_wroot);
                char outpath[MAX_PATH];
                snprintf(outpath,sizeof outpath,"%s\\updata",utf8_wroot);
                unzip(utf8_wroot,outpath);
                return 1;   //更新完成，返回1需重启
            }
            else
                return -1;  //更新失败
        }
    }
    return 0;   //无需更新
}