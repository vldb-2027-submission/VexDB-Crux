/* ---------------------------------------------------------------------------------------
 * 
 * license.h
 * 
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * 
 * IDENTIFICATION
 *        src/include/postmaster/license.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef LICENSE_H_
#define LICENSE_H_

#include <time.h>

extern int generateLicense(char* outputLicenseFile, time_t from, time_t to);
extern int verifyLicense(const char* licenseFile, char* errmsg);

#endif /* LICENSE_H_ */
