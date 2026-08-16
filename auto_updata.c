#include <stdio.h>
#include <string.h>
#include <winhttp.h>
#include <windows.h>
#include <stdlib.h>
#include "auto_updata.h"
#include "net.h"

HINSTANCE hSession = NULL;

static HINSTANCE winhttp_init(LPCWSTR HostName){
    hSession = WinHttpOpen(L"WinHttp-GitHub-Client/1.0",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!hSession){
        fprintf(stderr,"WinHttpOpen:%s失败",GetLastError());
        return 1;
    }
    DWORD dwProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    

}

int updata_main(int version){
    
    BOOL bResults = FALSE;

}