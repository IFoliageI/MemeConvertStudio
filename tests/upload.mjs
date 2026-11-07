import http from "node:http";
import fs from "node:fs/promises";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import assert from "node:assert/strict";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const directory = path.join(root, "tmp", "upload");
await fs.mkdir(directory, { recursive: true });
const fixtures = path.join(root, "tmp", "tests");
const image = await fs.readFile(path.join(fixtures, "character.png"));
const models = [];
const broken = process.argv.includes("--expect-broken");
const executable = process.argv.find(arg => arg.startsWith("--exe="))?.slice(6) ??
  path.join(root, "bin", "MemeStudio.exe");
const server = http.createServer(async (request, response) => {
  try {
    assert.equal(request.method, "POST");
    const type = request.headers["content-type"];
    const boundary = type.match(/boundary=(.*)$/)?.[1];
    // Reject non-standard boundary characters instead of relying on Node's permissive parser.
    if (!boundary || !/^[A-Za-z0-9-]{1,70}$/.test(boundary)) {
      response.writeHead(400, { "Content-Type": "application/json" });
      response.end(JSON.stringify({ error: { type: "new_api_error",
        message: "Model name not specified, model name cannot be empty" } }));
      request.resume();
      return;
    }
    const buffers = [];
    for await (const buffer of request) buffers.push(buffer);
    const form = await new Request("http://localhost", {
      method: "POST", headers: { "Content-Type": type }, body: Buffer.concat(buffers),
    }).formData();
    models.push(form.get("model"));
    assert.equal(form.getAll("model").length, 1);
    assert.equal(form.getAll("image[]").length, 2);
    response.writeHead(200, { "Content-Type": "application/json" });
    response.end(JSON.stringify({ data: [{ b64_json: image.toString("base64") }] }));
  } catch (error) {
    response.writeHead(500, { "Content-Type": "application/json" });
    response.end(JSON.stringify({ error: String(error) }));
  }
});
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
async function run(model, index, expected) {
  const config = path.join(directory, `job-${index}.json`);
  await fs.writeFile(config, JSON.stringify({
    source: path.join(fixtures, "source.png"), character: path.join(fixtures, "character.png"),
    output: path.join(directory, `result-${index}.png`), model,
    base_url: `http://127.0.0.1:${server.address().port}/v1`, resume: false,
  }));
  const code = await new Promise((resolve, reject) => {
    const child = spawn(executable, ["--job", config], {
      windowsHide: true, env: { ...process.env, OPENAI_API_KEY: "test-upload-key" },
    });
    const timer = setTimeout(() => { child.kill(); reject(new Error("Upload check timed out")); }, 15000);
    child.on("error", error => { clearTimeout(timer); reject(error); });
    child.on("exit", code => { clearTimeout(timer); resolve(code); });
  });
  assert.equal(code, expected);
}
try {
  await run("gpt-image-custom", 1, broken ? 1 : 0);
  if (broken) {
    const error = await fs.readFile(path.join(directory, "MemeStudio-error.txt"), "utf8");
    assert(error.includes("Model name not specified"));
    console.log("REPRODUCED: invalid boundary causes the model-empty HTTP 400");
  } else {
    await run("  gpt-image-custom  ", 2, 0);
    await run(" \t\r\n ", 3, 1);
    assert.deepEqual(models, ["gpt-image-custom", "gpt-image-custom"]);
    console.log("PASS standard multipart parsing, exact model value, whitespace validation (3 checks)");
  }
} finally {
  server.closeAllConnections();
  await new Promise(resolve => server.close(resolve));
}
