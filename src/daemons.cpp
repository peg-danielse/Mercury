#include "daemons.hpp"

#include "gnn.hpp"

#include "http.hpp"
#include "policy.hpp"
#include "resources.hpp"
#include "ring.hpp"
#include "ring_archive.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

void raise_nofile() {
  rlimit limit{};
  if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    return;
  }
  auto target = limit.rlim_max;
  if (target == RLIM_INFINITY) {
    target = 65536;
  }
  if (limit.rlim_cur >= target) {
    return;
  }
  limit.rlim_cur = target;
  ::setrlimit(RLIMIT_NOFILE, &limit);
}

namespace {

constexpr std::string_view kJson = "application/json";

std::vector<int> daemon_cpus;
int mercury_linux_cpu = -1;

// arm_daemon attaches saturate (min 1, max 16) to every daemon it starts. /scale updates it.
struct ScaleState {
  bool enabled = false;
  int min_cores = 1;
  int max_cores = kDefaultMaxCores;
  std::shared_ptr<Policy> policy;
  std::int64_t changed_at_ns = 0;
  // CPU choice for add_core. shift and widen keep their own rules.
  CpuPlacement placement = CpuPlacement::LeastLoaded;
  // gnn policy (SPEC.md): monitor_daemon runs a GnnController every gnn_tick_ms instead of saturate.
  bool gnn = false;
  int gnn_tick_ms = 10;
  bool gnn_pin = true;  // false: gnn only sizes the cpuset; every thread keeps the cpuset-wide mask
};

}  // namespace
// Defined beside scale_json (outside the anonymous namespace).
std::shared_ptr<const GnnWeights> gnn_weights(std::string& error);
namespace {

struct PolicyDoc {
  bool shift = false;
  bool grant_on = false;
  bool widen = false;
  ResourceLevel level = ResourceLevel::Core;
  std::string ceiling;
  int max_groups = 0;  // 0 means the default: every group at the level inside the ceiling
  std::vector<DegradeComponent> components;
};

struct PolicyState {
  bool attached = false;
  std::uint64_t generation = 0;
  PolicyDoc doc;
  std::vector<std::string> grant;
  // Parallel to grant: when the cpuset was first seen holding none of that group's CPUs (0 = holds some).
  std::vector<std::int64_t> grant_empty_since;
  std::vector<std::string> disabled;
  std::int64_t attached_at_ns = 0;
  std::int64_t shifted_at_ns = 0;
  std::int64_t widened_at_ns = 0;
  std::int64_t degraded_at_ns = 0;
  std::string path;
};

struct Daemon {
  std::string daemon_id;
  std::string exec_path;
  pid_t pid = -1;
  bool running = false;
  int exit_code = -1;
  std::string log;
  std::optional<std::int64_t> finished;
  ScaleState scale;
  int cpu = 0;
  int listen_fd = -1;
  bool counters_done = false;
  // Extra environment for the exec'd process. Empty keeps the inherited environment.
  std::vector<std::pair<std::string, std::string>> env;
  // policies.md document attached through POST /policies. Default is no document.
  PolicyState policy;
  // cgroup cpu.stat ring published by monitor_daemon; read by policy_loop outside daemons_mu.
  std::shared_ptr<Ring> cgroup_ring;
};

std::mutex daemons_mu;
std::condition_variable daemons_cv;
std::vector<Daemon> daemons;
std::uint64_t policy_generation = 0;  // under daemons_mu

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Daemon* find_daemon(const std::string& daemon_id) {
  for (auto& daemon : daemons) {
    if (daemon.daemon_id == daemon_id) {
      return &daemon;
    }
  }
  return nullptr;
}

bool daemon_running(const std::string& daemon_id, pid_t pid) {
  std::lock_guard lock(daemons_mu);
  const auto* daemon = find_daemon(daemon_id);
  return daemon != nullptr && daemon->pid == pid && daemon->running;
}

std::string ok_json(bool ok) { return ok ? R"({"ok":true})" : R"({"ok":false})"; }

std::string kill_json(std::int64_t finished) {
  return "{\"ok\":true,\"finished\":" + std::to_string(finished) + "}";
}

std::string request_body(const std::string& request) {
  const auto header_end = request.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    return {};
  }
  return request.substr(header_end + 4);
}

bool read_json_string(const std::string& json, std::size_t open, std::string& out, std::size_t& close) {
  if (open >= json.size() || json[open] != '"') {
    return false;
  }
  out.clear();
  for (std::size_t i = open + 1; i < json.size(); ++i) {
    if (json[i] == '\\') {
      if (i + 1 >= json.size()) {
        return false;
      }
      const char next = json[++i];
      switch (next) {
        case '"':
        case '\\':
        case '/':
          out.push_back(next);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        default:
          return false;
      }
      continue;
    }
    if (json[i] == '"') {
      close = i;
      return true;
    }
    if (static_cast<unsigned char>(json[i]) < 0x20) {
      return false;
    }
    out.push_back(json[i]);
  }
  return false;
}

// Index of the colon for a key on the outermost object. Keys inside nested objects are ignored.
std::optional<std::size_t> top_level_colon(const std::string& json, std::string_view key) {
  int depth = 0;
  for (std::size_t i = 0; i < json.size();) {
    const char c = json[i];
    if (c == '"') {
      std::string text;
      std::size_t close = 0;
      if (!read_json_string(json, i, text, close)) {
        return std::nullopt;
      }
      std::size_t after = close + 1;
      while (after < json.size() && std::isspace(static_cast<unsigned char>(json[after])) != 0) {
        ++after;
      }
      if (depth == 1 && text == key && after < json.size() && json[after] == ':') {
        return after;
      }
      i = close + 1;
      continue;
    }
    if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      if (depth == 0) {
        return std::nullopt;
      }
      --depth;
    }
    ++i;
  }
  return std::nullopt;
}

std::optional<std::string> json_value(const std::string& json, std::string_view key) {
  const auto colon = top_level_colon(json, key);
  if (!colon) {
    return std::nullopt;
  }
  auto at = *colon + 1;
  while (at < json.size() && std::isspace(static_cast<unsigned char>(json[at])) != 0) {
    ++at;
  }
  if (at >= json.size()) {
    return std::nullopt;
  }
  if (json[at] == '"') {
    std::string value;
    std::size_t close = 0;
    if (!read_json_string(json, at, value, close)) {
      return std::nullopt;
    }
    return value;
  }
  const auto end = json.find_first_of(",}", at);
  auto token = json.substr(at, end == std::string::npos ? std::string::npos : end - at);
  while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())) != 0) {
    token.pop_back();
  }
  return token;
}

bool valid_env_name(const std::string& name) {
  if (name.empty() || (std::isalpha(static_cast<unsigned char>(name.front())) == 0 && name.front() != '_')) {
    return false;
  }
  for (const unsigned char c : name) {
    if (std::isalnum(c) == 0 && c != '_') {
      return false;
    }
  }
  return true;
}

// Minimal JSON tree for the nested /policies document.
struct JNode {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string text;
  std::vector<JNode> items;
  std::vector<std::pair<std::string, JNode>> fields;

  const JNode* get(std::string_view key) const {
    for (const auto& [name, value] : fields) {
      if (name == key) {
        return &value;
      }
    }
    return nullptr;
  }
};

class JParser {
 public:
  explicit JParser(const std::string& json) : json_(json) {}

  bool parse(JNode& out) {
    if (!value(out, 0)) {
      return false;
    }
    skip();
    return at_ == json_.size();
  }

 private:
  void skip() {
    while (at_ < json_.size() && std::isspace(static_cast<unsigned char>(json_[at_])) != 0) {
      ++at_;
    }
  }

  bool literal(std::string_view word) {
    if (json_.compare(at_, word.size(), word) != 0) {
      return false;
    }
    at_ += word.size();
    return true;
  }

  bool value(JNode& out, int depth) {
    if (depth > 16) {
      return false;
    }
    skip();
    if (at_ >= json_.size()) {
      return false;
    }
    const char c = json_[at_];
    if (c == '"') {
      std::size_t close = 0;
      out.type = JNode::Type::String;
      if (!read_json_string(json_, at_, out.text, close)) {
        return false;
      }
      at_ = close + 1;
      return true;
    }
    if (c == '{') {
      out.type = JNode::Type::Object;
      ++at_;
      skip();
      if (at_ < json_.size() && json_[at_] == '}') {
        ++at_;
        return true;
      }
      while (true) {
        skip();
        std::string key;
        std::size_t close = 0;
        if (!read_json_string(json_, at_, key, close)) {
          return false;
        }
        at_ = close + 1;
        skip();
        if (at_ >= json_.size() || json_[at_] != ':') {
          return false;
        }
        ++at_;
        JNode child;
        if (!value(child, depth + 1)) {
          return false;
        }
        for (const auto& field : out.fields) {
          if (field.first == key) {
            return false;  // duplicate key
          }
        }
        out.fields.emplace_back(std::move(key), std::move(child));
        skip();
        if (at_ < json_.size() && json_[at_] == ',') {
          ++at_;
          continue;
        }
        if (at_ < json_.size() && json_[at_] == '}') {
          ++at_;
          return true;
        }
        return false;
      }
    }
    if (c == '[') {
      out.type = JNode::Type::Array;
      ++at_;
      skip();
      if (at_ < json_.size() && json_[at_] == ']') {
        ++at_;
        return true;
      }
      while (true) {
        JNode child;
        if (!value(child, depth + 1)) {
          return false;
        }
        out.items.push_back(std::move(child));
        skip();
        if (at_ < json_.size() && json_[at_] == ',') {
          ++at_;
          continue;
        }
        if (at_ < json_.size() && json_[at_] == ']') {
          ++at_;
          return true;
        }
        return false;
      }
    }
    if (literal("true")) {
      out.type = JNode::Type::Bool;
      out.boolean = true;
      return true;
    }
    if (literal("false")) {
      out.type = JNode::Type::Bool;
      return true;
    }
    if (literal("null")) {
      out.type = JNode::Type::Null;
      return true;
    }
    const auto begin = at_;
    while (at_ < json_.size() && (std::isdigit(static_cast<unsigned char>(json_[at_])) != 0 || json_[at_] == '-' ||
                                  json_[at_] == '+' || json_[at_] == '.' || json_[at_] == 'e' || json_[at_] == 'E')) {
      ++at_;
    }
    if (at_ == begin) {
      return false;
    }
    try {
      std::size_t used = 0;
      const auto token = json_.substr(begin, at_ - begin);
      out.number = std::stod(token, &used);
      if (used != token.size()) {
        return false;
      }
    } catch (const std::exception&) {
      return false;
    }
    out.type = JNode::Type::Number;
    return true;
  }

  const std::string& json_;
  std::size_t at_ = 0;
};

bool level_from(const std::string& name, ResourceLevel& level) {
  for (const auto candidate : {ResourceLevel::Core, ResourceLevel::L3, ResourceLevel::Node, ResourceLevel::Socket}) {
    if (name == resource_level_name(candidate)) {
      level = candidate;
      return true;
    }
  }
  return false;
}

