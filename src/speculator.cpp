#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

class Socket {
 public:
  explicit Socket(int fd) : fd_(fd) {}
  ~Socket() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  int get() const { return fd_; }

 private:
  int fd_;
};

void send_all(int fd, std::string_view message) {
  while (!message.empty()) {
    const auto n = ::send(fd, message.data(), message.size(), MSG_NOSIGNAL);
    if (n < 0) {
      throw std::runtime_error("send failed");
    }
    message.remove_prefix(static_cast<std::size_t>(n));
  }
}

std::string recv_all(int fd) {
  std::string response;
  char buffer[4096];
  while (true) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n == 0) {
      return response;
    }
    if (n < 0) {
      throw std::runtime_error("recv failed");
    }
    response.append(buffer, static_cast<std::size_t>(n));
  }
}

std::string response_body(const std::string& response) {
  const auto at = response.find("\r\n\r\n");
  if (at == std::string::npos) {
    throw std::runtime_error("map response had no body");
  }
  return response.substr(at + 4);
}

int response_status(const std::string& response) {
  const auto line_end = response.find("\r\n");
  const auto line = response.substr(0, line_end == std::string::npos ? response.size() : line_end);
  const auto space = line.find(' ');
  if (space == std::string::npos) {
    throw std::runtime_error("map response had no status");
  }
  return std::stoi(line.substr(space + 1));
}

std::string fetch_map(std::uint16_t port) {
  Socket sock{::socket(AF_INET, SOCK_STREAM, 0)};
  if (sock.get() < 0) {
    throw std::runtime_error("socket failed");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(sock.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    throw std::runtime_error("connect failed");
  }
  send_all(sock.get(), "GET /map HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
  const auto response = recv_all(sock.get());
  const auto status = response_status(response);
  if (status != 200) {
    throw std::runtime_error("map status " + std::to_string(status));
  }
  return response_body(response);
}

struct Group {
  std::string name;
  std::string parent;
  std::vector<std::string> children;
  std::vector<int> linux_cpus;
  double load = 0;
  std::vector<std::string> daemons;
};

struct Daemon {
  std::string id;
  std::vector<int> cpus;
};

struct MapView {
  int mercury_cpu = -1;
  std::vector<int> allocatable;
  std::vector<Group> groups;
  std::vector<Daemon> daemons;
};

class Cursor {
 public:
  explicit Cursor(const std::string& text) : text_(text) {}

  char peek() {
    skip_ws();
    if (at_ >= text_.size()) {
      throw std::runtime_error("bad map");
    }
    return text_[at_];
  }

  char get() {
    const char c = peek();
    ++at_;
    return c;
  }

  void expect(char want) {
    if (get() != want) {
      throw std::runtime_error("bad map");
    }
  }

  std::string parse_string() {
    if (get() != '"') {
      throw std::runtime_error("bad map");
    }
    std::string value;
    while (at_ < text_.size()) {
      const char c = text_[at_++];
      if (c == '"') {
        return value;
      }
      if (c != '\\') {
        value.push_back(c);
        continue;
      }
      if (at_ >= text_.size()) {
        throw std::runtime_error("bad map");
      }
      const char escaped = text_[at_++];
      if (escaped == 'n') {
        value.push_back('\n');
      } else if (escaped == 't') {
        value.push_back('\t');
      } else if (escaped == 'u') {
        if (at_ + 4 > text_.size()) {
          throw std::runtime_error("bad map");
        }
        at_ += 4;
      } else {
        value.push_back(escaped);
      }
    }
    throw std::runtime_error("bad map");
  }

  double parse_number() {
    skip_ws();
    const auto start = at_;
    if (at_ < text_.size() && (text_[at_] == '-' || text_[at_] == '+')) {
      ++at_;
    }
    const auto digits = at_;
    while (at_ < text_.size()) {
      const unsigned char c = static_cast<unsigned char>(text_[at_]);
      if (std::isdigit(c) == 0 && c != '.' && c != 'e' && c != 'E' && c != '+' && c != '-') {
        break;
      }
      ++at_;
    }
    if (at_ == digits) {
      throw std::runtime_error("bad map");
    }
    try {
      return std::stod(text_.substr(start, at_ - start));
    } catch (const std::exception&) {
      throw std::runtime_error("bad map");
    }
  }

  std::vector<int> parse_ints() {
    expect('[');
    std::vector<int> values;
    if (peek() == ']') {
      get();
      return values;
    }
    while (true) {
      values.push_back(static_cast<int>(parse_number()));
      if (peek() == ']') {
        get();
        return values;
      }
      expect(',');
    }
  }

  std::vector<std::string> parse_strings() {
    expect('[');
    std::vector<std::string> values;
    if (peek() == ']') {
      get();
      return values;
    }
    while (true) {
      values.push_back(parse_string());
      if (peek() == ']') {
        get();
        return values;
      }
      expect(',');
    }
  }

  void skip_value() {
    const char c = peek();
    if (c == '"') {
      parse_string();
    } else if (c == '{') {
      skip_object();
    } else if (c == '[') {
      skip_array();
    } else if (c == 't' || c == 'f' || c == 'n') {
      while (at_ < text_.size() && std::isalpha(static_cast<unsigned char>(text_[at_])) != 0) {
        ++at_;
      }
    } else {
      parse_number();
    }
  }

 private:
  void skip_ws() {
    while (at_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[at_])) != 0) {
      ++at_;
    }
  }

  void skip_object() {
    expect('{');
    if (peek() == '}') {
      get();
      return;
    }
    while (true) {
      parse_string();
      expect(':');
      skip_value();
      if (peek() == '}') {
        get();
        return;
      }
      expect(',');
    }
  }

  void skip_array() {
    expect('[');
    if (peek() == ']') {
      get();
      return;
    }
    while (true) {
      skip_value();
      if (peek() == ']') {
        get();
        return;
      }
      expect(',');
    }
  }

  const std::string& text_;
  std::size_t at_ = 0;
};

