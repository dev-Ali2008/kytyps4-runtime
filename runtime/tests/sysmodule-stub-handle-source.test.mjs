import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("sysmodule fallback handle is declared before all uses", () => {
  const source = read("sources/shadps4/src/core/libraries/sysmodule/sysmodule_internal.cpp");
  const declaration = source.indexOf("static s32 stub_handle = 100;");
  assert.notEqual(declaration, -1);
  const firstUse = source.indexOf("stub_handle++");
  assert.notEqual(firstUse, -1);
  assert.ok(declaration < firstUse, "stub_handle must be declared before its first fallback use");
  assert.equal((source.match(/static s32 stub_handle = 100;/g) ?? []).length, 1);
});
