// Unit tests for the pure policy decisions in src/policy.cpp (policies.md).
// No root, no cgroups: build/test/policy_test exits 0 when every check holds.

#include "json.hpp"
#include "policy.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::cerr << "FAIL " << what << std::endl;
  } else {
    std::cout << "ok   " << what << std::endl;
  }
}

IntervalLoads loads(std::initializer_list<double> values) {
  IntervalLoads out;
  for (const double v : values) {
    out.emplace_back(v);
  }
  return out;
}

const std::vector<std::string> kNames = {"user", "nice", "system", "idle", "iowait", "irq", "softirq", "busy"};

Sample group_sample(std::int64_t t_ns, std::uint64_t busy, std::uint64_t idle) {
  Sample s;
  s.timestamp_ns = t_ns;
  s.nvalues = 8;
  s.value[3] = idle;
  s.value[7] = busy;
  return s;
}

}  // namespace

int main() {
  // interval_loads buckets 10 ms samples into five 1 s intervals.
  {
    std::vector<Sample> samples;
    const std::int64_t end = 100 * kIntervalNs;
    for (int i = 0; i < 500; ++i) {
      const std::int64_t t = end - kIntervalNs * 5 + (i + 1) * 10000000LL;
      const int second = i / 100;  // 0..4, oldest first
      const std::uint64_t busy = second >= 3 ? 9 : 1;
      samples.push_back(group_sample(t, busy, 10 - busy));
    }
    const auto got = interval_loads(kNames, samples, end);
    check(got.size() == 5, "interval_loads returns five intervals");
    check(got[0] && std::fabs(*got[0] - 0.1) < 1e-9, "oldest interval load 0.1");
    check(got[4] && std::fabs(*got[4] - 0.9) < 1e-9, "newest interval load 0.9");
    std::vector<Sample> sparse(samples.begin() + 200, samples.end());
    const auto missing = interval_loads(kNames, sparse, end);
    check(!missing[0] && !missing[1] && missing[2], "intervals with no samples are missing");
  }

  // reactive_move: 3 of the last 5 above 0.75.
  check(node_hot(loads({0.1, 0.8, 0.9, 0.2, 0.76})), "hotspot at 3 of 5 above 0.75");
  check(!node_hot(loads({0.1, 0.8, 0.9, 0.2, 0.75})), "0.75 itself is not above the cut");
  check(!node_hot(loads({0.9, 0.9, 0.9, 0.9})), "fewer than five intervals is cold");
  {
    auto gap = loads({0.9, 0.9, 0.9, 0.9, 0.9});
    gap[2].reset();
    check(!node_hot(gap), "a missing interval makes the node cold");
  }
  check(node_hot(loads({0.86, 0.86, 0.86, 0.1, 0.1}), 0.85), "combined hotspot cut 0.85");

  // scale_nodes_ind: mean above theta.
  check(node_overloaded(loads({0.9, 0.9, 0.7, 0.8, 0.85})), "mean 0.83 is above 0.8");
  check(!node_overloaded(loads({0.8, 0.8, 0.8, 0.8, 0.8})), "mean 0.8 is not above 0.8");
  {
    ShiftRule mean{ShiftTrigger::Mean, kHotspotUtil, kNodeTheta};
    ShiftRule either{ShiftTrigger::Either, kHotspotUtil, kNodeTheta};
    const auto spiky = loads({0.76, 0.76, 0.76, 0.1, 0.1});
    check(!shift_triggered(spiky, mean), "mean trigger ignores a short hotspot");
    check(shift_triggered(spiky, either), "either trigger fires on the hotspot");
  }

  // shift destination: least loaded legal node, tie on lower CPU.
  {
    std::vector<NodeCandidate> nodes = {
        {"node.0", 0.9, 0, true, true, 8},
        {"node.1", 0.3, 8, false, true, 8},
        {"node.2", 0.2, 16, false, false, 8},  // outside ceiling
        {"node.3", 0.3, 4, false, true, 8},
        {"node.4", 0.1, 24, false, true, 1},   // too small
    };
    const auto got = shift_destination("node.0", nodes, 2);
    check(got && *got == "node.3", "destination is least loaded legal node with lower CPU tie break");
    nodes[1].hot = true;
    nodes[3].hot = true;
    check(!shift_destination("node.0", nodes, 2), "no legal node returns nothing");
  }

  // least_allocated (planned baseline, decision only).
  {
    std::vector<GroupCandidate> groups = {{"l3.0", 4, 1, 0.1, 0}, {"l3.1", 4, 3, 0.5, 4}, {"l3.2", 4, 3, 0.2, 8}};
    const auto got = least_allocated(groups);
    check(got && *got == "l3.2", "least_allocated prefers unallocated share, then load");
  }

  // degrade_optional brownout.
  {
    std::vector<DegradeComponent> parts = {{"trace", 0.05, true}, {"recommend", 0.2, true}, {"thumbs", 0.1, true}};
    auto changed = degrade_step(parts, loads({0.9, 0.9, 0.9, 0.9, 0.95}));
    check(changed && *changed == 1 && !parts[1].enabled, "degrade disables the costliest component first");
    changed = degrade_step(parts, loads({0.9, 0.9, 0.9, 0.9, 0.95}));
    check(!changed, "0.95 - 0.2 is under 0.8, nothing more is disabled");
    changed = degrade_step(parts, loads({0.9, 0.9, 0.9, 0.9, 1.05}));
    check(changed && *changed == 2 && !parts[2].enabled, "next costliest is disabled when still over");
    changed = degrade_step(parts, loads({1.0, 1.0, 1.0, 1.0, 1.05}));
    check(!changed, "restore waits until all five intervals fit with the cost added back");
    changed = degrade_step(parts, loads({0.6, 0.6, 0.6, 0.6, 0.6}));
    check(changed && *changed == 2 && parts[2].enabled, "restore enables the cheapest disabled component");
    changed = degrade_step(parts, loads({0.5, 0.5, 0.5, 0.5, 0.5}));
    check(changed && *changed == 1 && parts[1].enabled, "then the next one");
    check(!degrade_step(parts, loads({0.5, 0.5, 0.5, 0.5, 0.5})), "nothing to restore");
  }

  // tune ladder.
  {
    std::vector<TuneParameter> params = {{"concurrency", {"1", "2", "4"}, 2}, {"cache", {"small", "big"}, 1}};
    auto changed = tune_step(params, loads({0.5, 0.5, 0.5, 0.5, 0.9}), false);
    check(changed && *changed == 0 && params[0].index == 1, "tune steps the first parameter down under load");
    changed = tune_step(params, loads({0.5, 0.5, 0.5, 0.5, 0.5}), true);
    check(changed && params[0].index == 0, "tune steps down while degraded");
    changed = tune_step(params, loads({0.5, 0.5, 0.5, 0.5, 0.5}), true);
    check(changed && *changed == 1 && params[1].index == 0, "then the next parameter");
    changed = tune_step(params, loads({0.5, 0.5, 0.5, 0.5, 0.5}), false);
    check(changed && *changed == 0 && params[0].index == 1, "tune steps up when calm");
  }

  // json reader used by POST /policies.
  {
    const auto doc = parse_json(R"({"daemon_id":"a","shift":{"trigger":"mean","theta":0.8},
      "degrade":{"components":[{"name":"x","cost":0.2}]},"tune":{"parameters":[{"name":"c","values":[1,2]}]}})");
    check(doc && doc->is_object(), "json parses a policy document");
    check(doc && doc->get("shift")->get("theta")->number == 0.8, "json nested number");
    check(doc && doc->get("degrade")->get("components")->items.size() == 1, "json nested array");
    check(!parse_json(R"({"a":1,"a":2})"), "json rejects duplicate keys");
    check(!parse_json(R"({"a":1} trailing)"), "json rejects trailing text");
  }

  // zeroctl: model actions are validated before Mercury applies them.
  {
    ZeroctlView view;
    view.move_level = "node";
    view.max_cpus = 6;
    view.groups["node.0"] = {"node", 0.95, 3};
    view.groups["node.1"] = {"node", 0.20, 4};
    view.groups["node.2"] = {"node", 1.00, 4};
    view.groups["node.3"] = {"node", 0.10, 1};
    view.groups["core.5"] = {"core", 0.0, 1};
    view.daemons["svc0"] = {{"socket.0", "node.0", "core.1"}, 2, {{"extra", true}, {"trace", false}}};
    auto act = [](std::string action, std::string workload, std::string node = "", std::string component = "",
                  int cpus = 0) {
      ZeroctlAction a;
      a.action = std::move(action);
      a.workload = std::move(workload);
      a.node = std::move(node);
      a.component = std::move(component);
      a.cpus = cpus;
      return a;
    };
    check(validate_zeroctl(act("none", ""), view).empty(), "zeroctl none is always legal");
    check(validate_zeroctl(act("move", "svc0", "node.1"), view).empty(), "zeroctl move to a cool node");
    check(!validate_zeroctl(act("move", "svc0", "node.0"), view).empty(), "zeroctl rejects move onto own node");
    check(!validate_zeroctl(act("move", "svc0", "node.2"), view).empty(), "zeroctl rejects move onto a full node");
    check(!validate_zeroctl(act("move", "svc0", "node.3"), view).empty(), "zeroctl rejects a node too small");
    check(!validate_zeroctl(act("move", "svc0", "core.5"), view).empty(), "zeroctl rejects the wrong level");
    check(!validate_zeroctl(act("move", "svc0", "node.9"), view).empty(), "zeroctl rejects an unknown group");
    check(!validate_zeroctl(act("move", "mercury", "node.1"), view).empty(), "zeroctl rejects an unknown daemon");
    check(validate_zeroctl(act("scale_out", "svc0", "", "", 3), view).empty(), "zeroctl scale_out in range");
    check(!validate_zeroctl(act("scale_out", "svc0", "", "", 7), view).empty(), "zeroctl scale_out above max");
    check(!validate_zeroctl(act("scale_out", "svc0", "", "", 0), view).empty(), "zeroctl scale_out to zero");
    check(validate_zeroctl(act("degrade", "svc0", "", "extra"), view).empty(), "zeroctl degrade enabled component");
    check(!validate_zeroctl(act("degrade", "svc0", "", "trace"), view).empty(), "zeroctl degrade already off");
    check(validate_zeroctl(act("restore", "svc0", "", "trace"), view).empty(), "zeroctl restore disabled component");
    check(!validate_zeroctl(act("degrade", "svc0", "", "db"), view).empty(), "zeroctl rejects a critical part");
    check(!validate_zeroctl(act("add_worker", "svc0"), view).empty(), "zeroctl rejects actions Mercury lacks");
  }

  if (failures != 0) {
    std::cerr << failures << " policy checks failed" << std::endl;
    return 1;
  }
  std::cout << "all policy checks passed" << std::endl;
  return 0;
}
