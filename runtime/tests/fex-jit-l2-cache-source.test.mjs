import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("FEX keeps the L2 translation lookup cache enabled", () => {
  const engine = read("sources/shadps4/src/core/fex/fex_guest_engine.cpp");
  const configBlock = engine.slice(
    engine.indexOf("FEXCore::Config::Initialize();"),
    engine.indexOf("const bool traceEnabled"),
  );

  assert.match(configBlock, /CONFIG_DISABLEL2CACHE, "0"/);
  assert.doesNotMatch(configBlock, /CONFIG_DISABLEL2CACHE, "1"/);
  assert.match(engine, /second-level translation lookup enabled/);
});