Daemon parse_daemon(Cursor& cursor) {
  Daemon daemon;
  cursor.expect('{');
  if (cursor.peek() == '}') {
    cursor.get();
    return daemon;
  }
  while (true) {
    const auto key = cursor.parse_string();
    cursor.expect(':');
    if (key == "daemon_id") {
      daemon.id = cursor.parse_string();
    } else if (key == "cpus") {
      daemon.cpus = cursor.parse_ints();
    } else {
      cursor.skip_value();
    }
    if (cursor.peek() == '}') {
      cursor.get();
      return daemon;
    }
    cursor.expect(',');
  }
}

Group parse_group(Cursor& cursor) {
  Group group;
  cursor.expect('{');
  if (cursor.peek() == '}') {
    cursor.get();
    return group;
  }
  while (true) {
    const auto key = cursor.parse_string();
    cursor.expect(':');
    if (key == "name") {
      group.name = cursor.parse_string();
    } else if (key == "parent") {
      group.parent = cursor.parse_string();
    } else if (key == "children") {
      group.children = cursor.parse_strings();
    } else if (key == "linux_cpus") {
      group.linux_cpus = cursor.parse_ints();
    } else if (key == "load") {
      group.load = cursor.parse_number();
    } else if (key == "daemons") {
      group.daemons = cursor.parse_strings();
    } else {
      cursor.skip_value();
    }
    if (cursor.peek() == '}') {
      cursor.get();
      return group;
    }
    cursor.expect(',');
  }
}

MapView parse_map(const std::string& json) {
  Cursor cursor(json);
  MapView map;
  cursor.expect('{');
  if (cursor.peek() == '}') {
    throw std::runtime_error("bad map");
  }
  while (true) {
    const auto key = cursor.parse_string();
    cursor.expect(':');
    if (key == "mercury_cpu") {
      map.mercury_cpu = static_cast<int>(cursor.parse_number());
    } else if (key == "allocatable") {
      map.allocatable = cursor.parse_ints();
    } else if (key == "groups") {
      cursor.expect('[');
      if (cursor.peek() != ']') {
        while (true) {
          map.groups.push_back(parse_group(cursor));
          if (cursor.peek() == ']') {
            break;
          }
          cursor.expect(',');
        }
      }
      cursor.expect(']');
    } else if (key == "daemons") {
      cursor.expect('[');
      if (cursor.peek() != ']') {
        while (true) {
          map.daemons.push_back(parse_daemon(cursor));
          if (cursor.peek() == ']') {
            break;
          }
          cursor.expect(',');
        }
      }
      cursor.expect(']');
    } else {
      cursor.skip_value();
    }
    if (cursor.peek() == '}') {
      cursor.get();
      return map;
    }
    cursor.expect(',');
  }
}

