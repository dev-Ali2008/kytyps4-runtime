// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::JpegDec {

enum OrbisJpegDecColorSpace : u32 {
    ORBIS_JPEG_DEC_COLORSPACE_RGB = 0,
    ORBIS_JPEG_DEC_COLORSPACE_YUV444 = 1,
    ORBIS_JPEG_DEC_COLORSPACE_YUV422 = 2,
    ORBIS_JPEG_DEC_COLORSPACE_YUV420 = 3,
    ORBIS_JPEG_DEC_COLORSPACE_GRAYSCALE = 4,
};

enum OrbisJpegDecOutputColorSpace : u32 {
    ORBIS_JPEG_DEC_OUTPUT_COLORSPACE_RGBA = 0,
    ORBIS_JPEG_DEC_OUTPUT_COLORSPACE_ARGB = 1,
    ORBIS_JPEG_DEC_OUTPUT_COLORSPACE_BGRA = 2,
    ORBIS_JPEG_DEC_OUTPUT_COLORSPACE_ABGR = 3,
    ORBIS_JPEG_DEC_OUTPUT_COLORSPACE_YUV444 = 0x10,
    ORBIS_JPEG_DEC_OUTPUT_COLORSPACE_YUV422 = 0x11,
    ORBIS_JPEG_DEC_OUTPUT_COLORSPACE_YUV420 = 0x12,
    ORBIS_JPEG_DEC_OUTPUT_COLORSPACE_GRAYSCALE = 0x13,
};

enum OrbisJpegDecMode : u32 {
    ORBIS_JPEG_DEC_MODE_DEFAULT = 0,
    ORBIS_JPEG_DEC_MODE_FAST = 1,
};

struct OrbisJpegDecCreateParam {
    u32 size;
    u32 mode;
    u32 maxImageWidth;
    u32 maxImageHeight;
};
static_assert(sizeof(OrbisJpegDecCreateParam) == 0x10);

struct OrbisJpegDecDecodeParam {
    void* jpeg;
    u32 jpegSize;
    void* output;
    u32 outputSize;
    OrbisJpegDecOutputColorSpace outputColorSpace;
    u32 degradationLevel;
    u32 rewindFlag;
};
// Two 64-bit pointers plus five 32-bit fields occupy 0x28 bytes on the PS4 ABI.
static_assert(sizeof(OrbisJpegDecDecodeParam) == 0x28);

struct OrbisJpegDecImageInfo {
    u32 width;
    u32 height;
    u32 colorSpace;
    u32 samplingFrequency;
};

struct OrbisJpegDecHandleInternal {
    OrbisJpegDecHandleInternal* handle;
    u32 handleSize;
    u32 maxWidth;
    u32 maxHeight;
};
// The decoder handle stores one pointer and three 32-bit fields.
static_assert(sizeof(OrbisJpegDecHandleInternal) == 0x18);

typedef OrbisJpegDecHandleInternal* OrbisJpegDecHandle;

s32 PS4_SYSV_ABI sceJpegDecCreate(const OrbisJpegDecCreateParam* param, void* memory,
                                   u32 memorySize, OrbisJpegDecHandle* handle);
s32 PS4_SYSV_ABI sceJpegDecDelete(OrbisJpegDecHandle handle);
s32 PS4_SYSV_ABI sceJpegDecDecode(OrbisJpegDecHandle handle, const OrbisJpegDecDecodeParam* param,
                                   OrbisJpegDecImageInfo* imageInfo);
s32 PS4_SYSV_ABI sceJpegDecDecodeWithInputControl(OrbisJpegDecHandle handle,
                                                   const OrbisJpegDecDecodeParam* param,
                                                   OrbisJpegDecImageInfo* imageInfo);
s32 PS4_SYSV_ABI sceJpegDecParseHeader(const void* jpeg, u32 jpegSize, OrbisJpegDecImageInfo* info);
s32 PS4_SYSV_ABI sceJpegDecQueryMemorySize(const OrbisJpegDecCreateParam* param);

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::JpegDec
