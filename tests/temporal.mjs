import assert from "node:assert/strict";
import http from "node:http";
import fs from "node:fs/promises";
import path from "node:path";
import { spawn } from "node:child_process";
import { deflateSync } from "node:zlib";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const exe = path.join(root, "bin", "MemeStudio.exe");
const directory = path.join(root, "tmp", "temporal", Date.now().toString(36));
await fs.mkdir(directory, { recursive: true });

// Independent 1x1 fixtures keep all requests local and make every frame distinct.
const gif = [...Buffer.from("GIF89a"), 1, 0, 1, 0, 0x83, 0, 0];
for (let i = 0; i < 16; ++i) gif.push(i * 16, 0, 0);
gif.push(0x21, 0xff, 11, ...Buffer.from("NETSCAPE2.0"), 3, 1, 0, 0, 0);
for (let i = 0; i < 9; ++i) {
  const codes = 16 | (i << 5) | (17 << 10);
  gif.push(0x21, 0xf9, 4, 4, (i + 1) * 4, 0, 0, 0,
    0x2c, 0, 0, 0, 0, 1, 0, 1, 0, 0, 4, 2, codes & 255, codes >> 8, 0);
}
gif.push(0x3b);
const source = path.join(directory, "source.gif");
await fs.writeFile(source, Buffer.from(gif));
function chunk(type, data) {
  const name = Buffer.from(type);
  let crc = 0xffffffff;
  for (const byte of Buffer.concat([name, data])) {
    crc ^= byte;
    for (let i = 0; i < 8; ++i) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  const size = Buffer.alloc(4), checksum = Buffer.alloc(4);
  size.writeUInt32BE(data.length); checksum.writeUInt32BE((crc ^ 0xffffffff) >>> 0);
  return Buffer.concat([size, name, data, checksum]);
}
function resultPng(index) {
  const header = Buffer.alloc(13);
  header.writeUInt32BE(1, 0); header.writeUInt32BE(1, 4); header[8] = 8; header[9] = 6;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk("IHDR", header), chunk("IDAT", deflateSync(Buffer.from([0, index * 20, 120, 240, 255]))),
    chunk("IEND", Buffer.alloc(0))]);
}

