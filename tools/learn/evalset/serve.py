#!/usr/bin/env python3
"""Serve the eval-set MLP, GRU and memory transformer on one GPU.

  python serve.py [--host 10.30.0.1] [--port 8790] [--root RESULTS ...]

Each --root is a directory written by run_gpu.sh (mlp/, gru/ and tf/ with
model.pt and results.json). The process loads every checkpoint once and keeps
it on the GPU. A client POSTs one tick of daemon features and gets back P and
ceil(P), the core count train.py's ceil_req would ask for.

The wire is WireGuard. The default bind is pd-gpu's wg0 address, 10.30.0.1,
so the service is not on the lab network. dtn-01 is 10.30.0.3 and POSTs to
http://10.30.0.1:8790/predict.

One request is one tick of the whole machine. The body is JSON:

  {"model": "luna-20261102/gru", "session": "hotel-1", "reset": false,
   "daemons": [{"id": "frontend", "daemon": [25 floats]}]}

daemon is the 25-wide vector from build.py daemon_features. A list of such
vectors is several ticks, oldest first. token (17) and ctx (5) are optional;
when they are absent they are derived from daemon the way daemon_features
fills them from a cgroup ring. The reply is one object per id, with p and
cores. cores is an integer from 1 to REQ_MAX.

reset true drops that session before the tick. GRU hidden state and the
transformer's ring live on the server, keyed by model and session. Peer slots
for the transformer are assigned once per session, busiest task-clock first,
and do not move until reset. At most CTX_MAX other daemons are visible.

GET /health lists the loaded checkpoints.
"""

import argparse
import json
import sys
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "slo"))
from seq_models import CTX_F, CTX_MAX, DAEMON_F  # noqa: E402
from train import ceil_req, make_model, threads_of  # noqa: E402

TOKEN_F = 17  # threads_of packs one cgroup token into 17 positions
REPO = Path(__file__).resolve().parents[3]
DEFAULT_HOST = "10.30.0.1"
DEFAULT_PORT = 8790


def token_of(daemon):
    """The 17-wide cgroup token daemon_features builds when there is no per-thread trace."""
    token = np.zeros(TOKEN_F, np.float32)
    token[0] = token[1] = daemon[0]
    token[2] = daemon[2]
    token[5] = daemon[9]
    token[6] = daemon[10]
    return token


def ctx_of(daemon):
    """The 5-wide context row of this daemon, as daemon_features stacks it."""
    clock = float(daemon[0])
    held = float(daemon[10])
    cores = float(daemon[13]) * 16.0
    step = float(daemon[14]) * 4.0
    return np.array([clock / 16.0, held, cores / 16.0, step / 4.0, clock / max(cores, 1.0)], np.float32)


def as_rows(value, width, what):
    """One vector, or a list of vectors oldest first, each of length width."""
    if not isinstance(value, list) or not value:
        raise ValueError(f"{what} must be a list of {width} floats")
    rows = value if isinstance(value[0], list) else [value]
    out = []
    for row in rows:
        if not isinstance(row, list) or len(row) != width:
            raise ValueError(f"{what} must have length {width}")
        try:
            arr = np.asarray(row, np.float32)
        except (TypeError, ValueError) as failed:
            raise ValueError(f"{what} must be floats") from failed
        if not np.isfinite(arr).all():
            raise ValueError(f"{what} must be finite")
        out.append(arr)
    return out


class Series:
    def __init__(self, span):
        self.daemon = deque(maxlen=span)
        self.token = deque(maxlen=span)
        self.ctx = deque(maxlen=span)
        self.gru = None

    def append(self, daemon, token, ctx):
        self.daemon.append(daemon)
        self.token.append(token)
        self.ctx.append(ctx)


class Session:
    def __init__(self):
        self.order = []
        self.series = {}


