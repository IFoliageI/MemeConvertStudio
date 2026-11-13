import assert from "node:assert/strict";
import http from "node:http";
import fs from "node:fs/promises";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const exe = path.join(root, "bin", "MemeStudio-routing.exe");
const directory = path.join(root, "tmp", "routing", Date.now().toString(36));
const fixtures = path.join(root, "tmp", "tests");
await fs.mkdir(directory, { recursive: true });
const image = await fs.readFile(path.join(fixtures, "character.png"));
let observed = [], failure = "";
const closed = http.createServer();
await new Promise(resolve => closed.listen(0, "127.0.0.1", resolve));
const deadBase = `http://127.0.0.1:${closed.address().port}/v1`;
await new Promise(resolve => closed.close(resolve));
const server = http.createServer(async (request, response) => {
  try {
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    if (request.url === "/download") {
      observed.push({ download: true, key: request.headers.authorization });
      response.writeHead(503); response.end("download failure"); return;
    }
    const body = Buffer.concat(chunks);
    const parameters = request.headers["content-type"] === "application/json" ?
      JSON.parse(body) : Object.fromEntries(await new Request("http://localhost", {
        method: "POST", headers: { "Content-Type": request.headers["content-type"] }, body,
      }).formData());
    observed.push({ url: request.url, key: request.headers.authorization, model: parameters.model,
      prompt: parameters.prompt });
    assert.equal(parameters.model, request.url.startsWith("/b/") ? "model-b" : "model-a");
    if (request.url.includes("/images/edits")) assert(parameters["image[]"]);
    if (failure === "all" || (failure && request.url.startsWith("/a/"))) {
      if (failure === "drop") { request.socket.destroy(); return; }
      if (failure === "truncated") {
        response.writeHead(200, { "Content-Type": "text/event-stream" });
        response.end(`event: image_generation.partial_image\ndata: ${JSON.stringify({
          b64_json: image.toString("base64"), partial_image_index: 0,
        })}\n\n`); return;
      }
      if (failure === "download" || failure === "download-connect") {
        response.writeHead(200, { "Content-Type": "application/json" });
        response.end(JSON.stringify({ data: [{ url: failure === "download-connect" ? deadBase :
          `http://127.0.0.1:${server.address().port}/download` }] })); return;
      }
      response.writeHead(failure === "all" ? 429 : Number(failure), { "Content-Type": "application/json" });
      response.end(JSON.stringify({ error: { message: "echo route-key-a and route-key-b" } })); return;
    }
    if (parameters.stream === true || parameters.stream === "true") {
      response.writeHead(200, { "Content-Type": "text/event-stream" });
      response.end(`event: image_generation.completed\ndata: ${JSON.stringify({
        b64_json: image.toString("base64"),
      })}\n\n`); return;
    }
    response.writeHead(200, { "Content-Type": "application/json" });
    response.end(JSON.stringify({ data: [{ b64_json: image.toString("base64") }] }));
  } catch (error) {
    response.writeHead(500); response.end(String(error));
  }
});
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
const base = `http://127.0.0.1:${server.address().port}`;
function run(args) {
  return new Promise((resolve, reject) => {
    const child = spawn(exe, args, { windowsHide: true,
      env: { ...process.env, ROUTE_A: "route-key-a", ROUTE_B: "route-key-b", OPENAI_API_KEY: "" } });
    const timer = setTimeout(() => { child.kill(); reject(new Error("Routing test timeout")); }, 25000);
    child.on("error", error => { clearTimeout(timer); reject(error); });
    child.on("exit", code => { clearTimeout(timer); resolve(code); });
  });
}
const channels = [
  { name: "A", base_url: `${base}/a`, api_key_env: "ROUTE_A", weight: 3, model: "model-a" },
  { name: "B", base_url: `${base}/b`, api_key_env: "ROUTE_B", weight: 1, model: "model-b" },
];
async function job(name, routing = {}, extra = {}) {
  observed = [];
  const config = { mode: "generate", model: "fallback", instruction: "Draw a waving character",
    output: path.join(directory, `${name}.png`), resume: false,
    plugins: ["memestudio.prompt-draw", "memestudio.api-load-balancing"],
    api_routing: { enabled: true, channels, ...routing },
    routing_report: path.join(directory, `${name}-report.json`), ...extra };
  const filename = path.join(directory, `${name}.json`);
  await fs.writeFile(filename, JSON.stringify(config));
  return { code: await run(["--job", filename]), config };
}
try {
  assert.equal(await run(["--routing-test", directory, `${base}/v1`]), 0);
  let result = await job("weighted", {}, { count: 8 });
  assert.equal(result.code, 0);
  assert.equal(observed.filter(item => item.key === "Bearer route-key-a").length, 6);
  assert.equal(observed.filter(item => item.key === "Bearer route-key-b").length, 2);
  result = await job("cost", { spending: true, channels: [
    { ...channels[0], weight: 1, unit_cost: 1 }, { ...channels[1], unit_cost: 3 },
  ] }, { count: 8 });
  assert.equal(result.code, 0);
  const stats = JSON.parse(await fs.readFile(result.config.routing_report, "utf8"));
  assert.equal(stats[0].estimated_cost, stats[1].estimated_cost);
  for (const code of ["401", "403", "429"]) {
    failure = code;
    assert.equal((await job(`fallback-${code}`, {
      channels: [channels[0], { ...channels[1], priority: 1 }],
    }, { stream: true })).code, 0);
    assert.equal(observed.length, 2);
    assert(observed[0].url.startsWith("/a/") && observed[1].url.startsWith("/b/"));
  }
  failure = "503";
  assert.equal((await job("service-opt-in", { service_failover: true })).code, 0);
  assert.equal(observed.length, 2);
  failure = "";
  assert.equal((await job("connection-fallback", { channels: [
    { ...channels[0], base_url: deadBase }, channels[1],
  ] })).code, 0);
  assert.equal(observed.length, 1);
  assert.equal(observed[0].key, "Bearer route-key-b");
  for (const code of ["400", "503", "drop", "truncated", "download", "download-connect"]) {
    failure = code;
    assert.equal((await job(`stop-${code}`, {}, { stream: true })).code, 1);
    assert.equal(observed.filter(item => !item.download).length, 1);
    const message = await fs.readFile(path.join(directory, "MemeStudio-error.txt"), "utf8");
    assert(!message.includes("route-key-a") && !message.includes("route-key-b"));
    if (code === "download") assert.equal(observed[1].key, undefined);
  }
  failure = "all";
  assert.equal((await job("exhausted")).code, 1); assert.equal(observed.length, 2);
  failure = "";
  result = await job("edit", {}, { mode: "replace", source: path.join(fixtures, "source.png"),
    character: path.join(fixtures, "character.png") });
  assert.equal(result.code, 0); assert.equal(observed.length, 1);
  result = await job("cached", {}, { resume: true });
  assert.equal(result.code, 0); assert.equal(observed.length, 1);
  assert.equal(await run(["--job", path.join(directory, "cached.json")]), 0);
  assert.equal(observed.length, 1, "Cached output made a new request");
  for (const entry of await fs.readdir(directory, { recursive: true })) {
    if (!entry.endsWith("manifest.json") && !entry.endsWith("-report.json")) continue;
    const data = await fs.readFile(path.join(directory, entry), "utf8");
    assert(!data.includes("route-key-a") && !data.includes("route-key-b"));
  }
  result = await job("disabled", {}, { plugins: ["memestudio.prompt-draw"] });
  assert.equal(result.code, 1); assert.equal(observed.length, 0);
  console.log(`Routing checks passed: weights, estimated cost, failover, unknown outcome stop, edit/generate, SSE, caches, credentials. ${directory}`);
} finally {
  server.closeAllConnections(); await new Promise(resolve => server.close(resolve));
}
