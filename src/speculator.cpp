#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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

struct MapView {
  int mercury_cpu = -1;
  std::vector<int> allocatable;
  std::vector<Group> groups;
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

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: speculator <port>\n";
    return 1;
  }
  try {
    const int port_number = std::stoi(argv[1]);
    if (port_number < 1 || port_number > 65535) {
      throw std::runtime_error("invalid port");
    }
    const auto map = parse_map(fetch_map(static_cast<std::uint16_t>(port_number)));
    print_tree(std::cout, map);
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
