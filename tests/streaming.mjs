import assert from "node:assert/strict";
import http from "node:http";
import fs from "node:fs/promises";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const exe = process.argv.find(arg => arg.startsWith("--exe="))?.slice(6) ??
  path.join(root, "bin", "MemeStudio.exe");
const directory = path.join(root, "tmp", "streaming", Date.now().toString(36));
await fs.mkdir(directory, { recursive: true });
const fixture = path.join(root, "tmp", "tests");
const partial = await fs.readFile(path.join(fixture, "source.png"));
const final = await fs.readFile(path.join(fixture, "character.png"));
let requests = 0, livePreview = false;
const observed = [];
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const exists = file => fs.access(file).then(() => true, () => false);
function record(type, value) {
  // Multi-line JSON, BOM, comments and CRLF exercise SSE framing, not TCP boundaries.
  return Buffer.from(`event: ${type}\r\n` +
    JSON.stringify(value, null, 1).split("\n").map(line => "data: " + line).join("\r\n") + "\r\n\r\n");
}
const server = http.createServer(async (request, response) => {
  try {
    ++requests;
    const buffers = [];
    for await (const buffer of request) buffers.push(buffer);
    const form = await new Request("http://localhost", {
      method: "POST", headers: { "Content-Type": request.headers["content-type"] },
      body: Buffer.concat(buffers),
    }).formData();
    assert.equal(form.get("stream"), "true");
    assert.equal(form.get("partial_images"), "2");
    assert.equal(form.get("quality"), "low");
    assert.equal(form.get("output_format"), "jpeg");
    assert.equal(form.get("output_compression"), "75");
    observed.push(form.get("complexity"));
    if (request.url.startsWith("/json/")) {
      response.writeHead(200, { "Content-Type": "application/json" });
      response.end(JSON.stringify({ data: [{ b64_json: final.toString("base64") }] }));
      return;
    }
    response.writeHead(200, { "Content-Type": "text/event-stream; charset=utf-8" });
    const preview = Buffer.concat([Buffer.from("\uFEFF: 心跳\r\n\r\n"),
      record("image_edit.partial_image", { b64_json: partial.toString("base64"), partial_image_index: 0 })]);
    for (let i = 0; i < preview.length; i += 73) {
      response.write(preview.subarray(i, i + 73));
      if (i < 400) await delay(2);
    }
    if (request.url.startsWith("/sse/") && requests === 1) {
      const progress = path.join(directory, "sse-progress.json");
      const deadline = Date.now() + 5000;
      while (Date.now() < deadline) {
        try {
          const data = JSON.parse(await fs.readFile(progress, "utf8"));
          if (data.partial_image_index === 0 && await exists(path.join(directory, "sse-preview.png"))) {
            livePreview = true; break;
          }
        } catch {}
        await delay(20);
      }
      assert(livePreview, "Preview was not delivered until the final image");
      assert(!await exists(path.join(directory, "sse.png")), "Partial image saved as completed output");
    }
    if (request.url.startsWith("/truncated/")) {
      response.end("data: [DONE]\r\n\r\n"); return;
    }
    if (request.url.startsWith("/error/")) {
      response.end(record("error", { type: "error", message: "denied test-stream-key" })); return;
    }
    const completed = record("image_edit.completed", {
      type: "image_edit.completed", b64_json: final.toString("base64"),
    });
    // Split specifically inside a CRLF pair.
    const split = completed.indexOf("\r\n") + 1;
    response.write(completed.subarray(0, split));
    await delay(2);
    response.write(completed.subarray(split));
    response.end("data: [DONE]\n\n");
  } catch (error) {
    if (!response.headersSent) response.writeHead(500, { "Content-Type": "application/json" });
    response.end(JSON.stringify({ error: String(error) }));
  }
});
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
async function job(name, extra = {}) {
  const config = path.join(directory, `${name}.json`);
  await fs.writeFile(config, JSON.stringify({
    source: path.join(fixture, "source.png"), character: path.join(fixture, "character.png"),
    output: path.join(directory, `${name}.png`), model: "gpt-image-test",
    base_url: `http://127.0.0.1:${server.address().port}/${name}/v1`, stream: true,
    partial_images: 2, quality: "low", complexity: "high", output_format: "jpeg", output_compression: 75,
    progress_file: path.join(directory, `${name}-progress.json`),
    preview_file: path.join(directory, `${name}-preview.png`), ...extra,
  }));
  return new Promise((resolve, reject) => {
    const child = spawn(exe, ["--job", config], {
      windowsHide: true, env: { ...process.env, OPENAI_API_KEY: "test-stream-key" },
    });
    const timer = setTimeout(() => { child.kill(); reject(new Error("Streaming check timed out")); }, 20000);
    child.on("error", error => { clearTimeout(timer); reject(error); });
    child.on("exit", code => { clearTimeout(timer); resolve(code); });
  });
}
try {
  assert.equal(await job("sse"), 0);
  assert(livePreview);
  assert.equal(await job("sse"), 0);
  assert.equal(requests, 1, "Same settings should reuse the completed frame");
  assert.equal(await job("sse", { complexity: "medium" }), 0);
  assert.equal(requests, 2, "Complexity must affect the cache signature");
  assert.equal(await job("json"), 0);
  for (const name of ["truncated", "error"]) {
    assert.equal(await job(name), 1);
    assert(!await exists(path.join(directory, `${name}.png`)));
    const cacheRoot = path.join(directory, `${name}.png.memestudio`);
    for (const folder of await fs.readdir(cacheRoot))
      assert(!await exists(path.join(cacheRoot, folder, "frame_00000.png")));
  }
  const error = await fs.readFile(path.join(directory, "MemeStudio-error.txt"), "utf8");
  assert(!error.includes("test-stream-key") && error.includes("[REDACTED]"));
  const beforeInvalid = requests;
  assert.equal(await job("invalid", { output_format: "png", output_compression: 75 }), 1);
  assert.equal(requests, beforeInvalid, "Invalid compression must be rejected locally");
  assert.deepEqual(observed, ["high", "medium", "high", "high", "high"]);
  console.log("PASS live fragmented SSE preview; final-only cache and JSON fallback; independent parameters/cache; truncated/error streams and key redaction");
} finally {
  server.closeAllConnections();
  await new Promise(resolve => server.close(resolve));
}
