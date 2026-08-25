'use strict';

const { spawn } = require('node:child_process');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');

const port = 18080;
const uploadDir = path.join(os.tmpdir(), `dy-ep4-http-test-${process.pid}`);
const server = spawn(process.execPath, [path.join(__dirname, 'server.js')], {
  env: { ...process.env, PORT: String(port), UPLOAD_DIR: uploadDir },
  stdio: ['ignore', 'pipe', 'pipe'],
});

let serverError = '';
server.stderr.on('data', (chunk) => { serverError += chunk.toString(); });

async function waitUntilReady() {
  for (let attempt = 0; attempt < 30; attempt += 1) {
    try {
      const response = await fetch(`http://127.0.0.1:${port}/health`);
      if (response.ok) return response.json();
    } catch {}
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`server_not_ready: ${serverError}`);
}

async function run() {
  try {
    const health = await waitUntilReady();
    const jpeg = Uint8Array.from([0xff, 0xd8, 0xff, 0xd9]);
    const uploadResponse = await fetch(
      `http://127.0.0.1:${port}/api/dryers/80F1B2D37D16/captures`,
      {
        method: 'POST',
        headers: {
          'Content-Type': 'image/jpeg',
          'X-Equipment-Name': 'DY-EP4',
          'X-Image-Width': '1',
          'X-Image-Height': '1',
        },
        body: jpeg,
      },
    );
    const upload = await uploadResponse.json();
    if (uploadResponse.status !== 201 || !upload.success) throw new Error(JSON.stringify(upload));

    const downloadResponse = await fetch(`http://127.0.0.1:${port}${upload.imageUrl}`);
    const downloaded = new Uint8Array(await downloadResponse.arrayBuffer());
    if (!downloadResponse.ok || downloaded.length !== jpeg.length) throw new Error('download_mismatch');

    console.log(JSON.stringify({
      node: process.version,
      health: health.status,
      uploadSuccess: upload.success,
      savedBytes: upload.size,
      downloadStatus: downloadResponse.status,
      downloadBytes: downloaded.length,
    }, null, 2));
  } finally {
    server.kill();
    await fs.rm(uploadDir, { recursive: true, force: true });
  }
}

run().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
