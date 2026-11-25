#!/usr/bin/env python3
"""Web dashboard and allowlisted controls for the PS4 inference cluster."""

from __future__ import annotations

import argparse
import hmac
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import subprocess
import sys
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    from .collector import Collector, Node, load_nodes
except ImportError:
    from collector import Collector, Node, load_nodes


ACTIONS = {"start-server", "stop-server", "start-rpc", "stop-rpc", "bench"}
ROOT = Path(__file__).resolve().parent.parent


def validate_model_path(model: object) -> str:
    if not isinstance(model, str) or not model.startswith("~/models/") or any(ord(c) < 32 for c in model):
        raise ValueError("model must be a path below ~/models")
    relative = PurePosixPath(model.removeprefix("~/models/"))
    if not relative.parts or any(part in {"", ".", ".."} for part in relative.parts):
        raise ValueError("model must be a path below ~/models without traversal")
    return model


def build_remote_command(node: Node, action: str, model: object = None) -> str:
    if action not in ACTIONS - {"bench"}:
        raise ValueError("action is not an allowed remote action")
    binaries = "$HOME/llama.cpp/build-vk/bin"
    if action == "start-server":
        validated = validate_model_path(model)
        safe_model = "$HOME/" + shlex.quote(validated.removeprefix("~/"))
        return (f"nohup {binaries}/llama-server --model {safe_model} --port {node.llama_server_port} "
                ">/tmp/ps4-llama-server.log 2>&1 </dev/null &")
    if action == "stop-server":
        return "pkill -x llama-server || true"
    if action == "start-rpc":
        return (f"nohup {binaries}/rpc-server --host 0.0.0.0 --port {node.rpc_port} "
                ">/tmp/ps4-rpc-server.log 2>&1 </dev/null &")
    return "pkill -x rpc-server || true"


def authorize_token(expected: str | None, supplied: str | None) -> bool:
    return bool(expected and supplied and hmac.compare_digest(expected, supplied))


def run_action(nodes: dict[str, Node], node_name: object, action: object, model: object = None,
               timeout: float = 30.0) -> dict:
    if not isinstance(node_name, str) or node_name not in nodes:
        raise ValueError("unknown node")
    if not isinstance(action, str) or action not in ACTIONS:
        raise ValueError("unknown action")
    node = nodes[node_name]
    if action == "bench":
        safe_model = validate_model_path(model)
        command = [str(ROOT / "tools" / "bench.sh"), node.ssh, safe_model]
    else:
        command = ["ssh", "-o", "BatchMode=yes", "--", node.ssh, build_remote_command(node, action, model)]
    result = subprocess.run(command, text=True, capture_output=True, timeout=timeout, check=False)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or f"command exited {result.returncode}")
    return {"ok": True, "node": node_name, "action": action, "output": result.stdout[-4000:]}


