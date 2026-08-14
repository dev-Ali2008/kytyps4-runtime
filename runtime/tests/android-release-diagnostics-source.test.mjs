import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";
import assert from "node:assert/strict";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("release gameplay disables always-on diagnostic tracing", () => {
  const service = read(
    "sources/shadps4/android/BachataS4/app/src/main/kotlin/com/bachatas4/android/service/EmulationService.kt",
  );

  assert.match(service, /"BOX64_LOG" to if \(BuildConfig\.DEBUG\) "1" else "0"/);
  assert.match(
    service,
    /"BACHATA_VORTEK_TRACE_BIND_VERTEX_BUFFERS" to if \(BuildConfig\.DEBUG\) "1" else "0"/,
  );
  assert.match(
    service,
    /"BACHATA_FEX_TRACE_SIGSYS" to if \(BuildConfig\.DEBUG\) "1" else "0"/,
  );
  assert.doesNotMatch(service, /"BOX64_LOG" to "1"/);
  assert.doesNotMatch(service, /"BACHATA_VORTEK_TRACE_BIND_VERTEX_BUFFERS" to "1"/);
  assert.doesNotMatch(service, /"BACHATA_FEX_TRACE_SIGSYS" to "1"/);
});