// Parse the shape. Group names are checked later against the tree. Returns an error text or empty.
std::string parse_policy_doc(const JNode& root, PolicyDoc& doc, bool& empty) {
  empty = true;
  for (const auto& [key, value] : root.fields) {
    if (key == "daemon_id" || key == "exec_path") {
      if (value.type != JNode::Type::String) {
        return key + " is not a string";
      }
      continue;
    }
    if (key == "shift") {
      if (value.type != JNode::Type::Bool) {
        return "shift is not a boolean";
      }
      doc.shift = value.boolean;
      empty = false;
      continue;
    }
    if (key == "restrict") {
      if (value.type != JNode::Type::Object) {
        return "restrict is not an object";
      }
      empty = false;
      doc.grant_on = true;
      for (const auto& [rkey, rvalue] : value.fields) {
        if (rkey == "level") {
          if (rvalue.type != JNode::Type::String || !level_from(rvalue.text, doc.level)) {
            return "unknown restrict.level";
          }
        } else if (rkey == "ceiling") {
          if (rvalue.type != JNode::Type::String || rvalue.text.empty()) {
            return "restrict.ceiling is not a group name";
          }
          doc.ceiling = rvalue.text;
        } else {
          return "unknown field restrict." + rkey;
        }
      }
      continue;
    }
    if (key == "widen") {
      empty = false;
      if (value.type == JNode::Type::Bool) {
        doc.widen = value.boolean;
        doc.grant_on = doc.grant_on || value.boolean;
        continue;
      }
      if (value.type != JNode::Type::Object) {
        return "widen is not an object";
      }
      doc.widen = true;
      doc.grant_on = true;
      for (const auto& [wkey, wvalue] : value.fields) {
        if (wkey != "max_groups") {
          return "unknown field widen." + wkey;
        }
        if (wvalue.type != JNode::Type::Number || wvalue.number < 1 || wvalue.number > 4096 ||
            wvalue.number != static_cast<double>(static_cast<int>(wvalue.number))) {
          return "widen.max_groups is not a positive integer";
        }
        doc.max_groups = static_cast<int>(wvalue.number);
      }
      continue;
    }
    if (key == "degrade") {
      empty = false;
      if (value.type != JNode::Type::Object) {
        return "degrade is not an object";
      }
      for (const auto& [dkey, dvalue] : value.fields) {
        if (dkey != "components") {
          return "unknown field degrade." + dkey;
        }
        if (dvalue.type != JNode::Type::Array) {
          return "degrade.components is not an array";
        }
        for (const auto& item : dvalue.items) {
          if (item.type != JNode::Type::Object) {
            return "degrade component is not an object";
          }
          DegradeComponent component;
          bool have_name = false;
          bool have_cost = false;
          for (const auto& [ckey, cvalue] : item.fields) {
            if (ckey == "name") {
              if (cvalue.type != JNode::Type::String || !valid_env_name(cvalue.text)) {
                return "bad component name";
              }
              component.name = cvalue.text;
              have_name = true;
            } else if (ckey == "cost") {
              if (cvalue.type != JNode::Type::Number || !(cvalue.number > 0) || cvalue.number > 1) {
                return "component cost is outside (0, 1]";
              }
              component.cost = cvalue.number;
              have_cost = true;
            } else {
              return "unknown component field " + ckey;
            }
          }
          if (!have_name || !have_cost) {
            return "component needs name and cost";
          }
          for (const auto& seen : doc.components) {
            if (seen.name == component.name) {
              return "duplicate component " + component.name;
            }
          }
          doc.components.push_back(std::move(component));
        }
      }
      continue;
    }
    return "unknown field " + key;
  }
  return {};
}

struct EnvRequest {
  bool present = false;
  bool ok = true;
  std::vector<std::pair<std::string, std::string>> vars;
};

// "env" is an object of string values. Absent means the daemon inherits Mercury's environment unchanged.
EnvRequest parse_env(const std::string& json) {
  EnvRequest result;
  const auto colon = top_level_colon(json, "env");
  if (!colon) {
    return result;
  }
  result.present = true;
  std::size_t i = *colon + 1;
  while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])) != 0) {
    ++i;
  }
  if (i >= json.size() || json[i] != '{') {
    result.ok = false;
    return result;
  }
  ++i;
  while (true) {
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])) != 0) {
      ++i;
    }
    if (i < json.size() && json[i] == '}') {
      return result;
    }
    std::string name;
    std::size_t close = 0;
    if (!read_json_string(json, i, name, close) || !valid_env_name(name)) {
      result.ok = false;
      return result;
    }
    i = close + 1;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])) != 0) {
      ++i;
    }
    if (i >= json.size() || json[i] != ':') {
      result.ok = false;
      return result;
    }
    ++i;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])) != 0) {
      ++i;
    }
    std::string value;
    if (!read_json_string(json, i, value, close)) {
      result.ok = false;
      return result;
    }
    i = close + 1;
    bool replaced = false;
    for (auto& entry : result.vars) {
      if (entry.first == name) {
        entry.second = std::move(value);
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      result.vars.emplace_back(std::move(name), std::move(value));
    }
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])) != 0) {
      ++i;
    }
    if (i < json.size() && json[i] == ',') {
      ++i;
      continue;
    }
    if (i < json.size() && json[i] == '}') {
      return result;
    }
    result.ok = false;
    return result;
  }
}

std::string read_pipes(int out_fd, int err_fd) {
  std::string log;
  pollfd fds[2]{};
  fds[0].fd = out_fd;
  fds[0].events = POLLIN;
  fds[1].fd = err_fd;
  fds[1].events = POLLIN;
  int open_count = 2;
  while (open_count > 0) {
    if (poll(fds, 2, -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    for (auto& fd : fds) {
      if (fd.fd < 0 || (fd.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        continue;
      }
      char buffer[1024];
      const auto n = ::read(fd.fd, buffer, sizeof(buffer));
      if (n > 0) {
        log.append(buffer, static_cast<std::size_t>(n));
      } else {
        ::close(fd.fd);
        fd.fd = -1;
        --open_count;
      }
    }
  }
  return log;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

std::string rings_dir() {
  if (const char* env = std::getenv("MERCURY_RINGS")) {
    if (env[0] != '\0') {
      return env;
    }
  }
  return "rings";
}

std::string archive_name(const std::string& daemon_id) {
  std::string safe;
  for (const unsigned char c : daemon_id) {
    if (std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-') {
      safe.push_back(static_cast<char>(c));
    } else {
      safe.push_back('_');
    }
  }
  if (safe.empty()) {
    safe = "daemon";
  }
  return safe;
}

std::string trimmed(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

// cpuset list: "4", "0-4", or a comma-separated mix such as "0-2,5".
std::optional<std::vector<int>> parse_cpuset(std::string_view text) {
  std::vector<int> cpus;
  if (text.empty()) {
    return std::nullopt;
  }
  std::size_t i = 0;
  auto read_cpu = [&](int& cpu) {
    if (i >= text.size() || std::isdigit(static_cast<unsigned char>(text[i])) == 0) {
      return false;
    }
    if (text[i] == '0' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1])) != 0) {
      return false;
    }
    int value = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
      const int digit = text[i] - '0';
      if (value > (CPU_SETSIZE - digit) / 10) {
        return false;
      }
      value = value * 10 + digit;
      ++i;
    }
    if (value < 0 || value >= CPU_SETSIZE) {
      return false;
    }
    cpu = value;
    return true;
  };
  while (i < text.size()) {
    if (!cpus.empty()) {
      if (text[i] != ',') {
        return std::nullopt;
      }
      ++i;
    }
    int first = 0;
    if (!read_cpu(first)) {
      return std::nullopt;
    }
    int last = first;
    if (i < text.size() && text[i] == '-') {
      ++i;
      if (!read_cpu(last) || last < first) {
        return std::nullopt;
      }
    }
    for (int cpu = first; cpu <= last; ++cpu) {
      cpus.push_back(cpu);
    }
  }
  if (cpus.empty()) {
    return std::nullopt;
  }
  std::sort(cpus.begin(), cpus.end());
  cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
  return cpus;
}

bool controller_enabled(const std::string& text, std::string_view name) {
  std::size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])) != 0) {
      ++at;
    }
    if (at >= text.size()) {
      break;
    }
    std::size_t end = at;
    while (end < text.size() && std::isspace(static_cast<unsigned char>(text[end])) == 0) {
      ++end;
    }
    if (text.compare(at, end - at, name) == 0) {
      return true;
    }
    at = end;
  }
  return false;
}

bool write_sysfs(const std::string& path, std::string_view value) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return false;
  }
  std::string data(value);
  if (data.empty() || data.back() != '\n') {
    data.push_back('\n');
  }
  const auto n = ::write(fd, data.data(), data.size());
  const int saved = errno;
  ::close(fd);
  errno = saved;
  return n == static_cast<ssize_t>(data.size());
}

bool ensure_cpuset_controller(const std::string& subtree_control) {
  if (controller_enabled(read_file(subtree_control), "cpuset")) {
    return true;
  }
  if (write_sysfs(subtree_control, "+cpuset")) {
    return true;
  }
  return controller_enabled(read_file(subtree_control), "cpuset");
}

// cpuset.cpus exists on the daemon cgroup only when cpuset is enabled on both ancestors.
std::optional<std::string> ensure_daemon_cpuset(const std::string& daemon_id) {
  const std::string parent = "/sys/fs/cgroup/mercury";
  if (::mkdir(parent.c_str(), 0755) != 0 && errno != EEXIST) {
    return std::nullopt;
  }
  if (!ensure_cpuset_controller("/sys/fs/cgroup/cgroup.subtree_control")) {
    return std::nullopt;
  }
  if (!ensure_cpuset_controller(parent + "/cgroup.subtree_control")) {
    return std::nullopt;
  }
  const std::string dir = parent + "/" + daemon_id;
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    return std::nullopt;
  }
  return dir;
}

bool cpuset_applied(const std::string& dir, const std::vector<int>& want) {
  const auto configured = parse_cpuset(trimmed(read_file(dir + "/cpuset.cpus")));
  const auto effective = parse_cpuset(trimmed(read_file(dir + "/cpuset.cpus.effective")));
  return configured && effective && *configured == want && *effective == want;
}

bool pin_pid(pid_t pid, int linux_cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (linux_cpu < 0 || linux_cpu >= CPU_SETSIZE) {
    return false;
  }
  CPU_SET(linux_cpu, &set);
  return ::sched_setaffinity(pid, sizeof(set), &set) == 0;
}