std::string format_cpus(std::vector<int> cpus) {
  std::sort(cpus.begin(), cpus.end());
  cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
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

double shown_load(double load) {
  if (!(load > 0)) {
    return 0;
  }
  if (load > 1) {
    return 1;
  }
  return load;
}

std::string load_bar(double load) {
  int filled = static_cast<int>(std::lround(shown_load(load) * 10.0));
  if (filled < 0) {
    filled = 0;
  }
  if (filled > 10) {
    filled = 10;
  }
  std::string bar(10, '-');
  bar.replace(0, static_cast<std::size_t>(filled), static_cast<std::size_t>(filled), '#');
  return bar;
}

void print_group(std::ostream& out, const Group& group, int depth,
                 const std::unordered_map<std::string, const Group*>& by_name) {
  constexpr int kBarColumn = 24;
  std::string label(static_cast<std::size_t>(depth) * 2, ' ');
  label += group.name;
  if (static_cast<int>(label.size()) < kBarColumn) {
    label.append(static_cast<std::size_t>(kBarColumn - static_cast<int>(label.size())), ' ');
  } else {
    label.push_back(' ');
  }
  const double load = shown_load(group.load);
  out << label << '[' << load_bar(load) << "] " << load << "  cpus " << format_cpus(group.linux_cpus);
  for (std::size_t i = 0; i < group.daemons.size(); ++i) {
    out << (i == 0 ? "  " : " ") << group.daemons[i];
  }
  out << '\n';
  for (const auto& child : group.children) {
    const auto found = by_name.find(child);
    if (found == by_name.end()) {
      continue;
    }
    print_group(out, *found->second, depth + 1, by_name);
  }
}

void print_tree(std::ostream& out, const MapView& map) {
  out << std::fixed << std::setprecision(2);
  out << "mercury cpu " << map.mercury_cpu << "\nallocatable " << format_cpus(map.allocatable) << "\n\n";
  std::unordered_map<std::string, const Group*> by_name;
  for (const auto& group : map.groups) {
    by_name.emplace(group.name, &group);
  }
  for (const auto& group : map.groups) {
    if (!group.parent.empty()) {
      continue;
    }
    print_group(out, group, 0, by_name);
  }
}

// Per-app view: daemons are grouped by the longest --app name they start with
// (hotel-a-geo with --app hotel-a), else by the id prefix before the first '-'
// (hotel-geo, social-mongod). cores = distinct CPUs in the app's cpusets,
// core_sum = sum of cpuset sizes (cpusets may overlap).
void print_apps(std::ostream& out, const MapView& map, const std::vector<std::string>& names) {
  std::map<std::string, std::pair<std::set<int>, std::pair<int, int>>> apps;  // prefix -> (cpus, (daemons, core_sum))
  for (const auto& daemon : map.daemons) {
    const auto dash = daemon.id.find('-');
    std::string key = dash == std::string::npos ? daemon.id : daemon.id.substr(0, dash);
    for (const auto& name : names) {
      if (daemon.id.rfind(name + "-", 0) == 0 && name.size() > key.size()) {
        key = name;
      }
    }
    auto& app = apps[key];
    app.first.insert(daemon.cpus.begin(), daemon.cpus.end());
    app.second.first += 1;
    app.second.second += static_cast<int>(daemon.cpus.size());
  }
  if (apps.empty()) {
    return;
  }
  out << "\napps\n";
  for (const auto& [name, app] : apps) {
    std::string label = "  " + name;
    label.append(label.size() < 12 ? 12 - label.size() : 1, ' ');
    out << label << "daemons " << app.second.first << "  cores " << app.first.size() << "  core_sum " << app.second.second
        << "  cpus " << format_cpus(std::vector<int>(app.first.begin(), app.first.end())) << '\n';
  }
}

// --status FILE: the "key value" lines an operator rewrites every window
// (operate/goperate -status-file, demoload.go).
void print_status(std::ostream& out, const std::vector<std::string>& files) {
  if (files.empty()) {
    return;
  }
  out << "\ndemo (operator status files)\n";
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  for (const auto& file : files) {
    std::ifstream in(file);
    if (!in) {
      out << "  " << file << ": no status yet\n";
      continue;
    }
    std::unordered_map<std::string, std::string> kv;
    std::vector<std::string> cps;
    std::string line;
    while (std::getline(in, line)) {
      const auto space = line.find(' ');
      if (space == std::string::npos) {
        continue;
      }
      const auto key = line.substr(0, space);
      if (key == "cp") {
        cps.push_back(line.substr(space + 1));
      } else {
        kv[key] = line.substr(space + 1);
      }
    }
    const auto get = [&](const char* key) { return kv.count(key) ? kv[key] : std::string("?"); };
    double age = -1;
    try {
      age = static_cast<double>(now_ms - std::stoll(get("unix_ms"))) / 1000.0;
    } catch (const std::exception&) {
    }
    std::string label = "  " + get("app");
    label.append(label.size() < 12 ? 12 - label.size() : 1, ' ');
    auto p95 = get("p95_ms_max");  // "4.10 search": worst route's p95 and its name
    if (const auto space = p95.find(' '); space != std::string::npos) {
      p95 = p95.substr(0, space) + " ms (" + p95.substr(space + 1) + ")";
    } else {
      p95 += " ms";
    }
    out << label << "users " << get("target_users") << "  rate " << get("rate") << "/s  p95 " << p95
        << "  wmean " << get("p95_ms_wmean") << " ms  cores " << get("cores") << " (sum " << get("core_sum")
        << ", gen " << get("gen_cores") << ")  errors " << get("errors") << "  window " << get("window") << "  t "
        << get("t_s") << " s  age " << std::setprecision(1) << age << " s\n";
    for (const auto& cp : cps) {
      out << "              cp " << cp << '\n';
    }
  }
}


// An app on screen: name groups daemons (<name>-...) in "apps"; status is an
// operator -status-file (demo section). Either may be empty.
struct App {
  std::string name;
  std::string status;
};

std::string status_app(const std::string& file) {
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("app ", 0) == 0) {
      return line.substr(4);
    }
  }
  return "";
}

