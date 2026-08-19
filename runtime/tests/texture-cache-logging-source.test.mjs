import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("texture-cache warning uses the registered Vulkan renderer logger", () => {
  const source = read("sources/shadps4/src/video_core/texture_cache/texture_cache.cpp");
  assert.doesNotMatch(source, /Render_TextureCache/);
  assert.match(source, /LOG_WARNING\(Render_Vulkan,/);
});
