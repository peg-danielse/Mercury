# Sourced by dsb_ab.sh, dsb_seed.sh, and compare_systems.sh. $M is the Mercury root.
# benchmarks/perturb/perturb/hosts/$(hostname -s).env sets this machine's Linux CPUs.
# HOST_ENV selects another file. Names the file leaves unset keep the dtn-01 defaults.
# POOL may be a cpuset (1-28, 1,2,5-9) or a space-separated list. Afterwards it is a
# word list, which is what mercury's argv accepts.
HOST_ENV=${HOST_ENV:-$M/benchmarks/perturb/perturb/hosts/$(hostname -s).env}
[ -f "$HOST_ENV" ] && . "$HOST_ENV"
POOL=${POOL:-$(seq 1 60)}
GEN_CPU=${GEN_CPU:-60}
OP_CPU=${OP_CPU:-61}
TRACE_CPUS=${TRACE_CPUS:-62,63}
SAMPLE_CPU=${SAMPLE_CPU:-63}
expand_cpus() {
  local spec=${1//,/ } tok a b
  for tok in $spec; do
    if [[ $tok == *-* ]]; then
      a=${tok%%-*}; b=${tok#*-}
      seq "$a" "$b"
    else
      printf '%s\n' "$tok"
    fi
  done
}
POOL=$(expand_cpus "$POOL" | paste -sd' ' -)
