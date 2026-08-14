import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("direct-memory allocation validates search range before unsigned length comparison", () => {
  const memory = read("src/core/libraries/kernel/memory.cpp");
  const start = memory.indexOf("s32 PS4_SYSV_ABI sceKernelAllocateDirectMemory");
  const end = memory.indexOf("s32 PS4_SYSV_ABI sceKernelAllocateMainDirectMemory", start);
  assert.ok(start >= 0 && end > start, "direct-memory allocator definition must exist");

  const allocator = memory.slice(start, end);
  const rangeGuard = allocator.indexOf("if (searchEnd <= searchStart)");
  const difference = allocator.indexOf("const u64 available_range = static_cast<u64>(searchEnd - searchStart)");
  const lengthGuard = allocator.indexOf("if (available_range < len)");

  assert.ok(rangeGuard >= 0, "invalid search ranges must be rejected");
  assert.ok(difference > rangeGuard, "the signed range must be validated before subtraction");
  assert.ok(lengthGuard > difference, "the validated unsigned range must constrain len");
  assert.match(allocator, /memory->Allocate\(searchStart, searchEnd, len, alignment, memoryType\)/);
});
