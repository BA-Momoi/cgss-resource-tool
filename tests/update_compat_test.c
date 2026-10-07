/* Compile against the unchanged v1.61 updater or the current updater.
   Only network input, module location and child launch are replaced in the simulation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winhttp.h>

static int live_probe, launch_count, pick_count;
static wchar_t module_path[MAX_PATH], package_path[MAX_PATH];
static char feed[1024];
static size_t feed_pos;
static FILE *package;
static HINTERNET fake_session = (HINTERNET)1, fake_connection = (HINTERNET)2;
static HINTERNET fake_feed = (HINTERNET)3, fake_package = (HINTERNET)4;

static HINTERNET test_open(LPCWSTR a, DWORD b, LPCWSTR c, LPCWSTR d, DWORD e){
    return live_probe ? WinHttpOpen(a,b,c,d,e) : fake_session;
}
static BOOL test_timeouts(HINTERNET h, int a, int b, int c, int d){
    return live_probe ? WinHttpSetTimeouts(h,a,b,c,d) : TRUE;
}
static BOOL test_option(HINTERNET h, DWORD a, LPVOID b, DWORD c){
    return live_probe ? WinHttpSetOption(h,a,b,c) : TRUE;
}
static HINTERNET test_connect(HINTERNET h, LPCWSTR host, INTERNET_PORT port, DWORD flags){
    return live_probe ? WinHttpConnect(h,host,port,flags) : fake_connection;
}
static HINTERNET test_request(HINTERNET h, LPCWSTR verb, LPCWSTR uri, LPCWSTR version,
                             LPCWSTR referer, LPCWSTR *types, DWORD flags){
    if(live_probe) return WinHttpOpenRequest(h,verb,uri,version,referer,types,flags);
    if(wcsstr(uri,L"releases.atom")){ feed_pos = 0; return fake_feed; }
    if(!wcsstr(uri,L"/releases/latest/download/CGSS_ResourceTool.zip")) return NULL;
    package = _wfopen(package_path,L"rb");
    return package ? fake_package : NULL;
}
static BOOL test_send(HINTERNET h, LPCWSTR a, DWORD b, LPVOID c, DWORD d, DWORD e, DWORD_PTR f){
    return live_probe ? WinHttpSendRequest(h,a,b,c,d,e,f) : TRUE;
}
static BOOL test_receive(HINTERNET h, LPVOID reserved){
    return live_probe ? WinHttpReceiveResponse(h,reserved) : TRUE;
}
static BOOL test_available(HINTERNET h, LPDWORD available){
    if(live_probe) return WinHttpQueryDataAvailable(h,available);
    if(h == fake_feed){ *available = (DWORD)(strlen(feed)-feed_pos); return TRUE; }
    long pos = ftell(package);
    if(pos < 0 || fseek(package,0,SEEK_END)) return FALSE;
    long end = ftell(package);
    if(end < 0 || fseek(package,pos,SEEK_SET)) return FALSE;
    *available = (DWORD)((end-pos) > 131072 ? 131072 : (end-pos));
    return TRUE;
}
static BOOL test_read(HINTERNET h, LPVOID buffer, DWORD wanted, LPDWORD got){
    if(live_probe) return WinHttpReadData(h,buffer,wanted,got);
    if(h == fake_feed){ memcpy(buffer,feed+feed_pos,wanted); feed_pos += wanted; *got=wanted; return TRUE; }
    *got = (DWORD)fread(buffer,1,wanted,package);
    return !ferror(package);
}
static BOOL test_headers(HINTERNET h, DWORD a, LPCWSTR b, LPVOID c, LPDWORD d, LPDWORD e){
    if(live_probe) return WinHttpQueryHeaders(h,a,b,c,d,e);
    if(*d < sizeof(DWORD)) return FALSE;
    *(DWORD*)c = 200; *d=sizeof(DWORD); return TRUE;
}
static BOOL test_close(HINTERNET h){
    if(live_probe) return WinHttpCloseHandle(h);
    if(h == fake_package && package){ fclose(package); package=NULL; }
    return TRUE;
}
static DWORD test_module(HMODULE module, LPWSTR target, DWORD capacity){
    (void)module;
    if(wcslen(module_path) >= capacity) return capacity;
    wcscpy(target,module_path); return (DWORD)wcslen(target);
}
static BOOL test_process(LPCWSTR app, LPWSTR command, LPSECURITY_ATTRIBUTES proc_attrs,
                         LPSECURITY_ATTRIBUTES thread_attrs, BOOL inherit, DWORD flags,
                         LPVOID environment, LPCWSTR directory, LPSTARTUPINFOW startup,
                         LPPROCESS_INFORMATION process){
    (void)app; (void)command; (void)proc_attrs; (void)thread_attrs; (void)inherit;
    (void)flags; (void)environment; (void)startup;
    wchar_t original[MAX_PATH], runnable[MAX_PATH];
    swprintf(original,MAX_PATH,L"%ls\\update.bat",directory);
    swprintf(runnable,MAX_PATH,L"%ls\\update.test.bat",directory);
    FILE *in = _wfopen(original,L"rb"), *out = _wfopen(runnable,L"wb");
    if(!in || !out){ if(in)fclose(in); if(out)fclose(out); return FALSE; }
    char line[4096];
    while(fgets(line,sizeof line,in)){
        /* Keep asynchronous restart, but use the new CLI to avoid opening its menu or syncing game data. */
        if(strncmp(line,"start \"\" ",9)==0)
            fputs("start \"\" /b \"%TARGET_DIR%\\%EXE_NAME%\" --version >\"%TARGET_DIR%\\restarted-version.txt\" 2>&1\r\n",out);
        else fputs(line,out);
    }
    int ok = !ferror(in);
    fclose(in);
    if(fclose(out)) ok=0;
    if(!ok) return FALSE;
    process->hProcess=CreateEventW(NULL,FALSE,FALSE,NULL);
    process->hThread=CreateEventW(NULL,FALSE,FALSE,NULL);
    launch_count++;
    return TRUE;
}