const calls = { enabled: [], disabled: [] };
const completed = new Set();
let failed = false, maxUploads = 0, bothSides = false;
const server = http.createServer(async (request, response) => {
  try {
    const enabled = request.url.startsWith("/enabled/");
    const buffers = [];
    for await (const buffer of request) buffers.push(buffer);
    const form = await new Request("http://localhost", {
      method: "POST", headers: { "Content-Type": request.headers["content-type"] },
      body: Buffer.concat(buffers),
    }).formData();
    const prompt = form.get("prompt");
    const match = prompt.match(/This is frame (\d+) of (\d+)/);
    assert(match);
    const index = Number(match[1]) - 1, total = Number(match[2]);
    calls[enabled ? "enabled" : "disabled"].push(index);
    assert.equal(form.get("model"), "gpt-image-test");
    const files = form.getAll("image[]");
    const roles = [...prompt.matchAll(/Image (\d+) is the already REDRAWN animation frame (\d+) (BEFORE|AFTER)/g)];
    if (enabled) {
      assert.equal(total, 9);
      const before = [...completed].filter(i => i < index).sort((a, b) => b - a).slice(0, 2);
      const after = [...completed].filter(i => i > index).sort((a, b) => a - b).slice(0, 2);
      const expected = [...before, ...after].filter(i => i !== 0).sort((a, b) => a - b);
      assert.deepEqual(roles.map(role => Number(role[2]) - 1), expected);
      bothSides ||= before.length === 2 && after.length === 2;
      assert.equal(files.length, 10 + expected.length + (index ? 1 : 0));
      assert(files.length <= 15);
      maxUploads = Math.max(maxUploads, files.length);
      const cacheRoot = path.join(directory, "enabled.gif.memestudio");
      const cache = path.join(cacheRoot, (await fs.readdir(cacheRoot))[0]);
      const cached = i => fs.readFile(path.join(cache, `frame_${String(i).padStart(5, "0")}.png`));
      for (let i = 0; i < roles.length; ++i) {
        const [role, slot, frame, direction] = roles[i];
        assert.equal(Number(slot), 11 + i, role);
        assert.equal(direction, Number(frame) - 1 < index ? "BEFORE" : "AFTER");
        assert.deepEqual(Buffer.from(await files[Number(slot) - 1].arrayBuffer()), await cached(Number(frame) - 1));
      }
      if (index) {
        assert(prompt.includes("LAST image"));
        assert.deepEqual(Buffer.from(await files.at(-1).arrayBuffer()), await cached(0));
      }
      if (calls.enabled.length === 4 && !failed) {
        failed = true;
        response.writeHead(429, { "Content-Type": "application/json" });
        response.end(JSON.stringify({ error: { message: "simulated interruption" } }));
        return;
      }
      assert(!completed.has(index), "Completed frame generated twice");
      completed.add(index);
    } else {
      assert.equal(total, 3);
      assert.equal(files.length, 10);
      assert.equal(roles.length, 0);
      assert(!prompt.includes("LAST image"));
    }
    response.writeHead(200, { "Content-Type": "application/json" });
    response.end(JSON.stringify({ data: [{ b64_json: resultPng(index).toString("base64") }] }));
  } catch (error) {
    response.writeHead(500, { "Content-Type": "application/json" });
    response.end(JSON.stringify({ error: { message: String(error) } }));
  }
});
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
const invoke = args => new Promise((resolve, reject) => {
  const child = spawn(exe, args, {
    windowsHide: true, env: { ...process.env, OPENAI_API_KEY: "test-temporal-key" },
  });
  const timer = setTimeout(() => { child.kill(); reject(new Error("Temporal check timed out")); }, 30000);
  child.on("error", error => { clearTimeout(timer); reject(error); });
  child.on("exit", code => { clearTimeout(timer); resolve(code); });
});
try {
  async function job(name, enabled) {
    const config = path.join(directory, `${name}.json`);
    const fixture = path.join(root, "tmp", "tests");
    await fs.writeFile(config, JSON.stringify({
      source, character: path.join(fixture, "character.png"),
      styles: Array(8).fill(path.join(fixture, "style.png")),
      output: path.join(directory, `${name}.gif`), consistency: enabled, stride: enabled ? 1 : 4,
      base_url: `http://127.0.0.1:${server.address().port}/${name}/v1`, model: "gpt-image-test",
    }));
    return invoke(["--job", config]);
  }
  assert.equal(await job("enabled", true), 1);
  assert.deepEqual(calls.enabled, [0, 8, 4, 2]);
  assert.equal(await job("enabled", true), 0);
  assert.deepEqual(calls.enabled, [0, 8, 4, 2, 2, 6, 1, 3, 5, 7]);
  assert(bothSides && maxUploads === 15);
  assert.equal(await job("enabled", true), 0);
  assert.equal(calls.enabled.length, 10);
  const report = path.join(directory, "inspect.json");
  assert.equal(await invoke(["--inspect", path.join(directory, "enabled.gif"), report]), 0);
  const info = JSON.parse(await fs.readFile(report, "utf8"));
  assert.deepEqual(info.delays, Array.from({ length: 9 }, (_, i) => (i + 1) * 40));
  assert.equal(info.loop, 0);
  assert.equal(await job("disabled", false), 0);
  assert.deepEqual(calls.disabled, [0, 1, 2]);
  assert.equal(await invoke(["--inspect", path.join(directory, "disabled.gif"), report]), 0);
  assert.deepEqual(JSON.parse(await fs.readFile(report, "utf8")).delays, [400, 1040, 360]);
  console.log("PASS bidirectional redrawn uploads and 15-image cap; interrupted/cache resume; ordered GIF timing; disabled/stride mode");
} finally {
  server.closeAllConnections();
  await new Promise(resolve => server.close(resolve));
}
