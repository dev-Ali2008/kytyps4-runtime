import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

const escapeRegex = (value) => value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

const aliases = [
  ["+P6FRGH4LfA", "internal_memmove"],
  ["aesyjrHVWy4", "internal_strncmp"],
  ["6sJWiWSRuqk", "internal_strncpy"],
  ["ob5xAW4ln-0", "internal_strchr"],
  ["viiwFMaNamA", "internal_strstr"],
  ["AV6ipCNa4Rw", "internal_strcasecmp"],
  ["pXvbDfchu6k", "internal_strncasecmp"],
  ["SRI6S9B+-a4", "internal_atof"],
  ["cpCOXWMgha0", "internal_rand"],
  ["VPbJwTCgME0", "internal_srand"],
  ["xeYO4u7uyJ0", "internal_fopen"],
  ["lbB+UlZqVG0", "internal_fread"],
  ["uodLYyUip20", "internal_fclose"],
];

test("FEX registers DEADBOLT-critical libc imports instead of ENOSYS fallbacks", () => {
  const memory = read("sources/shadps4/src/core/libraries/libc_internal/libc_internal_memory.cpp");
  const strings = read("sources/shadps4/src/core/libraries/libc_internal/libc_internal_str.cpp");
  const math = read("sources/shadps4/src/core/libraries/libc_internal/libc_internal_math.cpp");
  const io = read("sources/shadps4/src/core/libraries/libc_internal/libc_internal_io.cpp");
  const sources = `${memory}\n${strings}\n${math}\n${io}`;

  for (const [nid, functionName] of aliases) {
    assert.match(
      sources,
      new RegExp(`LIB_FUNCTION\\("${escapeRegex(nid)}", "libc", 1, "libc", ${functionName}\\)`),
    );
  }
  assert.match(io, /void RegisterFexLibcIoAliases/);
});

test("FEX CXA logging avoids invalid function-pointer casts", () => {
  const cxa = read("sources/shadps4/src/core/libraries/libc_internal/libc_internal_cxa.cpp");
  assert.doesNotMatch(cxa, /static_cast<void\*>\(reinterpret_cast<void\(\*\)\(\)\>/);
  assert.match(cxa, /registered atexit handler arg=\{\} dso=\{\}/);
});

