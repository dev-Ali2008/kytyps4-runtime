import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("presenter frame tracing is opt-in instead of unconditional", () => {
  const presenter = read("sources/shadps4/src/video_core/renderer_vulkan/vk_presenter.cpp");
  const service = read(
    "sources/shadps4/android/BachataS4/app/src/main/kotlin/com/bachatas4/android/service/EmulationService.kt",
  );
  const launcher = read(
    "sources/shadps4/android/BachataS4/core/runtime/src/main/kotlin/com/bachatas4/android/runtime/process/RuntimeProcessLauncher.kt",
  );

  assert.match(presenter, /std::getenv\("BACHATA_PRESENT_TRACE"\)/);
  assert.match(presenter, /const bool trace = PresentTraceEnabled\(\) &&/);
  assert.match(service, /"BACHATA_PRESENT_TRACE" to if \(BuildConfig\.DEBUG\) "1" else "0"/);
  assert.match(launcher, /"BACHATA_PRESENT_TRACE"/);
  assert.doesNotMatch(presenter, /const bool trace = trace_id < 64/);
});

