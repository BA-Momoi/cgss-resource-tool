#ifndef _AUTO_UPDATA_H_
#define _AUTO_UPDATA_H_

double updata_latest_version(void);
int updata_apply_version(double version, double latest);
int updata_main(double version);

int unzip(const char*,const char*);
#endif