#define WinHttpOpen test_open
#define WinHttpSetTimeouts test_timeouts
#define WinHttpSetOption test_option
#define WinHttpConnect test_connect
#define WinHttpOpenRequest test_request
#define WinHttpSendRequest test_send
#define WinHttpReceiveResponse test_receive
#define WinHttpQueryDataAvailable test_available
#define WinHttpReadData test_read
#define WinHttpQueryHeaders test_headers
#define WinHttpCloseHandle test_close
#define GetModuleFileNameW test_module
#define CreateProcessW test_process
#ifdef CGSS_TEST_CURRENT
#include "../auto_updata.c"
#else
#include "legacy/auto_updata.c"
#endif

int pager_pick_version(const char *title, versiondef *choices, double version, int multi){
    (void)title; (void)choices; (void)version; (void)multi;
    pick_count++;
    return live_probe ? 0 : 1;
}

/* Fresh per-case directories must never require destructive cleanup in the simulation. */
void wipe_dir(const wchar_t *path){
    fwprintf(stderr,L"Unexpected existing update directory: %ls\n",path);
    exit(10);
}

int wmain(int argc, wchar_t **argv){
    if(argc != 6){ fprintf(stderr,"Arguments: module_path package_path release_title current_version expected_result\n"); return 1; }
    SetConsoleOutputCP(CP_UTF8);
    wcsncpy(module_path,argv[1],MAX_PATH-1);
    wcsncpy(package_path,argv[2],MAX_PATH-1);
    char title[256];
    WideCharToMultiByte(CP_UTF8,0,argv[3],-1,title,sizeof title,NULL,NULL);
    snprintf(feed,sizeof feed,"<feed><title>Releases</title><entry><title>%s</title></entry></feed>",title);
    live_probe = wcscmp(argv[3],L"--live") == 0;
    int result = updata_main(wcstod(argv[4],NULL));
    int expected = _wtoi(argv[5]);
    printf("TEST result=%d expected=%d selected=%d scheduled=%d\n",result,expected,pick_count,launch_count);
    if(result != expected || (expected==2 && launch_count!=1) || (expected==0 && launch_count!=0)) return 2;
    if(!live_probe && expected==0 && wcscmp(argv[3],L"v1.7")==0 && pick_count!=0) return 3;
    if(package) fclose(package);
    return 0;
}
