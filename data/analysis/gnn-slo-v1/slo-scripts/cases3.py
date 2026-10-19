"""Training/held-out case list for gnn-slo-v1 (DESIGN.md v3). python slo/cases3.py <sweep_dir> <labels_dir> > cases.txt
Prints one case=<trace>@<windows.json>@<daemon>[@test] per line and the remote file list (for rsync) on stderr-free mode --files."""
import glob, os, sys
root, lab = sys.argv[1], sys.argv[2]; files = "--files" in sys.argv
LIGHT = ["11211", "11212", "11213", "11214", "frontend"]; SKIP = ("mongod", "loadgen")  # 50 ms traces: not used for training
TEST_SVC = ("geo", "review")
ALL = "attractions geo profile rate recommendation reservation review search user".split() + LIGHT
for run in sorted(os.listdir(root)):
    if run == "warmup" or run.startswith("ref-"):
        continue
    ts = glob.glob(os.path.join(root, run, "*", "1-saturate"))
    if not ts:
        continue
    d = ts[0]
    if run.startswith("G-"):
        progs, test = ALL, run == "G-p2-c"
    elif run.startswith("light-k1"):
        progs, test = LIGHT, False
    else:
        s = run.rsplit("-k", 1)[0]
        if s in SKIP:
            continue
        progs, test = [s], run.endswith("-p2") and s in TEST_SVC
    for p in progs:
        t = os.path.join(d, f"trace-hotel-{p}.jsonl")
        if files:
            print(os.path.relpath(t, root)); continue
        print(f"case={t}@{os.path.join(lab, run + '.windows.json')}@hotel-{p}" + ("@test" if test else ""))
