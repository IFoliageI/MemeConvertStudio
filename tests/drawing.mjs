import assert from "node:assert/strict";
import http from "node:http";
import fs from "node:fs/promises";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const exe = process.argv.find(arg => arg.startsWith("--exe="))?.slice(6) ??
  path.join(root, "bin", "MemeStudio-drawing.exe");
const fixtures = path.join(root, "tmp", "tests");
const directory = path.join(root, "tmp", "drawing", Date.now().toString(36));
await fs.mkdir(directory, { recursive: true });
const final = await fs.readFile(path.join(fixtures, "character.png"));
const style = await fs.readFile(path.join(fixtures, "style.png"));
const partial = await fs.readFile(path.join(fixtures, "source.png"));
let calls = 0, failAt = 0, livePreview = false;
const observations = [];
const exists = file => fs.access(file).then(() => true, () => false);
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const event = (type, data) => `event: ${type}\ndata: ${JSON.stringify({ type, ...data })}\n\n`;
const server = http.createServer(async (request, response) => {
  try {
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    const body = Buffer.concat(chunks);
    const json = request.headers["content-type"] === "application/json";
    let parameters, images = [];
    if (json) {
      assert(request.url.includes("/images/generations"));
      parameters = JSON.parse(body);
      assert(!("image" in parameters) && !("image[]" in parameters));
      assert.equal(parameters.output_compression, 75);
      assert.equal(parameters.extra_switch, true);
      assert.deepEqual(parameters.nested_option, { value: 3 });
    } else {
      assert(request.url.includes("/images/edits"));
      const form = await new Request("http://localhost", { method: "POST",
        headers: { "Content-Type": request.headers["content-type"] }, body }).formData();
      parameters = Object.fromEntries(form);
      images = form.getAll("image[]");
      if (!request.url.startsWith("/legacy/")) {
        assert.equal(images.length, 1, "Only style reference is uploaded");
        assert.deepEqual(Buffer.from(await images[0].arrayBuffer()), style);
        assert.match(parameters.prompt, /STYLE references only/);
        assert(!parameters.prompt.includes("Replace its main person"));
      } else assert.equal(images.length, 2, "Existing character replacement still uploads source and identity");
    }
    assert.equal(parameters.model, "gpt-image-test");
    assert.equal(parameters.quality, "low");
    assert.equal(parameters.complexity, "high");
    assert.equal(parameters.output_format, "jpeg");
    observations.push({ json, url: request.url }); ++calls;
    if (calls === failAt) {
      response.writeHead(429); response.end('{"error":{"message":"test limit"}}'); return;
    }
    if (parameters.stream === true || parameters.stream === "true") {
      response.writeHead(200, { "Content-Type": "text/event-stream" });
      response.write(event("image_generation.partial_image", {
        b64_json: partial.toString("base64"), partial_image_index: 0,
      }));
      if (request.url.startsWith("/live/")) {
        for (let attempt = 0; attempt < 100; ++attempt) {
          try {
            const progress = JSON.parse(await fs.readFile(path.join(directory, "progress.json"), "utf8"));
            if (progress.partial_image_index === 0 && await exists(path.join(directory, "preview.png"))) {
              livePreview = true; break;
            }
          } catch {}
          await delay(20);
        }
        assert(livePreview, "Prompt-only partial preview is received before completion");
        assert(!await exists(path.join(directory, "live.png")), "Partial image is not a final output");
      }
      if (!request.url.startsWith("/truncated/"))
        response.write(event("image_generation.completed", { b64_json: final.toString("base64") }));
      response.end("data: [DONE]\n\n");
    } else {
      response.writeHead(200, { "Content-Type": "application/json" });
      response.end(JSON.stringify({ data: [{ b64_json: final.toString("base64") }] }));
    }
  } catch (error) {
    if (!response.headersSent) response.writeHead(500);
    response.end(JSON.stringify({ error: String(error) }));
  }
});
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
function run(args) {
  return new Promise((resolve, reject) => {
    const child = spawn(exe, args, { windowsHide: true,
      env: { ...process.env, OPENAI_API_KEY: "drawing-test-key" } });
    const timer = setTimeout(() => { child.kill(); reject(new Error("Drawing check timed out")); }, 20000);
    child.on("error", error => { clearTimeout(timer); reject(error); });
    child.on("exit", code => { clearTimeout(timer); resolve(code); });
  });
}
const base = `http://127.0.0.1:${server.address().port}`;
async function job(name, extra = {}) {
  const config = path.join(directory, "job.json");
  await fs.writeFile(config, JSON.stringify({
    mode: "generate", instruction: "Draw a happy waving character.",
    plugins: ["memestudio.prompt-draw"], output: path.join(directory, `${name}.png`),
    source: path.join(directory, "intentionally-missing-source.gif"),
    character: path.join(directory, "intentionally-missing-character.png"),
    model: " gpt-image-test ", quality: "low", complexity: "high",
    output_format: "jpeg", output_compression: 75,
    extra: { extra_switch: true, nested_option: { value: 3 } },
    base_url: `${base}/${name}/v1`,
    preview_file: path.join(directory, "preview.png"), progress_file: path.join(directory, "progress.json"),
    ...extra,
  }));
  return run(["--job", config]);
}
try {
  assert.equal(await job("plain", { count: 2 }), 0);
  assert.equal(calls, 2);
  for (const suffix of ["001", "002"]) {
    const info = path.join(directory, `inspect-${suffix}.json`);
    assert.equal(await run(["--inspect", path.join(directory, `plain_${suffix}.png`), info]), 0);
    const image = JSON.parse(await fs.readFile(info, "utf8"));
    assert.equal(image.width, 320); assert.equal(image.height, 240);
  }
  assert.equal(await job("plain", { count: 2, stream: true }), 0);
  assert.equal(calls, 2, "Streaming setting preserves completed-image cache");
  assert.equal(await job("live", { stream: true }), 0);
  assert(livePreview);
  assert.equal(await job("styled", { styles: [path.join(fixtures, "style.png")], stream: true,
    base_url: `${base}/styled/v1/images/generations?tenant=demo` }), 0);
  assert(observations.at(-1).url.endsWith("/images/edits?tenant=demo"));
  assert.equal(await job("endpoint", { base_url: `${base}/endpoint/v1/images/edits?tenant=demo` }), 0);
  assert(observations.at(-1).url.endsWith("/images/generations?tenant=demo"));
  assert.equal(await job("truncated", { stream: true }), 1);
  assert(!await exists(path.join(directory, "truncated.png")));
  const before = calls;
  assert.equal(await job("empty", { instruction: " \r\n " }), 1);
  assert.equal(await job("disabled", { plugins: [] }), 1);
  assert.equal(await job("invalid-format", { output: path.join(directory, "invalid.gif") }), 1);
  assert.equal(await job("collision", { styles: [path.join(fixtures, "style.png")],
    output: path.join(fixtures, "style.png") }), 1);
  assert.equal(calls, before, "Invalid jobs reject before API requests");
  failAt = calls + 2;
  assert.equal(await job("recovery", { count: 2 }), 1);
  await fs.access(path.join(directory, "recovery_001.png"));
  const failed = calls; failAt = 0;
  assert.equal(await job("recovery", { count: 2 }), 0);
  assert.equal(calls - failed, 1, "Recovery reuses completed copy");
  assert.equal(await job("legacy", { mode: "replace", plugins: [],
    source: path.join(fixtures, "source.png"), character: path.join(fixtures, "character.png") }), 0);
  console.log("PASS prompt-only JSON; style-only multipart and SSE; endpoint normalization; live partial preview; native dimensions; copies/cache/recovery; local validation; existing edit path");
} finally {
  server.closeAllConnections();
  await new Promise(resolve => server.close(resolve));
}
