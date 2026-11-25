# PS4 head-node dashboard

The dashboard is a Python-standard-library service for a PS4 Pro head node. It polls all configured consoles in parallel over SSH, retains roughly one hour of in-memory graphs, and exposes allowlisted server/RPC/benchmark controls.

## Configure and run

Copy the example inventory and edit the SSH aliases and ports:

```sh
cp head/nodes.example.ini head/nodes.ini
PS4_DASH_TOKEN='use-a-long-random-value' python3 head/dashboard.py --lan --nodes head/nodes.ini
```

Open `http://HEAD-NODE-IP:8000/` from a LAN browser. Without `--lan`, the service binds only to `127.0.0.1`. Useful options include `--port`, `--interval`, and `--ssh-timeout`.

The inventory section name is the dashboard node name. `ssh` is an SSH config alias; key authentication must already work. The real `head/nodes.ini` is ignored by git.

Run a benchmark directly with:

```sh
tools/bench.sh thebe '~/models/model.gguf' 99
```

`BENCH_TIMEOUT` changes the default 600-second limit. Results append to `results/bench.jsonl`.

## Security

- LAN exposure is opt-in. There is no TLS, so use this only on a trusted LAN or behind a secure tunnel/reverse proxy.
- Every POST action requires `X-Token` to exactly match `PS4_DASH_TOKEN`. If the environment variable is unset, all actions are disabled. The browser asks for the token for each action and does not persist it.
- Node names and actions are allowlisted against the loaded inventory and known action set.
- Model arguments must remain below `~/models`; traversal and control characters are rejected, and the path is shell-quoted before being sent remotely.
- Start/stop commands use fixed binary paths and inventory ports. Stops use `pkill -x`, never `pkill -f`.
- Dashboard state has no authentication. It can reveal hostnames, model names, and hardware information to anyone who can reach the listening port.

The collector executes one SSH call per node per poll. A node timeout does not hold up probes for the other nodes.

## Known limitation: rpc-server exposure

`start-rpc` binds llama.cpp's `rpc-server` to `0.0.0.0`. The RPC protocol is **unauthenticated**: any host that
can reach the port can read and write the node's GPU buffers. Only start it on an isolated cluster network
(e.g. the host's ethernet link / a dedicated switch), never on a shared Wi-Fi or LAN.
TODO: a per-node `rpc_host` setting that binds the cluster interface only.
