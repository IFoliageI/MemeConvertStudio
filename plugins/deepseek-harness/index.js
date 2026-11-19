import { createConnection } from 'node:net';

export const inject = ['tools'];
const pipe = String.raw`\\.\pipe\MemeConvertStudio.DeepSeekHarness.v1`;
const limit = 1024 * 1024;

const timeoutMs = 3600000;
export function callStudio(operation, args, signal) {
  return new Promise((resolve, reject) => {
    signal?.throwIfAborted();
    const request = JSON.stringify({ version: 1, operation, arguments: args }) + '\n';
    if (Buffer.byteLength(request) > 65536) throw new Error('MemeConvertStudio request exceeds 64 KiB.');
    const client = createConnection(pipe);
    let reply = '', bytes = 0, finished = false;
    const complete = (error, result) => {
      if (finished) return;
      finished = true;
      signal?.removeEventListener('abort', abort);
      clearTimeout(timer);
      client.destroy();
      if (error) reject(error);
      else resolve(result);
    };
    const abort = () => complete(new Error('MemeConvertStudio call cancelled.'));
    const timer = setTimeout(() => complete(new Error('MemeConvertStudio call timed out. Check the app before retrying.')), timeoutMs);
    signal?.addEventListener('abort', abort, { once: true });
    client.setEncoding('utf8');
    client.on('connect', () => client.write(request));
    client.on('error', error => complete(new Error(error.code === 'EBUSY'
      ? 'MemeConvertStudio is handling another Harness request.'
      : 'Cannot reach MemeConvertStudio. Open it and enable the DeepSeek Harness plugin under the same Windows account.')));
    client.on('close', () => complete(new Error('MemeConvertStudio disconnected. Check the app before retrying.')));
    client.on('data', chunk => {
      bytes += Buffer.byteLength(chunk);
      if (bytes > limit) return complete(new Error('MemeConvertStudio response exceeds 1 MiB.'));
      reply += chunk;
      const end = reply.indexOf('\n');
      if (end < 0) return;
      try {
        const response = JSON.parse(reply.slice(0, end));
        if (response.ok !== true) return complete(new Error(response.error || 'MemeConvertStudio rejected the request.'));
        complete(null, response.result);
      } catch {
        complete(new Error('Invalid MemeConvertStudio response.'));
      }
    });
  });
}

const localPath = { type: 'string', description: 'Absolute Windows path on this computer, e.g. D:/images/input.png.' };
const instruction = { type: 'string', description: 'Instructions or drawing prompt (UTF-8, maximum 24,000 bytes).' };
const confirmed = { type: 'boolean', description: 'Set true only when the user has approved generation and potential supplier charges.' };
const channel = { type: 'integer', description: 'Enabled supplier-pool channel index from memestudio_status.' };
const rect = {
  type: 'object', additionalProperties: false, required: ['x', 'y', 'width', 'height'],
  description: 'Fractions of image width and height in 0..1; width and height must be positive.',
  properties: Object.fromEntries(['x', 'y', 'width', 'height'].map(key => [key, { type: 'number' }])),
};
const pluginSettings = {
  type: 'object', additionalProperties: false,
  description: 'Optional per-task overrides. Requires permission for settings changes and the target plugin. Never changes saved configuration.',
  properties: {
    waifu2x: { type: 'object', additionalProperties: false, properties: {
      input: { type: 'boolean' }, output: { type: 'boolean' },
      scale: { type: 'integer', description: '1, 2, 4, 8, 16, or 32.' },
      noise: { type: 'integer', description: '-1 through 3.' },
    } },
    gateway: { type: 'object', additionalProperties: false, properties: { failover: { type: 'boolean' } } },
  },
};
const common = {
  instruction, confirmed,
  styles: { type: 'array', items: localPath, description: 'Up to eight style reference images.' },
  model: { type: 'string', description: 'Optional allowlisted model ID for this task only; defaults to the current app selection.' },
  plugin_settings: pluginSettings,
  count: { type: 'integer', description: '1 to 10 copies per source; defaults to one.' },
  output_parent: { ...localPath, description: 'Existing output parent directory. A new subdirectory is always created.' },
};
const edit = {
  ...common, character: localPath, crop: rect,
  max_frames: { type: 'integer', description: 'Override the GIF frame limit (0 to 600); zero means no sampling limit.' },
};
const specs = [
  ['status', 'Read the running app’s enabled plugins, model, granted permissions, request quotas and visible supplier-pool channels.', {}, []],
  ['inspect', 'Read dimensions, frame count and timing of a local image or GIF without calling a provider.', { path: localPath }, ['path']],
  ['models', 'List models from the current supplier, the enabled pool catalogue, or one selected pool channel.', { channel }, []],
  ['replace', 'Replace a character in one image or GIF using a reference image. Requires user approval of potential charges.',
    { ...edit, source: localPath }, ['source', 'character', 'confirmed']],
  ['batch', 'Replace characters in multiple local files or folders. Requires the Batch conversion plugin and user approval of costs.',
    { ...edit, sources: { type: 'array', items: localPath, description: '1 to 500 local paths.' } }, ['sources', 'character', 'confirmed']],
  ['draw', 'Draw from a prompt with optional style references. Requires the Prompt drawing plugin and user approval of costs.',
    common, ['instruction', 'confirmed']],
  ['refine', 'Retouch an existing image or GIF, optionally inside a normalized rectangle. Requires the Local redraw plugin and cost approval.',
    { ...common, source: localPath, region: rect }, ['source', 'instruction', 'confirmed']],
  ['probe', 'List supplier parameter probe cases and estimates without sending a request. Only run when the user approves costs. Requires the API parameter test plugin.',
    { channel, model: common.model, text_only: { type: 'boolean' }, run: { type: 'boolean' }, confirmed,
      cases: { type: 'array', items: { type: 'integer' }, description: 'Up to seven additional case indices; the baseline runs first.' } }, []],
];

export function apply(ctx) {
  for (const [operation, description, properties, required] of specs) {
    ctx.effect(() => ctx.tools.register({
      name: `memestudio_${operation}`,
      description,
      timeoutMs,
      parameters: { type: 'object', properties, required, additionalProperties: false },
      output: { schema: { type: 'object', additionalProperties: true },
        render: (_args, result) => [{ type: 'text', text: JSON.stringify(result) }] },
      execute: (args, execution) => callStudio(operation, args, execution.signal),
    }));
  }
}
