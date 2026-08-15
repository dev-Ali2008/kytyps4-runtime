// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <cstdlib>

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"

namespace Libraries::LibcInternal {

double PS4_SYSV_ABI internal_sin(double x) {
    return std::sin(x);
}

float PS4_SYSV_ABI internal_sinf(float x) {
    return sinf(x);
}

double PS4_SYSV_ABI internal_cos(double x) {
    return std::cos(x);
}

float PS4_SYSV_ABI internal_cosf(float x) {
    return cosf(x);
}

void PS4_SYSV_ABI internal_sincos(double x, double* sinp, double* cosp) {
    *sinp = std::sin(x);
    *cosp = std::cos(x);
}

void PS4_SYSV_ABI internal_sincosf(float x, float* sinp, float* cosp) {
    *sinp = sinf(x);
    *cosp = cosf(x);
}

double PS4_SYSV_ABI internal_tan(double x) {
    return std::tan(x);
}

float PS4_SYSV_ABI internal_tanf(float x) {
    return tanf(x);
}

double PS4_SYSV_ABI internal_asin(double x) {
    return std::asin(x);
}

float PS4_SYSV_ABI internal_asinf(float x) {
    return asinf(x);
}

double PS4_SYSV_ABI internal_acos(double x) {
    return std::acos(x);
}

float PS4_SYSV_ABI internal_acosf(float x) {
    return acosf(x);
}

double PS4_SYSV_ABI internal_atan(double x) {
    return std::atan(x);
}

float PS4_SYSV_ABI internal_atanf(float x) {
    return atanf(x);
}

double PS4_SYSV_ABI internal_atan2(double y, double x) {
    return std::atan2(y, x);
}

float PS4_SYSV_ABI internal_atan2f(float y, float x) {
    return atan2f(y, x);
}

double PS4_SYSV_ABI internal_exp(double x) {
    return std::exp(x);
}

float PS4_SYSV_ABI internal_expf(float x) {
    return expf(x);
}

double PS4_SYSV_ABI internal_exp2(double x) {
    return std::exp2(x);
}

float PS4_SYSV_ABI internal_exp2f(float x) {
    return std::exp2f(x);
}

double PS4_SYSV_ABI internal_pow(double x, double y) {
    return std::pow(x, y);
}

double PS4_SYSV_ABI internal_atof(const char* value) {
    return std::atof(value);
}

double PS4_SYSV_ABI internal_fmod(double x, double y) {
    return std::fmod(x, y);
}

double PS4_SYSV_ABI internal_ldexp(double x, s32 exponent) {
    return std::ldexp(x, exponent);
}

s32 PS4_SYSV_ABI internal_rand() {
    return std::rand();
}

void PS4_SYSV_ABI internal_srand(u32 seed) {
    std::srand(seed);
}

float PS4_SYSV_ABI internal_powf(float x, float y) {
    return powf(x, y);
}

double PS4_SYSV_ABI internal_log(double x) {
    return std::log(x);
}

float PS4_SYSV_ABI internal_logf(float x) {
    return logf(x);
}

double PS4_SYSV_ABI internal_log10(double x) {
    return std::log10(x);
}

float PS4_SYSV_ABI internal_log10f(float x) {
    return log10f(x);
}

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
float PS4_SYSV_ABI fex_libc_fsin(float arg, u32 m, s32 n) {
    ASSERT(n == 0);
    return m != 0 ? cosf(arg) : sinf(arg);
}

void RegisterFexLibcMathAliases(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("ZtjspkJQ+vw", "libc", 1, "libc", fex_libc_fsin);
    LIB_FUNCTION("ZE6RNL+eLbk", "libc", 1, "libc", internal_tanf);
    LIB_FUNCTION("GZWjF-YIFFk", "libc", 1, "libc", internal_asinf);
    LIB_FUNCTION("QI-x0SL8jhw", "libc", 1, "libc", internal_acosf);
    LIB_FUNCTION("EH-x713A99c", "libc", 1, "libc", internal_atan2f);
    LIB_FUNCTION("T7uyNqP7vQA", "libc", 1, "libc", internal_tan);
    LIB_FUNCTION("HUbZmOnT-Dg", "libc", 1, "libc", internal_atan2);
    LIB_FUNCTION("9LCjpWyQ5Zc", "libc", 1, "libc", internal_pow);
    LIB_FUNCTION("1D0H2KNjshE", "libc", 1, "libc", internal_powf);
    LIB_FUNCTION("8zsu04XNsZ4", "libc", 1, "libc", internal_expf);
    LIB_FUNCTION("wuAQt-j+p4o", "libc", 1, "libc", internal_exp2f);
    LIB_FUNCTION("SRI6S9B+-a4", "libc", 1, "libc", internal_atof);
    LIB_FUNCTION("pKwslsMUmSk", "libc", 1, "libc", internal_fmod);
    LIB_FUNCTION("JrwFIMzKNr0", "libc", 1, "libc", internal_ldexp);
    LIB_FUNCTION("cpCOXWMgha0", "libc", 1, "libc", internal_rand);
    LIB_FUNCTION("VPbJwTCgME0", "libc", 1, "libc", internal_srand);
}
#endif

void RegisterlibSceLibcInternalMath(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("H8ya2H00jbI", "libSceLibcInternal", 1, "libSceLibcInternal", internal_sin);
    LIB_FUNCTION("Q4rRL34CEeE", "libSceLibcInternal", 1, "libSceLibcInternal", internal_sinf);
    LIB_FUNCTION("2WE3BTYVwKM", "libSceLibcInternal", 1, "libSceLibcInternal", internal_cos);
    LIB_FUNCTION("-P6FNMzk2Kc", "libSceLibcInternal", 1, "libSceLibcInternal", internal_cosf);
    LIB_FUNCTION("jMB7EFyu30Y", "libSceLibcInternal", 1, "libSceLibcInternal", internal_sincos);
    LIB_FUNCTION("pztV4AF18iI", "libSceLibcInternal", 1, "libSceLibcInternal", internal_sincosf);
    LIB_FUNCTION("T7uyNqP7vQA", "libSceLibcInternal", 1, "libSceLibcInternal", internal_tan);
    LIB_FUNCTION("ZE6RNL+eLbk", "libSceLibcInternal", 1, "libSceLibcInternal", internal_tanf);
    LIB_FUNCTION("7Ly52zaL44Q", "libSceLibcInternal", 1, "libSceLibcInternal", internal_asin);
    LIB_FUNCTION("GZWjF-YIFFk", "libSceLibcInternal", 1, "libSceLibcInternal", internal_asinf);
    LIB_FUNCTION("JBcgYuW8lPU", "libSceLibcInternal", 1, "libSceLibcInternal", internal_acos);
    LIB_FUNCTION("QI-x0SL8jhw", "libSceLibcInternal", 1, "libSceLibcInternal", internal_acosf);
    LIB_FUNCTION("OXmauLdQ8kY", "libSceLibcInternal", 1, "libSceLibcInternal", internal_atan);
    LIB_FUNCTION("weDug8QD-lE", "libSceLibcInternal", 1, "libSceLibcInternal", internal_atanf);
    LIB_FUNCTION("HUbZmOnT-Dg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_atan2);
    LIB_FUNCTION("EH-x713A99c", "libSceLibcInternal", 1, "libSceLibcInternal", internal_atan2f);
    LIB_FUNCTION("NVadfnzQhHQ", "libSceLibcInternal", 1, "libSceLibcInternal", internal_exp);
    LIB_FUNCTION("8zsu04XNsZ4", "libSceLibcInternal", 1, "libSceLibcInternal", internal_expf);
    LIB_FUNCTION("dnaeGXbjP6E", "libSceLibcInternal", 1, "libSceLibcInternal", internal_exp2);
    LIB_FUNCTION("wuAQt-j+p4o", "libSceLibcInternal", 1, "libSceLibcInternal", internal_exp2f);
    LIB_FUNCTION("9LCjpWyQ5Zc", "libSceLibcInternal", 1, "libSceLibcInternal", internal_pow);
    LIB_FUNCTION("1D0H2KNjshE", "libSceLibcInternal", 1, "libSceLibcInternal", internal_powf);
    LIB_FUNCTION("rtV7-jWC6Yg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_log);
    LIB_FUNCTION("RQXLbdT2lc4", "libSceLibcInternal", 1, "libSceLibcInternal", internal_logf);
    LIB_FUNCTION("WuMbPBKN1TU", "libSceLibcInternal", 1, "libSceLibcInternal", internal_log10);
    LIB_FUNCTION("lhpd6Wk6ccs", "libSceLibcInternal", 1, "libSceLibcInternal", internal_log10f);
}

} // namespace Libraries::LibcInternal
