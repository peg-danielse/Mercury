"""Sequence models for the SLO allocation target (see train_seq.py): a memoryless MLP and a GRU on daemon-level features,
and a memory transformer over thread tokens with long fine and coarse context that also sees its past decisions.

All models map a case's per-tick inputs to P(t), the daemon's core demand (softplus, >= 0), the quantity train_slo.py's
GNN produces as the sum of its per-thread outputs.

Inputs per tick (featurize):
  threads [T, K+1, 17]  the K busiest threads by run+wait (train_gnn F_NAMES; feature 7, cpuset size, zeroed) and one
                        aggregate token summing the rest; mask [T, K+1] marks real tokens
  daemon  [T, DF]       daemon-level stats (sums/maxima of thread run, wait, task-clock, ctx switches, migrations,
                        busy-thread count, thread count, cgroup occupancy and pressure, per-node thread shares) and the
                        decision history: allocation a(t)/16, a(t)-a(t-1), ticks since the last change (log), changes
                        in the last second. In logged data a(t) is the policy that ran (fixed k in the sweeps); in
                        Mercury it is the model's own previous decision.
"""
import math
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

K_THREADS = 16
NODES = 8


def featurize(X, alloc, k=K_THREADS):
    """X [T, N, 17] (train_gnn.case_tensors, last node = aggregate), alloc [T] cores -> threads, mask, daemon."""
    X = X.clone()
    X[:, :, 7] = 0
    T, N, Fd = X.shape
    W = N - 1
    w = X[:, :W]
    score = w[:, :, 1] + w[:, :, 2]  # run + wait
    kk = min(k, W)
    top = torch.topk(score, kk, dim=1).indices if kk > 0 else torch.zeros((T, 0), dtype=torch.long)
    sel = torch.gather(w, 1, top[:, :, None].expand(-1, -1, Fd)) if kk > 0 else torch.zeros((T, 0, Fd))
    rest = w.sum(1) - sel.sum(1) + X[:, W]  # everything not selected, plus the daemon's own aggregate node
    rest[:, 5:7] = X[:, 0, 5:7]; rest[:, 8] = 1.0; rest[:, 9:] = X[:, W, 9:]
    threads = torch.zeros((T, k + 1, Fd))
    threads[:, :kk] = sel; threads[:, k] = rest
    mask = torch.zeros((T, k + 1), dtype=torch.bool)
    mask[:, :kk] = score.gather(1, top) > 0 if kk > 0 else False
    mask[:, k] = True
    alive = (w[:, :, :3].abs().sum(-1) > 0).float()
    nthr = alive.sum(1)
    busy = (w[:, :, 1] > 0.1).float().sum(1)
    node_share = (w[:, :, 9:9 + NODES] * alive[:, :, None]).sum(1) / nthr.clamp(min=1)[:, None]
    a = torch.as_tensor(alloc, dtype=torch.float32)
    da = torch.cat([torch.zeros(1), a[1:] - a[:-1]])
    changed = (da != 0).float()
    since = torch.zeros(T); last = -10_000
    ch = changed.numpy()
    for t in range(T):
        if ch[t]:
            last = t
        since[t] = t - last
    recent = torch.from_numpy(np.convolve(ch, np.ones(100), "full")[:T].astype(np.float32))
    daemon = torch.stack([
        w[:, :, 0].sum(1), w[:, :, 1].sum(1), w[:, :, 2].sum(1), w[:, :, 1].max(1).values, w[:, :, 2].max(1).values,
        w[:, :, 3].sum(1), w[:, :, 4].sum(1), busy / 16, nthr / 100, X[:, 0, 5], X[:, 0, 6],
        X[:, W, 1], X[:, W, 2],
        a / 16, da / 4, torch.log1p(since) / 10, recent / 10,
    ], 1)
    daemon = torch.cat([daemon, node_share], 1)
    return threads, mask, daemon


DAEMON_F = 17 + NODES
# Cross-daemon context per other daemon on the route and tick, from cgroup values (rings in the sweeps, threadtrace
# live): busy cores / 16, pressure share, cores held / 16, change in cores / 4, busy / held.
CTX_F = 5
CTX_MAX = 9  # frontend's route has 10 daemons


