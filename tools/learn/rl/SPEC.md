# RL training for the Mercury controller

## Implemented library

The current library is the "RL training" section of `SPEC.md`. It names `build_actor`, `MlpActor`, `GnnActor`, `TransformerActor`, `attention_mask`, `TwinCritic`, `observe`, `encode_call`, `decode_action`, `clone`, `Replay`, `TD3Trainer.update`, `constrain_actions`, `step_reward`, `TrainConfig`, `StudentTrainer`, `CorrectiveQueue`, `student_act`, and `decide` (`control.py`). `decide` returns the `hold` or `scale_daemon` call `execute` posts. `ActionModel` in `actions.py` (`mlp`, `gnn`, or `transformer`) scores every teacher tool, and `choose` returns the call `execute` posts. `rl_episode` uses `choose` when the checkpoint's `kind` is `action`.

`settings.py` is the evaluation switch. The seven names are `llm`, `gnn-imitation`, `tf-imitation`, `gnn-rl`, `tf-rl`, `gnn-imitation-rl`, and `tf-imitation-rl`. `train.py` `fit` trains a scale name. `fit_action` trains `mlp-action`, `gnn-action`, and `tf-action` by cloning every accepted teacher call in the turn. `save_controller` stores a scale name in the checkpoint. `save_action_model` stores an action checkpoint. `llmctl.py --llm rl` loads that file and posts `decide`'s call through `execute`. `compare_systems.sh` selects the file with provider `rl`. `cycle.py` `cycle_loop` (`src/test/experiments/action_cycle.sh`) runs that checkpoint on Mercury at the teacher's cadence (0.5 s check, 1000 ms window, `llmctl.py --gate`), scores each of its decisions by the reward change that followed, relative to other decisions in the same load phase (`decision_rows`, `advantage`, `phase_baseline`), clones the better ones, avoids the worse and refused ones, and trains from the best round's weights on half teacher rows and half student rows balanced across tools (`mix_probs`). The tool head reads the pool and latency context (`context_features`, `scene_logits`). The action heads only offer calls Mercury accepts (`legal_tools`, `legal_cpus`, `legal_logits`); `would_refuse` and `refusal_rate` check that offline.

`curve.py` (`src/test/experiments/rl_curve.sh`) trains the four RL names on a two-process plant and writes `reward` and the performance terms at every scored step. The plant calls `step_reward`. It does not start Mercury.

The sections from "Goal" downward are the earlier live-controller contract (one discrete action, Double DQN). That contract is not what `--llm rl` runs.

## Earlier contract

**PROPOSED, NOT IMPLEMENTED.** This is the training contract for one shared policy that chooses Mercury placement actions and learns from the measured result. The teacher is the tool-using LLM controller (`tools/llmctl.py`, Luna: `openai/gpt-6-luna`). The teacher bootstraps the replay buffer. It is not a ground-truth label.

The core-count models (`tools/learn/evalset/`, `llmctl.py --llm remote`, `remote_cycle`) are a different controller. They return a core count. This policy returns one action and `execute` applies it.

## Goal

One set of weights across hotel, social, and media, and across the workload schedules those runs already use. The policy reads Mercury, picks one feasible action, and `execute` posts it. The reward is the requests that completed, their tail latency and errors, the pool it spent, and how disruptive the action was.

Luna stays in the loop only while collecting teacher transitions. Evaluation runs the learned policy with exploration off, one local forward pass per decision.

## Loop

A live run is `dsb_ab.sh`: Mercury, the DeathStar operator, the load generator, and `llmctl.py`. There is no second simulator. One RL episode is one such run at one schedule.

Three clocks stay separate:

- Observation window. `Mercury.state` is `GET /llm/state?window_ms=1000`, the same window `trouble` reads. `Latency.summary` (`tools/llmctl.py`) adds end-to-end latency from `loadgen.hist.jsonl`.
- Decision period. The first value is 5 s. The actor may read state faster. It posts at most one placement action per period.
- Outcome horizon. The same 5 s. The reward compares the horizon after the post with the window before it. The next decision waits until that horizon has elapsed, so a slow `swap_domain`, `shrink_pool`, or replica start is not charged to the following action. `since` on the next `GET /llm/state` (the cursor from the decision) is the placement record for that horizon.

`hold` is a real decision. It posts nothing and still waits the horizon.

`trouble` keeps gating Luna. It does not gate the learned policy. The policy may `hold` on a quiet window; that is how it learns not to move.

## Observation

`observe` returns the state `opening_message` already shows the model, plus the run config the state does not carry.

From `GET /llm/state`: host, `pool` (`start`, `cores`, `max`, `busy`, `pressure`, `allocated`, `free_domains`), each L3 domain (`load`, `cpus`, `free`, `daemons`, `in_pool`, `foreign_cpus`, `ipc` when present), each daemon (`cpus`, `cores`, `cpu_use`, `occupancy`, `pressure`, `in_pool`, `set`, `replica`, `replica_of`), `cursor`, and `since` (`events`, `l3_history`).

