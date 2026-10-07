#include <assert.h>
#include "../acb.c"

int pager_picks(const char *title, dbdef *menu, sqlite3 *db, sqlite3 *rdb, int multi){
    (void)title;
    (void)menu;
    (void)db;
    (void)rdb;
    (void)multi;
    return -1;
}

static void create_file(const wchar_t *path){
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    CloseHandle(file);
}

/* Only called for the fresh temporary directory created by this test. */
static void remove_fixture(const wchar_t *dir){
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, _countof(pattern), L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE){
        do {
            if (wcscmp(fd.cFileName, L".") == 0 ||
                wcscmp(fd.cFileName, L"..") == 0) continue;
            wchar_t path[MAX_PATH];
            swprintf(path, _countof(path), L"%ls\\%ls", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_fixture(path);
            else
                assert(DeleteFileW(path));
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    assert(RemoveDirectoryW(dir));
}

int wmain(int argc, wchar_t **argv){
    if (argc == 3){
        AcbList actual = {0};
        assert(scan_acb(argv[1], &actual, NULL, NULL) == 0);
        assert(actual.count == wcstol(argv[2], NULL, 10));
        printf("Actual ACB count: %d\n", actual.count);
        free(actual.items);
    }

    wchar_t temporary[MAX_PATH], root[MAX_PATH];
    assert(GetTempPathW(_countof(temporary), temporary));
    assert(GetTempFileNameW(temporary, L"acb", 0, root));
    assert(DeleteFileW(root));
    assert(CreateDirectoryW(root, NULL));

    wchar_t bgm[MAX_PATH], path[MAX_PATH];
    swprintf(bgm, _countof(bgm), L"%ls\\BGM", root);
    assert(CreateDirectoryW(bgm, NULL));
    for (int i = 0; i < 257; i++){
        swprintf(path, _countof(path), L"%ls\\bgm_%03d.%ls",
                 bgm, i, i == 256 ? L"ACB" : L"acb");
        create_file(path);
    }
    swprintf(path, _countof(path), L"%ls\\root.acb", root);
    create_file(path);
    swprintf(path, _countof(path), L"%ls\\ignore.wav", bgm);
    create_file(path);

    wchar_t deep[MAX_PATH];
    swprintf(deep, _countof(deep), L"%ls\\Deep", root);
    assert(CreateDirectoryW(deep, NULL));
    for (int i = 0; i < 6; i++){
        wcscat(deep, L"\\nested");
        assert(CreateDirectoryW(deep, NULL));
    }
    swprintf(path, _countof(path), L"%ls\\deep.acb", deep);
    create_file(path);
    wchar_t dotted[MAX_PATH];
    swprintf(dotted, _countof(dotted), L"%ls\\.audio", root);
    assert(CreateDirectoryW(dotted, NULL));
    swprintf(path, _countof(path), L"%ls\\hidden.acb", dotted);
    create_file(path);

    AcbList list = {0};
    assert(scan_acb(root, &list, NULL, NULL) == 0);
    assert(list.count == 260);
    int root_found = 0, deep_found = 0, uppercase_found = 0, dotted_found = 0;
    for (int i = 0; i < list.count; i++){
        AcbItem *item = &list.items[i];
        if (strcmp(item->acb_name, "root.acb") == 0){
            root_found++;
            assert(wcscmp(item->folder, root) == 0);
        }
        if (strcmp(item->acb_name, "deep.acb") == 0){
            deep_found++;
            assert(strcmp(item->folder_name, "Deep") == 0);
        }
        if (strcmp(item->acb_name, "bgm_256.ACB") == 0) uppercase_found++;
        if (strcmp(item->acb_name, "hidden.acb") == 0) dotted_found++;
    }
    assert(root_found == 1 && deep_found == 1 &&
           uppercase_found == 1 && dotted_found == 1);
    printf("Fixture ACB count: %d; root, deep, dotted folders and uppercase extension passed\n",
           list.count);
    free(list.items);
    remove_fixture(root);
    return 0;
}