PAGE = r'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>PS4 cluster</title><style>
:root{color-scheme:dark;--bg:#0b1020;--card:#151d31;--muted:#91a0bd;--good:#49d17d;--bad:#ff657a;--line:#53b7ff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:#eef3ff;font:14px system-ui,sans-serif}header{padding:22px max(20px,4vw);display:flex;align-items:center;justify-content:space-between}h1{margin:0;font-size:22px}#status{color:var(--muted)}main{display:grid;grid-template-columns:repeat(auto-fit,minmax(330px,1fr));gap:16px;padding:0 max(20px,4vw) 40px}.card{background:var(--card);border:1px solid #263451;border-radius:14px;padding:18px;box-shadow:0 12px 35px #0004}.title{display:flex;justify-content:space-between;align-items:center}.pill{border-radius:99px;padding:4px 9px;background:#27334c}.up{color:var(--good)}.down{color:var(--bad)}.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin:15px 0}.stat{background:#0d1528;border-radius:8px;padding:9px}.stat small,.meta{color:var(--muted)}.stat b{display:block;margin-top:4px;font-size:16px}.graphs{display:grid;grid-template-columns:1fr 1fr;gap:8px}.graph{background:#0d1528;border-radius:8px;padding:7px}.graph label{color:var(--muted);font-size:11px}canvas{display:block;width:100%;height:42px}.actions{display:flex;flex-wrap:wrap;gap:7px;margin-top:14px}button,input{background:#263451;color:#eef3ff;border:1px solid #3d4d70;border-radius:7px;padding:7px}button{cursor:pointer}input{flex:1;min-width:160px}.model{margin-top:10px;display:flex;gap:7px}.error{color:var(--bad);white-space:pre-wrap}
</style></head><body><header><h1>PS4 inference cluster</h1><span id="status">connecting…</span></header><main id="nodes"></main>
<script>
const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const val=(v,s='')=>v==null?'—':`${v}${s}`; let state;
function spark(canvas,points){const c=canvas.getContext('2d'),d=devicePixelRatio||1,w=canvas.clientWidth,h=canvas.clientHeight;canvas.width=w*d;canvas.height=h*d;c.scale(d,d);c.clearRect(0,0,w,h);const p=points.filter(x=>x[1]!=null);if(p.length<2)return;const ys=p.map(x=>x[1]),lo=Math.min(...ys),hi=Math.max(...ys);c.strokeStyle='#53b7ff';c.lineWidth=1.5;c.beginPath();p.forEach((x,i)=>{const px=i*w/(p.length-1),py=h-3-(x[1]-lo)/(hi-lo||1)*(h-6);i?c.lineTo(px,py):c.moveTo(px,py)});c.stroke()}
function render(s){state=s;const root=document.querySelector('#nodes');root.innerHTML='';Object.values(s.nodes).forEach(n=>{const hist=s.history[n.name]||{},m=n.memory||{},card=document.createElement('section');card.className='card';card.innerHTML=`<div class="title"><h2>${esc(n.name)}</h2><span class="pill ${n.up?'up':'down'}">${n.up?'online':'offline'}</span></div><div class="meta">${esc(n.role)} · ${esc(n.info?.PS4_MODEL||'unknown')} · ${esc(n.info?.GPU_CHIP||'unknown')}</div><div class="stats"><div class="stat"><small>GPU</small><b>${val(n.gpu_busy,'%')}</b></div><div class="stat"><small>Memory</small><b>${val(m.used_mib)} / ${val(m.total_mib)} MiB</b></div><div class="stat"><small>Temp</small><b>${val(n.temp_c,'°C')}</b></div></div><div class="graphs">${[['gpu','GPU %'],['mem','Memory MiB'],['temp','Temperature °C'],['tok_s','Tokens/s']].map(([k,l])=>`<div class="graph"><label>${l}</label><canvas data-k="${k}"></canvas></div>`).join('')}</div><p>llama-server: <b>${n.llama_server?'running':'stopped'}</b> · RPC: <b>${n.rpc_server?'running':'stopped'}</b><br><span class="meta">${esc(n.model||'no model')}</span></p><div class="model"><input value="~/models/" aria-label="model path"><button data-a="start-server">Start server</button><button data-a="bench">Bench</button></div><div class="actions"><button data-a="stop-server">Stop server</button><button data-a="start-rpc">Start RPC</button><button data-a="stop-rpc">Stop RPC</button></div><div class="error">${esc(n.error||'')}</div>`;card.querySelectorAll('canvas').forEach(c=>spark(c,hist[c.dataset.k]||[]));card.querySelectorAll('button').forEach(b=>b.onclick=()=>act(n.name,b.dataset.a,card.querySelector('input').value));root.append(card)})}
async function refresh(){try{const r=await fetch('/api/state',{cache:'no-store'});render(await r.json());document.querySelector('#status').textContent='updated '+new Date().toLocaleTimeString()}catch(e){document.querySelector('#status').textContent=e}}
async function act(node,action,model){const token=prompt('Dashboard token');if(token===null)return;const r=await fetch('/api/action',{method:'POST',headers:{'Content-Type':'application/json','X-Token':token},body:JSON.stringify({node,action,model})});const data=await r.json();if(!r.ok)alert(data.error||'action failed');else refresh()}
refresh();setInterval(refresh,2000);
</script></body></html>'''


class Handler(BaseHTTPRequestHandler):
    server_version = "PS4Dashboard/1"

    def _json(self, status: int, data: dict) -> None:
        body = json.dumps(data, separators=(",", ":")).encode()
        self.send_response(status); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body))); self.send_header("Cache-Control", "no-store")
        self.end_headers(); self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path == "/":
            body = PAGE.encode(); self.send_response(HTTPStatus.OK); self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
        elif self.path == "/api/state":
            self._json(HTTPStatus.OK, self.server.collector.snapshot())
        else:
            self._json(HTTPStatus.NOT_FOUND, {"error": "not found"})

    def do_POST(self) -> None:
        if self.path != "/api/action":
            self._json(HTTPStatus.NOT_FOUND, {"error": "not found"}); return
        if not self.server.token:
            self._json(HTTPStatus.SERVICE_UNAVAILABLE, {"error": "actions disabled: PS4_DASH_TOKEN is unset"}); return
        if not authorize_token(self.server.token, self.headers.get("X-Token")):
            self._json(HTTPStatus.UNAUTHORIZED, {"error": "invalid token"}); return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length < 1 or length > 16384:
                raise ValueError("invalid request size")
            payload = json.loads(self.rfile.read(length))
            result = run_action(self.server.nodes, payload.get("node"), payload.get("action"), payload.get("model"),
                                timeout=self.server.action_timeout)
            if payload.get("action") == "bench":
                for line in result["output"].splitlines():
                    try:
                        bench = json.loads(line)
                        if bench.get("tg") is not None:
                            self.server.collector.record_tok_s(payload["node"], float(bench["tg"]))
                    except (json.JSONDecodeError, TypeError, ValueError):
                        pass
            self._json(HTTPStatus.OK, result)
        except (ValueError, json.JSONDecodeError) as exc:
            self._json(HTTPStatus.BAD_REQUEST, {"error": str(exc)})
        except subprocess.TimeoutExpired:
            self._json(HTTPStatus.GATEWAY_TIMEOUT, {"error": "action timed out"})
        except (OSError, RuntimeError) as exc:
            self._json(HTTPStatus.BAD_GATEWAY, {"error": str(exc)})

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nodes", default=str(ROOT / "head" / "nodes.ini"))
    parser.add_argument("--lan", action="store_true", help="listen on all interfaces (default: loopback only)")
    parser.add_argument("--port", type=int, default=8000); parser.add_argument("--interval", type=float, default=5)
    parser.add_argument("--ssh-timeout", type=float, default=4); parser.add_argument("--action-timeout", type=float, default=630)
    args = parser.parse_args()
    nodes = load_nodes(args.nodes); collector = Collector(nodes, args.interval, args.ssh_timeout); collector.start()
    address = "0.0.0.0" if args.lan else "127.0.0.1"
    server = ThreadingHTTPServer((address, args.port), Handler)
    server.nodes, server.collector = nodes, collector
    server.token, server.action_timeout = os.environ.get("PS4_DASH_TOKEN"), args.action_timeout
    print(f"PS4 dashboard listening on http://{address}:{args.port}")
    if not server.token: print("WARNING: PS4_DASH_TOKEN unset; POST actions are disabled", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close(); collector.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
