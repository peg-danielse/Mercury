#!/bin/bash
# Social at 150 req/s: saturate, l3shift and the Luna controller, one repeat each. Waits for any running
# comparison to finish first (the runs share ports).
M=$(cd "$(dirname "$0")/../../.." && pwd)
while pgrep -f "[c]ompare_systems.sh" >/dev/null; do sleep 20; done
export OPENROUTER_API_KEY="$(cat $HOME/.config/mercury/openrouter.key)"
env RATE=150 REPEATS=1 DB=seed POOL_START=8 SEED_N=1 PHASES=12 WARMUP=30 OFF=20 ON=30 \
  CRITICAL=compose-post,read-home-timeline,read-user-timeline \
  ROUTE=compose-post,read-home-timeline,read-user-timeline,follow \
  LLM_KEY_VARS=OPENROUTER_API_KEY LLM_MODELS="luna=openai/gpt-6-luna@openrouter:low" \
  SYSTEMS="saturate l3shift llm25-luna" \
  $M/src/test/compare_systems.sh $M/data/dsb/compare/social-150-20261027 social
echo "$(date -Is) social 150 finished"
