import assert from "node:assert/strict";
import http from "node:http";
import fs from "node:fs/promises";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const exe = process.argv.find(a => a.startsWith("--exe="))?.slice(6) ??
  path.join(root, "bin", "MemeStudio-source-batch.exe");
const fixtures = path.join(root, "tmp", "tests");
const directory = path.join(root, "tmp", "source-batch", Date.now().toString(36));
await fs.mkdir(path.join(directory, "inputs"), { recursive: true });
await fs.copyFile(path.join(fixtures, "source.png"), path.join(directory, "inputs", "01.png"));
await fs.copyFile(path.join(fixtures, "source.gif"), path.join(directory, "inputs", "02.gif"));
await fs.writeFile(path.join(directory, "inputs", "broken.png"), "invalid");
const image = await fs.readFile(path.join(fixtures, "character.png"));
let calls = 0, failAt = 0;
const server = http.createServer(async (request, response) => {
  try {
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    const form = await new Request("http://localhost", {
      method: "POST", headers: { "Content-Type": request.headers["content-type"] },
      body: Buffer.concat(chunks),
    }).formData();
    assert.equal(form.get("model"), "gpt-image-test");
    assert.equal(form.getAll("image[]").length >= 2, true);
    ++calls;
    if (calls === failAt) {
      response.writeHead(429); response.end('{"error":{"message":"test limit"}}'); return;
    }
    response.writeHead(200, { "Content-Type": "application/json" });
    response.end(JSON.stringify({ data: [{ b64_json: image.toString("base64") }] }));
  } catch (error) { response.writeHead(500); response.end(String(error)); }
});
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
function run(args) {
  return new Promise((resolve, reject) => {
    const child = spawn(exe, args, { windowsHide: true, env: { ...process.env, OPENAI_API_KEY: "test-batch-key" } });
    const timer = setTimeout(() => { child.kill(); reject(new Error("Source batch timed out")); }, 30000);
    child.on("error", error => { clearTimeout(timer); reject(error); });
    child.on("exit", code => { clearTimeout(timer); resolve(code); });
  });
}
const output = path.join(directory, "outputs");
async function job(overrides = {}) {
  const file = path.join(directory, "job.json");
  await fs.writeFile(file, JSON.stringify({
    sources: [path.join(directory, "inputs"), path.join(directory, "inputs", "01.png")],
    character: path.join(fixtures, "character.png"), output, count: 2,
    model: "gpt-image-test", plugins: ["memestudio.batch-convert"],
    base_url: `http://127.0.0.1:${server.address().port}/v1`,
    progress_file: path.join(directory, "progress.json"), ...overrides,
  }));
  return run(["--job", file]);
}
try {
  assert.equal(await job(), 0);
  const firstCalls = calls;
  assert.equal(calls, 8, "Two PNG copies and two three-frame GIF copies");
  for (const name of ["001_01-edited_001.png", "001_01-edited_002.png",
    "002_02-edited_001.gif", "002_02-edited_002.gif"]) await fs.access(path.join(output, name));
  const inspect = path.join(directory, "inspect.json");
  assert.equal(await run(["--inspect", path.join(output, "002_02-edited_001.gif"), inspect]), 0);
  const gif = JSON.parse(await fs.readFile(inspect, "utf8"));
  assert.equal(gif.delays.length, 3);
  const progress = JSON.parse(await fs.readFile(path.join(directory, "progress.json"), "utf8"));
  assert.equal(progress.done, 8); assert.equal(progress.total, 8);
  assert.equal(await job(), 0); assert.equal(calls, firstCalls, "Completed queue reuses caches");
  assert.equal(await job({ plugins: [] }), 1);
  assert.equal(calls, firstCalls, "Disabled batch plugin rejects before any request");
  failAt = calls + 2;
  const recovery = path.join(directory, "recovery");
  assert.equal(await job({ count: 1, output: recovery }), 1);
  await fs.access(path.join(recovery, "001_01-edited.png"));
  const failedCalls = calls;
  failAt = 0;
  assert.equal(await job({ count: 1, output: recovery }), 0);
  assert.equal(calls - failedCalls, 3, "Resume skips finished PNG and completes GIF");
  // Planned destination aliases a later input: reject before overwriting either input.
  const collision = path.join(directory, "collision");
  await fs.mkdir(collision);
  const victim = path.join(collision, "001_01-edited.png");
  await fs.copyFile(path.join(fixtures, "source.png"), victim);
  const beforeReject = calls;
  assert.equal(await job({ sources: [path.join(directory, "inputs", "01.png"), victim],
    output: collision, count: 1 }), 1);
  assert.equal(calls, beforeReject);
  console.log("PASS mixed PNG/GIF source queue; folder import/deduplication; numbered outputs; copy counts; GIF frames; aggregate progress; cache resume; failure recovery; plugin gating; cross-source output protection");
} finally {
  server.closeAllConnections();
  await new Promise(resolve => server.close(resolve));
}
