// A static server for the wallpaper's pages (node serve.js <dir> [port])
import http from 'http'; import fs from 'fs'; import path from 'path';
const [dir = '.', port = '8099'] = process.argv.slice(2);
const types = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.json': 'application/json', '.wasm': 'application/wasm' };
http.createServer((req, res) => {
  const p = path.join(dir, decodeURIComponent(req.url.split('?')[0]) === '/' ? 'index.html' : decodeURIComponent(req.url.split('?')[0]));
  fs.readFile(p, (e, data) => { if (e) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'Content-Type': types[path.extname(p)] || 'application/octet-stream' }); res.end(data); });
}).listen(Number(port), '127.0.0.1');
