'use strict';

const http = require('node:http');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');
const { randomUUID } = require('node:crypto');

const HOST = process.env.HOST || '0.0.0.0';
const PORT = Number.parseInt(process.env.PORT || '8080', 10);
const MAX_IMAGE_BYTES = Number.parseInt(
  process.env.MAX_IMAGE_BYTES || String(5 * 1024 * 1024),
  10,
);
const API_KEY = process.env.API_KEY || '';
const UPLOAD_ROOT = path.resolve(process.env.UPLOAD_DIR || path.join(__dirname, 'uploads'));

function sendJson(res, status, body) {
  const payload = Buffer.from(JSON.stringify(body));
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': payload.length,
    'Cache-Control': 'no-store',
  });
  res.end(payload);
}

function isAuthorized(req) {
  if (!API_KEY) return true;
  return req.headers['x-api-key'] === API_KEY;
}

function isValidMac(value) {
  return /^[0-9A-F]{12}$/.test(value);
}

function safeImageId(value) {
  return /^[0-9]{8}T[0-9]{6}Z-[0-9a-f-]{36}$/.test(value);
}

function makeImageId(now) {
  const timestamp = now.toISOString().replace(/[-:]/g, '').replace(/\.\d{3}Z$/, 'Z');
  return `${timestamp}-${randomUUID()}`;
}

function decodeHeader(value) {
  try {
    return decodeURIComponent(String(value || '')).slice(0, 64);
  } catch {
    return '';
  }
}

function waitForStream(stream, successEvent) {
  return new Promise((resolve, reject) => {
    const done = () => { stream.off('error', failed); resolve(); };
    const failed = (error) => { stream.off(successEvent, done); reject(error); };
    stream.once(successEvent, done);
    stream.once('error', failed);
  });
}

async function uploadJpeg(req, res, equipmentCode) {
  if (!isAuthorized(req)) {
    req.resume();
    return sendJson(res, 401, { success: false, error: 'unauthorized' });
  }
  if (req.headers['content-type']?.split(';', 1)[0].trim().toLowerCase() !== 'image/jpeg') {
    req.resume();
    return sendJson(res, 415, { success: false, error: 'content_type_must_be_image_jpeg' });
  }

  const announcedLength = Number.parseInt(req.headers['content-length'] || '0', 10);
  if (announcedLength > MAX_IMAGE_BYTES) {
    req.resume();
    return sendJson(res, 413, { success: false, error: 'image_too_large' });
  }

  const now = new Date();
  const imageId = makeImageId(now);
  const equipmentDir = path.join(UPLOAD_ROOT, equipmentCode);
  const finalPath = path.join(equipmentDir, `${imageId}.jpg`);
  const tempPath = path.join(equipmentDir, `${imageId}.tmp`);
  await fsp.mkdir(equipmentDir, { recursive: true });

  let received = 0;
  let signature = Buffer.alloc(0);
  const output = fs.createWriteStream(tempPath, { flags: 'wx' });
  let outputError = null;
  output.on('error', (error) => { outputError = error; });

  try {
    for await (const chunk of req) {
      received += chunk.length;
      if (received > MAX_IMAGE_BYTES) throw Object.assign(new Error('image_too_large'), { status: 413 });
      if (signature.length < 2) signature = Buffer.concat([signature, chunk.subarray(0, 2 - signature.length)]);
      if (!output.write(chunk)) await waitForStream(output, 'drain');
      if (outputError) throw outputError;
    }
    output.end();
    if (!output.writableFinished) await waitForStream(output, 'finish');
    if (outputError) throw outputError;

    if (received < 4 || signature[0] !== 0xff || signature[1] !== 0xd8) {
      throw Object.assign(new Error('invalid_jpeg_signature'), { status: 400 });
    }
    await fsp.rename(tempPath, finalPath);

    const metadata = {
      success: true,
      imageId,
      equipmentCode,
      equipmentName: decodeHeader(req.headers['x-equipment-name']),
      capturedAt: String(req.headers['x-captured-at'] || now.toISOString()),
      width: Number.parseInt(req.headers['x-image-width'] || '0', 10),
      height: Number.parseInt(req.headers['x-image-height'] || '0', 10),
      size: received,
      imageUrl: `/api/dryers/${equipmentCode}/captures/${imageId}`,
    };
    await fsp.writeFile(path.join(equipmentDir, `${imageId}.json`), JSON.stringify(metadata, null, 2));
    return sendJson(res, 201, metadata);
  } catch (error) {
    output.destroy();
    await fsp.rm(tempPath, { force: true }).catch(() => {});
    console.error('Upload failed:', error);
    return sendJson(res, error.status || 500, {
      success: false,
      error: error.status ? error.message : 'upload_failed',
    });
  }
}

async function serveJpeg(req, res, equipmentCode, imageId) {
  if (!isAuthorized(req)) return sendJson(res, 401, { success: false, error: 'unauthorized' });
  const imagePath = path.join(UPLOAD_ROOT, equipmentCode, `${imageId}.jpg`);
  try {
    const stat = await fsp.stat(imagePath);
    res.writeHead(200, {
      'Content-Type': 'image/jpeg',
      'Content-Length': stat.size,
      'Cache-Control': 'private, max-age=3600',
    });
    fs.createReadStream(imagePath).pipe(res);
  } catch (error) {
    if (error.code === 'ENOENT') return sendJson(res, 404, { success: false, error: 'image_not_found' });
    throw error;
  }
}

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
    if (req.method === 'GET' && url.pathname === '/health') {
      return sendJson(res, 200, { status: 'ok', time: new Date().toISOString() });
    }

    let match = url.pathname.match(/^\/api\/dryers\/([0-9A-Fa-f]{12})\/captures$/);
    if (req.method === 'POST' && match) {
      const equipmentCode = match[1].toUpperCase();
      if (!isValidMac(equipmentCode)) return sendJson(res, 400, { success: false, error: 'invalid_equipment_code' });
      return await uploadJpeg(req, res, equipmentCode);
    }

    match = url.pathname.match(/^\/api\/dryers\/([0-9A-Fa-f]{12})\/captures\/([^/]+)$/);
    if (req.method === 'GET' && match) {
      const equipmentCode = match[1].toUpperCase();
      const imageId = match[2];
      if (!isValidMac(equipmentCode) || !safeImageId(imageId)) {
        return sendJson(res, 400, { success: false, error: 'invalid_path' });
      }
      return await serveJpeg(req, res, equipmentCode, imageId);
    }

    return sendJson(res, 404, { success: false, error: 'not_found' });
  } catch (error) {
    console.error('Request failed:', error);
    if (!res.headersSent) sendJson(res, 500, { success: false, error: 'internal_server_error' });
    else res.destroy();
  }
});

server.requestTimeout = 30_000;
server.headersTimeout = 10_000;

server.listen(PORT, HOST, () => {
  console.log(`DY-EP4 image server listening on http://${HOST}:${PORT}`);
  console.log(`Upload directory: ${UPLOAD_ROOT}`);
  console.log(`Maximum JPEG size: ${MAX_IMAGE_BYTES} bytes`);
  console.log(API_KEY ? 'API key authentication: enabled' : 'API key authentication: disabled');
});
