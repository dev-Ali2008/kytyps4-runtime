// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/ngs2/ngs2_custom.h"
#include "core/libraries/ngs2/ngs2_error.h"
#include "core/libraries/ngs2/ngs2_geom.h"
#include "core/libraries/ngs2/ngs2_impl.h"
#include "core/libraries/ngs2/ngs2_pan.h"
#include "core/libraries/ngs2/ngs2_report.h"

#include <cstring>
#include <limits>
#include <vector>

namespace Libraries::Ngs2 {

// Ngs2

namespace {

constexpr u32 WAVEFORM_TYPE_PCM8 = 0x11;
constexpr u32 WAVEFORM_TYPE_PCM16 = 0x12;
constexpr u32 WAVEFORM_TYPE_PCM24 = 0x13;
constexpr u32 WAVEFORM_TYPE_PCM32 = 0x14;
constexpr u32 WAVEFORM_TYPE_OGG = 0x15;
constexpr u32 WAVEFORM_TYPE_ATRAC9 = 0x40;

constexpr u32 DEFAULT_UNITS_PER_FRAME = 256;

u32 ReadLE32(const u8* p) {
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

u16 ReadLE16(const u8* p) {
    return u16(p[0]) | (u16(p[1]) << 8);
}

u32 BytesPerUnit(u32 waveformType) {
    switch (waveformType) {
    case WAVEFORM_TYPE_PCM8:
        return 1;
    case WAVEFORM_TYPE_PCM16:
        return 2;
    case WAVEFORM_TYPE_PCM24:
        return 3;
    case WAVEFORM_TYPE_PCM32:
        return 4;
    default:
        return 2;
    }
}

s32 ParseWaveformBuffer(const u8* data, size_t dataSize, OrbisNgs2WaveformInfo* outInfo) {
    if (!outInfo) {
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS;
    }
    std::memset(outInfo, 0, sizeof(*outInfo));
    if (!data || dataSize < 12) {
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
    }

    // OGG/Vorbis detection
    if (std::memcmp(data, "OggS", 4) == 0) {
        if (dataSize < 28) {
            return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
        }
        const u8 numSegments = data[26];
        if (dataSize < 27 + numSegments + 23) {
            return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
        }
        const u8* packet = data + 27 + numSegments;
        if (std::memcmp(packet, "\x01vorbis", 7) != 0) {
            return ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
        }
        const u8 channels = packet[11];
        const u32 sampleRate = ReadLE32(packet + 12);

        outInfo->format.waveformType = WAVEFORM_TYPE_OGG;
        outInfo->format.numChannels = channels;
        outInfo->format.sampleRate = sampleRate;
        outInfo->format.configData = 0;
        outInfo->format.frameOffset = 0;
        outInfo->format.frameMargin = 0;
        outInfo->dataOffset = 0;
        outInfo->dataSize = static_cast<u32>(dataSize);
        outInfo->numSamples = 0;
        outInfo->audioUnitSize = channels * 2;
        outInfo->numAudioUnitSamples = 1;
        outInfo->numAudioUnitPerFrame = DEFAULT_UNITS_PER_FRAME;
        outInfo->audioFrameSize = outInfo->audioUnitSize * DEFAULT_UNITS_PER_FRAME;
        outInfo->numAudioFrameSamples = DEFAULT_UNITS_PER_FRAME;
        outInfo->numDelaySamples = 0;
        outInfo->numBlocks = 1;
        outInfo->aBlock[0].dataOffset = 0;
        outInfo->aBlock[0].dataSize = static_cast<u32>(dataSize);
        outInfo->aBlock[0].numSamples = 0;
        return ORBIS_OK;
    }

    // RIFF/WAVE detection
    if (std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        return ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
    }

    u32 audioFormat = 1;
    u32 numChannels = 0;
    u32 sampleRate = 0;
    u32 bitsPerSample = 0;
    u32 dataOffset = 0;
    u32 dataChunkSize = 0;
    bool foundData = false;

    size_t pos = 12;
    while (pos + 8 <= dataSize) {
        const u32 chunkSize = ReadLE32(data + pos + 4);
        const size_t chunkStart = pos + 8;
        if (chunkStart + chunkSize > dataSize) {
            break;
        }
        if (std::memcmp(data + pos, "fmt ", 4) == 0 && chunkSize >= 16) {
            const u8* fmt = data + chunkStart;
            audioFormat = ReadLE16(fmt);
            numChannels = ReadLE16(fmt + 2);
            sampleRate = ReadLE32(fmt + 4);
            bitsPerSample = ReadLE16(fmt + 14);
        } else if (std::memcmp(data + pos, "data", 4) == 0) {
            dataOffset = static_cast<u32>(chunkStart);
            dataChunkSize = chunkSize;
            foundData = true;
        }
        pos = chunkStart + chunkSize + (chunkSize & 1);
    }

    if (!foundData || numChannels == 0 || sampleRate == 0) {
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
    }

    u32 waveformType;
    if (audioFormat == 3) {
        waveformType = WAVEFORM_TYPE_PCM32;
    } else if (audioFormat == 1) {
        if (bitsPerSample == 8) {
            waveformType = WAVEFORM_TYPE_PCM8;
        } else if (bitsPerSample == 24) {
            waveformType = WAVEFORM_TYPE_PCM24;
        } else if (bitsPerSample == 32) {
            waveformType = WAVEFORM_TYPE_PCM32;
        } else {
            waveformType = WAVEFORM_TYPE_PCM16;
        }
    } else if (audioFormat == 0xFFFE) {
        waveformType = WAVEFORM_TYPE_PCM16;
    } else {
        return ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
    }

    const u32 frameBytes = (bitsPerSample / 8) * numChannels;
    const u32 numSamples = frameBytes ? (dataChunkSize / frameBytes) : 0;

    outInfo->format.waveformType = waveformType;
    outInfo->format.numChannels = numChannels;
    outInfo->format.sampleRate = sampleRate;
    outInfo->format.configData = 0;
    outInfo->format.frameOffset = 0;
    outInfo->format.frameMargin = 0;
    outInfo->dataOffset = dataOffset;
    outInfo->dataSize = dataChunkSize;
    outInfo->loopBeginPosition = 0;
    outInfo->loopEndPosition = numSamples;
    outInfo->numSamples = numSamples;
    outInfo->audioUnitSize = frameBytes;
    outInfo->numAudioUnitSamples = 1;
    outInfo->numAudioUnitPerFrame = DEFAULT_UNITS_PER_FRAME;
    outInfo->audioFrameSize = frameBytes * DEFAULT_UNITS_PER_FRAME;
    outInfo->numAudioFrameSamples = DEFAULT_UNITS_PER_FRAME;
    outInfo->numDelaySamples = 0;
    outInfo->numBlocks = 1;
    outInfo->aBlock[0].dataOffset = 0;
    outInfo->aBlock[0].dataSize = dataChunkSize;
    outInfo->aBlock[0].numRepeats = 0;
    outInfo->aBlock[0].numSkipSamples = 0;
    outInfo->aBlock[0].numSamples = numSamples;
    return ORBIS_OK;
}

} // namespace

s32 PS4_SYSV_ABI sceNgs2CalcWaveformBlock(const OrbisNgs2WaveformFormat* format, u32 samplePos,
                                          u32 numSamples, OrbisNgs2WaveformBlock* outBlock) {
    if (!format || !outBlock) {
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS;
    }
    const u32 unitSize = BytesPerUnit(format->waveformType) * format->numChannels;
    std::memset(outBlock, 0, sizeof(*outBlock));
    outBlock->dataOffset = samplePos * unitSize;
    outBlock->dataSize = numSamples * unitSize;
    outBlock->numRepeats = 0;
    outBlock->numSkipSamples = 0;
    outBlock->numSamples = numSamples;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2GetWaveformFrameInfo(const OrbisNgs2WaveformFormat* format,
                                             u32* outFrameSize, u32* outNumFrameSamples,
                                             u32* outUnitsPerFrame, u32* outNumDelaySamples) {
    if (!format || !outFrameSize || !outNumFrameSamples || !outUnitsPerFrame || !outNumDelaySamples) {
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS;
    }
    const u32 unitSize = BytesPerUnit(format->waveformType) * format->numChannels;
    *outFrameSize = unitSize * DEFAULT_UNITS_PER_FRAME;
    *outNumFrameSamples = DEFAULT_UNITS_PER_FRAME;
    *outUnitsPerFrame = DEFAULT_UNITS_PER_FRAME;
    *outNumDelaySamples = 0;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2ParseWaveformData(const void* data, size_t dataSize,
                                          OrbisNgs2WaveformInfo* outInfo) {
    if (!data || !outInfo) {
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS;
    }
    const s32 result = ParseWaveformBuffer(static_cast<const u8*>(data), dataSize, outInfo);
    LOG_INFO(Lib_Ngs2, "dataSize = {}, result = {:#x}", dataSize, result);
    return result;
}

s32 PS4_SYSV_ABI sceNgs2ParseWaveformFile(const char* path, u64 offset,
                                          OrbisNgs2WaveformInfo* outInfo) {
    if (!path || !outInfo) {
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS;
    }
    const auto mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const auto filepath = mnt->GetHostPath(path);
    if (filepath.empty()) {
        LOG_ERROR(Lib_Ngs2, "Failed to resolve guest path {}", path);
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
    }
    Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen()) {
        LOG_ERROR(Lib_Ngs2, "Failed to open {}", filepath.string());
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
    }
    const u64 fileSize = file.GetSize();
    if (offset >= fileSize) {
        LOG_ERROR(Lib_Ngs2, "Invalid offset {} for file of size {}", offset, fileSize);
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
    }
    const u64 readSize = fileSize - offset;
    if (readSize > std::numeric_limits<size_t>::max() || readSize > 512_MB) {
        LOG_ERROR(Lib_Ngs2, "File too large to parse: {}", readSize);
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE;
    }
    std::vector<u8> buffer(static_cast<size_t>(readSize));
    if (!file.Seek(static_cast<s64>(offset))) {
        LOG_ERROR(Lib_Ngs2, "Failed to seek in {}", filepath.string());
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
    }
    const size_t got = file.Read(buffer);
    if (got < 12) {
        LOG_ERROR(Lib_Ngs2, "Failed to read {}", filepath.string());
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
    }
    buffer.resize(got);
    const s32 result = ParseWaveformBuffer(buffer.data(), got, outInfo);
    LOG_INFO(Lib_Ngs2, "path = {}, offset = {}, result = {:#x}", path, offset, result);
    return result;
}

s32 PS4_SYSV_ABI sceNgs2ParseWaveformUser(OrbisNgs2ParseReadHandler handler, uintptr_t userData,
                                          OrbisNgs2WaveformInfo* outInfo) {
    if (!handler || !outInfo) {
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS;
    }
    std::vector<u8> buffer(64 * 1024);
    size_t total = 0;
    for (;;) {
        if (total == buffer.size()) {
            if (buffer.size() > 512_MB) {
                return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE;
            }
            buffer.resize(buffer.size() * 2);
        }
        const s32 got = handler(userData, static_cast<u32>(total), buffer.data() + total,
                                buffer.size() - total);
        if (got <= 0) {
            break;
        }
        total += static_cast<size_t>(got);
    }
    buffer.resize(total);
    if (total < 12) {
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
    }
    const s32 result = ParseWaveformBuffer(buffer.data(), total, outInfo);
    LOG_INFO(Lib_Ngs2, "userData = {:#x}, bytes = {}, result = {:#x}", userData, total, result);
    return result;
}

s32 PS4_SYSV_ABI sceNgs2RackCreate(OrbisNgs2Handle systemHandle, u32 rackId,
                                   const OrbisNgs2RackOption* option,
                                   const OrbisNgs2ContextBufferInfo* bufferInfo,
                                   OrbisNgs2Handle* outHandle) {
    LOG_DEBUG(Lib_Ngs2, "rackId = {}", rackId);
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    if (!outHandle) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    // Return a non-null fake handle encoded from rackId so games can query it.
    // Bit pattern: high 16 bits = rackId, low 16 bits = 0x2 (Rack type).
    *outHandle = static_cast<OrbisNgs2Handle>(((u64)rackId << 16) | 0x2);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackCreateWithAllocator(OrbisNgs2Handle systemHandle, u32 rackId,
                                                const OrbisNgs2RackOption* option,
                                                const OrbisNgs2BufferAllocator* allocator,
                                                OrbisNgs2Handle* outHandle) {
    LOG_DEBUG(Lib_Ngs2, "rackId = {}", rackId);
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    if (!outHandle) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    *outHandle = static_cast<OrbisNgs2Handle>(((u64)rackId << 16) | 0x2);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackDestroy(OrbisNgs2Handle rackHandle,
                                    OrbisNgs2ContextBufferInfo* outBufferInfo) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackGetInfo(OrbisNgs2Handle rackHandle, OrbisNgs2RackInfo* outInfo,
                                    size_t infoSize) {
    LOG_ERROR(Lib_Ngs2, "infoSize = {}", infoSize);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackGetUserData(OrbisNgs2Handle rackHandle, uintptr_t* outUserData) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackGetVoiceHandle(OrbisNgs2Handle rackHandle, u32 voiceIndex,
                                           OrbisNgs2Handle* outHandle) {
    LOG_DEBUG(Lib_Ngs2, "voiceIndex = {}", voiceIndex);
    if (!outHandle) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    // Fake voice handle: high 32 bits from rack handle, low 16 bits = voice index, type = Voice(3).
    *outHandle = static_cast<OrbisNgs2Handle>((rackHandle & 0xFFFFFFFF00000000ULL) |
                                              ((u64)voiceIndex << 16) | 0x3);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackLock(OrbisNgs2Handle rackHandle) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackQueryBufferSize(u32 rackId, const OrbisNgs2RackOption* option,
                                            OrbisNgs2ContextBufferInfo* outBufferInfo) {
    LOG_ERROR(Lib_Ngs2, "rackId = {}", rackId);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackSetUserData(OrbisNgs2Handle rackHandle, uintptr_t userData) {
    LOG_ERROR(Lib_Ngs2, "userData = {}", userData);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackUnlock(OrbisNgs2Handle rackHandle) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemCreate(const OrbisNgs2SystemOption* option,
                                     const OrbisNgs2ContextBufferInfo* bufferInfo,
                                     OrbisNgs2Handle* outHandle) {
    s32 result;
    OrbisNgs2ContextBufferInfo localInfo;
    if (!bufferInfo || !outHandle) {
        if (!bufferInfo) {
            result = ORBIS_NGS2_ERROR_INVALID_BUFFER_INFO;
            LOG_ERROR(Lib_Ngs2, "Invalid system buffer info {}", (void*)bufferInfo);
        } else {
            result = ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
            LOG_ERROR(Lib_Ngs2, "Invalid system handle address {}", (void*)outHandle);
        }

        // TODO: Report errors?
    } else {
        // Make bufferInfo copy
        localInfo.hostBuffer = bufferInfo->hostBuffer;
        localInfo.hostBufferSize = bufferInfo->hostBufferSize;
        for (int i = 0; i < 5; i++) {
            localInfo.reserved[i] = bufferInfo->reserved[i];
        }
        localInfo.userData = bufferInfo->userData;

        result = SystemSetup(option, &localInfo, 0, outHandle);
    }

    // TODO: API reporting?

    LOG_INFO(Lib_Ngs2, "called");
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemCreateWithAllocator(const OrbisNgs2SystemOption* option,
                                                  const OrbisNgs2BufferAllocator* allocator,
                                                  OrbisNgs2Handle* outHandle) {
    s32 result;
    if (allocator && allocator->allocHandler != 0) {
        OrbisNgs2BufferAllocHandler hostAlloc = allocator->allocHandler;
        if (outHandle) {
            OrbisNgs2BufferFreeHandler hostFree = allocator->freeHandler;
            OrbisNgs2ContextBufferInfo bufferInfo;
            result = SystemSetup(option, &bufferInfo, 0, 0);
            if (result >= 0) {
                uintptr_t sysUserData = allocator->userData;
                result = hostAlloc(&bufferInfo);
                if (result >= 0) {
                    OrbisNgs2Handle* handleCopy = outHandle;
                    result = SystemSetup(option, &bufferInfo, hostFree, handleCopy);
                    if (result < 0) {
                        if (hostFree) {
                            hostFree(&bufferInfo);
                        }
                    }
                }
            }
        } else {
            result = ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
            LOG_ERROR(Lib_Ngs2, "Invalid system handle address {}", (void*)outHandle);
        }
    } else {
        result = ORBIS_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR;
        LOG_ERROR(Lib_Ngs2, "Invalid system buffer allocator {}", (void*)allocator);
    }
    LOG_INFO(Lib_Ngs2, "called");
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemDestroy(OrbisNgs2Handle systemHandle,
                                      OrbisNgs2ContextBufferInfo* outBufferInfo) {
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    LOG_INFO(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemEnumHandles(OrbisNgs2Handle* aOutHandle, u32 maxHandles) {
    LOG_ERROR(Lib_Ngs2, "maxHandles = {}", maxHandles);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemEnumRackHandles(OrbisNgs2Handle systemHandle,
                                              OrbisNgs2Handle* aOutHandle, u32 maxHandles) {
    LOG_ERROR(Lib_Ngs2, "maxHandles = {}", maxHandles);
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemGetInfo(OrbisNgs2Handle rackHandle, OrbisNgs2SystemInfo* outInfo,
                                      size_t infoSize) {
    LOG_ERROR(Lib_Ngs2, "infoSize = {}", infoSize);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemGetUserData(OrbisNgs2Handle systemHandle, uintptr_t* outUserData) {
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemLock(OrbisNgs2Handle systemHandle) {
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemQueryBufferSize(const OrbisNgs2SystemOption* option,
                                              OrbisNgs2ContextBufferInfo* outBufferInfo) {
    s32 result;
    if (outBufferInfo) {
        result = SystemSetup(option, outBufferInfo, 0, 0);
        LOG_INFO(Lib_Ngs2, "called");
    } else {
        result = ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
        LOG_ERROR(Lib_Ngs2, "Invalid system buffer info {}", (void*)outBufferInfo);
    }

    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemRender(OrbisNgs2Handle systemHandle,
                                     const OrbisNgs2RenderBufferInfo* aBufferInfo,
                                     u32 numBufferInfo) {
    LOG_DEBUG(Lib_Ngs2, "(STUBBED) numBufferInfo = {}", numBufferInfo);
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceNgs2SystemResetOption(OrbisNgs2SystemOption* outOption) {
    static const OrbisNgs2SystemOption option = {
        sizeof(OrbisNgs2SystemOption), "", 0, 512, 256, 48000, {0}};

    if (!outOption) {
        LOG_ERROR(Lib_Ngs2, "Invalid system option address {}", (void*)outOption);
        return ORBIS_NGS2_ERROR_INVALID_OPTION_ADDRESS;
    }
    *outOption = option;

    LOG_INFO(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemSetGrainSamples(OrbisNgs2Handle systemHandle, u32 numSamples) {
    LOG_ERROR(Lib_Ngs2, "numSamples = {}", numSamples);
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemSetSampleRate(OrbisNgs2Handle systemHandle, u32 sampleRate) {
    LOG_ERROR(Lib_Ngs2, "sampleRate = {}", sampleRate);
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemSetUserData(OrbisNgs2Handle systemHandle, uintptr_t userData) {
    LOG_ERROR(Lib_Ngs2, "userData = {}", userData);
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemUnlock(OrbisNgs2Handle systemHandle) {
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2VoiceControl(OrbisNgs2Handle voiceHandle,
                                     const OrbisNgs2VoiceParamHeader* paramList) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetMatrixInfo(OrbisNgs2Handle voiceHandle, u32 matrixId,
                                           OrbisNgs2VoiceMatrixInfo* outInfo, size_t outInfoSize) {
    LOG_ERROR(Lib_Ngs2, "matrixId = {}, outInfoSize = {}", matrixId, outInfoSize);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetOwner(OrbisNgs2Handle voiceHandle, OrbisNgs2Handle* outRackHandle,
                                      u32* outVoiceId) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetPortInfo(OrbisNgs2Handle voiceHandle, u32 port,
                                         OrbisNgs2VoicePortInfo* outInfo, size_t outInfoSize) {
    LOG_ERROR(Lib_Ngs2, "port = {}, outInfoSize = {}", port, outInfoSize);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetState(OrbisNgs2Handle voiceHandle, OrbisNgs2VoiceState* outState,
                                      size_t stateSize) {
    if (!outState) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    // Report voice as "idle" (state 0) so games don't spin-wait for playback.
    outState->stateFlags = 0;
    LOG_DEBUG(Lib_Ngs2, "voiceHandle = {:#x}", voiceHandle);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetStateFlags(OrbisNgs2Handle voiceHandle, u32* outStateFlags) {
    if (!outStateFlags) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    *outStateFlags = 0;
    return ORBIS_OK;
}

// Ngs2Custom

s32 PS4_SYSV_ABI sceNgs2CustomRackGetModuleInfo(OrbisNgs2Handle rackHandle, u32 moduleIndex,
                                                OrbisNgs2CustomModuleInfo* outInfo,
                                                size_t infoSize) {
    LOG_ERROR(Lib_Ngs2, "moduleIndex = {}, infoSize = {}", moduleIndex, infoSize);
    return ORBIS_OK;
}

// Ngs2Geom

s32 PS4_SYSV_ABI sceNgs2GeomResetListenerParam(OrbisNgs2GeomListenerParam* outListenerParam) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2GeomResetSourceParam(OrbisNgs2GeomSourceParam* outSourceParam) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2GeomCalcListener(const OrbisNgs2GeomListenerParam* param,
                                         OrbisNgs2GeomListenerWork* outWork, u32 flags) {
    LOG_ERROR(Lib_Ngs2, "flags = {}", flags);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2GeomApply(const OrbisNgs2GeomListenerWork* listener,
                                  const OrbisNgs2GeomSourceParam* source,
                                  OrbisNgs2GeomAttribute* outAttrib, u32 flags) {
    LOG_ERROR(Lib_Ngs2, "flags = {}", flags);
    return ORBIS_OK;
}

// Ngs2Pan

s32 PS4_SYSV_ABI sceNgs2PanInit(OrbisNgs2PanWork* work, const float* aSpeakerAngle, float unitAngle,
                                u32 numSpeakers) {
    LOG_ERROR(Lib_Ngs2, "unitAngle = {}, numSpeakers = {}", unitAngle, numSpeakers);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2PanGetVolumeMatrix(OrbisNgs2PanWork* work, const OrbisNgs2PanParam* aParam,
                                           u32 numParams, u32 matrixFormat,
                                           float* outVolumeMatrix) {
    LOG_ERROR(Lib_Ngs2, "numParams = {}, matrixFormat = {}", numParams, matrixFormat);
    return ORBIS_OK;
}

// Ngs2Report

s32 PS4_SYSV_ABI sceNgs2ReportRegisterHandler(u32 reportType, OrbisNgs2ReportHandler handler,
                                              uintptr_t userData, OrbisNgs2Handle* outHandle) {
    LOG_INFO(Lib_Ngs2, "reportType = {}, userData = {}", reportType, userData);
    if (!handler) {
        LOG_ERROR(Lib_Ngs2, "handler is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_REPORT_HANDLE;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2ReportUnregisterHandler(OrbisNgs2Handle reportHandle) {
    if (!reportHandle) {
        LOG_ERROR(Lib_Ngs2, "reportHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_REPORT_HANDLE;
    }
    LOG_INFO(Lib_Ngs2, "called");
    return ORBIS_OK;
}

// Unknown

int PS4_SYSV_ABI sceNgs2FftInit() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2FftProcess() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2FftQuerySize() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2JobSchedulerResetOption() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2ModuleArrayEnumItems() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2ModuleEnumConfigs() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2ModuleQueueEnumItems() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2RackQueryInfo() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2RackRunCommands() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2SystemQueryInfo() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2SystemRunCommands() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2SystemSetLoudThreshold() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2StreamCreate() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2StreamCreateWithAllocator() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2StreamDestroy() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2StreamQueryBufferSize() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2StreamQueryInfo() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2StreamResetOption() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2StreamRunCommands() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2VoiceQueryInfo() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2VoiceRunCommands() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("3pCNbVM11UA", "libSceNgs2", 1, "libSceNgs2", sceNgs2CalcWaveformBlock);
    LIB_FUNCTION("6qN1zaEZuN0", "libSceNgs2", 1, "libSceNgs2", sceNgs2CustomRackGetModuleInfo);
    LIB_FUNCTION("Kg1MA5j7KFk", "libSceNgs2", 1, "libSceNgs2", sceNgs2FftInit);
    LIB_FUNCTION("D8eCqBxSojA", "libSceNgs2", 1, "libSceNgs2", sceNgs2FftProcess);
    LIB_FUNCTION("-YNfTO6KOMY", "libSceNgs2", 1, "libSceNgs2", sceNgs2FftQuerySize);
    LIB_FUNCTION("eF8yRCC6W64", "libSceNgs2", 1, "libSceNgs2", sceNgs2GeomApply);
    LIB_FUNCTION("1WsleK-MTkE", "libSceNgs2", 1, "libSceNgs2", sceNgs2GeomCalcListener);
    LIB_FUNCTION("7Lcfo8SmpsU", "libSceNgs2", 1, "libSceNgs2", sceNgs2GeomResetListenerParam);
    LIB_FUNCTION("0lbbayqDNoE", "libSceNgs2", 1, "libSceNgs2", sceNgs2GeomResetSourceParam);
    LIB_FUNCTION("ekGJmmoc8j4", "libSceNgs2", 1, "libSceNgs2", sceNgs2GetWaveformFrameInfo);
    LIB_FUNCTION("BcoPfWfpvVI", "libSceNgs2", 1, "libSceNgs2", sceNgs2JobSchedulerResetOption);
    LIB_FUNCTION("EEemGEQCjO8", "libSceNgs2", 1, "libSceNgs2", sceNgs2ModuleArrayEnumItems);
    LIB_FUNCTION("TaoNtmMKkXQ", "libSceNgs2", 1, "libSceNgs2", sceNgs2ModuleEnumConfigs);
    LIB_FUNCTION("ve6bZi+1sYQ", "libSceNgs2", 1, "libSceNgs2", sceNgs2ModuleQueueEnumItems);
    LIB_FUNCTION("gbMKV+8Enuo", "libSceNgs2", 1, "libSceNgs2", sceNgs2PanGetVolumeMatrix);
    LIB_FUNCTION("xa8oL9dmXkM", "libSceNgs2", 1, "libSceNgs2", sceNgs2PanInit);
    LIB_FUNCTION("hyVLT2VlOYk", "libSceNgs2", 1, "libSceNgs2", sceNgs2ParseWaveformData);
    LIB_FUNCTION("iprCTXPVWMI", "libSceNgs2", 1, "libSceNgs2", sceNgs2ParseWaveformFile);
    LIB_FUNCTION("t9T0QM17Kvo", "libSceNgs2", 1, "libSceNgs2", sceNgs2ParseWaveformUser);
    LIB_FUNCTION("cLV4aiT9JpA", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackCreate);
    LIB_FUNCTION("U546k6orxQo", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackCreateWithAllocator);
    LIB_FUNCTION("lCqD7oycmIM", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackDestroy);
    LIB_FUNCTION("M4LYATRhRUE", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackGetInfo);
    LIB_FUNCTION("Mn4XNDg03XY", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackGetUserData);
    LIB_FUNCTION("MwmHz8pAdAo", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackGetVoiceHandle);
    LIB_FUNCTION("MzTa7VLjogY", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackLock);
    LIB_FUNCTION("0eFLVCfWVds", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackQueryBufferSize);
    LIB_FUNCTION("TZqb8E-j3dY", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackQueryInfo);
    LIB_FUNCTION("MI2VmBx2RbM", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackRunCommands);
    LIB_FUNCTION("JNTMIaBIbV4", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackSetUserData);
    LIB_FUNCTION("++YZ7P9e87U", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackUnlock);
    LIB_FUNCTION("uBIN24Tv2MI", "libSceNgs2", 1, "libSceNgs2", sceNgs2ReportRegisterHandler);
    LIB_FUNCTION("nPzb7Ly-VjE", "libSceNgs2", 1, "libSceNgs2", sceNgs2ReportUnregisterHandler);
    LIB_FUNCTION("koBbCMvOKWw", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemCreate);
    LIB_FUNCTION("mPYgU4oYpuY", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemCreateWithAllocator);
    LIB_FUNCTION("u-WrYDaJA3k", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemDestroy);
    LIB_FUNCTION("vubFP0T6MP0", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemEnumHandles);
    LIB_FUNCTION("U-+7HsswcIs", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemEnumRackHandles);
    LIB_FUNCTION("vU7TQ62pItw", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemGetInfo);
    LIB_FUNCTION("4lFaRxd-aLs", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemGetUserData);
    LIB_FUNCTION("gThZqM5PYlQ", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemLock);
    LIB_FUNCTION("pgFAiLR5qT4", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemQueryBufferSize);
    LIB_FUNCTION("3oIK7y7O4k0", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemQueryInfo)
    LIB_FUNCTION("i0VnXM-C9fc", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemRender);
    LIB_FUNCTION("AQkj7C0f3PY", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemResetOption);
    LIB_FUNCTION("gXiormHoZZ4", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemRunCommands);
    LIB_FUNCTION("l4Q2dWEH6UM", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemSetGrainSamples);
    LIB_FUNCTION("Wdlx0ZFTV9s", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemSetLoudThreshold);
    LIB_FUNCTION("-tbc2SxQD60", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemSetSampleRate);
    LIB_FUNCTION("GZB2v0XnG0k", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemSetUserData);
    LIB_FUNCTION("JXRC5n0RQls", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemUnlock);
    LIB_FUNCTION("sU2St3agdjg", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamCreate);
    LIB_FUNCTION("I+RLwaauggA", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamCreateWithAllocator);
    LIB_FUNCTION("bfoMXnTRtwE", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamDestroy);
    LIB_FUNCTION("dxulc33msHM", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamQueryBufferSize);
    LIB_FUNCTION("rfw6ufRsmow", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamQueryInfo);
    LIB_FUNCTION("q+2W8YdK0F8", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamResetOption);
    LIB_FUNCTION("qQHCi9pjDps", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamRunCommands);
    LIB_FUNCTION("uu94irFOGpA", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceControl);
    LIB_FUNCTION("jjBVvPN9964", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceGetMatrixInfo);
    LIB_FUNCTION("W-Z8wWMBnhk", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceGetOwner);
    LIB_FUNCTION("WCayTgob7-o", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceGetPortInfo);
    LIB_FUNCTION("-TOuuAQ-buE", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceGetState);
    LIB_FUNCTION("rEh728kXk3w", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceGetStateFlags);
    LIB_FUNCTION("9eic4AmjGVI", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceQueryInfo);
    LIB_FUNCTION("AbYvTOZ8Pts", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceRunCommands);
};

} // namespace Libraries::Ngs2
