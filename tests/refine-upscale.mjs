import assert from "node:assert/strict";
import http from "node:http";
import fs from "node:fs/promises";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { deflateSync, inflateSync } from "node:zlib";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const exe = path.join(root, "bin", "MemeStudio-plugins.exe");
const directory = path.join(root, "tmp", "refine-upscale", Date.now().toString(36));
const tool = path.join(directory, "外部 工具");
await fs.mkdir(path.join(tool, "models-cunet"), { recursive: true });
await fs.copyFile(path.join(root, "tmp", "waifu-mock", "mock-waifu.exe"), path.join(tool, "mock-waifu.exe"));
for (const ext of ["bin", "param"]) await fs.writeFile(path.join(tool, "models-cunet", `test.${ext}`), "mock");
function chunk(type, data) {
  const name = Buffer.from(type); let crc = 0xffffffff;
  for (const byte of Buffer.concat([name, data])) {
    crc ^= byte;
    for (let i = 0; i < 8; ++i) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  const length = Buffer.alloc(4), checksum = Buffer.alloc(4);
  length.writeUInt32BE(data.length); checksum.writeUInt32BE((crc ^ 0xffffffff) >>> 0);
  return Buffer.concat([length, name, data, checksum]);
}
function solid(width, height, rgba) {
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width); header.writeUInt32BE(height, 4); header[8] = 8; header[9] = 6;
  const scan = Buffer.alloc(height * (width * 4 + 1));
  for (let y = 0; y < height; ++y)
    for (let x = 0; x < width; ++x) Buffer.from(rgba).copy(scan, y * (width * 4 + 1) + 1 + x * 4);
  return Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]), chunk("IHDR", header),
    chunk("IDAT", deflateSync(scan)), chunk("IEND", Buffer.alloc(0))]);
}
function pixels(data) {
  let width, height, channels; const chunks = [];
  for (let offset = 8; offset < data.length;) {
    const length = data.readUInt32BE(offset), type = data.toString("ascii", offset + 4, offset + 8);
    const value = data.subarray(offset + 8, offset + 8 + length);
    if (type === "IHDR") {
      width = value.readUInt32BE(0); height = value.readUInt32BE(4);
      assert.equal(value[8], 8); assert([2,6].includes(value[9])); channels = value[9] === 6 ? 4 : 3;
    }
    if (type === "IDAT") chunks.push(value);
    offset += 12 + length;
  }
  const scan = inflateSync(Buffer.concat(chunks)), rows = Buffer.alloc(width * height * channels);
  const stride = width * channels;
  for (let y = 0; y < height; ++y) for (let x = 0; x < stride; ++x) {
    const at = y * stride + x, filter = scan[y * (stride + 1)];
    const a = x >= channels ? rows[at - channels] : 0;
    const b = y ? rows[at - stride] : 0, c = y && x >= channels ? rows[at - stride - channels] : 0;
    let predictor = 0;
    if (filter === 1) predictor = a;
    else if (filter === 2) predictor = b;
    else if (filter === 3) predictor = Math.floor((a+b)/2);
    else if (filter === 4) {
      const p = a+b-c, pa = Math.abs(p-a), pb = Math.abs(p-b), pc = Math.abs(p-c);
      predictor = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
    } else assert.equal(filter, 0);
    rows[at] = (scan[y * (stride + 1) + 1 + x] + predictor) & 255;
  }
  return { width, height, at: (x,y) => [...rows.subarray((y*width+x)*channels, (y*width+x)*channels+3)] };
}
const source = path.join(directory, "产物.png"), style = path.join(directory, "新画风.png");
await fs.writeFile(source, solid(8, 8, [40,50,60,255]));
await fs.writeFile(style, solid(8, 8, [80,90,100,255]));
const generated = solid(8, 8, [200,210,220,255]);
const log = path.join(directory, "tool.jsonl");
let calls = 0;
const uploads = [], prompts = [];
const server = http.createServer(async (request, response) => {
  try {
    const chunks = []; for await (const chunk of request) chunks.push(chunk);
    const form = await new Request("http://localhost", { method: "POST",
      headers: { "Content-Type": request.headers["content-type"] }, body: Buffer.concat(chunks) }).formData();
    const files = form.getAll("image[]");
    assert(files.length >= 1);
    uploads.push(await Promise.all(files.map(async file => pixels(Buffer.from(await file.arrayBuffer())))));
    prompts.push(form.get("prompt")); ++calls;
    assert(!form.get("prompt").includes("Replace its main person"));
    if (form.get("stream") === "true") {
      response.writeHead(200, { "Content-Type": "text/event-stream" });
      response.write(`event: image_edit.partial_image\ndata: ${JSON.stringify({
        b64_json: generated.toString("base64"), partial_image_index: 0 })}\n\n`);
      response.end(`event: image_edit.completed\ndata: ${JSON.stringify({ b64_json: generated.toString("base64") })}\n\n`);
    } else {
      response.writeHead(200, { "Content-Type": "application/json" });
      response.end(JSON.stringify({ data: [{ b64_json: generated.toString("base64") }] }));
    }
  } catch (error) { response.writeHead(500); response.end(String(error)); }
});
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
function run(args, environment = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(exe, args, { windowsHide: true, env: { ...process.env,
      OPENAI_API_KEY: "local-refine-test", MEMESTUDIO_WAIFU_TEST_LOG: log,
      TEMP: directory, TMP: directory, ...environment } });
    const timer = setTimeout(() => { child.kill(); reject(new Error("Focused check timeout")); }, 20000);
    child.on("error", error => { clearTimeout(timer); reject(error); });
    child.on("exit", code => { clearTimeout(timer); resolve(code); });
  });
}
async function job(name, extra = {}, environment = {}) {
  const config = path.join(directory, name + ".json");
  await fs.writeFile(config, JSON.stringify({ mode: "refine", source, styles: [style],
    output: path.join(directory, name + ".png"), model: "gpt-image-test",
    base_url: `http://127.0.0.1:${server.address().port}/v1`, instruction: "修正眼睛",
    region: { x: .25, y: .25, width: .5, height: .5 },
    preview_file: path.join(directory, "preview.png"), ...extra }));
  return run(["--job", config], environment);
}
async function toolCalls() {
  return (await fs.readFile(log, "utf8").catch(() => "")).trim().split("\n").filter(Boolean).map(JSON.parse);
}
const options = { executable: path.join(tool, "mock-waifu.exe"), scale: 2, noise: 2,
  gpu: -1, tile: 32, tta: true };
