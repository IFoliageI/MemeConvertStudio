import http from "node:http";
import fs from "node:fs/promises";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import assert from "node:assert/strict";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const exe = path.join(root, "bin", "MemeStudio.exe");
const directory = path.join(root, "tmp", "integration");
const fixtures = path.join(root, "tmp", "tests");
await fs.mkdir(directory, { recursive: true });
const image = await fs.readFile(path.join(fixtures, "character.png"));
// A new provider URL makes checkpoints from prior test runs inapplicable.
const runId = Date.now().toString(36);
const counters = new Map();
const uploads = [];
let base;
let authorizationLeak = false;
let unexpectedRedirects = 0;
const server = http.createServer(async (request, response) => {
  try {
    if (request.url === "/forbidden") {
      ++unexpectedRedirects;
      response.writeHead(500);
      response.end();
      return;
    }
    if (request.url === "/result.png") {
      authorizationLeak ||= !!request.headers.authorization;
      response.writeHead(200, { "Content-Type": "image/png" });
      response.end(image);
      return;
    }
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    const body = Buffer.concat(chunks).toString("latin1");
    assert.equal(request.method, "POST");
    assert.equal(request.headers.authorization, "Bearer test-token");
    assert.match(request.headers["content-type"], /^multipart\/form-data; boundary=/);
    assert.match(body, /name="model"\r\n\r\ngpt-image-/);
    assert.match(body, /name="prompt"\r\n\r\nEdit image 1|name="prompt"\r\n\r\ncancel|name="prompt"\r\n\r\nerror|name="prompt"\r\n\r\nredirect/);
    assert.match(body, /Content-Type: image\/png\r\n\r\n\x89PNG/);
    const route = request.url.replace("/" + runId, "");
    const count = (counters.get(route) ?? 0) + 1;
    counters.set(route, count);
    const parts = [...body.matchAll(/name="(image(?:\[\])?)"; filename="image_(\d+).png"/g)];
    uploads.push({ route, count, fields: parts.map(x => x[1]), images: parts.length });
    response.setHeader("Content-Type", "application/json");
    if (route.startsWith("/slow/")) {
      const timer = setTimeout(() => response.end(JSON.stringify({ data: [{ b64_json: image.toString("base64") }] })), 10000);
      response.on("close", () => clearTimeout(timer));
      return;
    }
    if (route.startsWith("/error/")) {
      response.writeHead(401);
      response.end(JSON.stringify({ error: { message: "invalid token test-token" } }));
      return;
    }
    if (route.startsWith("/redirect/")) {
      response.writeHead(302, { Location: base + "/forbidden" });
      response.end(JSON.stringify({ error: "redirect disallowed" }));
      return;
    }
    if (route.startsWith("/fail/") && count === 2) {
      response.writeHead(429);
      response.end(JSON.stringify({ error: { message: "mock rate limit" } }));
      return;
    }
    if (route.startsWith("/url/")) {
      response.end(JSON.stringify({ data: [{ url: base + "/result.png" }] }));
    } else response.end(JSON.stringify({ data: [{ b64_json: image.toString("base64") }] }));
  } catch (error) {
    response.writeHead(500, { "Content-Type": "application/json" });
    response.end(JSON.stringify({ error: { message: String(error) } }));
  }
});
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
base = `http://127.0.0.1:${server.address().port}`;
const invoke = args => new Promise((resolve, reject) => {
  const process = spawn(exe, args, { windowsHide: true, env: { ...globalThis.process.env, OPENAI_API_KEY: "test-token" } });
  const timer = setTimeout(() => { process.kill(); reject(new Error("Process exceeded 60 seconds")); }, 60000);
  process.on("error", error => { clearTimeout(timer); reject(error); });
  process.on("exit", code => { clearTimeout(timer); resolve(code); });
});
let number = 0;
const job = async (route, outputName, extra = {}) => {
  const config = {
    source: path.join(fixtures, "source.gif"), character: path.join(fixtures, "character.png"),
    styles: [path.join(fixtures, "style.png")], output: path.join(directory, outputName),
    base_url: base + route + "/" + runId + "/v1", model: "gpt-image-1", quality: "auto", size: "auto",
    extra: { input_fidelity: "high" }, max_frames: 0, ...extra,
  };
  const configPath = path.join(directory, `job-${++number}.json`);
  await fs.writeFile(configPath, JSON.stringify(config));
  return invoke(["--job", configPath]);
};
const inspect = async outputName => {
  const output = path.join(directory, outputName);
  const report = output + ".json";
  assert.equal(await invoke(["--inspect", output, report]), 0);
  return JSON.parse(await fs.readFile(report, "utf8"));
};
const results = [];
try {
  assert.equal(await job("/base64", "base64.gif"), 0);
  const info = await inspect("base64.gif");
  assert.deepEqual(info.delays, [180, 140, 240]);
  assert.equal(info.loop, 0);
  assert.equal(info.width, 320);
  assert.equal(info.height, 240);
  const first = uploads.filter(x => x.route.startsWith("/base64/"));
  assert.deepEqual(first.map(x => x.images), [3, 4, 5]);
  assert(first.every(x => x.fields.every(y => y === "image[]")));
  results.push("PASS GIF job: multipart source/identity/style/temporal/anchor, timings and loop");

  assert.equal(await job("/url", "url.png", {
    source: path.join(fixtures, "source.png"), image_field: "image",
  }), 0);
  assert.equal((await inspect("url.png")).animated, false);
  assert(!authorizationLeak);
  assert(uploads.filter(x => x.route.startsWith("/url/")).every(x => x.fields.every(y => y === "image")));
  results.push("PASS Static PNG job: custom image field and URL download without bearer leakage");

  assert.equal(await job("/fail", "resume.gif"), 1);
  assert.equal(counters.get("/fail/v1/images/edits"), 2);
  assert.equal(await job("/fail", "resume.gif"), 0);
  assert.equal(counters.get("/fail/v1/images/edits"), 4);
  assert.equal(await job("/fail", "resume.gif"), 0);
  assert.equal(counters.get("/fail/v1/images/edits"), 4);
  assert.deepEqual((await inspect("resume.gif")).delays, [180, 140, 240]);
  results.push("PASS HTTP 429 failure, frame checkpoint resume, completed-job cache reuse");

  assert.equal(await job("/base64", "base64.gif", { model: "gpt-image-custom" }), 0);
  assert.equal(counters.get("/base64/v1/images/edits"), 6);
  results.push("PASS Changed model invalidates cached frames");

  assert.equal(await job("/stride", "stride.gif", { stride: 2 }), 0);
  assert.deepEqual((await inspect("stride.gif")).delays, [320, 240]);
  assert.equal(counters.get("/stride/v1/images/edits"), 2);
  assert.equal(await job("/budget", "budget.gif", { max_frames: 1 }), 0);
  assert.deepEqual((await inspect("budget.gif")).delays, [560]);
  results.push("PASS Stride and request cap preserve full animation duration");

  assert.equal(await invoke(["--network-test", directory, base]), 0);
  results.push((await fs.readFile(path.join(directory, "network-results.txt"), "utf8")).trim());
  assert.equal(unexpectedRedirects, 0);

  const cache = path.join(directory, "resume.gif.memestudio");
  for (const folder of await fs.readdir(cache)) {
    const manifest = await fs.readFile(path.join(cache, folder, "manifest.json"), "utf8");
    assert(!manifest.includes("test-token"));
  }
  results.push("PASS No API key in checkpoint manifests");
  await fs.writeFile(path.join(directory, "results.txt"), results.join("\n") + "\n");
  console.log(results.join("\n"));
} finally {
  server.closeAllConnections();
  await new Promise(resolve => server.close(resolve));
}
