#include "access/annvector/distance/distance_utils.h"

#if defined(__arm__) || defined(__arm) || defined(__aarch64__) || defined(__aarch64)
#include <stdio.h>
#include <string.h>
#ifdef __linux__
#include <sys/auxv.h>
#include <asm/hwcap.h>
#ifndef HWCAP_NEON
#define HWCAP_NEON      (1 << 12)
#endif
#ifndef HWCAP2_SME
#define HWCAP2_SME   (1ULL << 23)
#endif
#ifndef HWCAP2_SVE2
#define HWCAP2_SVE2 (1 << 1)
#endif
#ifndef HWCAP2_SME
#define HWCAP2_SVE (1 << 23)
#endif
#ifndef HWCAP2_SME2
#define HWCAP2_SME2 (1UL << 37)
#endif
#else
static bool check_cpuinfo_feature(const char *feature) {
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (!fp) {
        return 0;
    }
    
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Features")) {
            found = (strstr(line, feature) != NULL);
            if (found) {
                break;
            }
        }
    }
    fclose(fp);
    return found;
}
#endif

static bool supports_neon()
{
#ifdef __linux__
    return getauxval(AT_HWCAP) & HWCAP_NEON;
#else
    return check_cpuinfo_feature(" neon");
#endif
}
static bool supports_asmid()
{
#ifdef __linux__
    return getauxval(AT_HWCAP) & HWCAP_ASIMD;
#else
    return check_cpuinfo_feature(" neon");
#endif
}
static bool supports_sve()
{
#ifdef __linux__
    return getauxval(AT_HWCAP) & HWCAP_SVE;
#else
    /* sve2 or any sve* should imply sve */
    return check_cpuinfo_feature(" sve");
#endif
}
static bool supports_sve2()
{
#ifdef __linux__
    return getauxval(AT_HWCAP2) & HWCAP2_SVE2;
#else
    return check_cpuinfo_feature(" sve2");
#endif
}
static bool supports_sme()
{
#ifdef __linux__
    return getauxval(AT_HWCAP2) & HWCAP2_SME;
#else
    return check_cpuinfo_feature(" sme");
#endif
}
static bool supports_sme2()
{
#ifdef __linux__
    return getauxval(AT_HWCAP2) & HWCAP2_SME2;
#else
    return check_cpuinfo_feature(" sme2");
#endif
}

static bool try_get_arm_arch_version(int &version) {
    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (!cpuinfo) {
        return false;
    }
    char line[256];
    while (fgets(line, sizeof(line), cpuinfo)) {
        if (strstr(line, "CPU architecture")) {
            char *colon = strchr(line, ':');
            if (!colon) {
                continue;
            }
            char *version_str = colon + 1;
            while (*version_str == ' ' || *version_str == '\t') {
                ++version_str;
            }

            char *endptr;
            version = strtol(version_str, &endptr, 10);
            
            if (endptr == version_str) {
                continue;
            }
            fclose(cpuinfo);
            return true;
        }
    }
    fclose(cpuinfo);
    return false;
}
static int get_arm_arch_version()   /* 0 for unknown */
{
    int res;
    if (try_get_arm_arch_version(res)) {
        return res;
    }
    if (!supports_asmid()) {
        return 0;
    }
    if (!supports_neon()) {
        return 7;
    }
    if (!supports_sme()) {
        return 8;
    }
    return 9;
}
#endif /* arm */

#ifdef __x86_64__
void cpuid(uint32 leaf, uint32 subleaf, uint32 *eax, uint32 *ebx, uint32 *ecx, uint32 *edx) {
    __asm__ volatile (
        "cpuid"
        : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx)
        : "a" (leaf), "c" (subleaf)
    );
}

static bool check_os_xsave_ymm() {
    uint32 eax, ebx, ecx, edx;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1 << 27))) {
        return false;
    }

    uint32 eax_val;
    uint32 edx_val;
    __asm__ volatile ("xgetbv" : "=a" (eax_val), "=d" (edx_val) : "c" (0));
    uint64 xcr0 = ((uint64)edx_val << 32) | eax_val;
    return (xcr0 & 0x6) == 0x6;
}

static bool supports_sse() {
    uint32 eax, ebx, ecx, edx;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    return (ecx & (1 << 12)) && /* fma */
        (ecx & (1 << 0)) &&  /* sse3 */
        (ecx & (1 << 19)) &&    /* sse4.1 */
        (ecx & (1 << 20));      /* sse4.2 */
}

static bool supports_avx() {
    uint32 eax, ebx, ecx, edx;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1 << 28)) ||   /* AVX mark the 28th bit of ecx */
        !(ecx & (1 << 12))      /* FMA mark the 12th bit of ecx */) {
        return false;
    }
    cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    if (!(ebx & (1 << 5))) { /* AVX2 mark the 5th bit of ebx */
        return false;
    }
    return check_os_xsave_ymm();
}

static bool supports_avx512f() {
    uint32 eax, ebx, ecx, edx;
    cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    if (!(ebx & (1 << 16))) { /* AVX512F mark the 16th bit of ebx */
        return false;
    }
    if (!(ebx & (1 << 30))) { /* AVX512DQ mark the 30th bit of ebx */
        return false;
    }
    uint32 eax_val;
    uint32 edx_val;
    __asm__ volatile ("xgetbv" : "=a" (eax_val), "=d" (edx_val) : "c" (0));
    uint64 xcr0 = ((uint64)edx_val << 32) | eax_val;
    return (xcr0 & (0x7 << 5)) == (0x7 << 5) && /* must set OPMSK/ZMM_Hi256/Hi16_ZMM */
        check_os_xsave_ymm();
}
#endif /* x86_64 */

ann_helper::Arch ann_helper::get_best_arch()
{
#if defined(__arm__) || defined(__arm) || defined(__aarch64__) || defined(__aarch64)
    int v = get_arm_arch_version();
    if (v == 8) {
        if (supports_sve2()) {
            return Arch::SVE2V8;
        }
        if (supports_sve()) {
            return Arch::SVEV8;
        }
        if (supports_neon()) {
            return Arch::NEONV8;
        }
    }
#if __GNUC__ >= 12
    if (v >= 9) {
        if (supports_sme2()) {
            return Arch::SME2V9;
        }
        if (supports_sme()) {
            return Arch::SMEV9;
        }
        if (supports_sve2()) {
            return Arch::SVE2V9;
        }
        if (supports_sve()) {
            return Arch::SVEV9;
        }
        if (supports_neon()) {
            return Arch::NEONV9;
        }
    }
#endif /* gcc 12 or greater */
#endif /* arm */
#ifdef __x86_64__
    if (supports_avx512f()) {
        return Arch::AVX512;
    }
    if (supports_avx()) {
        return Arch::AVX;
    }
    if (supports_sse()) {
        return Arch::SSE;
    }
#endif /* x86_64 */
    return Arch::GENERAL;
}