try {
  assert.equal(await job("refine", { stream: true, plugins: ["memestudio.local-redraw"] }), 0);
  assert.equal(calls, 1); assert.equal(uploads[0].length, 2);
  assert.deepEqual(uploads[0][1].at(0,0), [80,90,100]);
  assert.match(prompts[0], /Images 2 through 2 are STYLE/);
  for (const name of ["refine.png", "preview.png"]) {
    const result = pixels(await fs.readFile(path.join(directory, name)));
    for (let y=0;y<8;++y) for (let x=0;x<8;++x)
      assert.deepEqual(result.at(x,y), x>=2 && x<6 && y>=2 && y<6 ? [200,210,220] : [40,50,60]);
  }
  assert.equal(await job("refine"), 0); assert.equal(calls, 1);
  assert.equal(await job("refine", { region: { x: 0, y: 0, width: .5, height: .5 } }), 0);
  assert.equal(calls, 2, "Region must affect cache");
  assert.equal(await job("upscale"), 0);
  const beforeOutput = calls;
  assert.equal(await job("upscale", { plugins: ["memestudio.waifu2x"],
    upscale: { ...options, output: true } }), 0);
  assert.equal(calls, beforeOutput, "Output-only scaling must reuse existing generated frame");
  assert.equal(pixels(await fs.readFile(path.join(directory,"upscale.png"))).width, 16);
  assert.equal((await toolCalls()).length, 1);
  assert.equal(await job("upscale", { upscale: { ...options, output: true, scale: 4 } }), 0);
  assert.equal(calls, beforeOutput);
  assert.equal(pixels(await fs.readFile(path.join(directory,"upscale.png"))).width, 32);
  assert.equal(await job("input", { upscale: { ...options, input: true } }), 0);
  assert.deepEqual(uploads.at(-1).map(image => image.width), [16,16]);
  const recent = (await toolCalls()).slice(-2);
  assert(recent.every(call => call.noise === "2" && call.tile === "32" && call.gpu === "-1" && call.tta));
  const cached = calls, localCached = (await toolCalls()).length;
  assert.equal(await job("input", { upscale: { ...options, input: true } }), 0);
  assert.equal(calls, cached); assert.equal((await toolCalls()).length, localCached);
  const savedOutput = await fs.readFile(path.join(directory,"upscale.png"));
  assert.equal(await job("upscale", { upscale: { ...options, output: true } },
    { MEMESTUDIO_WAIFU_TEST_MODE: "fail" }), 1);
  assert.equal(calls, cached);
  assert.deepEqual(await fs.readFile(path.join(directory,"upscale.png")), savedOutput);
  assert.equal(await job("upscale", { upscale: { ...options, output: true } },
    { MEMESTUDIO_WAIFU_TEST_MODE: "wrong-size" }), 1);
  assert.equal(await job("upscale", { upscale: { ...options, output: true, timeout: 5 } },
    { MEMESTUDIO_WAIFU_TEST_MODE: "timeout" }), 1);
  assert.equal(calls, cached);
  const sourceGif = path.join(root,"tmp","tests","source.gif");
  assert.equal(await job("animated", { source: sourceGif, output: path.join(directory,"animated.gif"),
    upscale: { ...options, output: true } }), 0);
  assert.equal(await run(["--inspect", sourceGif, path.join(directory,"before.json")]), 0);
  assert.equal(await run(["--inspect", path.join(directory,"animated.gif"), path.join(directory,"after.json")]), 0);
  const before = JSON.parse(await fs.readFile(path.join(directory,"before.json"),"utf8"));
  const after = JSON.parse(await fs.readFile(path.join(directory,"after.json"),"utf8"));
  assert.equal(after.width, before.width*2); assert.equal(after.height, before.height*2);
  assert.deepEqual(after.delays,before.delays); assert.equal(after.loop,before.loop);
  const beforeInvalid = calls;
  assert.equal(await job("invalid", { instruction: "" }), 1);
  assert.equal(await job("invalid", { region: { x: -1, y: 0, width: 1, height: 1 } }), 1);
  assert.equal(await job("invalid", { upscale: { ...options, executable: source, input: true } }), 1);
  assert.equal(calls, beforeInvalid);
  console.log("PASS refinement prompt/new styles; exact region and streaming preview; cache isolation; external input/output scaling and parameters; output-only cache reuse; tool failure/dimension/timeout; GIF dimensions/timing; local validation");
} finally {
  server.closeAllConnections(); await new Promise(resolve => server.close(resolve));
}
