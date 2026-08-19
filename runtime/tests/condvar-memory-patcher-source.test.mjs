import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("condvar logging uses the Pthread name field", () => {
  const condvar = read("sources/shadps4/src/core/libraries/kernel/threads/condvar.cpp");
  assert.doesNotMatch(condvar, /GetName\(\)/);
  assert.match(condvar, /curthread->name/);
});

test("memory patcher files are installed and wired into the build", () => {
  const header = read("sources/shadps4/src/common/memory_patcher.h");
  const source = read("sources/shadps4/src/common/memory_patcher.cpp");
  const cmake = read("sources/shadps4/CMakeLists.txt");
  assert.match(header, /void StartPatchWatcher\(\);/);
  assert.match(source, /void StartPatchWatcher\(\)/);
  assert.match(cmake, /src\/common\/memory_patcher\.h/);
  assert.match(cmake, /src\/common\/memory_patcher\.cpp/);
});