class Server:
    def __init__(self, models, device):
        self.models = models
        self.device = device
        self.sessions = {}
        self.lock = threading.Lock()

    def health(self):
        return {
            "ok": True,
            "device": self.device,
            "models": [
                {"name": name, "kind": spec["kind"], "params": spec["params"], "ctx": spec["ctx"]}
                for name, spec in self.models.items()
            ],
        }

    def predict(self, body):
        name = body.get("model")
        spec = self.models.get(name)
        if spec is None:
            known = ", ".join(self.models)
            raise ValueError(f"unknown model {name!r}; loaded: {known}")
        session_id = str(body.get("session") or "default")
        key = (name, session_id)
        with self.lock:
            if body.get("reset"):
                self.sessions.pop(key, None)
            session = self.sessions.setdefault(key, Session())
            rows = self._ingest(session, spec, body.get("daemons"))
            started = time.perf_counter()
            out = [self._one(spec, session, item) for item in rows]
            ms = (time.perf_counter() - started) * 1000
        return {"model": name, "session": session_id, "ms": round(ms, 3), "daemons": out}

    def _ingest(self, session, spec, daemons):
        if not isinstance(daemons, list) or not daemons:
            raise ValueError("daemons must be a non-empty list")
        span = spec["span"]
        parsed = []
        seen = set()
        for item in daemons:
            if not isinstance(item, dict) or not item.get("id"):
                raise ValueError("each daemon needs an id")
            ident = str(item["id"])
            if ident in seen:
                raise ValueError(f"duplicate daemon id {ident}")
            seen.add(ident)
            daemon_rows = as_rows(item.get("daemon"), DAEMON_F, "daemon")
            token_rows = as_rows(item["token"], TOKEN_F, "token") if "token" in item else [token_of(r) for r in daemon_rows]
            ctx_rows = as_rows(item["ctx"], CTX_F, "ctx") if "ctx" in item else [ctx_of(r) for r in daemon_rows]
            if not (len(daemon_rows) == len(token_rows) == len(ctx_rows)):
                raise ValueError(f"{ident}: daemon, token and ctx must have the same number of ticks")
            series = session.series.get(ident)
            if series is None:
                series = Series(span)
                session.series[ident] = series
            for daemon, token, ctx in zip(daemon_rows, token_rows, ctx_rows):
                series.append(daemon, token, ctx)
            parsed.append(ident)
        self._assign_slots(session, parsed)
        return parsed

    def _assign_slots(self, session, ids):
        """Freeze peer order. New ids join at the end, busiest task-clock first, and never displace an existing slot."""
        known = set(session.order)
        fresh = [ident for ident in ids if ident not in known]
        fresh.sort(key=lambda ident: (-float(session.series[ident].daemon[-1][0]), ident))
        session.order.extend(fresh)

    def _one(self, spec, session, ident):
        kind = spec["kind"]
        series = session.series[ident]
        if kind == "mlp":
            pred = self._mlp(spec["model"], series.daemon[-1])
        elif kind == "gru":
            pred, series.gru = self._gru(spec["model"], series.daemon[-1], series.gru)
        else:
            pred = self._transformer(spec, session, ident)
        cores = int(ceil_req(np.asarray([pred], np.float32))[0])
        return {"id": ident, "p": round(float(pred), 4), "cores": cores, "ticks": len(series.daemon)}

    def _mlp(self, model, daemon):
        tensor = torch.from_numpy(daemon).to(self.device)[None, None]
        with torch.no_grad():
            pred = model(None, None, tensor)[0]
        return float(pred[0, -1])

    def _gru(self, model, daemon, state):
        tensor = torch.from_numpy(daemon).to(self.device)[None, None]
        with torch.no_grad():
            pred, state = model(None, None, tensor, state)
        return float(pred[0, -1]), state

    def _transformer(self, spec, session, ident):
        series = session.series[ident]
        fine = min(spec["fine"], len(series.daemon))
        daemon = torch.tensor(np.stack(list(series.daemon)[-fine:]), device=self.device)
        token = torch.tensor(np.stack(list(series.token)[-fine:]), device=self.device)
        hist = self._hist(series, fine, spec["span_hist"])
        ctx, mask = self._ctx(session, ident, fine)
        threads, thread_mask = threads_of(token)
        with torch.no_grad():
            pred = spec["model"](
                threads[None], thread_mask[None], daemon[None], hist[None], ctx[None], mask[None])
        return float(pred[0, -1])

    def _hist(self, series, fine, span):
        """Coarse prefix: the span ticks strictly before the fine window, zeros where the run has not reached them."""
        before = list(series.daemon)[:-fine]
        tail = before[-span:]
        hist = np.zeros((span, DAEMON_F), np.float32)
        if tail:
            hist[-len(tail):] = np.stack(tail)
        return torch.from_numpy(hist).to(self.device)

    def _ctx(self, session, ident, fine):
        peers = [peer for peer in session.order if peer != ident][:CTX_MAX]
        ctx = np.zeros((fine, CTX_MAX, CTX_F), np.float32)
        mask = np.zeros(CTX_MAX, dtype=bool)
        for slot, peer in enumerate(peers):
            mask[slot] = True
            tail = list(session.series[peer].ctx)[-fine:]
            if tail:
                ctx[fine - len(tail):, slot] = np.stack(tail)
        return torch.from_numpy(ctx).to(self.device), torch.from_numpy(mask).to(self.device)


