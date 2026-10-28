#!/bin/bash
# Hotel batch: 14 LLMs x 3 repeats, then single runs of saturate (8 CPUs), l3, l3shift, the distilled
# transformer, and saturate on a 40-CPU pool. Resumable: finished runs (DONE) are skipped.
M=$(cd "$(dirname "$0")/../../.." && pwd)
export OPENROUTER_API_KEY="$(cat $HOME/.config/mercury/openrouter.key)"
COMMON="RATE=100 DB=seed SEED_N=1 PHASES=12 WARMUP=30 OFF=20 ON=30 CRITICAL=hotels,reservation ROUTE=hotels,reservation,recommendations,review,restaurants,museums,cinema LLM_KEY_VARS=OPENROUTER_API_KEY"
LLMS="ministral3b=mistralai/ministral-3b-2512@openrouter:low,gemma4b=google/gemma-3-4b-it@openrouter:low,qwen35=qwen/qwen3.5-9b@openrouter:low,qwen38=qwen/qwen3.8-27b@openrouter:low,qwencoder=qwen/qwen3-coder-next@openrouter:low,qwen36=qwen/qwen3.6-35b-a3b@openrouter:low,gptoss=openai/gpt-oss-120b@openrouter:low,mimo=xiaomi/mimo-v2.6-flash@openrouter:low,deepseek=deepseek/deepseek-v4.1-flash@openrouter:low,glm=z-ai/glm-5.3-flash@openrouter:low,luna=openai/gpt-6-luna@openrouter:low,haiku=anthropic/claude-haiku-5.5@openrouter:low,gemini38=google/gemini-3.8-flash@openrouter:low,gemini4=google/gemini-4-argon@openrouter:low"
ALIASES="ministral3b gemma4b qwen35 qwen38 qwencoder qwen36 gptoss mimo deepseek glm luna haiku gemini38 gemini4"
SYS_LLMS=$(for a in $ALIASES; do printf 'llm25-%s ' $a; done)

echo "$(date -Is) batch A: 14 LLMs x 3 repeats"
env $COMMON REPEATS=3 POOL_START=8 LLM_MODELS="$LLMS" SYSTEMS="$SYS_LLMS" \
  $M/src/test/compare_systems.sh $M/data/dsb/compare/hotel-llms-20261027 hotel

echo "$(date -Is) batch B: saturate, l3, l3shift, distilled transformer"
env $COMMON REPEATS=1 POOL_START=8 LLMCTL_PYTHON=$HOME/opt/llmpy \
  LLM_MODELS="distill=$M/models/distill-v2-noshed.pt@distilled" \
  SYSTEMS="saturate l3 l3shift llm25-distill" \
  $M/src/test/compare_systems.sh $M/data/dsb/compare/hotel-single-20261027 hotel

echo "$(date -Is) batch C: saturate on a 40-CPU pool"
env $COMMON REPEATS=1 POOL_START=40 SYSTEMS="saturate" \
  $M/src/test/compare_systems.sh $M/data/dsb/compare/hotel-fixed40-20261027 hotel

echo "$(date -Is) all batches finished"
