// Drives Chrome in Arctic over the DevTools protocol: starts it with the
// flags given, opens a page, evaluates an expression and prints the result.
// Used to see how WebGPU runs here (chrome://gpu, benchmarks) and later to
// render the wallpaper's frames.
//
//   node cdp.js <url> <expression> [waitMs] [--flag ...]

import { spawn } from 'child_process';
import os from 'os';
import path from 'path';
import fs from 'fs';

const [url, expression, waitArg = '8000', ...flags] = process.argv.slice(2);
const CHROME = 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';
const PORT = 9333;
const profile = path.join(os.tmpdir(), 'arctic-cdp-profile');
fs.mkdirSync(profile, { recursive: true });

const chrome = spawn(CHROME, [`--remote-debugging-port=${PORT}`, `--user-data-dir=${profile}`, '--no-first-run',
    '--no-default-browser-check', '--enable-unsafe-webgpu', ...flags, 'about:blank'], { stdio: 'ignore' });

const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let target;
for (let i = 0; i < 60 && !target; i++) {
    await sleep(500);
    try { target = (await (await fetch(`http://127.0.0.1:${PORT}/json`)).json()).find(t => t.type === 'page'); } catch { }
}
if (!target) { console.error('no Chrome'); chrome.kill(); process.exit(1); }

const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise(r => ws.addEventListener('open', r));
let id = 0;
const pending = new Map();
ws.addEventListener('message', (e) => {
    const m = JSON.parse(e.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
});
const send = (method, params = {}) => new Promise(r => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });

await send('Page.enable');
await send('Page.navigate', { url });
await sleep(Number(waitArg));
const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true, timeout: 120000 });
console.log(typeof r.result?.result?.value === 'string' ? r.result.result.value : JSON.stringify(r.result, null, 1));
await send('Browser.close').catch(() => { });
setTimeout(() => { try { chrome.kill(); } catch { } process.exit(0); }, 1500);