def load_root(root, device):
    """One run_gpu.sh directory: its mlp, gru and tf children that have a checkpoint."""
    loaded = {}
    label = root.name
    for kind in ("mlp", "gru", "tf"):
        path = root / kind
        weights = path / "model.pt"
        meta = path / "results.json"
        if not weights.is_file() or not meta.is_file():
            print(f"skip {path}: no checkpoint", flush=True)
            continue
        doc = json.load(open(meta))
        args = argparse.Namespace(**doc["args"])
        model = make_model(args).to(device)
        model.load_state_dict(torch.load(weights, map_location=device, weights_only=True))
        model.eval()
        name = f"{label}/{kind}"
        loaded[name] = {
            "model": model,
            "kind": kind,
            "params": int(doc.get("params") or 0),
            "ctx": bool(args.ctx) if kind == "tf" else False,
            "fine": int(args.fine),
            "span_hist": int(args.mem) * int(args.bucket),
            "span": int(args.fine) + int(args.mem) * int(args.bucket),
        }
        print(f"loaded {name}: {loaded[name]['params']} parameters on {device}", flush=True)
    return loaded


def make_handler(server):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path != "/health":
                self._send(404, {"error": "not found"})
                return
            self._send(200, server.health())

        def do_POST(self):
            path = self.path.split("?", 1)[0]
            if path != "/predict":
                self._send(404, {"error": "not found"})
                return
            try:
                length = int(self.headers.get("Content-Length") or 0)
            except ValueError:
                self._send(400, {"error": "bad Content-Length"})
                return
            if length <= 0 or length > 8_000_000:
                self._send(400, {"error": "body must be under 8 MB"})
                return
            try:
                body = json.loads(self.rfile.read(length))
                self._send(200, server.predict(body))
            except json.JSONDecodeError:
                self._send(400, {"error": "body is not JSON"})
            except ValueError as failed:
                self._send(400, {"error": str(failed)})
            except Exception as failed:  # a model bug should not kill the process
                self._send(500, {"error": f"{type(failed).__name__}: {failed}"})

        def _send(self, status, payload):
            raw = json.dumps(payload).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def log_message(self, fmt, *args):
            return

    return Handler


def main():
    parser = argparse.ArgumentParser(description="Serve eval-set MLP, GRU and transformer checkpoints")
    parser.add_argument("--host", default=DEFAULT_HOST, help="bind address (pd-gpu wg0 is 10.30.0.1)")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--root", action="append", type=Path, help="a results directory with mlp/, gru/ and tf/")
    parser.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    args = parser.parse_args()
    if args.device == "cuda" and not torch.cuda.is_available():
        sys.exit("CUDA was requested and is not available")
    roots = args.root or [REPO / "results" / "luna-20261102", REPO / "results" / "eval-20261101"]
    torch.set_num_threads(1)
    models = {}
    for root in roots:
        if not root.is_dir():
            print(f"skip {root}: not a directory", flush=True)
            continue
        models.update(load_root(root, args.device))
    if not models:
        sys.exit("no checkpoints loaded")
    server = Server(models, args.device)
    httpd = ThreadingHTTPServer((args.host, args.port), make_handler(server))
    print(f"listening on http://{args.host}:{args.port}", flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
