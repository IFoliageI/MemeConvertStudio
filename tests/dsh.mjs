import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import fs from 'node:fs/promises';
import http from 'node:http';
import path from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { pathToFileURL } from 'node:url';

const exe = path.resolve(process.argv.find(arg => arg.startsWith('--exe='))?.slice(6) ?? 'bin/dsh-policy/MemeStudio.exe');
const plugin = path.join(path.dirname(exe), 'plugins', 'deepseek-harness');
const parent = path.resolve('tmp', 'dsh');
await fs.mkdir(parent, { recursive: true });
const directory = await fs.mkdtemp(path.join(parent, 'policy-'));
const image = await fs.readFile(path.resolve('tmp/tests/character.png'));
const source = path.join(directory, '素材.png');
const character = path.join(directory, '人物.png');
const animation = path.resolve('tmp/tests/source.gif');
await Promise.all([fs.writeFile(source, image), fs.writeFile(character, image)]);

const manifest = JSON.parse(await fs.readFile(path.join(plugin, 'package.json')));
assert.equal(manifest.dsh.bundle.patch, './cordis.patch.yml');
assert.match(await fs.readFile(path.join(plugin, manifest.dsh.bundle.patch), 'utf8'), /@memestudio\/deepseek-harness/);
const { apply, callStudio } = await import(pathToFileURL(path.join(plugin, manifest.exports['.'])));
const tools = new Map();
apply({ effect: run => run(), tools: { register: tool => tools.set(tool.name, tool) } });
assert.equal(tools.size, 8);
const call = (tool, args = {}, options = {}) => tools.get(`memestudio_${tool}`).execute(args, options);

