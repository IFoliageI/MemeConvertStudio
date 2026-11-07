import http from "node:http";
import fs from "node:fs/promises";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import assert from "node:assert/strict";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const directory = path.join(root, "tmp", "models");
await fs.mkdir(directory, { recursive: true });
let requests = 0;
const server = http.createServer((request, response) => {
  ++requests;
  assert.equal(request.method, "GET");
  assert.equal(request.headers.authorization, "Bearer test-model-key");
  response.setHeader("Content-Type", "application/json");
  if (request.url === "/error/models") {
    response.writeHead(401);
    response.end(JSON.stringify({ error: { message: "invalid test-model-key" } }));
  } else if (request.url === "/empty/models") {
    response.end(JSON.stringify({ data: [] }));
  } else {
    assert(["/v1/models", "/v1/models?tenant=demo"].includes(request.url));
    response.end(JSON.stringify({
      data: [{ id: "gpt-image-1" }, { id: "custom-image" }, { id: "gpt-image-1" }, {}, { id: "" }],
    }));
  }
});
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
const base = `http://127.0.0.1:${server.address().port}`;
async function run(url, expectedCode) {
  const config = path.join(directory, "provider.json");
  const output = path.join(directory, "models.json");
  await fs.writeFile(config, JSON.stringify({ base_url: url }));
  const code = await new Promise((resolve, reject) => {
    const child = spawn(path.join(root, "bin", "MemeStudio.exe"), ["--list-models", config, output], {
      windowsHide: true, env: { ...process.env, OPENAI_API_KEY: "test-model-key" },
    });
    const timeout = setTimeout(() => { child.kill(); reject(new Error("Model fetch timed out")); }, 15000);
    child.on("error", error => { clearTimeout(timeout); reject(error); });
    child.on("exit", code => { clearTimeout(timeout); resolve(code); });
  });
  assert.equal(code, expectedCode);
  if (code === 0)
    assert.deepEqual(JSON.parse(await fs.readFile(output, "utf8")), ["custom-image", "gpt-image-1"]);
}
try {
  await run(base + "/v1/", 0);
  await run(base + "/v1/images/edits?tenant=demo", 0);
  await run(base + "/empty", 1);
  await run(base + "/error", 1);
  const error = await fs.readFile(path.join(directory, "MemeStudio-error.txt"), "utf8");
  assert(error.includes("[REDACTED]") && !error.includes("test-model-key"));
  assert.equal(requests, 4);
  console.log("PASS 4 focused model-list checks: base/full URL, deduplication, empty list, redacted errors");
} finally {
  server.closeAllConnections();
  await new Promise(resolve => server.close(resolve));
}
