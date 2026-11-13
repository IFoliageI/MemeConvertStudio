import assert from "node:assert/strict";
import http from "node:http";
import fs from "node:fs/promises";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const exe = process.argv.find(arg => arg.startsWith("--exe="))?.slice(6) ?? path.join(root, "bin", "MemeStudio-api-test.exe");
const directory = path.join(root, "tmp", "api-probe", Date.now().toString(36));
await fs.mkdir(directory, { recursive: true });
const image = (await fs.readFile(path.join(root, "tmp", "tests", "character.png"))).toString("base64");
const errors = [];
const calls = [];
const server = http.createServer(async (request, response) => {
  try {
    assert.equal(request.headers.authorization, "Bearer probe-test-secret");
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    const bytes = Buffer.concat(chunks);
    let fields;
    if (request.url.includes("/generations")) {
      assert.match(request.headers["content-type"], /application\/json/);
      fields = JSON.parse(bytes.toString("utf8"));
      assert(!Object.keys(fields).some(key => key.startsWith("image")));
    } else {
      const form = await new Request("http://localhost", { method: "POST", headers: {
        "Content-Type": request.headers["content-type"],
      }, body: bytes }).formData();
      assert.equal(form.getAll("image[]").length, 1);
      fields = Object.fromEntries([...form.entries()].filter(([, value]) => typeof value === "string"));
    }
    calls.push({ url: request.url, fields });
    assert.equal(fields.model, "gpt-image-probe");
    const json = (status, body) => {
      response.writeHead(status, { "Content-Type": "application/json" });
      response.end(JSON.stringify(body));
    };
    if (request.url.includes("/unused/")) throw Error("Invalid input reached network");
    if (request.url.includes("/denied/")) return json(401, { error: { message: "bad token probe-test-secret" } });
    if (request.url.includes("/malformed/")) { response.writeHead(200); response.end("not JSON"); return; }
    if (request.url.includes("/corrupt/")) return json(200, { data: [{ b64_json: "aW52YWxpZC1pbWFnZQ==" }] });
    if (request.url.includes("/limited/") && fields.quality) return json(429, { error: { message: "rate limit" } });
    if (request.url.includes("/slow/")) {
      const timer = setTimeout(() => json(200, { data: [{ b64_json: image }] }), 8000);
      response.on("close", () => clearTimeout(timer)); return;
    }
    if (fields.complexity) return json(400, { error: { param: "complexity", message: "Unsupported complexity probe-test-secret" } });
    if (fields.quality && fields.output_format) return json(400, { error: { param: "quality", message: "Incompatible combination" } });
    if (String(fields.stream) === "true" && fields.mock_mode !== "json") {
      response.writeHead(200, { "Content-Type": "text/event-stream" });
      for (const type of ["image_edit.partial_image", "image_edit.completed"]) {
        const record = `event: ${type}\r\ndata: ${JSON.stringify({ type, b64_json: image, partial_image_index: 0 })}\r\n\r\n`;
        response.write(record.slice(0, 19)); response.write(record.slice(19));
      }
      response.end("data: [DONE]\n\n"); return;
    }
    json(200, { data: [{ b64_json: image }] });
  } catch (error) {
    errors.push(String(error));
    if (!response.headersSent) response.writeHead(500, { "Content-Type": "application/json" });
    response.end(JSON.stringify({ error: String(error) }));
  }
});
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
try {
  const code = await new Promise((resolve, reject) => {
    const child = spawn(exe, ["--parameter-probe-test", directory, `http://127.0.0.1:${server.address().port}`], { windowsHide: true });
    const timer = setTimeout(() => { child.kill(); reject(Error("Probe test timed out")); }, 25000);
    child.on("error", error => { clearTimeout(timer); reject(error); });
    child.on("exit", code => { clearTimeout(timer); resolve(code); });
  });
  const result = await fs.readFile(path.join(directory, "parameter-probe-results.txt"), "utf8");
  console.log(result);
  assert.equal(code, 0, result);
  assert.deepEqual(errors, []);
  assert.equal(calls.length, 15);
  console.log("PASS exact request budget; no retries; JSON/multipart shape; evidence and cancellation; no paid requests");
} finally {
  server.closeAllConnections();
  await new Promise(resolve => server.close(resolve));
}
