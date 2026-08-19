// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <memory>
#include <stb_image.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "jpeg_error.h"
#include "jpegdec.h"

namespace Libraries::JpegDec {

constexpr u32 ORBIS_JPEG_DEC_MINIMUM_MEMORY_SIZE = 0x1000;
constexpr u32 ORBIS_JPEG_DEC_MAX_IMAGE_DIMENSION = 0xFFFF;
constexpr u32 ORBIS_JPEG_DEC_MAX_JPEG_SIZE = 0x7FFFFFFF;

static s32 ValidateJpegDecCreateParam(const OrbisJpegDecCreateParam* param) {
    if (!param) {
        return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    }
    if (param->size != sizeof(OrbisJpegDecCreateParam)) {
        return ORBIS_JPEG_DEC_ERROR_INVALID_SIZE;
    }
    return ORBIS_OK;
}

static s32 ValidateJpegDecMemory(const void* memory, u32 memorySize) {
    if (!memory) {
        return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    }
    if (memorySize < ORBIS_JPEG_DEC_MINIMUM_MEMORY_SIZE) {
        return ORBIS_JPEG_DEC_ERROR_INVALID_SIZE;
    }
    return ORBIS_OK;
}

static s32 ValidateJpegDecDecodeParam(const OrbisJpegDecDecodeParam* param) {
    if (!param) {
        return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    }
    if (!param->jpeg || param->jpegSize == 0) {
        return ORBIS_JPEG_DEC_ERROR_INVALID_PARAM;
    }
    if (param->jpegSize > ORBIS_JPEG_DEC_MAX_JPEG_SIZE) {
        return ORBIS_JPEG_DEC_ERROR_INVALID_PARAM;
    }
    return ORBIS_OK;
}

static s32 ValidateJpegDecHandle(OrbisJpegDecHandle handle) {
    if (!handle || !Common::IsAligned(reinterpret_cast<VAddr>(handle), 0x10) ||
        handle->handle != handle) {
        return ORBIS_JPEG_DEC_ERROR_INVALID_HANDLE;
    }
    return ORBIS_OK;
}

static OrbisJpegDecOutputColorSpace StbToOrbisColorSpace(int channels) {
    switch (channels) {
    case 1:
        return ORBIS_JPEG_DEC_OUTPUT_COLORSPACE_GRAYSCALE;
    case 3:
        return ORBIS_JPEG_DEC_OUTPUT_COLORSPACE_RGBA; // stb loads as RGB, convert to RGBA
    case 4:
        return ORBIS_JPEG_DEC_OUTPUT_COLORSPACE_RGBA;
    default:
        return ORBIS_JPEG_DEC_OUTPUT_COLORSPACE_RGBA;
    }
}

static void RgbToRgba(const u8* rgb, u8* rgba, u32 width, u32 height) {
    const u32 pixelCount = width * height;
    for (u32 i = 0; i < pixelCount; ++i) {
        rgba[i * 4 + 0] = rgb[i * 3 + 0];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 0xFF;
    }
}

s32 PS4_SYSV_ABI sceJpegDecCreate(const OrbisJpegDecCreateParam* param, void* memory,
                                   u32 memorySize, OrbisJpegDecHandle* handle) {
    if (auto ret = ValidateJpegDecCreateParam(param); ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid create param");
        return ret;
    }
    if (auto ret = ValidateJpegDecMemory(memory, memorySize); ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid memory");
        return ret;
    }
    if (!handle) {
        LOG_ERROR(Lib_Jpeg, "Invalid handle output");
        return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    }

    auto* handleInternal = reinterpret_cast<OrbisJpegDecHandleInternal*>(
        Common::AlignUp(reinterpret_cast<VAddr>(memory), 0x10));
    handleInternal->handle = handleInternal;
    handleInternal->handleSize = sizeof(OrbisJpegDecHandleInternal);
    handleInternal->maxWidth = param->maxImageWidth ? param->maxImageWidth : 4096;
    handleInternal->maxHeight = param->maxImageHeight ? param->maxImageHeight : 4096;
    *handle = handleInternal;

    LOG_INFO(Lib_Jpeg, "Created JPEG decoder handle, max={}x{}", handleInternal->maxWidth,
             handleInternal->maxHeight);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegDecDelete(OrbisJpegDecHandle handle) {
    if (auto ret = ValidateJpegDecHandle(handle); ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid handle");
        return ret;
    }
    handle->handle = nullptr;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegDecDecode(OrbisJpegDecHandle handle, const OrbisJpegDecDecodeParam* param,
                                   OrbisJpegDecImageInfo* imageInfo) {
    if (auto ret = ValidateJpegDecHandle(handle); ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid handle");
        return ret;
    }
    if (auto ret = ValidateJpegDecDecodeParam(param); ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid decode param");
        return ret;
    }

    int width = 0, height = 0, channels = 0;
    unsigned char* decoded = stbi_load_from_memory(
        static_cast<const stbi_uc*>(param->jpeg), static_cast<int>(param->jpegSize), &width, &height,
        &channels, STBI_rgb_alpha);

    if (!decoded) {
        LOG_ERROR(Lib_Jpeg, "Failed to decode JPEG: {}", stbi_failure_reason());
        return ORBIS_JPEG_DEC_ERROR_DECODE_FAILED;
    }

    const u32 outputWidth = static_cast<u32>(width);
    const u32 outputHeight = static_cast<u32>(height);
    const u32 outputSize = outputWidth * outputHeight * 4;

    if (param->output && param->outputSize >= outputSize) {
        // stb already decoded as RGBA (STBI_rgb_alpha), copy directly
        std::memcpy(param->output, decoded, outputSize);
    }

    if (imageInfo) {
        imageInfo->width = outputWidth;
        imageInfo->height = outputHeight;
        imageInfo->colorSpace = static_cast<u32>(ORBIS_JPEG_DEC_OUTPUT_COLORSPACE_RGBA);
        imageInfo->samplingFrequency = 0;
    }

    stbi_image_free(decoded);

    LOG_INFO(Lib_Jpeg, "Decoded JPEG {}x{} ({} bytes output)", outputWidth, outputHeight,
             outputSize);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegDecDecodeWithInputControl(OrbisJpegDecHandle handle,
                                                   const OrbisJpegDecDecodeParam* param,
                                                   OrbisJpegDecImageInfo* imageInfo) {
    LOG_DEBUG(Lib_Jpeg, "sceJpegDecDecodeWithInputControl - delegating to sceJpegDecDecode");
    return sceJpegDecDecode(handle, param, imageInfo);
}

s32 PS4_SYSV_ABI sceJpegDecParseHeader(const void* jpeg, u32 jpegSize, OrbisJpegDecImageInfo* info) {
    if (!jpeg || jpegSize == 0 || !info) {
        return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    }

    int width = 0, height = 0, channels = 0;
    int result = stbi_info_from_memory(static_cast<const stbi_uc*>(jpeg), static_cast<int>(jpegSize),
                                       &width, &height, &channels);

    if (result == 0) {
        LOG_ERROR(Lib_Jpeg, "Failed to parse JPEG header: {}", stbi_failure_reason());
        return ORBIS_JPEG_DEC_ERROR_INVALID_PARAM;
    }

    info->width = static_cast<u32>(width);
    info->height = static_cast<u32>(height);
    info->colorSpace = static_cast<u32>(StbToOrbisColorSpace(channels));
    info->samplingFrequency = 0;

    LOG_DEBUG(Lib_Jpeg, "Parsed JPEG header: {}x{}, channels={}", width, height, channels);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegDecQueryMemorySize(const OrbisJpegDecCreateParam* param) {
    if (auto ret = ValidateJpegDecCreateParam(param); ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid create param");
        return ret;
    }
    return ORBIS_JPEG_DEC_MINIMUM_MEMORY_SIZE;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("1kzQRoWEgSA", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecDecode);
    LIB_FUNCTION("919MhccOiII", "libSceJpegDec", 1, "libSceJpegDec",
                 sceJpegDecDecodeWithInputControl);
    LIB_FUNCTION("Hwh11+m5KoI", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecDelete);
    LIB_FUNCTION("JPh3Zgg0Zwc", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecCreate);
    LIB_FUNCTION("LSinoSQH790", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecParseHeader);
    LIB_FUNCTION("uNAUmANZMEw", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecQueryMemorySize);
}

} // namespace Libraries::JpegDec