// Daemon id prefixes shared by at least two daemons (hotel, hotel-a, hotel-b).
std::vector<std::pair<std::string, int>> prefix_suggestions(const MapView& map) {
  std::map<std::string, std::set<std::string>> ids;
  for (const auto& daemon : map.daemons) {
    for (auto dash = daemon.id.find('-'); dash != std::string::npos; dash = daemon.id.find('-', dash + 1)) {
      ids[daemon.id.substr(0, dash)].insert(daemon.id);
    }
  }
  std::vector<std::pair<std::string, int>> out;
  for (const auto& [prefix, set] : ids) {
    if (set.size() >= 2) {
      out.emplace_back(prefix, static_cast<int>(set.size()));
    }
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return a.first.size() != b.first.size() ? a.first.size() > b.first.size() : a.first < b.first;
  });
  return out;
}

// Newest <runs>/<run>/<name>/status (run-demo.sh layout), or "".
std::string status_suggestion(const std::string& runs, const std::string& name) {
  namespace fs = std::filesystem;
  std::string best;
  fs::file_time_type best_time{};
  std::error_code ec;
  for (const auto& run : fs::directory_iterator(runs, ec)) {
    const auto path = run.path() / name / "status";
    if (fs::exists(path, ec)) {
      const auto t = fs::last_write_time(path, ec);
      if (best.empty() || t > best_time) {
        best = path.string();
        best_time = t;
      }
    }
  }
  return best;
}

std::string ask(const std::string& prompt) {
  std::cout << prompt << std::flush;
  std::string line;
  if (!std::getline(std::cin, line)) {
    return "";
  }
  return line;
}

// Interactive "add app": name (or a number from the daemon prefixes) and status file.
void add_app(std::vector<App>& apps, const MapView& map, const std::string& runs, std::string name, std::string status) {
  if (name.empty()) {
    const auto prefixes = prefix_suggestions(map);
    std::cout << "\nadd app: daemon prefixes in Mercury\n";
    for (std::size_t i = 0; i < prefixes.size(); ++i) {
      std::cout << "  " << i + 1 << ") " << prefixes[i].first << "  (" << prefixes[i].second << " daemons)\n";
    }
    name = ask("app name or number (empty = cancel): ");
    if (!name.empty() && std::all_of(name.begin(), name.end(), ::isdigit)) {
      const auto n = static_cast<std::size_t>(std::stoul(name));
      name = n >= 1 && n <= prefixes.size() ? prefixes[n - 1].first : "";
    }
    if (name.empty()) {
      return;
    }
  }
  if (status.empty()) {
    const auto suggestion = status_suggestion(runs, name);
    status = ask("status file for " + name + (suggestion.empty() ? "" : " [" + suggestion + "]") + " ('-' = none): ");
    if (status.empty()) {
      status = suggestion;
    }
  }
  if (status == "-") {
    status.clear();
  }
  apps.erase(std::remove_if(apps.begin(), apps.end(), [&](const App& a) { return a.name == name; }), apps.end());
  apps.push_back({name, status});
}

