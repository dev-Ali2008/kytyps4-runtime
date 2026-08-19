import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");
const escapeRegex = (value) => value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

test("JPEG decoder HLE is registered and included in the build", () => {
  const libs = read("sources/shadps4/src/core/libraries/libs.cpp");
  const cmake = read("sources/shadps4/CMakeLists.txt");
  const jpeg = read("sources/shadps4/src/core/libraries/jpeg/jpegdec.cpp");
  const jpegErrors = read("sources/shadps4/src/core/libraries/jpeg/jpeg_error.h");

  assert.match(libs, /Libraries::JpegDec::RegisterLib\(sym\)/);
  assert.match(cmake, /src\/core\/libraries\/jpeg\/jpegdec\.cpp/);
  assert.match(cmake, /src\/core\/libraries\/jpeg\/jpegdec\.h/);
  for (const nid of [
    "1kzQRoWEgSA",
    "919MhccOiII",
    "Hwh11+m5KoI",
    "JPh3Zgg0Zwc",
    "LSinoSQH790",
    "uNAUmANZMEw",
  ]) {
    assert.match(jpeg, new RegExp(`LIB_FUNCTION\\("${escapeRegex(nid)}"`));
  }
  assert.match(jpeg, /stbi_load_from_memory/);
  assert.match(jpeg, /ValidateJpegDecHandle/);
  assert.match(jpeg, /#include "common\/alignment\.h"/);
  assert.match(jpeg, /ORBIS_JPEG_DEC_ERROR_INVALID_ADDR/);
  assert.match(jpeg, /ORBIS_JPEG_DEC_ERROR_INVALID_SIZE/);
  assert.match(jpeg, /ORBIS_JPEG_DEC_ERROR_INVALID_PARAM/);
  assert.match(jpeg, /ORBIS_JPEG_DEC_ERROR_INVALID_HANDLE/);
  assert.match(jpeg, /ORBIS_JPEG_DEC_ERROR_DECODE_FAILED/);
  assert.match(jpegErrors, /ORBIS_JPEG_DEC_ERROR_INVALID_ADDR/);
  assert.match(jpegErrors, /ORBIS_JPEG_DEC_ERROR_DECODE_FAILED/);
  const header = read("sources/shadps4/src/core/libraries/jpeg/jpegdec.h");
  assert.match(header, /sizeof\(OrbisJpegDecDecodeParam\) == 0x28/);
  assert.match(header, /sizeof\(OrbisJpegDecHandleInternal\) == 0x18/);
});
