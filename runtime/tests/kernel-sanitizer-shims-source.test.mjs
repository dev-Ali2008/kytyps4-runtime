import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("libkernel sanitizer replacement hooks return benign values instead of failing", () => {
  const kernel = read("src/core/libraries/kernel/kernel.cpp");
  const memory = read("src/core/libraries/kernel/memory.cpp");
  const aerolib = read("src/core/aerolib/aerolib.inl");

  // Castlevania Anniversary Collection (CUSA15101) calls
  // sceKernelGetSanitizerMallocReplaceExternal during boot. The FEX ENOSYS
  // fallback previously delivered an error where a pointer was expected, which
  // the guest dereferenced and crashed on. The whole sanitizer family must
  // return "no override" so games proceed with their normal allocators.
  for (const nid of ["py6L8jiVAN8", "bt0POEUZddE", "F4Kib3Mb0wI", "bnZxYgAFeA0"]) {
    assert.match(aerolib, new RegExp(`STUB\\("${nid}"`));
  }

  assert.match(kernel, /void\* PS4_SYSV_ABI sceKernelGetSanitizerNewReplaceExternal\(\)/);
  assert.match(kernel, /void\* PS4_SYSV_ABI sceKernelGetSanitizerNewReplace\(\)/);
  assert.match(kernel, /void\* PS4_SYSV_ABI sceKernelGetSanitizerMallocReplace\(\)/);
  assert.match(kernel, /void\* PS4_SYSV_ABI sceKernelGetSanitizerMallocReplaceExternal\(\)/);

  // Every shim must return a benign value (nullptr / false), never an error
  // code that the guest will interpret as a pointer.
  const shims = kernel.slice(
    kernel.indexOf("sceKernelGetSanitizerNewReplaceExternal"),
    kernel.indexOf("sceKernelGetAllowedSdkVersionOnSystem"),
  );
  assert.doesNotMatch(shims, /return ORBIS_KERNEL_ERROR/);
  assert.doesNotMatch(shims, /return ENOSYS/);

  // And every shim must be registered so the resolver binds it by NID.
  const register = kernel.slice(kernel.indexOf("void RegisterLib"));
  assert.match(register, /LIB_FUNCTION\("py6L8jiVAN8".*sceKernelGetSanitizerMallocReplaceExternal/s);
  assert.match(register, /LIB_FUNCTION\("bt0POEUZddE".*sceKernelGetSanitizerMallocReplace/s);
  assert.match(register, /LIB_FUNCTION\("F4Kib3Mb0wI".*sceKernelGetSanitizerNewReplace/s);
  assert.match(register, /LIB_FUNCTION\("bnZxYgAFeA0".*sceKernelGetSanitizerNewReplaceExternal/s);
  // The sanitizer-state function already belongs to memory.cpp; keep its
  // implementation and resolver registration there to avoid duplicate symbols.
  assert.match(memory, /u32 PS4_SYSV_ABI sceKernelIsAddressSanitizerEnabled\(\)/);
  assert.match(memory, /LIB_FUNCTION\("jh\+8XiK4LeE".*sceKernelIsAddressSanitizerEnabled/s);
  assert.doesNotMatch(kernel, /sceKernelIsAddressSanitizerEnabled\(\)/);
});