From `Latency.summary`: `now.p50_ms`, `now.p95_ms`, `now.p99_ms`, `now.req_per_s` (completions), `now.error_rate` (errors / `sent`), `reference_p95_ms`, `p95_history_ms`. With `--critical`, the same split for `critical` and `optional`. `reference_p95_ms` is the 20th percentile of recent one-second p95s. It moves when the policy does. It is a feature. It is not the SLO in the reward.

From the run, because `/llm/state` does not have them: application, offered-rate schedule and the level now in force, `p95_slo_ms`, `error_budget`, pool start and budget, whether `--replicas` and `--shed` are on, and the previous action. The encoder's first trunk is `encode` in `tools/learn/distill/train.py`: 16 domain tokens, up to 32 service tokens, one global vector (`GLB_F`). A graph encoder waits until this loop has a learning curve.

## Actions

Each candidate is the object `execute` already posts through `run_tool`. The action record stores that object, not only the name.

| Action | Body | Mask |
|---|---|---|
| `hold` | `{}` | always legal |
| `scale_daemon` | `daemon_id`, `cpus` | `scale_cpus`: a non-zero integer, absolute value at most 64. Load generator and jaeger refused. First experiment scores `cpus` in `{+1,+2,+4,-1,-2}` per eligible daemon. The full `CPU_VALUES` range stays available once that set learns. |
| `grow_pool` | `l3` (`auto` or a free domain) | no candidate when `pool_at_budget` is true |
| `shrink_pool` | `l3` (a pooled domain or `auto`) | never below `pool.start` |
| `swap_domain` | `from_l3`, `to_l3` | `from` in the pool, `to` a free domain |
| `move_daemon` | `daemon_id`, `from_l3`, `to_l3` | not the load generator; `from` is a pool domain the daemon occupies; `to` is another pool domain with a free CPU |
| `add_replica` | `daemon_id` | only when the run was started with `MERCURY_REUSEPORT=1` and the actor was passed `--replicas`. One process, id `<original>-rN` from 2, at most `kMaxReplicaSet` (8). The new process starts on the pool CPU the fewest daemons hold. |
| `remove_replica` | `daemon_id` | same gate. The id is a daemon with `replica` true. `replica_act` resolves `replica_of` and stops the highest N. It never stops the original. |

`add_replica` candidates are running services that are not themselves replicas, excluding:

- any daemon whose id starts with `hotel-` (the Go servers bind their own port; `libreuseport.so` does not cover them, so a replica accepts nothing);
- social `UniqueIdService` (one MAC, so two processes can emit the same id in one millisecond);
- the load generator, jaeger, and any id or `exec_path` containing `mongod`, `redis`, `rabbitmq`, or `memcached`.

Hotel runs therefore have an empty replica set. Social and media C++ services are the ones that can enter it. A replica candidate is masked, not offered and then punished, when the run is not a reuseport run.

`shed_route` and `restore_route` stay out of the first experiment. They exist only with `--shed`, and shedding gives up a route. They join the candidate generator in a later stage, with the same rules as `run_tool`: a shed is refused while `pool_at_budget` is false, and only a route in `--shed` can be shed.

`valid_actions(state)` builds this list and is the only set the scorer may pick from. A 409 or a timeout is still logged. A timeout may have landed; the next observation is the record of what landed. An unexecuted suggestion from Luna is not an action.

The first online stage scores `hold` and the small `scale_daemon` set. `move_daemon`, `swap_domain`, `grow_pool`, and `shrink_pool` join after that policy beats the behavior-cloned scaler on the held-out runs. `add_replica` and `remove_replica` join with them, on social and media only. They have no teacher label in the Luna logs collected before the replica tools were offered (4387 episodes, 104 `openai/gpt-6-luna` logs, zero of either call), so behavior cloning cannot warm-start them. Their first gradient is the measured reward.

## Reward

Weights are starting hypotheses. The log stores every term and the raw metrics beside `reward`.

Over the outcome horizon, from the histogram windows `Latency` already aggregates:

```text
goodput_fraction   = min(done / max(sent, 1), 1)
latency_violation  = max(p95_ms / p95_slo_ms - 1, 0)
error_fraction     = errors / max(sent, 1)
utilization        = pool.busy          # daemon cpu_use / cpus when the run has no pool
resource_fraction  = pool.cores / pool.max
```

`p95_ms` is `latency.critical` when the run names critical routes, otherwise `latency.now`. `p95_slo_ms` and `error_budget` are fields of the run config, chosen from a fixed-pool baseline of that application and schedule, and written into the checkpoint. The utilization bonus is on only when `latency_violation == 0` and `error_fraction <= error_budget`. The starting error budget is 0.01, the same bar `critical_outcome` uses in `tools/learn/evalset/build_luna.py`.

```text
reward = + 1.0 * goodput_fraction
         - 2.0 * clip(latency_violation, 0, 2)
         - 3.0 * error_fraction
         + 0.2 * utilization * bonus
         - 0.2 * resource_fraction
         - 0.1 * action_cost
```

