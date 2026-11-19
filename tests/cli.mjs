import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { once } from 'node:events';
import fs from 'node:fs/promises';
import http from 'node:http';
import path from 'node:path';
import { promisify } from 'node:util';

const exec = promisify(execFile);
const exe = path.resolve(process.argv[2] ?? 'bin/cli/MemeStudio-cli.exe');
const temp = path.resolve('tmp', 'cli');
await fs.mkdir(temp, { recursive: true });
const directory = await fs.mkdtemp(path.join(temp, 'fixture-'));
const image = await fs.readFile(path.resolve('tmp', 'tests', 'character.png'));
const character = '\u4eba\u7269.png';
await fs.writeFile(path.join(directory, character), image);
await fs.writeFile(path.join(directory, 'source.png'), image);

const posted = [];
const server = http.createServer(async (request, response) => {
  try {
    assert.equal(request.headers.authorization, 'Bearer cli-fixture');
    if (request.url === '/v1/models') {
      response.end(JSON.stringify({ data: [{ id: 'cli-model' }] }));
      return;
    }
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    const body = Buffer.concat(chunks);
    if (request.url === '/v1/images/generations') posted.push(JSON.parse(body));
    else {
      assert.equal(request.url, '/v1/images/edits');
      const form = await new Request('http://fixture', { method: 'POST', body,
        headers: { 'Content-Type': request.headers['content-type'] } }).formData();
      posted.push({ model: form.get('model'), prompt: form.get('prompt'), inputs: form.getAll('image[]').length });
    }
    response.end(JSON.stringify({ data: [{ b64_json: image.toString('base64') }] }));
  } catch (error) {
    response.writeHead(500).end(String(error));
  }
});
server.listen(0, '127.0.0.1');
await once(server, 'listening');
const base = `http://127.0.0.1:${server.address().port}/v1`;
const envName = 'MEMESTUDIO_CLI_TEST_KEY';
const run = (...args) => exec(exe, args, { cwd: directory, windowsHide: true, timeout: 15000,
  env: { ...process.env, [envName]: 'cli-fixture' } });
const json = async (...args) => JSON.parse((await run(...args)).stdout);
const provider = ['--base-url', base, '--api-key-env', envName, '--model', 'cli-model'];
const exists = async result => {
  for (const file of result.outputs) assert((await fs.stat(file)).size > 0);
  return result.outputs;
};

try {
  assert.match((await run('--help')).stdout, /MemeStudio CLI/);
  assert.deepEqual((await json('models', ...provider)).models, ['cli-model']);
  assert.equal((await json('inspect', '--source', character)).frames, 1);
  const catalogue = (await json('plugins', '--load', 'memestudio.example')).plugins;
  assert(catalogue.some(plugin => plugin.id === 'memestudio.example' && plugin.loaded));
  assert.match((await json('plugin', 'memestudio.example', 'identity-note')).result.append_instruction, /identity/);
  const direct = await run('draw', ...provider, '--prompt', '\u753b\u4e00\u53ea\u84dd\u8272\u65b9\u5757',
    '--output', 'draw.png', '--count', '2', '--format', 'jpeg', '--compression', '81');
  assert.equal((await exists(JSON.parse(direct.stdout))).length, 2);
  assert(direct.stderr.length > 0);
  assert.equal(posted[0].output_compression, 81);
  assert.match(posted[0].prompt, /\u84dd\u8272/);

  const config = path.join(directory, 'preset');
  await fs.mkdir(config);
  const configJson = JSON.stringify({ command: 'replace', source: '../source.png', character: `../${character}`,
    output: 'config.png', model: 'from-file', base_url: base, api_key_env: envName });
  const configJsonc = configJson.replace(/^{/, '{// task comment\n');
  const configIni = `[job]\nsource=../source.png\ncharacter=../${character}\noutput=ini.png\n` +
    `model=ini-model\nbase_url=${base}\napi_key_env=${envName}\n[extra]\ncustom_hint="fixture"\n`;
  for (const [name, content] of [['task.json', configJson], ['task.jsonc', configJsonc], ['task.ini', configIni]])
    await fs.writeFile(path.join(config, name), content);
  await exists(await json('run', '--config', 'preset/task.json', '--model', 'override', '--output', 'override.png', '--quiet'));
  assert.equal(posted.at(-1).model, 'override');
  assert.equal(posted.at(-1).inputs, 2);
  const result = await exists(await json('--config=preset/task.jsonc'));
  assert.equal(result[0], path.join(config, 'config.png'));
  assert.equal(posted.at(-1).model, 'from-file');
  await exists(await json('run', '--config', 'preset/task.ini'));
  assert.equal(posted.at(-1).model, 'ini-model');
  await exists(await json('plugin', 'memestudio.prompt-draw', 'draw', ...provider,
    '--prompt', 'reference example', '--style', character, '--output', 'style-draw.png'));
  assert.equal(posted.at(-1).inputs, 1);
  await exists(await json('refine', ...provider, '--source', 'source.png', '--prompt', 'fix outline',
    '--region', '{"x":0.1,"y":0.1,"width":0.4,"height":0.4}', '--output', 'refine.png'));
  assert.equal((await exists(await json('batch', ...provider, '--input', 'source.png', '--input', character,
    '--character', character, '--output', 'batch'))).length, 2);

  const attempts = posted.length;
  assert.equal((await json('probe', ...provider, '--text-only')).sent_requests, 0);
  assert.equal(posted.length, attempts);
  assert.equal((await json('probe', ...provider, '--text-only', '--probe-run')).sent_requests, 1);
  assert.equal(posted.length, attempts + 1);
  await assert.rejects(run('run', '--config', 'preset/task.json', '--progress-file', 'preset/task.json'),
    error => error.code === 1 && /cannot overwrite/.test(error.stderr) && error.stdout === '');
  await assert.rejects(run('draw', ...provider, '--count', '4294967297', '--output', 'bad.png'),
    error => error.code === 1 && /Invalid value for count/.test(error.stderr));
  assert.equal(posted.length, attempts + 1);
  for (const [name, content] of [['task.json', configJson], ['task.jsonc', configJsonc], ['task.ini', configIni]])
    assert.equal(await fs.readFile(path.join(config, name), 'utf8'), content);
  await assert.rejects(fs.access(path.join(directory, 'MemeStudio-error.txt')));
  console.log('PASS CLI flags, three read-only preset formats, Unicode paths, plugin commands, image workflows and opt-in probe');
} finally {
  server.closeAllConnections();
  server.close();
}