bool widen_to_online() {
  const auto online = parse_cpuset(trimmed(read_file("/sys/devices/system/cpu/online")));
  if (!online || online->empty()) {
    return false;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  for (const int cpu : *online) {
    if (cpu >= 0 && cpu < CPU_SETSIZE && cpu != mercury_linux_cpu) {
      CPU_SET(cpu, &set);
    }
  }
  return ::sched_setaffinity(0, sizeof(set), &set) == 0;
}

struct Placement {
  std::string dir;
  int pin_linux = -1;
};

std::string node_list(const std::vector<int>& nodes) {
  std::string out;
  for (const int node : nodes) {
    if (!out.empty()) {
      out += ",";
    }
    out += std::to_string(node);
  }
  return out;
}

// cpuset.mems follows cpuset.cpus: the NUMA nodes whose CPUs the cpuset uses.
// Order keeps every intermediate state valid: mems grows to old|new before
// cpus is written, and shrinks to new only after.
const std::vector<std::pair<int, std::vector<int>>>& numa_node_cpus() {
  static const std::vector<std::pair<int, std::vector<int>>> nodes = [] {
    std::vector<std::pair<int, std::vector<int>>> out;
    for (int node = 0; node < 1024; ++node) {
      const auto text = trimmed(read_file("/sys/devices/system/node/node" + std::to_string(node) + "/cpulist"));
      if (text.empty()) {
        if (node > 64) {
          break;
        }
        continue;
      }
      if (auto cpus = parse_cpuset(text)) {
        out.emplace_back(node, *cpus);
      }
    }
    return out;
  }();
  return nodes;
}

std::vector<int> nodes_for_cpus(const std::vector<int>& cpus) {
  std::vector<int> nodes;
  for (const auto& [node, node_cpus] : numa_node_cpus()) {
    for (const int cpu : cpus) {
      if (std::find(node_cpus.begin(), node_cpus.end(), cpu) != node_cpus.end()) {
        nodes.push_back(node);
        break;
      }
    }
  }
  return nodes;
}

bool write_cpus_and_mems(const std::string& dir, const std::string& cpus_spec) {
  const auto want = parse_cpuset(trimmed(cpus_spec));
  const auto nodes = want ? nodes_for_cpus(*want) : std::vector<int>{};
  if (nodes.empty()) {
    return write_sysfs(dir + "/cpuset.cpus", cpus_spec);
  }
  std::vector<int> grown = nodes;
  if (const auto old = parse_cpuset(trimmed(read_file(dir + "/cpuset.mems")))) {
    grown.insert(grown.end(), old->begin(), old->end());
    std::sort(grown.begin(), grown.end());
    grown.erase(std::unique(grown.begin(), grown.end()), grown.end());
  }
  if (!write_sysfs(dir + "/cpuset.mems", node_list(grown))) {
    std::cerr << "cpuset.mems " << dir << " grow to " << node_list(grown) << " failed" << std::endl;
  }
  if (!write_sysfs(dir + "/cpuset.cpus", cpus_spec)) {
    return false;
  }
  if (grown != nodes && !write_sysfs(dir + "/cpuset.mems", node_list(nodes))) {
    std::cerr << "cpuset.mems " << dir << " shrink to " << node_list(nodes) << " failed" << std::endl;
  }
  return true;
}

// The cgroup is restricted before the new process is allowed to widen its mask.
std::optional<Placement> prepare_placement(const std::string& daemon_id, int linux_cpu) {
  if (linux_cpu < 0 || linux_cpu == mercury_linux_cpu) {
    return std::nullopt;
  }
  const auto dir = ensure_daemon_cpuset(daemon_id);
  if (!dir) {
    return std::nullopt;
  }
  const auto current = trimmed(read_file(*dir + "/cpuset.cpus"));
  if (current.empty()) {
    if (!write_cpus_and_mems(*dir, std::to_string(linux_cpu))) {
      return std::nullopt;
    }
    const std::vector<int> want{linux_cpu};
    bool applied = false;
    for (int attempt = 0; attempt < 50; ++attempt) {
      if (cpuset_applied(*dir, want)) {
        applied = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!applied) {
      return std::nullopt;
    }
    return Placement{*dir, linux_cpu};
  }
  const auto have = parse_cpuset(current);
  if (!have || have->empty()) {
    return std::nullopt;
  }
  return Placement{*dir, have->front()};
}

bool move_into_cgroup(const std::string& dir, pid_t pid) {
  return write_sysfs(dir + "/cgroup.procs", std::to_string(pid));
}

bool safe_daemon_id(const std::string& daemon_id) {
  if (daemon_id.empty() || daemon_id.size() > 255 || daemon_id == "." || daemon_id == "..") {
    return false;
  }
  for (const unsigned char c : daemon_id) {
    if (std::isalnum(c) == 0 && c != '.' && c != '_' && c != '-') {
      return false;
    }
  }
  return true;
}

void mark_counters_done(const std::string& daemon_id, pid_t pid) {
  {
    std::lock_guard lock(daemons_mu);
    if (auto* daemon = find_daemon(daemon_id); daemon != nullptr && daemon->pid == pid) {
      daemon->counters_done = true;
    }
  }
  daemons_cv.notify_all();
}

void watch_daemon(std::string daemon_id, pid_t pid, int out_fd, int err_fd) {
  const auto log = read_pipes(out_fd, err_fd);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  const auto finished = now_ms();
  {
    std::lock_guard lock(daemons_mu);
    if (auto* daemon = find_daemon(daemon_id); daemon != nullptr && daemon->pid == pid) {
      daemon->running = false;
      daemon->exit_code = exit_code;
      daemon->log = log;
      daemon->finished = finished;
    }
  }
  daemons_cv.notify_all();
  std::cout << "exited daemon_id=" << daemon_id << " exit_code=" << exit_code << " finished=" << finished
            << " log=" << log << std::endl;
}

}  // namespace

void set_daemon_cpus(std::vector<int> cpus) {
  std::sort(cpus.begin(), cpus.end());
  daemon_cpus = std::move(cpus);
}

void set_mercury_cpu(int cpu) { mercury_linux_cpu = cpu; }

std::optional<std::string> stat_route(const std::string& request) {
  if (request_target(request) != "/stat") {
    return std::nullopt;
  }
  const auto exec_path = json_value(request_body(request), "exec_path");
  if (!exec_path || exec_path->empty()) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  bool running = false;
  {
    std::lock_guard lock(daemons_mu);
    for (const auto& daemon : daemons) {
      if (daemon.exec_path == *exec_path && daemon.running) {
        running = true;
        break;
      }
    }
  }
  return http_response(200, "OK", ok_json(running), kJson);
}

int bind_loopback(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  const int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || ::listen(fd, 16) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

struct Launched {
  pid_t pid = -1;
  int out_fd = -1;
  int err_fd = -1;
};

[[noreturn]] void exec_daemon(const std::string& exec_path, int listen_fd,
                            const std::vector<std::pair<std::string, std::string>>& env) {
  std::vector<char> arg0(exec_path.begin(), exec_path.end());
  arg0.push_back('\0');
  char flag[] = "--listen-fd";
  char fd_text[] = "3";
  char* with_socket[] = {arg0.data(), flag, fd_text, nullptr};
  char* plain[] = {arg0.data(), nullptr};
  char** argv = listen_fd >= 0 ? with_socket : plain;
  if (env.empty()) {
    ::execv(exec_path.c_str(), argv);
    _exit(127);
  }
  extern char** environ;
  std::vector<std::string> owned;
  for (char** it = environ; it != nullptr && *it != nullptr; ++it) {
    owned.emplace_back(*it);
  }
  for (const auto& [name, value] : env) {
    const auto prefix = name + "=";
    bool replaced = false;
    for (auto& entry : owned) {
      if (entry.compare(0, prefix.size(), prefix) == 0) {
        entry = prefix + value;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      owned.push_back(prefix + value);
    }
  }
  std::vector<char*> envp;
  envp.reserve(owned.size() + 1);
  for (auto& entry : owned) {
    envp.push_back(entry.data());
  }
  envp.push_back(nullptr);
  ::execve(exec_path.c_str(), argv, envp.data());
  _exit(127);
}

namespace {

// Signal-safe registry of the daemon cgroups this mercury created, for kill_daemon_cgroups_raw.
// A slot is filled under g_cgroup_mu and published by its atomic flag; slots are never cleared,
// so a cgroup whose root pid exited is still killed (descendants may remain) and removed.
constexpr int kCgroupSlots = 1024;
constexpr std::size_t kCgroupPathMax = 320;  // "/sys/fs/cgroup/mercury/" + a 255-byte daemon_id
char g_cgroup_dirs[kCgroupSlots][kCgroupPathMax];
std::atomic<bool> g_cgroup_used[kCgroupSlots];
std::mutex g_cgroup_mu;  // only for insert, never in the signal handler

void remember_cgroup(const std::string& dir) {
  if (dir.size() + std::strlen("/cgroup.kill") >= kCgroupPathMax) {
    std::cout << "cgroup path too long to clean up on exit: " << dir << std::endl;
    return;
  }
  std::lock_guard lock(g_cgroup_mu);
  for (int i = 0; i < kCgroupSlots; ++i) {
    if (g_cgroup_used[i].load(std::memory_order_acquire) && dir == g_cgroup_dirs[i]) {
      return;
    }
  }
  for (int i = 0; i < kCgroupSlots; ++i) {
    if (!g_cgroup_used[i].load(std::memory_order_acquire)) {
      std::memcpy(g_cgroup_dirs[i], dir.c_str(), dir.size() + 1);
      g_cgroup_used[i].store(true, std::memory_order_release);
      return;
    }
  }
  std::cout << "cgroup registry full; not cleaned up on exit: " << dir << std::endl;
}

}  // namespace

void kill_daemon_cgroups_raw() {
  char path[kCgroupPathMax];
  bool any = false;
  for (int i = 0; i < kCgroupSlots; ++i) {
    if (!g_cgroup_used[i].load(std::memory_order_acquire)) {
      continue;
    }
    const std::size_t n = std::strlen(g_cgroup_dirs[i]);
    std::memcpy(path, g_cgroup_dirs[i], n);
    std::memcpy(path + n, "/cgroup.kill", sizeof("/cgroup.kill"));
    const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
      const auto written = ::write(fd, "1", 1);
      (void)written;
      ::close(fd);
      any = true;
    }
  }
  if (!any) {
    return;
  }
  // rmdir succeeds once the kernel has reaped every task (EBUSY until then). Give up after 2 s.
  const timespec pause{0, 10 * 1000 * 1000};
  for (int attempt = 0; attempt < 200; ++attempt) {
    bool busy = false;
    for (int i = 0; i < kCgroupSlots; ++i) {
      if (g_cgroup_used[i].load(std::memory_order_acquire) && ::rmdir(g_cgroup_dirs[i]) != 0 && errno == EBUSY) {
        busy = true;
      }
    }
    if (!busy) {
      return;
    }
    ::nanosleep(&pause, nullptr);
  }
}

std::optional<Launched> launch(const std::string& daemon_id, const std::string& exec_path, int cpu, int listen_fd,
                               const std::vector<std::pair<std::string, std::string>>& env) {
  const int linux_cpu = cpu;
  const auto placement = prepare_placement(daemon_id, linux_cpu);
  if (placement) {
    remember_cgroup(placement->dir);
  }
  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  int sync_pipe[2] = {-1, -1};
  if (::pipe(out_pipe) != 0) {
    return std::nullopt;
  }
  if (::pipe(err_pipe) != 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    return std::nullopt;
  }
  if (placement && ::pipe(sync_pipe) != 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    return std::nullopt;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    if (sync_pipe[0] >= 0) {
      ::close(sync_pipe[0]);
      ::close(sync_pipe[1]);
    }
    return std::nullopt;
  }
  if (pid == 0) {
    if (sync_pipe[1] >= 0) {
      ::close(sync_pipe[1]);
    }
    if (sync_pipe[0] >= 0) {
      char byte = 0;
      ssize_t n = 0;
      do {
        n = ::read(sync_pipe[0], &byte, 1);
      } while (n < 0 && errno == EINTR);
      ::close(sync_pipe[0]);
      if (n != 1 || byte != '1') {
        _exit(127);
      }
    }
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(err_pipe[1], STDERR_FILENO);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    if (listen_fd >= 0) {
      if (listen_fd != 3 && ::dup2(listen_fd, 3) < 0) {
        _exit(127);
      }
      ::syscall(__NR_close_range, 4U, ~0U, 0);
    } else {
      ::syscall(__NR_close_range, 3U, ~0U, 0);
    }

    if (placement) {
      if (!widen_to_online()) {
        _exit(127);
      }
    } else {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(linux_cpu, &set);
      if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
        _exit(127);
      }
    }
    raise_nofile();
    exec_daemon(exec_path, listen_fd, env);
  }

  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  if (placement) {
    ::close(sync_pipe[0]);
    bool ready = pin_pid(pid, placement->pin_linux) && move_into_cgroup(placement->dir, pid);
    const char go = '1';
    if (ready) {
      ssize_t n = 0;
      do {
        n = ::write(sync_pipe[1], &go, 1);
      } while (n < 0 && errno == EINTR);
      ready = n == 1;
    }
    ::close(sync_pipe[1]);
    if (!ready) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, nullptr, 0);
      ::close(out_pipe[0]);
      ::close(err_pipe[0]);
      return std::nullopt;
    }
    std::cout << "scheduled daemon_id=" << daemon_id << " cpuset=" << placement->pin_linux
              << " mask=online" << std::endl;
  }
  return Launched{pid, out_pipe[0], err_pipe[0]};
}

bool image_is(pid_t pid, const std::string& exec_path) {
  char buf[4096];
  const auto link = "/proc/" + std::to_string(pid) + "/exe";
  const auto n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
  if (n < 0) {
    return false;
  }
  return std::string(buf, static_cast<std::size_t>(n)) == exec_path;
}

bool wait_until_image(pid_t pid, const std::string& exec_path) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (image_is(pid, exec_path)) {
      return true;
    }
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return image_is(pid, exec_path);
}

