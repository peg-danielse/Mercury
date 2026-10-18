# Records cgroup cpu.stat / cpu.pressure traces of spinner runs every 10 ms for policy_test.
# Run as root on the target host: python3 record_traces.py <spinner> <outdir>
import os,sys,time,subprocess
spin,out=sys.argv[1],sys.argv[2]
CG="/sys/fs/cgroup/mercury/ptrec"
def rd(p): return open(p).read()
def usage():
    d={}
    for l in rd(CG+"/cpu.stat").split("\n"):
        if l: k,v=l.split(); d[k]=int(v)
    return d
def some():
    for l in rd(CG+"/cpu.pressure").split("\n"):
        if l.startswith("some"):
            return int(l.split("total=")[1])
runs=[("1cpu_1thread_hold","8",1,1,100,"hold"),("1cpu_6thread_add","8",1,6,100,"add"),
      ("4cpu_1thread_remove","8-9,40-41",4,1,100,"remove"),("4cpu_4thread_hold","8-9,40-41",4,4,100,"hold"),
      ("4cpu_6thread_add","8-9,40-41",4,6,100,"add"),("4cpu_idle_remove","8-9,40-41",4,1,2,"remove"),
      ("2cpu_2thread_half_hold","8,40",2,2,90,"hold")]
os.makedirs(out,exist_ok=True)
for name,cpus,n,th,duty,want in runs:
    os.makedirs(CG,exist_ok=True)
    open(CG+"/cpuset.cpus","w").write(cpus)
    p=subprocess.Popen([spin,str(th),str(duty)],stdout=subprocess.DEVNULL)
    open(CG+"/cgroup.procs","w").write(str(p.pid))
    time.sleep(1.5)
    rows=[]
    u0=usage(); s0=some()
    t_next=time.monotonic()
    for i in range(200):
        t_next+=0.01
        time.sleep(max(0,t_next-time.monotonic()))
        u=usage(); s=some(); ts=time.monotonic_ns()
        rows.append((ts,(u["usage_usec"]-u0["usage_usec"])*1000,(u["user_usec"]-u0["user_usec"])*1000,
                     (u["system_usec"]-u0["system_usec"])*1000,(s-s0)*1000))
        u0,s0=u,s
    p.kill(); p.wait(); time.sleep(0.2); os.rmdir(CG)
    with open(f"{out}/{name}.csv","w") as f:
        f.write(f"# cores={n} threads={th} duty={duty} expect={want} cpuset={cpus}\n")
        f.write("# timestamp_ns,task_clock_ns,user_ns,system_ns,pressure_ns\n")
        for r in rows: f.write(",".join(map(str,r))+"\n")
    print(name,"recorded",len(rows))
