// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

constexpr int ORBIS_JPEG_ENC_ERROR_INVALID_ADDR = 0x80650101;
constexpr int ORBIS_JPEG_ENC_ERROR_INVALID_SIZE = 0x80650102;
constexpr int ORBIS_JPEG_ENC_ERROR_INVALID_PARAM = 0x80650103;
constexpr int ORBIS_JPEG_ENC_ERROR_INVALID_HANDLE = 0x80650104;

// libSceJpegDec uses the same error domain as libSceJpegEnc for these common failures.
constexpr int ORBIS_JPEG_DEC_ERROR_INVALID_ADDR = ORBIS_JPEG_ENC_ERROR_INVALID_ADDR;
constexpr int ORBIS_JPEG_DEC_ERROR_INVALID_SIZE = ORBIS_JPEG_ENC_ERROR_INVALID_SIZE;
constexpr int ORBIS_JPEG_DEC_ERROR_INVALID_PARAM = ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
constexpr int ORBIS_JPEG_DEC_ERROR_INVALID_HANDLE = ORBIS_JPEG_ENC_ERROR_INVALID_HANDLE;
// No separate decode-failure value is exposed by this tree; report it as invalid input.
constexpr int ORBIS_JPEG_DEC_ERROR_DECODE_FAILED = ORBIS_JPEG_DEC_ERROR_INVALID_PARAM;
