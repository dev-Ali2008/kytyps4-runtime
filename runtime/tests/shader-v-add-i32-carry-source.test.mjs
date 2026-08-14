import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("V_ADD_I32 propagates unsigned carry-out", () => {
  const source = read("src/shader_recompiler/frontend/translate/vector_alu.cpp");
  const start = source.indexOf("void Translator::V_ADD_I32");
  const end = source.indexOf("void Translator::V_SUB_I32", start);
  assert.ok(start >= 0 && end > start, "V_ADD_I32 implementation must exist");

  const implementation = source.slice(start, end);
  assert.match(implementation, /const IR::U32 result\{ir\.IAdd\(src0, src1\)\};/);
  assert.match(implementation, /SetDst\(inst\.dst\[0\], result\);/);
  assert.match(implementation, /SetCarryOut\(inst, ir\.ILessThan\(result, src0, false\)\);/);
  assert.doesNotMatch(implementation, /TODO: Carry-out/);
});
