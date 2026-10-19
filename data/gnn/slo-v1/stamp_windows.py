"""Stamp operator window lines with CLOCK_MONOTONIC (same clock as threadtrace and dsb_sample).
python3 stamp_windows.py <run_root> [--stop FILE]
Polls every 50 ms for <run_root>/**/operate.out; every new line is appended as "<mono_ns>\t<line>" to stamps.tsv next to
that operate.out. Exits when FILE exists (default <run_root>/.stamp_stop) or after 6 h."""
import glob, os, sys, time
root = sys.argv[1]
stop = sys.argv[sys.argv.index("--stop") + 1] if "--stop" in sys.argv else os.path.join(root, ".stamp_stop")
pos, buf, outs = {}, {}, {}
end = time.monotonic() + 6 * 3600
while time.monotonic() < end and not os.path.exists(stop):
    for f in glob.glob(os.path.join(root, "**", "operate.out"), recursive=True):
        try:
            with open(f, errors="replace") as h:
                h.seek(pos.get(f, 0)); data = h.read(); pos[f] = h.tell()
        except OSError:
            continue
        if not data:
            continue
        now = time.monotonic_ns()
        s = buf.get(f, "") + data; lines = s.split("\n"); buf[f] = lines[-1]
        if f not in outs:
            outs[f] = open(os.path.join(os.path.dirname(f), "stamps.tsv"), "a")
        for l in lines[:-1]:
            outs[f].write(f"{now}\t{l}\n")
        outs[f].flush()
    time.sleep(0.05)
