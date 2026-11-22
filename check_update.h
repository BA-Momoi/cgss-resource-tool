#ifndef CGSS_CHECK_UPDATE_H
#define CGSS_CHECK_UPDATE_H

#include <wchar.h>

int check_update_run(const wchar_t *requested_dir);
long long check_update_latest_version(int quiet);
int check_update_run_with_version(const wchar_t *requested_dir, long long latest);

#endif