class MLPBase(nn.Module):
    """No memory: P(t) from daemon(t) alone."""

    def __init__(self, d=64):
        super().__init__()
        self.net = nn.Sequential(nn.Linear(DAEMON_F, d), nn.GELU(), nn.Linear(d, d), nn.GELU(), nn.Linear(d, 1))

    def forward(self, threads, mask, daemon, state=None):
        return F.softplus(self.net(daemon)).squeeze(-1), None


class GRUBase(nn.Module):
    """Recurrent memory over daemon-level features."""

    def __init__(self, d=64, layers=2):
        super().__init__()
        self.inp = nn.Linear(DAEMON_F, d)
        self.gru = nn.GRU(d, d, layers, batch_first=True)
        self.out = nn.Linear(d, 1)

    def forward(self, threads, mask, daemon, state=None):  # daemon [B, T, DF]
        h, state = self.gru(F.gelu(self.inp(daemon)), state)
        return F.softplus(self.out(h)).squeeze(-1), state


class ThreadSetEncoder(nn.Module):
    """One attention layer over a tick's thread tokens, pooled (mean of real tokens, and max) into one tick vector."""

    def __init__(self, d, heads):
        super().__init__()
        self.inp = nn.Linear(17, d)
        self.att = nn.MultiheadAttention(d, heads, batch_first=True)
        self.ln1, self.ln2 = nn.LayerNorm(d), nn.LayerNorm(d)
        self.ff = nn.Sequential(nn.Linear(d, 2 * d), nn.GELU(), nn.Linear(2 * d, d))

    def forward(self, threads, mask):  # threads [M, K+1, 17], mask [M, K+1]
        x = self.inp(threads)
        h = self.ln1(x)
        x = x + self.att(h, h, h, key_padding_mask=~mask, need_weights=False)[0]
        x = x + self.ff(self.ln2(x))
        m = mask[:, :, None].float()
        mean = (x * m).sum(1) / m.sum(1).clamp(min=1)
        mx = x.masked_fill(~mask[:, :, None], -1e4).max(1).values
        return torch.cat([mean, mx], -1)


