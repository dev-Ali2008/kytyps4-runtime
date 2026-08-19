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
  const header = read("sources/shadps4/src/core/libraries/jpeg/jpegdec.h");
  assert.match(header, /sizeof\(OrbisJpegDecDecodeParam\) == 0x28/);
  assert.match(header, /sizeof\(OrbisJpegDecHandleInternal\) == 0x18/);
});