let sent = 0;
const received = [];
let slowStarted;
const slow = new Promise(resolve => { slowStarted = resolve; });
const server = http.createServer(async (request, response) => {
  try {
    const key = request.headers.authorization;
    assert(['Bearer fixture-dsh-key', 'Bearer fixture-route-first', 'Bearer fixture-route-second'].includes(key));
    if (request.url === '/v1/models') return response.end(JSON.stringify({ data: [{ id: 'fixture-image' }] }));
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    let model, prompt;
    if (request.url.endsWith('/generations')) ({ model, prompt } = JSON.parse(Buffer.concat(chunks)));
    else {
      const form = await new Request('http://localhost', { method: 'POST',
        headers: { 'Content-Type': request.headers['content-type'] }, body: Buffer.concat(chunks) }).formData();
      model = form.get('model'); prompt = form.get('prompt');
      assert(form.getAll('image[]').length > 0);
    }
    assert(['fixture-image', 'fixture-alternate'].includes(model));
    ++sent;
    received.push({ model, prompt, key });
    if (key === 'Bearer fixture-route-first' && prompt.includes('route-fixture'))
      return response.writeHead(429).end('{"error":{"message":"Fixture throttled"}}');
    if (prompt.includes('hold-fixture')) return slowStarted();
    response.setHeader('Content-Type', 'application/json');
    response.end(JSON.stringify({ data: [{ b64_json: image.toString('base64') }] }));
  } catch (error) {
    response.writeHead(500).end(String(error));
  }
});
server.listen(0, '127.0.0.1');
await once(server, 'listening');
let child, exited;
async function stop() {
  if (!child) return;
  if (child.exitCode === null && child.signalCode === null) child.kill();
  await exited;
  child = undefined;
}
async function start(policy, routing = false) {
  await stop();
  await fs.rm(path.join(directory, 'ready.json'), { force: true });
  await fs.writeFile(path.join(directory, 'fixture.json'), JSON.stringify({ policy, routing }));
  child = spawn(exe, ['--dsh-test', `http://127.0.0.1:${server.address().port}/v1`, directory], { windowsHide: true });
  exited = once(child, 'exit');
  for (let i = 0; i < 100; i++) {
    if (await fs.access(path.join(directory, 'ready.json')).then(() => true, () => false)) return;
    if (child.exitCode !== null) throw new Error(`Harness fixture exited: ${child.exitCode}`);
    await delay(50);
  }
  throw new Error('Harness fixture was not ready in time');
}
try {
  const common = { output_parent: directory, confirmed: true, instruction: 'test fixture' };
  const replace = { ...common, source, character };
  await start({});
  assert.equal((await call('status')).model, 'fixture-image');
  assert.equal((await call('inspect', { path: source })).frames, 1);
  await assert.rejects(call('replace', replace), /permission denied: generate/);
  await assert.rejects(call('models'), /permission denied: local_api/);
  assert.equal(sent, 0);
  await start({ generate: true, local_api: true });
  await assert.rejects(call('draw', common), /permission denied: memestudio.prompt-draw/);
  await assert.rejects(call('replace', { ...replace, model: 'fixture-alternate' }), /permission denied: switch_model/);
  await assert.rejects(call('replace', { ...replace, plugin_settings: { waifu2x: { input: false } } }), /permission denied: plugin_settings/);
  assert.equal(sent, 0);

  const allowed = { generate: true, batch: true, draw: true, refine: true, probe: true, local_api: true,
    plugin_settings: true, upscale: true, switch_model: true, models: ['fixture-alternate'], per_task: 4, per_day: 14 };
  await start(allowed);
  assert.deepEqual((await call('models')).models, ['fixture-image']);
  await assert.rejects(call('draw', { ...common, confirmed: false }), /Confirm/);
  await assert.rejects(call('draw', { ...common, model: 'unknown' }), /not in the allowlist/);
  await assert.rejects(callStudio('draw', { ...common, plugin_settings: { waifu2x: { executable: source } } }), /Unsupported Waifu2x setting/);
  await assert.rejects(call('draw', { ...common, count: 5 }), /per-task/);
  await assert.rejects(call('batch', { ...common, sources: [source, character], character, count: 3 }), /per-task/);
  await assert.rejects(call('probe', { run: true, confirmed: true, text_only: true, cases: [1, 2, 3, 4] }), /per-task/);
  assert.equal(sent, 0);
  for (const [tool, args] of [['replace', { source, character }], ['draw', {}],
    ['refine', { source, region: { x: 0.1, y: 0.1, width: 0.5, height: 0.5 } }],
    ['batch', { sources: [source, character], character }]]) {
    const result = await call(tool, { ...common, ...args });
    assert.equal(result.outputs.length, tool === 'batch' ? 2 : 1);
    for (const output of result.outputs) {
      assert.equal(path.dirname(output), result.output_directory);
      assert((await fs.stat(output)).size > 0);
    }
  }
  assert.equal(sent, 5);
  assert(received.every(({ prompt }) => !prompt.includes('distinctive facial features')));
  assert.equal((await call('probe')).sent_requests, 0);
  assert.equal((await call('probe', { run: true, confirmed: true, text_only: true })).sent_requests, 1);
  const override = await call('draw', { ...common, model: 'fixture-alternate',
    plugin_settings: { waifu2x: { input: false, output: false, scale: 2, noise: -1 } } });
  assert.equal(override.outputs.length, 1);
  assert.equal(received.at(-1).model, 'fixture-alternate');
  const gif = await call('replace', { ...replace, source: animation, max_frames: 2 });
  const gifFrames = (await call('inspect', { path: gif.outputs[0] })).frames;
  assert.equal(gifFrames, 2);
  assert.equal(sent, 9);
  await assert.rejects(callStudio('execute', {}), /Unknown MemeConvertStudio operation/);
  const abort = new AbortController();
  const pending = call('draw', { ...common, instruction: 'hold-fixture' }, { signal: abort.signal });
  await Promise.race([slow, delay(5000, undefined, { ref: false }).then(() => { throw new Error('Fixture request did not arrive'); })]);
  abort.abort();
  await assert.rejects(pending, /cancelled/);
  await delay(250);
  assert.equal((await call('status')).model, 'fixture-image');
  assert.equal((await call('status')).quota.used_today, 10);

  await start(allowed, true);
  await assert.rejects(call('models'), /permission denied: gateway/);
  await assert.rejects(call('replace', replace), /permission denied: gateway/);
  assert.equal(sent, 10);

  await start({ ...allowed, gateway: true, local_api: false }, true);
  await assert.rejects(call('replace', replace), /permission denied: local_api/);
  assert.equal(sent, 10);

  const route = { ...common, instruction: 'route-fixture' };
  await start({ ...allowed, gateway: true, per_task: 1 }, true);
  assert.equal((await call('status')).quota.used_today, 10);
  await assert.rejects(call('draw', route), /per-task/);
  assert.equal(sent, 11);
  assert.equal(received.at(-1).key, 'Bearer fixture-route-first');
  await start({ ...allowed, gateway: true, frame_hooks: true }, true);
  await call('draw', route);
  assert.equal(sent, 13);
  assert.equal(received.at(-1).key, 'Bearer fixture-route-second');
  assert.match(received.at(-1).prompt, /distinctive facial features/);
  await assert.rejects(call('draw', { ...common, count: 2 }), /daily/);
  await assert.rejects(call('draw', { ...route, plugin_settings: { gateway: { failover: false } } }), /task failed/);
  assert.equal(sent, 14);
  assert.equal((await call('status')).quota.remaining_today, 0);
  await assert.rejects(call('probe', { channel: 1, run: true, confirmed: true, text_only: true }), /daily/);
  await assert.rejects(call('draw', common), /daily/);
  assert.equal(sent, 14);
  assert.deepEqual(await fs.readFile(source), image);
  console.log('PASS Harness bridge, permission gates, model and settings allowlists, GIF/batch/probe quotas, restart persistence and failover charging');
} finally {
  await stop();
  server.closeAllConnections();
  server.close();
}