class Block(nn.Module):
    def __init__(self, d, heads, drop):
        super().__init__()
        self.heads = heads
        self.ln1, self.ln2 = nn.LayerNorm(d), nn.LayerNorm(d)
        self.qkv = nn.Linear(d, 3 * d)
        self.proj = nn.Linear(d, d)
        self.ff = nn.Sequential(nn.Linear(d, 4 * d), nn.GELU(), nn.Linear(4 * d, d))
        self.drop = drop

    def forward(self, x, bias):  # x [B, L, d], bias [H, L, L] additive (causal + ALiBi; -inf masks)
        B, L, d = x.shape
        q, k, v = self.qkv(self.ln1(x)).view(B, L, 3, self.heads, d // self.heads).permute(2, 0, 3, 1, 4)
        a = F.scaled_dot_product_attention(q, k, v, attn_mask=bias[None], dropout_p=self.drop if self.training else 0.0)
        x = x + self.proj(a.transpose(1, 2).reshape(B, L, d))
        return x + self.ff(self.ln2(x))


def alibi_slopes(h):
    return torch.tensor([2 ** (-8 * (i + 1) / h) for i in range(h)])


class MemoryTransformer(nn.Module):
    """Causal transformer over the last `fine` ticks (10 ms each), preceded by `mem` coarse memory tokens, each the
    mean of `bucket` ticks' daemon features (use, pressure and decisions) before the fine window.

    Training sees windows of fine ticks with their own coarse prefix; every fine position predicts its tick, attending
    to all coarse tokens (strictly older) and causally to fine tokens, with ALiBi distance penalties (coarse tokens
    count their age in ticks), so it generalises to other fine lengths at inference.
    """

    def __init__(self, d=128, layers=4, heads=4, fine=1024, mem=120, bucket=50, drop=0.1, ctx=False):
        super().__init__()
        self.fine, self.mem, self.bucket, self.heads, self.use_ctx = fine, mem, bucket, heads, ctx
        self.threads = ThreadSetEncoder(d, heads)
        self.tick = nn.Sequential(nn.Linear(2 * d + DAEMON_F, d), nn.GELU(), nn.Linear(d, d))
        if ctx:  # the tick token attends to the other daemons' context tokens (section 10 of RL_DESIGN.md)
            self.ctx_in = nn.Sequential(nn.Linear(CTX_F, d), nn.GELU(), nn.Linear(d, d))
            self.ctx_att = nn.MultiheadAttention(d, heads, batch_first=True)
            self.ctx_ln = nn.LayerNorm(d)
        self.coarse = nn.Sequential(nn.Linear(2 * DAEMON_F, d), nn.GELU(), nn.Linear(d, d))
        self.kind = nn.Parameter(torch.zeros(2, d))
        self.blocks = nn.ModuleList([Block(d, heads, drop) for _ in range(layers)])
        self.ln = nn.LayerNorm(d)
        self.out = nn.Linear(d, 1)
        self.register_buffer("slopes", alibi_slopes(heads), persistent=False)

    def coarse_tokens(self, daemon_hist):
        """daemon_hist [B, mem*bucket, DF] (zeros where before the case start) -> [B, mem, d], oldest first."""
        B = daemon_hist.shape[0]
        h = daemon_hist.view(B, self.mem, self.bucket, -1)
        return self.coarse(torch.cat([h.mean(2), h.amax(2)], -1)) + self.kind[1]

    def bias(self, L, device):
        M, Fn, b = self.mem, L, self.bucket
        pos = torch.cat([(-(M - torch.arange(M, device=device)) * b + b // 2).float(), torch.arange(Fn, device=device).float()])
        dist = (pos[M:, None] - pos[None, :]).clamp(min=0)  # queries are fine positions only
        allowed = torch.ones(Fn, M + Fn, dtype=torch.bool, device=device)
        allowed[:, M:] = torch.tril(torch.ones(Fn, Fn, dtype=torch.bool, device=device))
        bias = -self.slopes.to(device)[:, None, None] * dist[None]
        bias = bias.masked_fill(~allowed[None], float("-inf"))
        full = torch.zeros(self.heads, M + Fn, M + Fn, device=device)  # coarse rows attend to coarse only (causal)
        cm = torch.tril(torch.ones(M, M, dtype=torch.bool, device=device))
        full[:, :M, :M] = (-self.slopes.to(device)[:, None, None] * (pos[:M, None] - pos[None, :M]).clamp(min=0)).masked_fill(~cm[None], float("-inf"))
        full[:, :M, M:] = float("-inf")
        full[:, M:] = bias
        return full

    def forward(self, threads, mask, daemon, daemon_hist, ctx=None, ctx_mask=None):
        """threads [B, L, K+1, 17], mask [B, L, K+1], daemon [B, L, DF], daemon_hist [B, mem*bucket, DF],
        ctx [B, L, CTX_MAX, CTX_F] with ctx_mask [B, CTX_MAX] (True = a daemon) -> P [B, L]."""
        B, L = daemon.shape[:2]
        t = self.threads(threads.reshape(B * L, *threads.shape[2:]), mask.reshape(B * L, -1)).view(B, L, -1)
        tick = self.tick(torch.cat([t, daemon], -1))
        if self.use_ctx and ctx is not None and ctx_mask is not None and bool(ctx_mask.any()):
            M = ctx.shape[2]
            kv = self.ctx_in(ctx).reshape(B * L, M, -1)
            km = (~ctx_mask)[:, None, :].expand(B, L, M).reshape(B * L, M)
            has = ~km.all(-1)  # rows without any other daemon: attend to a dummy slot, then zeroed
            km = km.clone(); km[~has, 0] = False
            q = self.ctx_ln(tick).reshape(B * L, 1, -1)
            att = self.ctx_att(q, kv, kv, key_padding_mask=km, need_weights=False)[0]
            tick = tick + (att * has[:, None, None].to(att.dtype)).view(B, L, -1).to(tick.dtype)
        x = torch.cat([self.coarse_tokens(daemon_hist), tick + self.kind[0]], 1)
        bias = self.bias(L, x.device)
        for blk in self.blocks:
            x = blk(x, bias)
        return F.softplus(self.out(self.ln(x[:, self.mem:]))).squeeze(-1)


def n_params(m):
    return sum(p.numel() for p in m.parameters())
