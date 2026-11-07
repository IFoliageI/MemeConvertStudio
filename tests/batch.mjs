import assert from "node:assert/strict";
import http from "node:http";
import fs from "node:fs/promises";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const exe = process.argv.find(a => a.startsWith("--exe="))?.slice(6) ??
  path.join(root, "bin", "MemeStudio-batch.exe");
const directory = path.join(root, "tmp", "batch", Date.now().toString(36));
const fixtures = path.join(root, "tmp", "tests");
await fs.mkdir(directory, { recursive: true });
if (process.argv.includes("--ui-only")) {
  assert.equal(await run(["--ui-check", fixtures]), 0);
  console.log(await fs.readFile(path.join(fixtures, "ui-results.json"), "utf8"));
  process.exit(0);
}
const image = await fs.readFile(path.join(fixtures, "character.png"));
let calls = 0;
const prompts = [];
const server = http.createServer(async (request, response) => {
  try {
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    const form = await new Request("http://localhost", { method: "POST",
      headers: { "Content-Type": request.headers["content-type"] },
      body: Buffer.concat(chunks) }).formData();
    assert.equal(form.get("model"), "gpt-image-test");
    assert.equal(form.getAll("image[]").length >= 2, true);
    prompts.push(form.get("prompt")); ++calls;
    response.writeHead(200, { "Content-Type": "application/json" });
    response.end(JSON.stringify({ data: [{ b64_json: image.toString("base64") }] }));
  } catch (error) {
    response.writeHead(500); response.end(String(error));
  }
});
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
function run(args) {
  return new Promise((resolve, reject) => {
    const child = spawn(exe, args, { windowsHide: true,
      env: { ...process.env, OPENAI_API_KEY: "batch-test-key" } });
    const timer = setTimeout(() => { child.kill(); reject(new Error("Batch check timed out")); }, 25000);
    child.on("error", error => { clearTimeout(timer); reject(error); });
    child.on("exit", code => { clearTimeout(timer); resolve(code); });
  });
}
async function job(name, extra = {}) {
  const config = path.join(directory, `${name}.json`);
  await fs.writeFile(config, JSON.stringify({
    source: path.join(fixtures, "source.png"), character: path.join(fixtures, "character.png"),
    output: path.join(directory, `${name}.png`), count: 2, model: "gpt-image-test",
    base_url: `http://127.0.0.1:${server.address().port}/v1`,
    progress_file: path.join(directory, "progress.json"), ...extra,
  }));
  return run(["--job", config]);
}
const exists = name => fs.access(path.join(directory, name)).then(() => true, () => false);
async function inspect(file, name) {
  const output = path.join(directory, `${name}.json`);
  assert.equal(await run(["--inspect", file, output]), 0);
  return JSON.parse(await fs.readFile(output, "utf8"));
}
try {
  assert.equal(await job("static"), 0);
  assert.equal(calls, 2);
  assert(await exists("static_001.png") && await exists("static_002.png"));
  assert(!await exists("static.png"));
  assert.equal(await job("static"), 0);
  assert.equal(calls, 2, "Numbered results must reuse independent caches");
  assert.equal(await job("static", { count: 3 }), 0);
  assert.equal(calls, 3);
  assert(await exists("static_003.png"));
  assert.equal(await job("static", { count: 3, plugins: ["memestudio.example"] }), 0);
  assert.equal(calls, 6, "Enabling a plugin must invalidate caches");
  assert(prompts.slice(3).every(text => text.includes("distinctive facial features")));
  assert(!prompts[0].includes("distinctive facial features"));
  assert.equal(await job("static", { count: 3, plugins: ["memestudio.example"] }), 0);
  assert.equal(calls, 6);
  assert.equal(await job("static", { count: 3 }), 0);
  assert.equal(calls, 6, "Disabled plugin must recover original caches");
  const source = path.join(fixtures, "source.gif");
  const original = await inspect(source, "original");
  const before = calls;
  assert.equal(await job("animated", { source, output: path.join(directory, "animated.gif") }), 0);
  assert.equal(calls - before, original.delays.length * 2);
  for (const index of ["001", "002"]) {
    const result = await inspect(path.join(directory, `animated_${index}.gif`), index);
    assert.deepEqual(result, original, "Each GIF must preserve full frame count, dimensions, delays and loop");
  }
  const progress = JSON.parse(await fs.readFile(path.join(directory, "progress.json"), "utf8"));
  assert.equal(progress.done, original.delays.length * 2);
  assert.equal(progress.total, progress.done);
  assert.equal(progress.count, 2);
  const beforeInvalid = calls;
  assert.equal(await job("invalid", { count: 11 }), 1);
  assert.equal(calls, beforeInvalid);
  console.log("PASS numbered PNG batches; per-copy cache and increased count; plugin hooks/cache isolation; complete GIF copies and cumulative progress; count validation");
} finally {
  server.closeAllConnections();
  await new Promise(resolve => server.close(resolve));
}
