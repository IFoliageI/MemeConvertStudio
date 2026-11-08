import assert from "node:assert/strict";
import http from "node:http";
import fs from "node:fs/promises";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const toolDirectory = process.argv.find(a => a.startsWith("--tool="))?.slice(7);
assert(toolDirectory, "Specify --tool=<waifu2x directory>");
const engine = path.join(path.resolve(toolDirectory), "waifu2x-ncnn-vulkan.exe");
const app = path.join(root, "bin", "MemeStudio-plugins.exe");
const fixtures = path.join(root, "tmp", "tests");
const directory = path.join(root, "tmp", "waifu-real", Date.now().toString(36));
await fs.mkdir(directory, { recursive: true });
const source = path.join(directory, "中文 产物.png"), style = path.join(directory, "新画风.png");
await fs.copyFile(path.join(fixtures, "source.png"), source);
await fs.copyFile(path.join(fixtures, "style.png"), style);
const png = await fs.readFile(source);
function dimensions(bytes) {
  assert.equal(bytes.toString("ascii", 1, 4), "PNG");
  return [bytes.readUInt32BE(16), bytes.readUInt32BE(20)];
}
const original = dimensions(png), doubled = original.map(n => n * 2);
async function run(program, args, name, environment = {}) {
  const started = Date.now();
  const result = await new Promise((resolve, reject) => {
    const child = spawn(program, args, { windowsHide: true,
      cwd: path.dirname(program), env: { ...process.env, TEMP: directory, TMP: directory, ...environment } });
    let output = "";
    child.stdout.on("data", data => { output = (output + data.toString()).slice(-32768); });
    child.stderr.on("data", data => { output = (output + data.toString()).slice(-32768); });
    const timer = setTimeout(() => { child.kill(); reject(new Error(`${name} timed out`)); }, 90000);
    child.on("error", error => { clearTimeout(timer); reject(error); });
    child.on("exit", code => { clearTimeout(timer); resolve({ code, output, elapsed_ms: Date.now() - started }); });
  });
  await fs.writeFile(path.join(directory, name + ".log"), result.output);
  if (result.code) {
    const error = await fs.readFile(path.join(directory, "MemeStudio-error.txt"), "utf8").catch(() => "");
    throw new Error(`${name} exited ${result.code}: ${result.output}\n${error}`);
  }
  return result;
}
console.log("Running installed Waifu2x with automatic GPU selection...");
const direct = path.join(directory, "direct.png");
const directRun = await run(engine, ["-i", source, "-o", direct, "-s", "2", "-n", "0",
  "-t", "0", "-m", path.join(path.dirname(engine), "models-cunet"), "-f", "png"], "direct");
assert.deepEqual(dimensions(await fs.readFile(direct)), doubled);
console.log("PASS direct real engine:", directRun.output.split(/\r?\n/).filter(Boolean).slice(0, 5).join(" | "));
let calls = 0;
const uploads = [];
const server = http.createServer(async (request, response) => {
  try {
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    const form = await new Request("http://localhost", { method: "POST",
      headers: { "Content-Type": request.headers["content-type"] }, body: Buffer.concat(chunks) }).formData();
    const files = form.getAll("image[]");
    uploads.push(await Promise.all(files.map(async file => dimensions(Buffer.from(await file.arrayBuffer())))));
    ++calls;
    response.writeHead(200, { "Content-Type": "application/json" });
    response.end(JSON.stringify({ data: [{ b64_json: png.toString("base64") }] }));
  } catch (error) { response.writeHead(500); response.end(String(error)); }
});
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
const options = { executable: engine, scale: 2, noise: 0, tile: 0, gpu: -2, timeout: 60 };
async function job(name, upscale = {}) {
  const config = path.join(directory, `${name}.json`);
  await fs.writeFile(config, JSON.stringify({ mode: "refine", source, styles: [style],
    output: path.join(directory, `${name}.png`), instruction: "保持现有构图和表情",
    model: "local-test", base_url: `http://127.0.0.1:${server.address().port}/v1`,
    api_key_env: "MEMESTUDIO_REAL_TEST_KEY", plugins: ["memestudio.local-redraw", "memestudio.waifu2x"],
    ...(Object.keys(upscale).length ? { upscale: { ...options, ...upscale } } : {}) }));
  return run(app, ["--job", config], name, { MEMESTUDIO_REAL_TEST_KEY: "local-only" });
}
try {
  await job("output");
  assert.equal(calls, 1);
  console.log("Testing real engine through output plugin and cached frame...");
  const outputRun = await job("output", { output: true });
  assert.equal(calls, 1, "Output-only upscale should reuse the existing API cache");
  assert.deepEqual(dimensions(await fs.readFile(path.join(directory, "output.png"))), doubled);
  console.log("Testing real engine on source and new style uploads...");
  const inputRun = await job("input", { input: true });
  assert.equal(calls, 2);
  assert.deepEqual(uploads[1], [doubled, doubled]);
  assert.deepEqual(dimensions(await fs.readFile(path.join(directory, "input.png"))), original);
  const beforeResume = calls;
  await job("input", { input: true });
  assert.equal(calls, beforeResume);
  assert(!(await fs.readdir(directory)).some(name => name.startsWith("MemeStudio-upscale-")),
    "Temporary engine files were not cleaned up");
  const report = { passed: true, tool: engine, source: original, upscaled: doubled, api_calls: calls,
    direct_ms: directRun.elapsed_ms, output_ms: outputRun.elapsed_ms, input_ms: inputRun.elapsed_ms,
    real_provider_used: false };
  await fs.writeFile(path.join(directory, "results.json"), JSON.stringify(report, null, 2));
  console.log("PASS installed real engine; Unicode paths; input/style 2x uploads; output 2x; cache reuse; temporary cleanup");
  console.log("RESULT " + path.join(directory, "results.json"));
} finally {
  server.closeAllConnections(); await new Promise(resolve => server.close(resolve));
}
