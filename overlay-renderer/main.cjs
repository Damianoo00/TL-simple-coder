const { app, BrowserWindow } = require('electron');
const net = require('node:net');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

// stdout/stderr carry diagnostics only; binary frames use a private Unix socket.
console.log = console.error;
const options = Object.fromEntries(process.argv.slice(2).filter(a => a.startsWith('--overlay-'))
  .map(a => { const i = a.indexOf('='); return [a.slice(10, i), a.slice(i + 1)]; }));
const width = Number(options.width), height = Number(options.height), fps = Number(options.fps);
if (!options.socket || !/^https?:\/\//.test(options.url || '') ||
    !Number.isInteger(width) || width < 1 || width > 4096 ||
    !Number.isInteger(height) || height < 1 || height > 4096 ||
    !Number.isInteger(fps) || fps < 1 || fps > 120) {
  console.error('Invalid overlay renderer arguments'); process.exit(1);
}
if (options.mode !== 'gpu') app.disableHardwareAcceleration();
// BrowserRenderer passes --no-sandbox on Electron's startup command line.
if (process.env.ASYNC_NODES_OVERLAY_NO_SANDBOX === '1') {
  console.error('[overlay] Chromium sandbox disabled by ASYNC_NODES_OVERLAY_NO_SANDBOX=1; use trusted overlay pages only');
}
app.commandLine.appendSwitch('force-device-scale-factor', '1');
app.commandLine.appendSwitch('disable-renderer-backgrounding');
app.commandLine.appendSwitch('disable-background-timer-throttling');
const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'async-overlay-profile-'));
app.setPath('userData', profile);
let win, socket, busy = false, loaded = false, pendingImage, quitting = false;
let sequence = 0n, paints = 0, skipped = 0, transmitted = 0;
let lastAdjustedSize;

function transmit(image) {
  if (!loaded || !socket || socket.destroyed) return;
  if (busy) { pendingImage = image; skipped++; return; }
  const size = image.getSize();
  if (size.width !== width || size.height !== height) {
    if (!size.width || !size.height) return; // Ignore empty startup paints.
    const adjustedSize = `${size.width}x${size.height}`;
    if (lastAdjustedSize !== adjustedSize) {
      console.error(`[overlay] resizing bitmap ${adjustedSize} to ${width}x${height}`);
      lastAdjustedSize = adjustedSize;
    }
    image = image.resize({ width, height, quality: 'best' });
  }
  const bitmap = image.toBitmap({ scaleFactor: 1 }); // Premultiplied BGRA, no PNG/JPEG encoding.
  if (bitmap.length !== width * height * 4) {
    console.error(`Unexpected bitmap byte length ${bitmap.length}`); app.exit(1); return;
  }
  const header = Buffer.alloc(24);
  header.write('OVL1', 0, 'ascii');
  header.writeUInt32LE(width, 4); header.writeUInt32LE(height, 8);
  header.writeUInt32LE(bitmap.length, 12); header.writeBigUInt64LE(++sequence, 16);
  transmitted++;
  busy = true;
  socket.cork();
  socket.write(header);
  socket.write(bitmap, () => {
    busy = false;
    if (pendingImage) { const next = pendingImage; pendingImage = undefined; transmit(next); }
  });
  socket.uncork();
}

app.whenReady().then(() => {
  socket = net.createConnection(options.socket);
  socket.on('error', error => { if (!quitting) { console.error(error.message); app.exit(1); } });
  socket.on('close', () => app.quit());
  socket.on('connect', () => {
    win = new BrowserWindow({
      width, height, useContentSize: true, show: false, transparent: true,
      enableLargerThanScreen: true,
      backgroundColor: '#00000000', frame: false,
      webPreferences: { offscreen: true, backgroundThrottling: false,
        nodeIntegration: false, contextIsolation: true, sandbox: true, zoomFactor: 1 }
    });
    win.setContentSize(width, height);
    win.webContents.setFrameRate(fps);
    win.webContents.setAudioMuted(true);
    win.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
    win.webContents.on('paint', (_event, _dirty, image) => { paints++; transmit(image); });
    win.webContents.on('render-process-gone', (_event, details) => {
      console.error('Overlay renderer exited:', details.reason); app.exit(1);
    });
    win.loadURL(options.url).then(async () => {
      await win.webContents.insertCSS('html, body { background-color: transparent !important; margin: 0; overflow: hidden; }');
      await win.webContents.executeJavaScript('document.fonts.ready.then(() => true)');
      loaded = true;
      win.webContents.invalidate();
      console.error(`[overlay] loaded ${width}x${height} target_fps=${fps} mode=${options.mode}`);
    }).catch(error => { console.error('Overlay load failed:', error.message); app.exit(1); });
  });
});
const stats = setInterval(() => {
  console.error(`[overlay] paints_per_5s=${paints} transmitted_per_5s=${transmitted} transport_skips=${skipped}`);
  paints = skipped = transmitted = 0;
}, 5000);
stats.unref();
process.on('SIGTERM', () => { quitting = true; app.quit(); });
process.on('SIGINT', () => { quitting = true; app.quit(); });
app.on('will-quit', () => { socket?.destroy(); fs.rmSync(profile, { recursive: true, force: true }); });