`action_cost` is 0 for `hold`, 0.2 for `scale_daemon`, 0.5 for `move_daemon`, 0.6 for `grow_pool` and `shrink_pool`, 0.8 for `swap_domain`, and 1 for `add_replica` and `remove_replica`. A post that returns `ok` false keeps its cost and gets no credit for a change that did not happen.

Busy CPUs raise the reward only inside the SLO. A larger pool lowers it. Completions, the p95, and the error rate are what a checkpoint is accepted or rejected on. The scalar is for the update.

## Training

**Teacher rollouts.** Luna, `SC_LLM_OWNS=1`, `--max-turns 1`, the tools she has today, including `add_replica` and `remove_replica` on social and media. Each transition is one accepted `execute`. A refused call is stored as a failure, not as a successful label. Luna's later calls in the same episode are their own transitions when each one has its own before and after window. The existing distill set (`encode` keeps the first accepted call only) is the behavior-cloning source for the actions she has taken. It is not the RL replay.

Teacher transitions are collected ahead of time and mixed into the replay buffer. Luna is not called on the learner's decision step. A model call of several seconds would land after the horizon it was meant to act on.

**Behavior cloning.** `tools/learn/distill/train.py` (`--arch mlp|gru|tf`) remains the warm start. The Q network reuses that trunk. Its head scores candidate actions instead of emitting one class. Replica outputs stay masked until replica transitions exist. No synthetic replica labels.

**Double DQN.** `tools/learn/rl/train.py`, on pd-gpu with `~/gnn-env`. The network takes the encoded state and one candidate and returns one Q. The actor scores every survivor of `valid_actions` and takes the argmax. Target network, replay, epsilon-greedy, invalid actions removed before the max. Teacher transitions stay in the buffer; new learner transitions are added online and the teacher fraction is annealed. Checkpoints, seed, reward weights, `p95_slo_ms`, `error_budget`, and the candidate rules are stored with the replay.

**Actor.** `llmctl.py --llm rl --model <checkpoint>`. Same `execute` path as `distilled_episode`. Epsilon is 0 unless the run is marked an exploration run. One action, then the horizon.

**Safety, on the actor before the post.** The masks above. At most one replica action per horizon. If `error_fraction` stays above 0.10 for three horizons, epsilon goes to 0 and the actor follows the behavior-cloned policy until the run ends. Exploration runs are `dsb_ab.sh` compare runs on dtn-01.

**Generalization.** The held-out unit is a whole run: an application, a schedule, and an antagonist kind (`interfere.py`: `same`, `smt`, `l3`, `membw`, `load`) that the update never saw. Random seconds from a training run are not a test set. dtn-01's topology is fixed (16 L3 domains, CPU 0 is Mercury's). What varies is the application, the offered-rate schedule, the initial pool (start 8, budget 40, against a fixed pool), and the antagonist. Hotel never supplies a replica gradient.

## Logging

`llmctl.py` keeps writing `llmctl.jsonl`. A decision adds one record:

```json
{"event":"rl","episode_id":"...","step":12,"env_config_id":"...","actor":"llm|rl",
 "action":{"name":"add_replica","daemon_id":"social-UserService"},
 "valid_actions":["hold",{"name":"scale_daemon","daemon_id":"...","cpus":1}],
 "ok":true,"reward":0.34,"terms":{},"metrics_before":{},"metrics_after":{},
 "elapsed_ms":4,"horizon_s":5,"done":false}
```

The `state` event already written at a decision is the state reference. `terms` is the six reward components. `metrics_before` and `metrics_after` are the raw `Latency.summary` and pool fields, so a rising reward with a worse p95 is visible.

## First experiment

1. Collect teacher runs of hotel, social, and media on one short schedule, Luna deciding, social and media started with `SC_REPLICAS=1` and `MERCURY_REUSEPORT=1`. Hold out one repeat of each application. Replay single-action stretches through the reward and check that the before and after windows move when the tool's `ok` is true and stay put when it is false.
2. Score three policies on that hold-out, matched schedule and seed, exploration off: `hold` on the starting pool, Luna, and the behavior-cloned policy (`--llm distilled`).
3. Online Double DQN on the training repeats. Scale first. Then pool and move actions, and replica actions on social and media. A checkpoint every 20 episodes.
4. Each checkpoint on the hold-out, epsilon 0. Report p95, error rate, completions per second, pool cores, actions per phase, and how often `add_replica` / `remove_replica` were legal, chosen, and accepted. The scalar reward is plotted and is not the acceptance test.

The first bar is a higher hold-out return than behavior cloning, with an error rate and a p95 that are no worse and without more placement posts per phase. Matching Luna's p95 and errors with a local forward pass is the bar after that.

## Build order

1. `tools/learn/rl/env.py`: `observe`, `valid_actions`, `step`, the reward, and the `rl` log record. `step` calls `execute` and waits the horizon.
2. Offline pass of existing `llmctl.jsonl` through that reward, single accepted action per horizon only.
3. Initialize the Q trunk from a `distill/train.py` checkpoint.
4. Replay-buffer Double DQN and `--llm rl`, exploration only on compare runs.
5. A graph encoder, and `shed_route` / `restore_route`, after that curve exists.