void remove_app(std::vector<App>& apps, std::string name) {
  if (apps.empty()) {
    return;
  }
  if (name.empty()) {
    std::cout << "\nremove app:\n";
    for (std::size_t i = 0; i < apps.size(); ++i) {
      std::cout << "  " << i + 1 << ") " << (apps[i].name.empty() ? status_app(apps[i].status) : apps[i].name)
                << (apps[i].status.empty() ? "" : "  " + apps[i].status) << '\n';
    }
    name = ask("app name or number (empty = cancel): ");
    if (!name.empty() && std::all_of(name.begin(), name.end(), ::isdigit)) {
      const auto n = static_cast<std::size_t>(std::stoul(name));
      if (n >= 1 && n <= apps.size()) {
        apps.erase(apps.begin() + static_cast<long>(n - 1));
      }
      return;
    }
  }
  apps.erase(std::remove_if(apps.begin(), apps.end(),
                            [&](const App& a) { return a.name == name || (a.name.empty() && status_app(a.status) == name); }),
             apps.end());
}

// One typed line in watch mode: "a [NAME [STATUS]]", "r [NAME]", "q"; empty = menu.
bool command(std::string line, std::vector<App>& apps, const MapView& map, const std::string& runs) {
  if (line.empty()) {
    line = ask("\na) add app   r) remove app   q) quit   Enter) back: ");
  }
  std::istringstream in(line);
  std::string verb, name, status;
  in >> verb >> name >> status;
  if (verb == "q" || verb == "quit") {
    return false;
  }
  if (verb == "a" || verb == "add") {
    add_app(apps, map, runs, name, status);
  } else if (verb == "r" || verb == "rm" || verb == "remove") {
    remove_app(apps, name);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const char* usage =
      "usage: speculator <port> [--watch SECONDS] [--once] [--status FILE]... [--app NAME[=STATUS]]... [--runs DIR]\n"
      "  on a terminal it refreshes every 2 s; Enter opens a menu (a add app, r remove app, q quit)\n";
  if (argc < 2) {
    std::cerr << usage;
    return 1;
  }
  double watch = 0;
  bool once = false;
  std::vector<App> apps;
  const std::string home = std::getenv("HOME") != nullptr ? std::getenv("HOME") : ".";
  std::string runs = std::getenv("SPECULATOR_RUNS") != nullptr ? std::getenv("SPECULATOR_RUNS")
                     : std::filesystem::is_directory(home + "/Documents/Benchmarks/demo/runs") ? home + "/Documents/Benchmarks/demo/runs"
                                                                                                : home + "/Benchmarks/demo/runs";
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--watch" && i + 1 < argc) {
      watch = std::stod(argv[++i]);
    } else if (arg == "--once") {
      once = true;
    } else if (arg == "--status" && i + 1 < argc) {
      apps.push_back({"", argv[++i]});
    } else if (arg == "--app" && i + 1 < argc) {
      const std::string v = argv[++i];
      const auto eq = v.find('=');
      apps.push_back({v.substr(0, eq), eq == std::string::npos ? "" : v.substr(eq + 1)});
    } else if (arg == "--runs" && i + 1 < argc) {
      runs = argv[++i];
    } else {
      std::cerr << usage;
      return 1;
    }
  }
  int port_number = 0;
  try {
    port_number = std::stoi(argv[1]);
    if (port_number < 1 || port_number > 65535) {
      throw std::runtime_error("invalid port");
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
  const bool interactive = !once && isatty(0) && isatty(1);
  if (interactive && watch <= 0) {
    watch = 2;
  }
  if (once) {
    watch = 0;
  }
  MapView last;
  while (true) {
    std::ostringstream frame;
    bool ok = true;
    std::vector<std::string> names, files;
    for (const auto& app : apps) {
      const auto name = app.name.empty() ? status_app(app.status) : app.name;
      if (!name.empty()) {
        names.push_back(name);
      }
      if (!app.status.empty()) {
        files.push_back(app.status);
      }
    }
    try {
      last = parse_map(fetch_map(static_cast<std::uint16_t>(port_number)));
      print_tree(frame, last);
      print_apps(frame, last, names);
    } catch (const std::exception& error) {
      ok = false;
      frame << "mercury :" << port_number << ": " << error.what() << '\n';
    }
    frame << std::fixed << std::setprecision(2);
    print_status(frame, files);
    if (watch <= 0) {
      (ok ? std::cout : std::cerr) << frame.str();
      return ok ? 0 : 1;
    }
    std::cout << "\033[H\033[2J" << frame.str();
    if (interactive) {
      std::cout << "\n[Enter] menu: a add app, r remove app, q quit   (or type: a hotel-a, r hotel-a)\n";
    }
    std::cout << std::flush;
    if (!interactive) {
      std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(watch * 1000)));
      continue;
    }
    pollfd in{0, POLLIN, 0};
    if (::poll(&in, 1, static_cast<int>(watch * 1000)) > 0) {
      std::string line;
      if (!std::getline(std::cin, line) || !command(line, apps, last, runs)) {
        return 0;
      }
    }
  }
}