static std::int64_t monotonic_ns() {
  timespec now{};
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

std::string format_cpuset(const std::vector<int>& cpus) {
  std::string out;
  for (std::size_t i = 0; i < cpus.size();) {
    const int first = cpus[i];
    int last = first;
    std::size_t j = i + 1;
    while (j < cpus.size() && cpus[j] == last + 1) {
      last = cpus[j];
      ++j;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out += std::to_string(first);
    if (last != first) {
      out.push_back('-');
      out += std::to_string(last);
    }
    i = j;
  }
  return out;
}

int cpuset_cores(const std::string& daemon_id) {
  const auto have = parse_cpuset(trimmed(read_file("/sys/fs/cgroup/mercury/" + daemon_id + "/cpuset.cpus")));
  if (!have) {
    return 0;
  }
  return static_cast<int>(have->size());
}

// grant_cpus, when set, is every CPU of the daemon's granted groups (policies.md). The exclude list is
// then the cpuset plus every allocatable CPU outside the grant.
bool add_core(const std::string& daemon_id, int max_cores, int& cores_after, std::string& spec_after,
              const std::vector<int>* grant_cpus = nullptr, CpuPlacement placement = CpuPlacement::LeastLoaded) {
  const auto dir = ensure_daemon_cpuset(daemon_id);
  if (!dir) {
    return false;
  }
  auto have = parse_cpuset(trimmed(read_file(*dir + "/cpuset.cpus")));
  if (!have || have->empty() || static_cast<int>(have->size()) >= max_cores) {
    return false;
  }
  // Least-loaded core. Another daemon may already run on the CPU it returns.
  std::vector<int> exclude;
  if (grant_cpus != nullptr) {
    for (const int cpu : allocatable_cpus()) {
      if (!std::binary_search(grant_cpus->begin(), grant_cpus->end(), cpu)) {
        exclude.push_back(cpu);
      }
    }
  }
  auto next = request_cpu_for(placement, *have, exclude);
  if (!next) {
    return false;
  }
  have->push_back(*next);
  std::sort(have->begin(), have->end());
  const auto spec = format_cpuset(*have);
  if (!write_cpus_and_mems(*dir, spec)) {
    return false;
  }
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (cpuset_applied(*dir, *have)) {
      cores_after = static_cast<int>(have->size());
      spec_after = spec;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool remove_core(const std::string& daemon_id, int min_cores, int& cores_after, std::string& spec_after) {
  const auto dir = ensure_daemon_cpuset(daemon_id);
  if (!dir) {
    return false;
  }
  auto have = parse_cpuset(trimmed(read_file(*dir + "/cpuset.cpus")));
  if (!have || have->size() <= 1 || static_cast<int>(have->size()) <= min_cores) {
    return false;
  }
  have->pop_back();
  const auto spec = format_cpuset(*have);
  if (!write_cpus_and_mems(*dir, spec)) {
    return false;
  }
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (cpuset_applied(*dir, *have)) {
      cores_after = static_cast<int>(have->size());
      spec_after = spec;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

// cpu.stat is the kernel total for every task in the daemon cgroup.
struct CgroupCpu {
  std::uint64_t usage_usec = 0;
  std::uint64_t user_usec = 0;
  std::uint64_t system_usec = 0;
};

std::optional<CgroupCpu> read_cgroup_cpu(const std::string& daemon_id) {
  const auto text = read_file("/sys/fs/cgroup/mercury/" + daemon_id + "/cpu.stat");
  if (text.empty()) {
    return std::nullopt;
  }
  CgroupCpu cpu;
  bool saw = false;
  std::istringstream in(text);
  std::string key;
  std::uint64_t value = 0;
  while (in >> key >> value) {
    if (key == "usage_usec") {
      cpu.usage_usec = value;
      saw = true;
    } else if (key == "user_usec") {
      cpu.user_usec = value;
    } else if (key == "system_usec") {
      cpu.system_usec = value;
    }
  }
  if (!saw) {
    return std::nullopt;
  }
  return cpu;
}

// "some total" of the daemon cgroup cpu.pressure, in microseconds. Empty when PSI is unavailable.
std::optional<std::uint64_t> read_cgroup_pressure(const std::string& daemon_id) {
  const auto text = read_file("/sys/fs/cgroup/mercury/" + daemon_id + "/cpu.pressure");
  const auto line = text.find("some ");
  if (line == std::string::npos) {
    return std::nullopt;
  }
  const auto total = text.find("total=", line);
  const auto end = text.find('\n', line);
  if (total == std::string::npos || (end != std::string::npos && total > end)) {
    return std::nullopt;
  }
  try {
    return static_cast<std::uint64_t>(std::stoull(text.substr(total + 6)));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::vector<pid_t> child_pids_of(pid_t pid) {
  std::vector<pid_t> children;
  std::istringstream in(read_file("/proc/" + std::to_string(pid) + "/task/" + std::to_string(pid) + "/children"));
  pid_t child = 0;
  while (in >> child) {
    children.push_back(child);
  }
  return children;
}

// Fallback for hosts without PSI: threads in state R across the daemon and its descendants.
std::uint64_t count_runnable(pid_t root) {
  std::uint64_t runnable = 0;
  std::vector<pid_t> todo{root};
  for (std::size_t n = 0; n < todo.size() && n < 4096; ++n) {
    const pid_t pid = todo[n];
    const auto dir = "/proc/" + std::to_string(pid) + "/task";
    if (DIR* tasks = ::opendir(dir.c_str())) {
      while (const auto* entry = ::readdir(tasks)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
          continue;
        }
        const auto stat = read_file(dir + "/" + entry->d_name + "/stat");
        const auto close = stat.rfind(')');
        if (close != std::string::npos && close + 2 < stat.size() && stat[close + 2] == 'R') {
          ++runnable;
        }
      }
      ::closedir(tasks);
    }
    for (const pid_t child : child_pids_of(pid)) {
      todo.push_back(child);
    }
  }
  return runnable;
}

std::uint64_t usec_delta_ns(std::uint64_t after, std::uint64_t before) {
  if (after < before) {
    return 0;
  }
  return (after - before) * 1000ULL;
}

void monitor_daemon(const std::string& daemon_id, pid_t root) {
  // PSI decides the fourth counter for the life of this daemon: pressure, or runnable without PSI.
  const bool use_psi = read_cgroup_pressure(daemon_id).has_value();
  const std::vector<std::string> daemon_names{"task-clock", "user", "system", use_psi ? "pressure" : "runnable"};
  std::shared_ptr<RingArchive> archive;
  try {
    archive = std::make_shared<RingArchive>(rings_dir() + "/" + archive_name(daemon_id) + ".mring", daemon_names);
  } catch (const std::exception& error) {
    std::cerr << "ring archive daemon_id=" << daemon_id << " " << error.what() << std::endl;
  }
  auto daemon_ring = std::make_shared<Ring>(kRingSlots, daemon_names, archive);
  {
    std::lock_guard lock(daemons_mu);
    if (auto* daemon = find_daemon(daemon_id); daemon != nullptr && daemon->pid == root) {
      daemon->cgroup_ring = daemon_ring;
    }
  }
  std::cout << "counters daemon_id=" << daemon_id << " cgroup " << daemon_names.back() << std::endl;
  auto previous_cpu = read_cgroup_cpu(daemon_id);
  auto previous_pressure = use_psi ? read_cgroup_pressure(daemon_id) : std::nullopt;
  std::unique_ptr<GnnController> gnn;  // only while the gnn policy is attached; its destructor restores affinity
  long gnn_ticks = 0, gnn_logged = 0, gnn_warned = 0;
  std::int64_t gnn_read_ns = 0, gnn_infer_ns = 0, gnn_act_ns = 0, gnn_cpuset_ns = 0, gnn_last_log = 0;
  try {
    while (daemon_running(daemon_id, root)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kSampleMs));
      if (!daemon_running(daemon_id, root)) {
        break;
      }
      const auto current_cpu = read_cgroup_cpu(daemon_id);
      const auto current_pressure = use_psi ? read_cgroup_pressure(daemon_id) : std::nullopt;
      const bool pressure_ok =
          !use_psi || (previous_pressure && current_pressure && *current_pressure >= *previous_pressure);
      if (previous_cpu && current_cpu && current_cpu->usage_usec >= previous_cpu->usage_usec && pressure_ok) {
        Sample total;
        total.timestamp_ns = monotonic_ns();
        total.nvalues = 4;
        total.value[0] = usec_delta_ns(current_cpu->usage_usec, previous_cpu->usage_usec);
        total.value[1] = usec_delta_ns(current_cpu->user_usec, previous_cpu->user_usec);
        total.value[2] = usec_delta_ns(current_cpu->system_usec, previous_cpu->system_usec);
        total.value[3] = use_psi ? usec_delta_ns(*current_pressure, *previous_pressure) : count_runnable(root);
        daemon_ring->push(total);
      }
      if (current_cpu) {
        previous_cpu = current_cpu;
      }
      if (current_pressure) {
        previous_pressure = current_pressure;
      }

      ScaleState scale;
      bool granted = false;
      std::vector<std::string> grant;
      {
        std::lock_guard lock(daemons_mu);
        const auto* daemon = find_daemon(daemon_id);
        if (daemon == nullptr || daemon->pid != root) {
          break;
        }
        scale = daemon->scale;
        granted = daemon->policy.attached && daemon->policy.doc.grant_on;
        if (granted) {
          grant = daemon->policy.grant;
        }
      }
      if (!scale.enabled || !scale.policy || !scale.gnn) {
        if (gnn) {
          gnn.reset();  // detached or switched away: affinities back to the cpuset-wide mask
          std::cout << "gnn daemon_id=" << daemon_id << " detached, affinity restored" << std::endl;
        }
      }
      if (!scale.enabled || !scale.policy) {
        continue;
      }
      if (scale.gnn) {
        if (!gnn) {
          std::string error;
          const auto weights = gnn_weights(error);
          if (!weights) {
            if (gnn_warned++ % 100 == 0) {
              std::cerr << "gnn daemon_id=" << daemon_id << " " << error << std::endl;
            }
            continue;
          }
          gnn = std::make_unique<GnnController>(daemon_id, weights);
          gnn_ticks = 0;
          std::cout << "gnn daemon_id=" << daemon_id << " attached tick_ms=" << scale.gnn_tick_ms << std::endl;
        }
        if (++gnn_ticks % std::max(1, scale.gnn_tick_ms / kSampleMs) != 0) {
          continue;
        }
        const auto t0 = monotonic_ns();
        int cores = cpuset_cores(daemon_id);
        GnnTickStats st;
        const int target = gnn->observe(cores, scale.min_cores, scale.max_cores, st);
        if (target < 0) {
          break;
        }
        const int before_cores = cores;
        int after = 0;
        std::string spec;
        while (cores < target && add_core(daemon_id, scale.max_cores, after, spec, nullptr, scale.placement)) {
          cores = after;
        }
        while (cores > target && remove_core(daemon_id, scale.min_cores, after, spec)) {
          cores = after;
        }
        if (cores != before_cores) {
          std::lock_guard lock(daemons_mu);
          if (auto* daemon = find_daemon(daemon_id); daemon != nullptr && daemon->pid == root) {
            daemon->scale.changed_at_ns = monotonic_ns();
          }
        }
        const auto t1 = monotonic_ns();
        if (const auto cpus = parse_cpuset(trimmed(read_file("/sys/fs/cgroup/mercury/" + daemon_id + "/cpuset.cpus")))) {
          if (scale.gnn_pin) {
            gnn->pin(*cpus);
          } else {
            gnn->restore();  // no pins: widen anything pinned earlier (cheap when nothing is)
          }
        }
        const auto t2 = monotonic_ns();
        gnn_read_ns += st.read_ns;
        gnn_infer_ns += st.infer_ns;
        gnn_act_ns += t2 - t0 - st.read_ns - st.infer_ns;
        gnn_cpuset_ns += t1 - t0 - st.read_ns - st.infer_ns;
        ++gnn_logged;
        if (cores != before_cores || t2 - gnn_last_log >= 1000000000LL) {
          std::cout << "gnn daemon_id=" << daemon_id << " cores=" << cores << " required=" << st.required
                    << " target=" << target << " demand=" << st.demand_sum << " pred=" << st.pred_sum << " breaks=" << st.breaks << " nodes=" << st.nodes
                    << " threads=" << st.threads << " ticks=" << gnn_logged
                    << " mean_us read=" << gnn_read_ns / 1000 / gnn_logged << " infer=" << gnn_infer_ns / 1000 / gnn_logged
                    << " cpuset=" << gnn_cpuset_ns / 1000 / gnn_logged << " act=" << gnn_act_ns / 1000 / gnn_logged
                    << std::endl;
          gnn_last_log = t2;
          gnn_read_ns = gnn_infer_ns = gnn_act_ns = gnn_cpuset_ns = 0;
          gnn_logged = 0;
        }
        continue;
      }
      OccupancyWindow window;
      window.cores = cpuset_cores(daemon_id);
      window.min_cores = scale.min_cores;
      window.max_cores = scale.max_cores;
      window.runnable = !use_psi;
      for (const auto& sample : daemon_ring->recent(static_cast<std::size_t>(kScaleWindow) + 1)) {
        if (sample.timestamp_ns > scale.changed_at_ns) {
          window.cgroup.push_back(sample);
        }
      }
      const int before = window.cores;
      const auto decision = scale.policy->decide(window);
      if (decision == Decision::Hold) {
        continue;
      }
      std::vector<int> grant_cpus;
      if (granted) {
        for (const auto& info : resource_groups()) {
          if (std::find(grant.begin(), grant.end(), info.name) != grant.end()) {
            grant_cpus.insert(grant_cpus.end(), info.linux_cpus.begin(), info.linux_cpus.end());
          }
        }
        std::sort(grant_cpus.begin(), grant_cpus.end());
      }
      int after = 0;
      std::string spec;
      const bool changed = decision == Decision::AddCore
                               ? add_core(daemon_id, scale.max_cores, after, spec, granted ? &grant_cpus : nullptr, scale.placement)
                               : remove_core(daemon_id, scale.min_cores, after, spec);
      if (!changed) {
        continue;
      }
      std::cout << "scale daemon_id=" << daemon_id << " cores=" << after << " cpus=" << spec << std::endl;
      // Every change, up or down, waits for a fresh full window.
      (void)before;
      {
        std::lock_guard lock(daemons_mu);
        if (auto* daemon = find_daemon(daemon_id); daemon != nullptr && daemon->pid == root) {
          daemon->scale.changed_at_ns = monotonic_ns();
        }
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "counters daemon_id=" << daemon_id << " " << error.what() << std::endl;
  }
  daemon_ring->flush();
  mark_counters_done(daemon_id, root);
}

void arm_daemon(const std::string& daemon_id, const std::string& exec_path, int cpu, int listen_fd, pid_t pid,
                int out_fd, int err_fd, std::vector<std::pair<std::string, std::string>> env) {
  {
    std::lock_guard lock(daemons_mu);
    Daemon created;
    created.daemon_id = daemon_id;
    created.exec_path = exec_path;
    created.pid = pid;
    created.running = true;
    created.cpu = cpu;
    created.listen_fd = listen_fd;
    created.env = std::move(env);
    created.scale.enabled = true;
    created.scale.min_cores = 1;
    created.scale.max_cores = kDefaultMaxCores;
    created.scale.policy = make_policy("saturate");
    created.scale.changed_at_ns = monotonic_ns();
    if (auto* daemon = find_daemon(daemon_id)) {
      if (daemon->listen_fd >= 0 && daemon->listen_fd != listen_fd) {
        ::close(daemon->listen_fd);
      }
      *daemon = std::move(created);
    } else {
      daemons.push_back(std::move(created));
    }
  }
  std::thread(watch_daemon, daemon_id, pid, out_fd, err_fd).detach();
  std::thread(monitor_daemon, daemon_id, pid).detach();
}

void discard_launch(Launched launched) {
  ::kill(launched.pid, SIGKILL);
  ::waitpid(launched.pid, nullptr, 0);
  ::close(launched.out_fd);
  ::close(launched.err_fd);
}

std::optional<std::string> start_route(const std::string& request) {
  if (request_target(request) != "/start") {
    return std::nullopt;
  }

  const auto body = request_body(request);
  const auto daemon_id = json_value(body, "daemon_id");
  const auto exec_path = json_value(body, "exec_path");
  const auto cpu_text = json_value(body, "cpu");
  if (!daemon_id || !exec_path || exec_path->empty() || !cpu_text) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }

  int cpu = 0;
  try {
    cpu = std::stoi(*cpu_text);
  } catch (const std::exception&) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }
  if (std::find(daemon_cpus.begin(), daemon_cpus.end(), cpu) == daemon_cpus.end() ||
      ::access(exec_path->c_str(), X_OK) != 0) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }
  const auto env = parse_env(body);
  if (!env.ok) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }
  {
    std::lock_guard lock(daemons_mu);
    if (const auto* daemon = find_daemon(*daemon_id); daemon != nullptr && daemon->running) {
      return http_response(400, "Bad Request", R"({"pid":0})", kJson);
    }
  }

  int listen_fd = -1;
  if (const auto port_text = json_value(body, "listen_port")) {
    int port = 0;
    try {
      port = std::stoi(*port_text);
    } catch (const std::exception&) {
      return http_response(400, "Bad Request", R"({"pid":0})", kJson);
    }
    if (port < 1 || port > 65535) {
      return http_response(400, "Bad Request", R"({"pid":0})", kJson);
    }
    listen_fd = bind_loopback(port);
    if (listen_fd < 0) {
      return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
    }
  }

  const auto launched = launch(*daemon_id, *exec_path, cpu, listen_fd, env.vars);
  if (!launched) {
    if (listen_fd >= 0) {
      ::close(listen_fd);
    }
    return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
  }
  arm_daemon(*daemon_id, *exec_path, cpu, listen_fd, launched->pid, launched->out_fd, launched->err_fd, env.vars);
  return http_response(200, "OK", "{\"pid\":" + std::to_string(launched->pid) + "}", kJson);
}

std::optional<std::string> migrate_route(const std::string& request) {
  if (request_target(request) != "/migrate") {
    return std::nullopt;
  }
  const auto body = request_body(request);
  const auto daemon_id = json_value(body, "daemon_id");
  const auto cpu_text = json_value(body, "cpu");
  if (!daemon_id || !safe_daemon_id(*daemon_id) || !cpu_text) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }
  int cpu = 0;
  try {
    cpu = std::stoi(*cpu_text);
  } catch (const std::exception&) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }
  if (std::find(daemon_cpus.begin(), daemon_cpus.end(), cpu) == daemon_cpus.end()) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }

  pid_t pid = -1;
  int old_cpu = 0;
  {
    std::lock_guard lock(daemons_mu);
    const auto* daemon = find_daemon(*daemon_id);
    if (daemon == nullptr || !daemon->running || daemon->cpu == cpu) {
      return http_response(400, "Bad Request", R"({"pid":0})", kJson);
    }
    pid = daemon->pid;
    old_cpu = daemon->cpu;
  }

  const auto dir = ensure_daemon_cpuset(*daemon_id);
  if (!dir) {
    std::cerr << "migrate daemon_id=" << *daemon_id << " cgroup setup failed" << std::endl;
    return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
  }
  const std::vector<int> want{cpu};
  const auto path = *dir + "/cpuset.cpus";
  if (!write_cpus_and_mems(*dir, std::to_string(cpu))) {
    std::cerr << "migrate daemon_id=" << *daemon_id << " write " << path << " failed" << std::endl;
    return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
  }
  bool applied = false;
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (cpuset_applied(*dir, want)) {
      applied = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!applied) {
    std::cerr << "migrate daemon_id=" << *daemon_id << " cpuset did not take effect" << std::endl;
    return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
  }

  {
    std::lock_guard lock(daemons_mu);
    auto* daemon = find_daemon(*daemon_id);
    if (daemon == nullptr || !daemon->running || daemon->pid != pid) {
      return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
    }
    daemon->cpu = cpu;
  }
  std::cout << "migrated daemon_id=" << *daemon_id << " pid " << pid << " cpu " << old_cpu << " -> " << cpu
            << std::endl;
  return http_response(200, "OK", "{\"pid\":" + std::to_string(pid) + "}", kJson);
}

