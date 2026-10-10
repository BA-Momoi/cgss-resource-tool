/* Exercise the real scheduler and file writes without depending on CDN availability. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>
#include <winhttp.h>

#define TEST_TASKS 65
typedef struct { int index; int read; } TestRequest;
static volatile LONG seen[TEST_TASKS];
static int fail_index = -1, thread_limit = 4, threads_started;

static HINTERNET test_open(LPCWSTR a, DWORD b, LPCWSTR c, LPCWSTR d, DWORD e){
    (void)a; (void)b; (void)c; (void)d; (void)e; return (HINTERNET)1;
}
static BOOL test_timeout(HINTERNET h, int a, int b, int c, int d){
    (void)h; (void)a; (void)b; (void)c; (void)d; return TRUE;
}
static BOOL test_option(HINTERNET h, DWORD a, LPVOID b, DWORD c){
    (void)h; (void)a; (void)b; (void)c; return TRUE;
}
static HINTERNET test_connect(HINTERNET h, LPCWSTR host, INTERNET_PORT port, DWORD flags){
    (void)h; (void)host; (void)port; (void)flags; return (HINTERNET)2;
}
static HINTERNET test_request(HINTERNET h, LPCWSTR verb, LPCWSTR path, LPCWSTR version,
                             LPCWSTR referer, LPCWSTR *types, DWORD flags){
    (void)h; (void)verb; (void)version; (void)referer; (void)types; (void)flags;
    const wchar_t *hash = wcsrchr(path, L'/');
    assert(hash);
    int index = (int)wcstol(hash + 1, NULL, 16);
    assert(index >= 0 && index < TEST_TASKS);
    InterlockedIncrement(&seen[index]);
    TestRequest *request = calloc(1, sizeof *request);
    assert(request);
    request->index = index;
    return (HINTERNET)request;
}
static BOOL test_add_headers(HINTERNET h, LPCWSTR a, DWORD b, DWORD c){
    (void)h; (void)a; (void)b; (void)c; return TRUE;
}
static BOOL test_send(HINTERNET h, LPCWSTR a, DWORD b, LPVOID c, DWORD d, DWORD e, DWORD_PTR f){
    (void)h; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return TRUE;
}
static BOOL test_receive(HINTERNET h, LPVOID reserved){ (void)h; (void)reserved; return TRUE; }
static BOOL test_headers(HINTERNET h, DWORD a, LPCWSTR b, LPVOID c, LPDWORD d, LPDWORD e){
    (void)a; (void)b; (void)e; assert(*d >= sizeof(DWORD));
    *(DWORD*)c = ((TestRequest*)h)->index == fail_index ? 503 : 200;
    *d = sizeof(DWORD); return TRUE;
}
static BOOL test_available(HINTERNET h, LPDWORD size){
    *size = ((TestRequest*)h)->read ? 0 : sizeof(int); return TRUE;
}
static BOOL test_read(HINTERNET h, LPVOID buffer, DWORD wanted, LPDWORD got){
    TestRequest *request = (TestRequest*)h;
    assert(wanted == sizeof(int));
    memcpy(buffer, &request->index, sizeof(int));
    *got = sizeof(int); request->read = 1; return TRUE;
}
static BOOL test_close(HINTERNET h){ if(h != (HINTERNET)1 && h != (HINTERNET)2) free(h); return TRUE; }
static HANDLE test_thread(LPSECURITY_ATTRIBUTES a, SIZE_T b, LPTHREAD_START_ROUTINE c,
                          LPVOID d, DWORD e, LPDWORD f){
    if(threads_started++ >= thread_limit){ SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    return CreateThread(a,b,c,d,e,f);
}

#define WinHttpOpen test_open
#define WinHttpSetTimeouts test_timeout
#define WinHttpSetOption test_option
#define WinHttpConnect test_connect
#define WinHttpOpenRequest test_request
#define WinHttpAddRequestHeaders test_add_headers
#define WinHttpSendRequest test_send
#define WinHttpReceiveResponse test_receive
#define WinHttpQueryHeaders test_headers
#define WinHttpQueryDataAvailable test_available
#define WinHttpReadData test_read
#define WinHttpCloseHandle test_close
#define CreateThread test_thread
#include "../net.c"

static void run_case(const wchar_t *root, int count, int limit, int failure){
    wchar_t directory[1024];
    swprintf(directory,1024,L"%ls\\count%d-threads%d-fail%d",root,count,limit,failure);
    mkdirs(directory);
    memset((void*)seen,0,sizeof seen);
    fail_index=failure; thread_limit=limit; threads_started=0;
    DlTask tasks[TEST_TASKS];
    char names[TEST_TASKS][64], hashes[TEST_TASKS][33];
    for(int i=0; i<count; i++){
        snprintf(names[i],sizeof names[i],"task-%d.bin",i);
        snprintf(hashes[i],sizeof hashes[i],"%032x",i);
        tasks[i]=(DlTask){names[i],hashes[i],directory};
    }
    int succeeded=dl_many(tasks,(size_t)count);
    assert(succeeded == count - (failure >= 0));
    for(int i=0; i<count; i++){
        assert(seen[i] == 1);
        wchar_t file[1200];
        swprintf(file,1200,L"%ls\\task-%d.bin",directory,i);
        FILE *f=_wfopen(file,L"rb");
        if(i == failure){ assert(!f); continue; }
        assert(f);
        int payload=-1;
        assert(fread(&payload,sizeof payload,1,f)==1 && payload==i);
        assert(fgetc(f)==EOF);
        fclose(f);
    }
    printf("PASS count=%d thread_limit=%d failure=%d\n",count,limit,failure);
}

int wmain(int argc, wchar_t **argv){
    assert(argc==2);
    assert(dl_many(NULL,0)==0);
    run_case(argv[1],1,4,-1);
    run_case(argv[1],2,4,-1);
    run_case(argv[1],9,4,-1);
    run_case(argv[1],65,4,-1);
    run_case(argv[1],1,0,-1);
    run_case(argv[1],9,2,-1);
    run_case(argv[1],9,4,3);
    puts("DOWNLOAD-QUEUE PASS");
    return 0;
}
