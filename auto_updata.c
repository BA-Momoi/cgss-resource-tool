#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <winhttp.h>
#include <stdlib.h>
#include <pathcch.h>
#include "auto_updata.h"
#include "net.h"

static HINTERNET g_sess = NULL;
int DOWNERROR = 0; 

typedef struct Version_Num{     //存提取出来的参数
    double version;
    wchar_t *down_url;
}Version_num;

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
// tag Version_num结构体，wroot存目录的地方，PATH_SIZE wroot的长度，
static int down_file(Version_num *tag,wchar_t *wroot,size_t PATH_SIZE){
    FILE *zip_file = NULL;
    BOOL bResults = FALSE;
    HINTERNET hConnect = NULL,hRequest = NULL;
    
    wprintf(L"将下载在%ls...\n",wroot);
    wchar_t host[256];
    wchar_t path[2048];

    URL_COMPONENTS url;

    url.lpszHostName = host;
    url.dwHostNameLength = ARRAYSIZE(host);

    url.lpszUrlPath = path;
    url.dwUrlPathLength = ARRAYSIZE(path);

    url.dwSchemeLength = (DWORD)-1;

    if(!WinHttpCrackUrl(tag->down_url,0,0,&url)){
        fprintf(stderr,"WinHttpCrackUrl失败\n");
        return -1;
    }

    wprintf("Host:%s\n",host);
    wprintf("Path:%s\n",path);
    wprintf("port:%s\n",url.nPort);

    if(g_sess)
        hConnect = WinHttpConnect(g_sess,host,url.nPort,0);
    else{
        fprintf(stderr,"g_sess初始化未完成\n");
        return -1;
    }
    if(hConnect)
        hRequest = WinHttpOpenRequest(hConnect,L"GET",path,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
        
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
    tag.down_url = L"https://github.com/BA-Momoi/cgss-resource-tool/releases/latest/download/CGSS_ResourceTool.zip";
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
                if(!WinHttpQueryDataAvailable(hRequest,&dwSize))
                    printf("ERROR %u IN WINHTTPQUERYDATAAVAILABLE.\n",
                    GetLastError());
                    pszOutBuffer = malloc((dwSize+1)*sizeof(char));
                if(!pszOutBuffer){
                    printf("Out of memory\n");
                    dwSize = 0;
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
        }while(DOWNERROR == 1 && error_count < 3);
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
                printf("Version:%lf\n",tag.version);
                printf("Url:%s\n",tag.down_url);
            }else
                fprintf(stderr,"获取版本号和链接失败\n");
        }
        else
            fprintf(stderr,"访问%s后并无有效信息\n","github.com/BA-Momoi/cgss-resource-tool/releases.atom");
        
    }
    if(!bResults)
        fprintf(stderr,"ERROR %d has occurred.\n",GetLastError());
    if(hRequest) WinHttpCloseHandle(hConnect);
    if(hConnect) WinHttpCloseHandle(hRequest);
    
    wchar_t wroot[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL,wroot,MAX_PATH);
    if(len == 0 || len == MAX_PATH){
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
    int rc = 0;
    if(version < tag.version){
        rc = down_file(&tag,wroot,MAX_PATH);
        if(rc == 0)
            return 1;   //更新完成，返回1需重启
        else
            return -1;  //更新失败
    }
    return 0;   //无需更新
}