static std::string resource_choice(const std::optional<ResourceGroupState>& state) {
  if (!state) {
    return "none";
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(2) << state->name << "=" << state->load;
  return out.str();
}

std::optional<std::string> cpus_route(const std::string& request) {
  if (request_target(request) != "/cpus") {
    return std::nullopt;
  }
  const auto body = request_body(request);
  const auto daemon_id = json_value(body, "daemon_id");
  const auto cpus_text = json_value(body, "cpus");
  if (!daemon_id || !safe_daemon_id(*daemon_id) || !cpus_text) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  const auto want = parse_cpuset(trimmed(*cpus_text));
  if (!want || (mercury_linux_cpu >= 0 &&
                std::find(want->begin(), want->end(), mercury_linux_cpu) != want->end())) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  if (!resource_groups_see(*want)) {
    std::cerr << "cpus daemon_id=" << *daemon_id << " cpus are not in a resource group" << std::endl;
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }

  const auto dir = ensure_daemon_cpuset(*daemon_id);
  if (!dir) {
    std::cerr << "cpus daemon_id=" << *daemon_id << " cgroup setup failed" << std::endl;
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }
  const auto path = *dir + "/cpuset.cpus";
  if (!write_cpus_and_mems(*dir, trimmed(*cpus_text))) {
    std::cerr << "cpus daemon_id=" << *daemon_id << " write " << path << " failed" << std::endl;
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (cpuset_applied(*dir, *want)) {
      std::cout << "cpus daemon_id=" << *daemon_id << " cpus=" << trimmed(*cpus_text) << " socket "
                << resource_choice(least_loaded_touching(ResourceLevel::Socket, *want)) << " node "
                << resource_choice(least_loaded_touching(ResourceLevel::Node, *want)) << " l3 "
                << resource_choice(least_loaded_touching(ResourceLevel::L3, *want)) << " core "
                << resource_choice(least_loaded_touching(ResourceLevel::Core, *want)) << std::endl;
      return http_response(200, "OK", ok_json(true), kJson);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::cerr << "cpus daemon_id=" << *daemon_id << " cpuset did not take effect" << std::endl;
  return http_response(500, "Internal Server Error", ok_json(false), kJson);
}

int scale_int(const std::string& body, const char* key, int fallback, bool& bad) {
  const auto text = json_value(body, key);
  if (!text) {
    return fallback;
  }
  try {
    std::size_t used = 0;
    const int value = std::stoi(*text, &used);
    if (used != text->size()) {
      bad = true;
      return fallback;
    }
    return value;
  } catch (const std::exception&) {
    bad = true;
    return fallback;
  }
}

// The gnn model, loaded once from gnn_model_path() (reloaded if that file changes).
std::shared_ptr<const GnnWeights> gnn_weights(std::string& error) {
  static std::mutex mu;
  static std::shared_ptr<const GnnWeights> cached;
  static std::string cached_path;
  static std::int64_t cached_mtime = 0;
  std::lock_guard lock(mu);
  const auto path = gnn_model_path();
  struct stat sb {};
  if (::stat(path.c_str(), &sb) != 0) {
    error = "cannot stat " + path;
    return nullptr;
  }
  if (cached && cached_path == path && cached_mtime == static_cast<std::int64_t>(sb.st_mtime)) {
    return cached;
  }
  auto w = load_gnn_weights(path, error);
  if (w) {
    cached = w;
    cached_path = path;
    cached_mtime = static_cast<std::int64_t>(sb.st_mtime);
    std::cout << "gnn model " << path << " K=" << w->K << " F=" << w->F << " H=" << w->H << std::endl;
  }
  return w;
}

std::string scale_json(int min_cores, int max_cores, int cores, int threads) {
  return "{\"ok\":true,\"min\":" + std::to_string(min_cores) + ",\"max\":" + std::to_string(max_cores) +
         ",\"cores\":" + std::to_string(cores) + ",\"threads\":" + std::to_string(threads) + "}";
}

std::optional<std::string> scale_route(const std::string& request) {
  if (request_target(request) != "/scale") {
    return std::nullopt;
  }
  const auto body = request_body(request);
  const auto daemon_id = json_value(body, "daemon_id");
  if (!daemon_id || !safe_daemon_id(*daemon_id)) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  bool bad = false;
  const int min_cores = scale_int(body, "min", 1, bad);
  const int max_cores = scale_int(body, "max", kDefaultMaxCores, bad);
  const int tick_ms = scale_int(body, "tick_ms", 10, bad);
  const auto pin_text = json_value(body, "pin");
  const bool gnn_pin = !(pin_text && (*pin_text == "false" || *pin_text == "0"));
  const auto policy_name = json_value(body, "policy");
  const std::string name = policy_name ? *policy_name : "saturate";
  // placement is optional; omitted keeps the daemon's current placement.
  const auto placement_text = json_value(body, "placement");
  std::optional<CpuPlacement> placement;
  if (placement_text) {
    placement = parse_placement(*placement_text);
    if (!placement) {
      return http_response(400, "Bad Request", ok_json(false), kJson);
    }
  }
  if (bad || min_cores < 1 || max_cores < min_cores || tick_ms < kSampleMs) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  const bool is_gnn = name == "gnn";
  if (is_gnn) {
    std::string error;
    if (!gnn_weights(error)) {
      std::cerr << "scale daemon_id=" << *daemon_id << " gnn model: " << error << std::endl;
      return http_response(500, "Internal Server Error", "{\"ok\":false,\"error\":\"gnn model not loadable\"}", kJson);
    }
  }
  // "none" detaches saturate from this daemon, for callers that hold a cpuset fixed.
  const auto policy = make_policy(name);
  if (!policy && name != "none") {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  if (!resources_ready() || !least_loaded(ResourceLevel::Core)) {
    std::cerr << "scale daemon_id=" << *daemon_id << " resource groups are not available" << std::endl;
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }

  {
    std::lock_guard lock(daemons_mu);
    auto* daemon = find_daemon(*daemon_id);
    if (daemon == nullptr || !daemon->running) {
      return http_response(400, "Bad Request", ok_json(false), kJson);
    }
    daemon->scale.enabled = policy != nullptr;
    daemon->scale.min_cores = min_cores;
    daemon->scale.max_cores = max_cores;
    daemon->scale.policy = policy;
    daemon->scale.gnn = is_gnn;
    daemon->scale.gnn_tick_ms = (tick_ms / kSampleMs) * kSampleMs;
    daemon->scale.gnn_pin = gnn_pin;
    if (placement) {
      daemon->scale.placement = *placement;
    } else if (is_gnn) {
      daemon->scale.placement = CpuPlacement::LeastLoaded;  // gnn defaults to the least_loaded hand-out policy
    }
    placement = daemon->scale.placement;
  }
  std::cout << "scale daemon_id=" << *daemon_id << " placement=" << placement_name(*placement) << " least socket "
            << resource_choice(least_loaded(ResourceLevel::Socket)) << " node "
            << resource_choice(least_loaded(ResourceLevel::Node)) << " l3 "
            << resource_choice(least_loaded(ResourceLevel::L3)) << " core "
            << resource_choice(least_loaded(ResourceLevel::Core)) << std::endl;
  auto json = scale_json(min_cores, max_cores, cpuset_cores(*daemon_id), 0);
  json.insert(json.size() - 1, std::string(",\"placement\":\"") + placement_name(*placement) + "\",\"policy\":\"" + name + "\"" +
                                   (is_gnn ? ",\"tick_ms\":" + std::to_string((tick_ms / kSampleMs) * kSampleMs) + ",\"pin\":" + (gnn_pin ? "true" : "false") : std::string()));
  return http_response(200, "OK", json, kJson);
}

static bool cpus_hit(const std::vector<int>& group_cpus, const std::vector<int>& want) {
  for (const int cpu : want) {
    if (std::binary_search(group_cpus.begin(), group_cpus.end(), cpu)) {
      return true;
    }
  }
  return false;
}

static void append_ints(std::ostringstream& out, const std::vector<int>& values) {
  out << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << values[i];
  }
  out << ']';
}

static void append_quoted(std::ostringstream& out, const std::vector<std::string>& values) {
  out << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << '"' << values[i] << '"';
  }
  out << ']';
}

static std::string json_load(double load) {
  if (!(load >= 0)) {
    return "0";
  }
  std::ostringstream out;
  out << std::setprecision(6) << load;
  const auto text = out.str();
  if (text.find("nan") != std::string::npos || text.find("inf") != std::string::npos) {
    return "0";
  }
  return text;
}

struct ScheduledDaemon {
  std::string daemon_id;
  pid_t pid = -1;
  std::vector<int> cpus;
};

static std::vector<ScheduledDaemon> scheduled_daemons() {
  struct Live {
    std::string daemon_id;
    pid_t pid = -1;
    int cpu = -1;
  };
  std::vector<Live> live;
  {
    std::lock_guard lock(daemons_mu);
    for (const auto& daemon : daemons) {
      if (!daemon.running) {
        continue;
      }
      live.push_back(Live{daemon.daemon_id, daemon.pid, daemon.cpu});
    }
  }
  std::vector<ScheduledDaemon> scheduled;
  scheduled.reserve(live.size());
  for (const auto& daemon : live) {
    ScheduledDaemon row;
    row.daemon_id = daemon.daemon_id;
    row.pid = daemon.pid;
    const auto text = trimmed(read_file("/sys/fs/cgroup/mercury/" + daemon.daemon_id + "/cpuset.cpus"));
    if (auto cpus = parse_cpuset(text)) {
      row.cpus = std::move(*cpus);
    } else if (daemon.cpu >= 0) {
      row.cpus.push_back(daemon.cpu);
    }
    scheduled.push_back(std::move(row));
  }
  return scheduled;
}

static std::string map_json(const std::vector<ScheduledDaemon>& daemons_on,
                            const std::vector<ResourceGroupInfo>& groups) {
  std::ostringstream out;
  out << "{\"mercury_cpu\":" << mercury_linux_cpu << ",\"allocatable\":";
  append_ints(out, allocatable_cpus());
  out << ",\"daemons\":[";
  for (std::size_t i = 0; i < daemons_on.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    const auto& daemon = daemons_on[i];
    out << "{\"daemon_id\":\"" << daemon.daemon_id << "\",\"pid\":" << daemon.pid << ",\"cpus\":";
    append_ints(out, daemon.cpus);
    out << '}';
  }
  out << "],\"groups\":[";
  for (std::size_t i = 0; i < groups.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    const auto& group = groups[i];
    out << "{\"level\":\"" << resource_level_name(group.level) << "\",\"name\":\"" << group.name << "\",\"parent\":\""
        << group.parent << "\",\"children\":";
    append_quoted(out, group.children);
    out << ",\"linux_cpus\":";
    append_ints(out, group.linux_cpus);
    out << ",\"load\":" << json_load(group.load) << ",\"samples\":" << group.samples << ",\"daemons\":[";
    bool first = true;
    for (const auto& daemon : daemons_on) {
      if (!cpus_hit(group.linux_cpus, daemon.cpus)) {
        continue;
      }
      if (!first) {
        out << ',';
      }
      first = false;
      out << '"' << daemon.daemon_id << '"';
    }
    out << "]}";
  }
  out << "]}";
  return out.str();
}

std::optional<std::string> map_route(const std::string& request) {
  if (request_target(request) != "/map") {
    return std::nullopt;
  }
  if (!resources_ready()) {
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }
  return http_response(200, "OK", map_json(scheduled_daemons(), resource_groups()), kJson);
}

static std::string rings_json(const std::vector<ResourceRingView>& views) {
  std::ostringstream out;
  out << "{\"sample_ms\":" << kSampleMs << ",\"capacity\":" << kRingSlots << ",\"groups\":[";
  for (std::size_t i = 0; i < views.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    const auto& view = views[i];
    out << "{\"level\":\"" << resource_level_name(view.level) << "\",\"name\":\"" << view.name << "\",\"counters\":";
    append_quoted(out, view.counters);
    out << ",\"stored\":" << view.stored << ",\"samples\":[";
    for (std::size_t s = 0; s < view.samples.size(); ++s) {
      if (s != 0) {
        out << ',';
      }
      const auto& sample = view.samples[s];
      out << "{\"timestamp_ns\":" << sample.timestamp_ns;
      for (std::size_t c = 0; c < view.counters.size() && c < static_cast<std::size_t>(kMaxCounters); ++c) {
        const auto value = c < static_cast<std::size_t>(sample.nvalues) ? sample.value[c] : 0;
        out << ",\"" << view.counters[c] << "\":" << value;
      }
      out << '}';
    }
    out << "]}";
  }
  out << "]}";
  return out.str();
}

static std::optional<std::size_t> rings_query_count(const std::string& target, bool& bad) {
  constexpr std::string_view path = "/rings";
  if (target == path) {
    return std::nullopt;
  }
  constexpr std::string_view prefix = "/rings?count=";
  if (target.rfind(prefix, 0) != 0 || target.size() == prefix.size()) {
    bad = true;
    return std::nullopt;
  }
  const auto text = target.substr(prefix.size());
  for (const unsigned char c : text) {
    if (std::isdigit(c) == 0) {
      bad = true;
      return std::nullopt;
    }
  }
  try {
    std::size_t used = 0;
    const auto value = std::stoull(text, &used);
    if (used != text.size() || value > kRingSlots) {
      bad = true;
      return std::nullopt;
    }
    return static_cast<std::size_t>(value);
  } catch (const std::exception&) {
    bad = true;
    return std::nullopt;
  }
}

std::optional<std::string> rings_route(const std::string& request) {
  const auto target = request_target(request);
  if (target != "/rings" && target.rfind("/rings?", 0) != 0) {
    return std::nullopt;
  }
  bool bad = false;
  const auto asked = rings_query_count(target, bad);
  if (bad) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  if (!resources_ready()) {
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }
  constexpr std::size_t kDefaultRingSamples = 100;
  const auto count = asked ? *asked : kDefaultRingSamples;
  return http_response(200, "OK", rings_json(resource_rings(count)), kJson);
}

std::optional<std::string> kill_route(const std::string& request) {
  if (request_target(request) != "/kill") {
    return std::nullopt;
  }
  const auto daemon_id = json_value(request_body(request), "daemon_id");
  if (!daemon_id || daemon_id->empty()) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }

  std::unique_lock lock(daemons_mu);
  auto* daemon = find_daemon(*daemon_id);
  if (daemon == nullptr) {
    return http_response(200, "OK", ok_json(false), kJson);
  }
  if (!daemon->finished) {
    const auto pid = daemon->pid;
    ::kill(pid, SIGTERM);
    if (!daemons_cv.wait_for(lock, std::chrono::seconds(2), [&] {
          daemon = find_daemon(*daemon_id);
          return daemon != nullptr && daemon->finished.has_value();
        })) {
      ::kill(pid, SIGKILL);
      daemons_cv.wait(lock, [&] {
        daemon = find_daemon(*daemon_id);
        return daemon != nullptr && daemon->finished.has_value();
      });
    }
  }
  if (daemon != nullptr && !daemon->counters_done) {
    daemons_cv.wait_for(lock, std::chrono::seconds(2), [&] {
      daemon = find_daemon(*daemon_id);
      return daemon != nullptr && daemon->counters_done;
    });
  }
  if (daemon != nullptr && daemon->listen_fd >= 0) {
    ::close(daemon->listen_fd);
    daemon->listen_fd = -1;
  }
  if (daemon == nullptr || !daemon->finished) {
    return http_response(200, "OK", ok_json(false), kJson);
  }
  return http_response(200, "OK", kill_json(*daemon->finished), kJson);
}

// ---------------------------------------------------------------------------
// POST /policies and policy_loop (policies.md)
// ---------------------------------------------------------------------------

namespace {

std::mutex policy_file_mu;  // serializes the temp+rename of rings/<daemon_id>.policy

std::string policy_file_path(const std::string& daemon_id) {
  auto dir = rings_dir();
  if (!dir.empty() && dir.front() != '/') {
    char cwd[4096];
    if (::getcwd(cwd, sizeof(cwd)) != nullptr) {
      dir = std::string(cwd) + "/" + dir;
    }
  }
  return dir + "/" + archive_name(daemon_id) + ".policy";
}

std::string policy_file_json(const std::vector<DegradeComponent>& components, const std::vector<std::string>& disabled) {
  std::ostringstream out;
  out << "{\"components\":[";
  for (std::size_t i = 0; i < components.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    const bool off = std::find(disabled.begin(), disabled.end(), components[i].name) != disabled.end();
    out << "{\"name\":\"" << components[i].name << "\",\"enabled\":" << (off ? "false" : "true") << '}';
  }
  out << "]}\n";
  return out.str();
}

// Temp file in the same directory, renamed into place.
bool write_policy_file(const std::string& path, const std::string& body) {
  std::lock_guard lock(policy_file_mu);
  const auto tmp = path + ".tmp." + std::to_string(::getpid());
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      return false;
    }
    out << body;
    out.flush();
    if (!out) {
      ::unlink(tmp.c_str());
      return false;
    }
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

void remove_policy_file(const std::string& path) {
  std::lock_guard lock(policy_file_mu);
  ::unlink(path.c_str());
}

// The group tree does not change after init_resources. Loads in this copy are stale; use it for shape only.
const std::vector<ResourceGroupInfo>& group_tree() {
  static const std::vector<ResourceGroupInfo> tree = resource_groups();
  return tree;
}

const ResourceGroupInfo* tree_group(const std::string& name) {
  for (const auto& group : group_tree()) {
    if (group.name == name) {
      return &group;
    }
  }
  return nullptr;
}

bool subset_of(const std::vector<int>& small, const std::vector<int>& big) {
  for (const int cpu : small) {
    if (!std::binary_search(big.begin(), big.end(), cpu)) {
      return false;
    }
  }
  return true;
}

bool touches(const std::vector<int>& group_cpus, const std::vector<int>& cpus) {
  for (const int cpu : cpus) {
    if (std::binary_search(group_cpus.begin(), group_cpus.end(), cpu)) {
      return true;
    }
  }
  return false;
}

std::vector<int> sorted_unique(std::vector<int> cpus) {
  std::sort(cpus.begin(), cpus.end());
  cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
  return cpus;
}

// CPUs of the ceiling, or every online CPU when there is none.
std::vector<int> ceiling_cpus(const std::string& ceiling) {
  std::vector<int> cpus;
  for (const auto& group : group_tree()) {
    if (ceiling.empty() ? group.level == ResourceLevel::Socket : group.name == ceiling) {
      cpus.insert(cpus.end(), group.linux_cpus.begin(), group.linux_cpus.end());
    }
  }
  return sorted_unique(std::move(cpus));
}

std::vector<int> allocatable_in(const std::vector<int>& cpus) {
  static const std::vector<int> pool = allocatable_cpus();
  std::vector<int> out;
  for (const int cpu : cpus) {
    if (std::binary_search(pool.begin(), pool.end(), cpu)) {
      out.push_back(cpu);
    }
  }
  return out;
}

// Groups at level that contain a CPU of cpus, in tree order.
std::vector<std::string> groups_holding(ResourceLevel level, const std::vector<int>& cpus) {
  std::vector<std::string> names;
  for (const auto& group : group_tree()) {
    if (group.level == level && touches(group.linux_cpus, cpus)) {
      names.push_back(group.name);
    }
  }
  return names;
}

std::vector<int> cpus_of(const std::vector<std::string>& names) {
  std::vector<int> cpus;
  for (const auto& name : names) {
    if (const auto* group = tree_group(name)) {
      cpus.insert(cpus.end(), group->linux_cpus.begin(), group->linux_cpus.end());
    }
  }
  return sorted_unique(std::move(cpus));
}

std::optional<std::vector<int>> daemon_cpuset(const std::string& daemon_id) {
  return parse_cpuset(trimmed(read_file("/sys/fs/cgroup/mercury/" + daemon_id + "/cpuset.cpus")));
}

bool write_cpuset(const std::string& daemon_id, const std::vector<int>& want) {
  const auto dir = ensure_daemon_cpuset(daemon_id);
  if (!dir || !write_cpus_and_mems(*dir, format_cpuset(want))) {
    return false;
  }
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (cpuset_applied(*dir, want)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

std::string policy_reply(bool ok, const std::string& path, const std::string& error = {}) {
  std::string body = std::string("{\"ok\":") + (ok ? "true" : "false");
  if (!path.empty()) {
    body += ",\"path\":\"" + path + "\"";
  }
  if (!error.empty()) {
    body += ",\"error\":\"" + error + "\"";
  }
  return body + "}";
}

}  // namespace

std::optional<std::string> policies_route(const std::string& request) {
  if (request_target(request) != "/policies") {
    return std::nullopt;
  }
  const auto body = request_body(request);
  JNode root;
  if (!JParser(body).parse(root) || root.type != JNode::Type::Object) {
    return http_response(400, "Bad Request", policy_reply(false, {}, "body is not a JSON object"), kJson);
  }
  const auto* id_node = root.get("daemon_id");
  if (id_node == nullptr || id_node->type != JNode::Type::String || !safe_daemon_id(id_node->text)) {
    return http_response(400, "Bad Request", policy_reply(false, {}, "bad daemon_id"), kJson);
  }
  const std::string daemon_id = id_node->text;
  PolicyDoc doc;
  bool empty = true;
  if (const auto error = parse_policy_doc(root, doc, empty); !error.empty()) {
    std::cerr << "policies daemon_id=" << daemon_id << " rejected: " << error << std::endl;
    return http_response(400, "Bad Request", policy_reply(false, {}, error), kJson);
  }
  if (!resources_ready() || !least_loaded(ResourceLevel::Core)) {
    std::cerr << "policies daemon_id=" << daemon_id << " resource groups are not available" << std::endl;
    return http_response(500, "Internal Server Error", policy_reply(false, {}, "no resource groups"), kJson);
  }
  if (!doc.ceiling.empty()) {
    const auto* ceiling = tree_group(doc.ceiling);
    if (ceiling == nullptr) {
      return http_response(400, "Bad Request", policy_reply(false, {}, "unknown ceiling"), kJson);
    }
    // The ceiling is an ancestor of the granted groups: its level is the grant level or above it.
    if (static_cast<int>(ceiling->level) > static_cast<int>(doc.level)) {
      return http_response(400, "Bad Request", policy_reply(false, {}, "ceiling is below the grant level"), kJson);
    }
  }
  const auto* exec_node = root.get("exec_path");

  pid_t pid = -1;
  int stored_cpu = -1;
  {
    std::lock_guard lock(daemons_mu);
    const auto* daemon = find_daemon(daemon_id);
    if (daemon == nullptr || !daemon->running) {
      return http_response(400, "Bad Request", policy_reply(false, {}, "daemon is not running"), kJson);
    }
    if (exec_node != nullptr && exec_node->text != daemon->exec_path) {
      return http_response(400, "Bad Request", policy_reply(false, {}, "exec_path differs"), kJson);
    }
    pid = daemon->pid;
    stored_cpu = daemon->cpu;
  }
  const auto path = policy_file_path(daemon_id);

  if (empty) {
    {
      std::lock_guard lock(daemons_mu);
      auto* daemon = find_daemon(daemon_id);
      if (daemon == nullptr || !daemon->running || daemon->pid != pid) {
        return http_response(400, "Bad Request", policy_reply(false, {}, "daemon is not running"), kJson);
      }
      daemon->policy = PolicyState{};
    }
    remove_policy_file(path);
    std::cout << "policies daemon_id=" << daemon_id << " removed" << std::endl;
    return http_response(200, "OK", policy_reply(true, {}), kJson);
  }

  auto cpuset = daemon_cpuset(daemon_id);
  std::vector<int> cpus = cpuset ? *cpuset : std::vector<int>{stored_cpu};
  if (!doc.ceiling.empty() && !subset_of(cpus, ceiling_cpus(doc.ceiling))) {
    return http_response(400, "Bad Request", policy_reply(false, {}, "cpuset sticks out of the ceiling"), kJson);
  }

  PolicyState state;
  state.attached = true;
  state.doc = doc;
  if (doc.grant_on) {
    state.grant = groups_holding(doc.level, cpus);
    state.grant_empty_since.assign(state.grant.size(), 0);
    if (state.doc.max_groups == 0) {
      const auto inside = ceiling_cpus(doc.ceiling);
      for (const auto& group : group_tree()) {
        if (group.level == doc.level && subset_of(group.linux_cpus, inside)) {
          ++state.doc.max_groups;
        }
      }
    }
  }
  state.attached_at_ns = monotonic_ns();
  {
    std::lock_guard lock(daemons_mu);
    auto* daemon = find_daemon(daemon_id);
    if (daemon == nullptr || !daemon->running || daemon->pid != pid) {
      return http_response(400, "Bad Request", policy_reply(false, {}, "daemon is not running"), kJson);
    }
    state.generation = ++policy_generation;
    state.path = doc.components.empty() ? std::string{} : path;
    daemon->policy = state;
  }
  std::string written;
  if (!doc.components.empty()) {
    if (!write_policy_file(path, policy_file_json(doc.components, {}))) {
      std::cerr << "policies daemon_id=" << daemon_id << " could not write " << path << std::endl;
      return http_response(500, "Internal Server Error", policy_reply(false, {}, "policy file write failed"), kJson);
    }
    written = path;
  } else {
    remove_policy_file(path);
  }
  std::cout << "policies daemon_id=" << daemon_id << " shift=" << (doc.shift ? 1 : 0)
            << " grant=" << (doc.grant_on ? resource_level_name(doc.level) : "none")
            << " ceiling=" << (doc.ceiling.empty() ? "none" : doc.ceiling) << " max_groups=" << state.doc.max_groups
            << " groups=" << state.grant.size() << " components=" << doc.components.size() << std::endl;
  return http_response(200, "OK", policy_reply(true, written), kJson);
}

namespace {

struct PolicySnap {
  std::string daemon_id;
  pid_t pid = -1;
  int stored_cpu = -1;
  PolicyState state;
  ScaleState scale;
  std::shared_ptr<Ring> cgroup_ring;
  std::vector<int> cpus;
  bool moved = false;
};

using Loads = std::vector<std::optional<double>>;

double latest_or_zero(const Loads& loads) { return !loads.empty() && loads.back() ? *loads.back() : 0.0; }

// task-clock (value[0]) of the cgroup ring over the last 1 s. Empty when no sample fell in it.
std::optional<std::uint64_t> clock_last_second(const std::shared_ptr<Ring>& ring, std::int64_t now) {
  if (!ring) {
    return std::nullopt;
  }
  std::optional<std::uint64_t> total;
  for (const auto& sample : ring->recent(static_cast<std::size_t>(kPolicyIntervalNs / 1000000 / kSampleMs) + 20)) {
    if (sample.timestamp_ns > now - kPolicyIntervalNs && sample.timestamp_ns <= now && sample.nvalues >= 1) {
      total = total.value_or(0) + sample.value[0];
    }
  }
  return total;
}

// The saturate add test over the last 1 s: the window means of the cgroup ring over the samples of
// that second, with the thresholds of SaturatePolicy (occupancy >= 0.85 and pressure >= 0.10).
bool saturated_last_second(const PolicySnap& snap, std::int64_t now) {
  if (!snap.cgroup_ring || snap.cpus.empty()) {
    return false;
  }
  const auto recent = snap.cgroup_ring->recent(static_cast<std::size_t>(kPolicyIntervalNs / 1000000 / kSampleMs) + 20);
  std::vector<Sample> second;
  for (std::size_t i = 0; i < recent.size(); ++i) {
    if (recent[i].timestamp_ns > now - kPolicyIntervalNs && recent[i].timestamp_ns <= now) {
      if (second.empty() && i > 0) {
        second.push_back(recent[i - 1]);  // the gap before the first sample of the second
      }
      second.push_back(recent[i]);
    }
  }
  if (second.size() < 2) {
    return false;
  }
  const bool runnable = snap.cgroup_ring->names().size() > 3 && snap.cgroup_ring->names()[3] == "runnable";
  const auto cores = static_cast<int>(snap.cpus.size());
  const auto means = window_means(second, cores, second.size() - 1, runnable);
  return means && saturate_add_test(*means, cores, runnable);
}

bool cooled(std::int64_t since, std::int64_t now) {
  return since == 0 || now - since >= kPolicyIntervalNs * kPolicyIntervals;
}

// Store a change only when the document was not replaced in between.
template <typename Fn>
bool store_policy(const PolicySnap& snap, Fn&& fn) {
  std::lock_guard lock(daemons_mu);
  auto* daemon = find_daemon(snap.daemon_id);
  if (daemon == nullptr || !daemon->running || daemon->pid != snap.pid || !daemon->policy.attached ||
      daemon->policy.generation != snap.state.generation) {
    return false;
  }
  fn(*daemon);
  return true;
}

void run_shift(std::vector<PolicySnap>& snaps, const std::unordered_map<std::string, Loads>& loads,
               std::unordered_map<std::string, std::int64_t>& node_left_at, std::int64_t now) {
  const auto& tree = group_tree();
  std::vector<const ResourceGroupInfo*> nodes;
  for (const auto& group : tree) {
    if (group.level == ResourceLevel::Node) {
      nodes.push_back(&group);
    }
  }
  auto hot = [&](const std::string& name) {
    const auto it = loads.find(name);
    return it != loads.end() && node_hot(it->second);
  };
  auto latest = [&](const std::string& name) {
    const auto it = loads.find(name);
    return it == loads.end() ? 0.0 : latest_or_zero(it->second);
  };
  for (const auto* source : nodes) {
    if (!hot(source->name) || !cooled(node_left_at[source->name], now)) {
      continue;
    }
    struct Ranked {
      PolicySnap* snap;
      std::uint64_t rank;
    };
    std::vector<Ranked> ranked;
    for (auto& snap : snaps) {
      if (snap.scale.gnn || !snap.state.doc.shift || snap.moved || !touches(source->linux_cpus, snap.cpus) ||
          !cooled(snap.state.attached_at_ns, now) || !cooled(snap.state.shifted_at_ns, now) ||
          !cooled(snap.state.widened_at_ns, now)) {
        continue;
      }
      const auto inside = ceiling_cpus(snap.state.doc.ceiling);
      bool other = false;
      for (const auto* node : nodes) {
        if (node != source && subset_of(node->linux_cpus, inside)) {
          other = true;
        }
      }
      if (!other) {
        continue;
      }
      const auto rank = clock_last_second(snap.cgroup_ring, now);
      if (!rank) {
        continue;
      }
      ranked.push_back(Ranked{&snap, *rank});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
      if (a.rank != b.rank) {
        return a.rank > b.rank;
      }
      return a.snap->daemon_id < b.snap->daemon_id;
    });
    for (auto& candidate : ranked) {
      auto& snap = *candidate.snap;
      const auto inside = ceiling_cpus(snap.state.doc.ceiling);
      const ResourceGroupInfo* dest = nullptr;
      for (const auto* node : nodes) {
        if (node == source || hot(node->name) || !subset_of(node->linux_cpus, inside) ||
            allocatable_in(node->linux_cpus).size() < snap.cpus.size()) {
          continue;
        }
        if (dest == nullptr || latest(node->name) < latest(dest->name) ||
            (latest(node->name) == latest(dest->name) && node->linux_cpus.front() < dest->linux_cpus.front())) {
          dest = node;
        }
      }
      if (dest == nullptr) {
        continue;
      }
      std::vector<int> eligible;
      const auto level = snap.state.doc.grant_on ? snap.state.doc.level : ResourceLevel::Core;
      if (level == ResourceLevel::Core || level == ResourceLevel::L3) {
        const ResourceGroupInfo* best_l3 = nullptr;
        for (const auto& child : dest->children) {
          const auto* l3 = tree_group(child);
          if (l3 == nullptr || allocatable_in(l3->linux_cpus).empty()) {
            continue;
          }
          if (best_l3 == nullptr || latest(l3->name) < latest(best_l3->name) ||
              (latest(l3->name) == latest(best_l3->name) && l3->linux_cpus.front() < best_l3->linux_cpus.front())) {
            best_l3 = l3;
          }
        }
        if (best_l3 != nullptr) {
          eligible = allocatable_in(best_l3->linux_cpus);
        }
      } else {
        eligible = allocatable_in(dest->linux_cpus);
      }
      std::vector<int> exclude;
      for (const int cpu : allocatable_cpus()) {
        if (!std::binary_search(eligible.begin(), eligible.end(), cpu)) {
          exclude.push_back(cpu);
        }
      }
      std::vector<int> chosen;
      while (chosen.size() < snap.cpus.size()) {
        const auto next = request_cpu(exclude);
        if (!next) {
          break;
        }
        chosen.push_back(*next);
        exclude.push_back(*next);
      }
      if (chosen.size() < snap.cpus.size()) {
        continue;  // short on the destination; try the next candidate
      }
      chosen = sorted_unique(std::move(chosen));
      if (std::find(chosen.begin(), chosen.end(), mercury_linux_cpu) != chosen.end()) {
        continue;
      }
      if (!write_cpuset(snap.daemon_id, chosen)) {
        std::cerr << "policy shift daemon_id=" << snap.daemon_id << " cpuset did not take effect" << std::endl;
        write_cpuset(snap.daemon_id, snap.cpus);
        break;
      }
      const auto stamp = monotonic_ns();
      store_policy(snap, [&](Daemon& daemon) {
        daemon.cpu = chosen.front();
        if (daemon.policy.doc.grant_on) {
          daemon.policy.grant = groups_holding(daemon.policy.doc.level, chosen);
          daemon.policy.grant_empty_since.assign(daemon.policy.grant.size(), 0);
        }
        daemon.policy.shifted_at_ns = stamp;
        daemon.scale.changed_at_ns = stamp;
      });
      std::cout << "policy shift daemon_id=" << snap.daemon_id << " " << source->name << " -> " << dest->name
                << " cpus " << format_cpuset(snap.cpus) << " -> " << format_cpuset(chosen) << std::endl;
      snap.cpus = chosen;
      snap.moved = true;
      snap.state.shifted_at_ns = stamp;
      if (snap.state.doc.grant_on) {
        snap.state.grant = groups_holding(snap.state.doc.level, chosen);
        snap.state.grant_empty_since.assign(snap.state.grant.size(), 0);
      }
      node_left_at[source->name] = stamp;
      break;  // one daemon leaves a node per five intervals
    }
  }
}

void run_widen(std::vector<PolicySnap>& snaps, const std::vector<int>& used_cpus, std::int64_t now) {
  std::vector<ResourceGroupInfo> live;  // current load_of, fetched only when a widen is due
  for (auto& snap : snaps) {
    auto& state = snap.state;
    if (!state.doc.grant_on) {
      continue;
    }
    if (!snap.scale.gnn && state.doc.widen && !snap.moved && cooled(state.attached_at_ns, now) && cooled(state.widened_at_ns, now) &&
        cooled(state.shifted_at_ns, now) && static_cast<int>(state.grant.size()) < state.doc.max_groups &&
        (!snap.scale.enabled || static_cast<int>(snap.cpus.size()) < snap.scale.max_cores)) {
      const auto granted = allocatable_in(cpus_of(state.grant));
      const bool exhausted = subset_of(granted, snap.cpus);
      if (exhausted && saturated_last_second(snap, now)) {
        const auto inside = ceiling_cpus(state.doc.ceiling);
        if (live.empty()) {
          live = resource_groups();
        }
        std::vector<GroupCandidate> legal;
        for (const auto& group : live) {
          if (group.level != state.doc.level || !subset_of(group.linux_cpus, inside) ||
              std::find(state.grant.begin(), state.grant.end(), group.name) != state.grant.end()) {
            continue;
          }
          auto alloc = allocatable_in(group.linux_cpus);
          if (alloc.empty()) {
            continue;
          }
          legal.push_back(GroupCandidate{group.name, std::move(alloc), group.load, group.linux_cpus.front()});
        }
        if (const auto pick = least_allocated(legal, used_cpus)) {
          const auto& group = legal[*pick];
          std::vector<int> exclude = snap.cpus;
          for (const int cpu : allocatable_cpus()) {
            if (!std::binary_search(group.allocatable.begin(), group.allocatable.end(), cpu)) {
              exclude.push_back(cpu);
            }
          }
          if (const auto next = request_cpu(exclude)) {
            auto want = snap.cpus;
            want.push_back(*next);
            want = sorted_unique(std::move(want));
            if (write_cpuset(snap.daemon_id, want)) {
              const auto stamp = monotonic_ns();
              store_policy(snap, [&](Daemon& daemon) {
                daemon.policy.grant.push_back(group.name);
                daemon.policy.grant_empty_since.push_back(0);
                daemon.policy.widened_at_ns = stamp;
                daemon.scale.changed_at_ns = stamp;
              });
              std::cout << "policy widen daemon_id=" << snap.daemon_id << " group " << group.name << " cpus "
                        << format_cpuset(want) << " groups " << state.grant.size() + 1 << "/" << state.doc.max_groups
                        << std::endl;
              snap.cpus = want;
              state.grant.push_back(group.name);
              state.grant_empty_since.push_back(0);
            } else {
              std::cerr << "policy widen daemon_id=" << snap.daemon_id << " cpuset did not take effect" << std::endl;
            }
          }
        }
      }
    }
    // A group other than the last leaves the grant after five intervals without a CPU of the cpuset.
    bool changed = false;
    state.grant_empty_since.resize(state.grant.size(), 0);
    for (std::size_t i = 0; i < state.grant.size();) {
      const auto* group = tree_group(state.grant[i]);
      const bool holds = group != nullptr && touches(group->linux_cpus, snap.cpus);
      if (holds) {
        if (state.grant_empty_since[i] != 0) {
          state.grant_empty_since[i] = 0;
          changed = true;
        }
        ++i;
        continue;
      }
      if (state.grant_empty_since[i] == 0) {
        state.grant_empty_since[i] = now;
        changed = true;
        ++i;
        continue;
      }
      if (state.grant.size() > 1 && now - state.grant_empty_since[i] >= kPolicyIntervalNs * kPolicyIntervals) {
        std::cout << "policy grant daemon_id=" << snap.daemon_id << " drops " << state.grant[i] << std::endl;
        state.grant.erase(state.grant.begin() + static_cast<std::ptrdiff_t>(i));
        state.grant_empty_since.erase(state.grant_empty_since.begin() + static_cast<std::ptrdiff_t>(i));
        changed = true;
        continue;
      }
      ++i;
    }
    if (changed) {
      store_policy(snap, [&](Daemon& daemon) {
        daemon.policy.grant = state.grant;
        daemon.policy.grant_empty_since = state.grant_empty_since;
      });
    }
  }
}

void run_degrade(std::vector<PolicySnap>& snaps, const std::unordered_map<std::string, Loads>& loads,
                 std::int64_t now) {
  for (auto& snap : snaps) {
    auto& state = snap.state;
    if (state.doc.components.empty() || !cooled(state.attached_at_ns, now) || !cooled(state.degraded_at_ns, now)) {
      continue;
    }
    Loads series(static_cast<std::size_t>(kPolicyIntervals));
    std::vector<std::string> sources;
    if (!state.doc.ceiling.empty()) {
      sources.push_back(state.doc.ceiling);
    } else {
      sources = groups_holding(ResourceLevel::Node, snap.cpus);
    }
    for (const auto& name : sources) {
      const auto it = loads.find(name);
      if (it == loads.end()) {
        continue;
      }
      for (std::size_t k = 0; k < series.size() && k < it->second.size(); ++k) {
        if (it->second[k] && (!series[k] || *it->second[k] > *series[k])) {
          series[k] = it->second[k];
        }
      }
    }
    const auto next = degrade_set(state.doc.components, state.disabled, series);
    auto before = state.disabled;
    auto after = next;
    std::sort(before.begin(), before.end());
    std::sort(after.begin(), after.end());
    if (before == after) {
      continue;
    }
    const auto stamp = monotonic_ns();
    if (!store_policy(snap, [&](Daemon& daemon) {
          daemon.policy.disabled = next;
          daemon.policy.degraded_at_ns = stamp;
        })) {
      continue;
    }
    const auto path = state.path.empty() ? policy_file_path(snap.daemon_id) : state.path;
    if (!write_policy_file(path, policy_file_json(state.doc.components, next))) {
      std::cerr << "policy degrade daemon_id=" << snap.daemon_id << " could not write " << path << std::endl;
    }
    std::ostringstream off;
    for (std::size_t i = 0; i < next.size(); ++i) {
      off << (i == 0 ? "" : ",") << next[i];
    }
    std::cout << "policy degrade daemon_id=" << snap.daemon_id << " U=" << latest_or_zero(series)
              << " disabled=" << (next.empty() ? "none" : off.str()) << std::endl;
  }
}

void policy_tick(std::unordered_map<std::string, std::int64_t>& node_left_at) {
  const auto now = monotonic_ns();
  // 1. One interval load per group for each of the last five 1 s stretches.
  std::unordered_map<std::string, Loads> loads;
  const auto window = static_cast<std::size_t>(kPolicyIntervals) * static_cast<std::size_t>(kPolicyIntervalNs / 1000000 / kSampleMs) + 50;
  for (const auto& view : resource_rings(window)) {
    loads[view.name] = interval_loads(view.samples, now);
  }
  // 2. Copy under daemons_mu, then drop it before any ring read or cpuset write.
  std::vector<PolicySnap> snaps;
  std::vector<std::pair<std::string, int>> running;
  {
    std::lock_guard lock(daemons_mu);
    for (const auto& daemon : daemons) {
      if (!daemon.running) {
        continue;
      }
      running.emplace_back(daemon.daemon_id, daemon.cpu);
      if (!daemon.policy.attached) {
        continue;
      }
      PolicySnap snap;
      snap.daemon_id = daemon.daemon_id;
      snap.pid = daemon.pid;
      snap.stored_cpu = daemon.cpu;
      snap.state = daemon.policy;
      snap.scale = daemon.scale;
      snap.cgroup_ring = daemon.cgroup_ring;
      snaps.push_back(std::move(snap));
    }
  }
  if (snaps.empty()) {
    return;
  }
  std::vector<int> used;
  for (const auto& [id, cpu] : running) {
    if (const auto cpus = daemon_cpuset(id)) {
      used.insert(used.end(), cpus->begin(), cpus->end());
    } else {
      used.push_back(cpu);
    }
  }
  used = sorted_unique(std::move(used));
  for (auto& snap : snaps) {
    const auto cpus = daemon_cpuset(snap.daemon_id);
    snap.cpus = cpus ? *cpus : std::vector<int>{snap.stored_cpu};
  }
  // 3. shift, 4. widen and grant pruning, 5. degrade.
  run_shift(snaps, loads, node_left_at, now);
  run_widen(snaps, used, now);
  run_degrade(snaps, loads, now);
}

}  // namespace

void policy_loop() {
  std::unordered_map<std::string, std::int64_t> node_left_at;
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    try {
      policy_tick(node_left_at);
    } catch (const std::exception& error) {
      std::cerr << "policy_loop " << error.what() << std::endl;
    }
  }
}
