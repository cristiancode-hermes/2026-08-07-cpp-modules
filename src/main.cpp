// SPDX-License-Identifier: MIT
// cpp-modules — Linux kernel module viewer from /proc/modules (C++23)
// Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o modules
// Usage: ./modules [--json] [--watch N] [--sort name|size|users] [--search TERM] [--no-color] [--selftest]

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <print>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace modules {

// ── ANSI palette (Catppuccin) ───────────────────────────────────────────────
constexpr const char* kRed     = "\x1b[31m";
constexpr const char* kGreen   = "\x1b[32m";
constexpr const char* kYellow  = "\x1b[33m";
constexpr const char* kCyan    = "\x1b[36m";
constexpr const char* kDim     = "\x1b[2m";
constexpr const char* kBold    = "\x1b[1m";
constexpr const char* kReset   = "\x1b[0m";
constexpr const char* kClear   = "\x1b[2J\x1b[H";

// ── Config ──────────────────────────────────────────────────────────────────
bool g_json       = false;
bool g_no_color   = false;
int  g_watch      = 0;
bool g_selftest   = false;
int  g_sort_mode  = 0;  // 0=name, 1=size, 2=users
std::string g_search;
volatile std::sig_atomic_t g_running = 1;

void signal_handler(int) { g_running = 0; }

// ── Module info ─────────────────────────────────────────────────────────────
struct Module {
  std::string name;
  unsigned long long size_bytes{};
  int used_by_count{};
  std::vector<std::string> used_by;
  bool live{};
};

std::vector<Module> parse_proc_modules() {
  std::vector<Module> result;
  std::ifstream f("/proc/modules");
  if (!f) return result;

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;

    Module mod;
    auto pos = line.find(' ');
    if (pos == std::string::npos) continue;
    mod.name = line.substr(0, pos);

    auto rest = line.substr(pos + 1);
    auto size_end = rest.find(' ');
    if (size_end == std::string::npos) continue;
    auto size_str = rest.substr(0, size_end);
    auto [ptr, ec] = std::from_chars(size_str.data(), size_str.data() + size_str.size(), mod.size_bytes);
    if (ec != std::errc()) continue;

    rest = rest.substr(size_end + 1);
    auto count_end = rest.find(' ');
    if (count_end == std::string::npos) continue;
    auto count_str = rest.substr(0, count_end);
    auto [ptr2, ec2] = std::from_chars(count_str.data(), count_str.data() + count_str.size(), mod.used_by_count);
    if (ec2 != std::errc()) continue;

    rest = rest.substr(count_end + 1);
    auto usedby_end = rest.find(' ');
    if (usedby_end == std::string::npos) continue;
    auto usedby_str = rest.substr(0, usedby_end);
    if (usedby_str != "-") {
      std::stringstream ss(std::string{usedby_str});
      std::string token;
      while (std::getline(ss, token, ',')) {
        if (!token.empty()) mod.used_by.push_back(token);
      }
    }

    // Rest is "Live 0x..." or similar
    mod.live = (rest.find("Live") != std::string::npos);

    result.push_back(std::move(mod));
  }
  return result;
}

// ── Human size formatting ───────────────────────────────────────────────────
std::string fmt_size(unsigned long long bytes) {
  char buf[32];
  if (bytes >= 1048576ULL) {
    std::snprintf(buf, sizeof(buf), "%.1fM", bytes / 1048576.0);
  } else if (bytes >= 1024ULL) {
    std::snprintf(buf, sizeof(buf), "%.1fK", bytes / 1024.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%lluB", bytes);
  }
  return buf;
}

// ── Color helpers ───────────────────────────────────────────────────────────
void print_colored(const std::string& s, const char* color) {
  if (g_no_color) { std::print(std::cout, "{}", s); return; }
  std::print(std::cout, "{}{}{}", color, s, kReset);
}

// ── Table output ────────────────────────────────────────────────────────────
void print_table(const std::vector<Module>& mods) {
  if (mods.empty()) {
    std::println(std::cout, "  No modules loaded or /proc/modules unavailable.");
    return;
  }

  auto with_color = [&](const std::string& s, const char* color) -> std::string {
    if (g_no_color || color == nullptr) return s;
    return std::string(color) + s + kReset;
  };

  // Header
  std::println(std::cout, "{}", with_color(
    "  MODULE                    SIZE     USERS  STATUS  DEPENDENCIES", kBold));
  std::println(std::cout, "  {}───", with_color("────", kDim));

  for (const auto& m : mods) {
    const char* status_color = m.live ? kGreen : kYellow;
    std::string status_str = m.live ? "Live" : "Persist";
    std::string used_by_str;
    if (m.used_by_count > 0) {
      bool first = true;
      for (const auto& dep : m.used_by) {
        if (!first) used_by_str += ", ";
        used_by_str += dep;
        first = false;
      }
    } else {
      used_by_str = "-";
    }

    std::string size_str = fmt_size(m.size_bytes);

    std::string color = kReset;
    if (!g_no_color) {
      if (m.size_bytes >= 1048576ULL) color = kRed;
      else if (m.size_bytes >= 262144ULL) color = kYellow;
      else color = kDim;
    }

    std::print(std::cout, "  ");
    print_colored(std::format("{:<26}", m.name), kCyan);
    std::print(std::cout, " ");
    if (!g_no_color) {
      std::print(std::cout, "{}{:>8}{}", color, size_str, kReset);
    } else {
      std::print(std::cout, "{:>8}", size_str);
    }
    std::print(std::cout, "  {:>4}", m.used_by_count);
    std::print(std::cout, "  ");
    print_colored(std::format("{:<7}", status_str), status_color);
    std::println(std::cout, "  {}", used_by_str);
  }

  // Footer
  std::println(std::cout, "  {}───", with_color("────", kDim));
  unsigned long long total_size = 0;
  int total_modules = 0;
  for (const auto& m : mods) {
    total_size += m.size_bytes;
    total_modules++;
  }
  std::println(std::cout, "  {} {} modules · {} used · total {}",
    with_color("∑", kBold),
    total_modules,
    mods.size(),
    fmt_size(total_size));
}

// ── JSON output ─────────────────────────────────────────────────────────────
void print_json(const std::vector<Module>& mods) {
  std::println(std::cout, "{{");
  std::println(std::cout, "  \"modules\": [");
  for (size_t i = 0; i < mods.size(); ++i) {
    const auto& m = mods[i];
    std::println(std::cout, "    {{");
    std::println(std::cout, "      \"name\": \"{}\",", m.name);
    std::println(std::cout, "      \"size_bytes\": {},", m.size_bytes);
    std::println(std::cout, "      \"used_by_count\": {},", m.used_by_count);
    std::println(std::cout, "      \"live\": {},", m.live ? "true" : "false");
    std::println(std::cout, "      \"used_by\": [");
    for (size_t j = 0; j < m.used_by.size(); ++j) {
      auto comma = (j < m.used_by.size() - 1) ? "," : "";
      std::println(std::cout, "        \"{}\"{}", m.used_by[j], comma);
    }
    std::println(std::cout, "      ]");
    auto comma = (i < mods.size() - 1) ? "," : "";
    std::println(std::cout, "    }}{}", comma);
  }
  std::println(std::cout, "  ],");
  unsigned long long total_size = 0;
  for (const auto& m : mods) total_size += m.size_bytes;
  std::println(std::cout, "  \"summary\": {{");
  std::println(std::cout, "    \"total_modules\": {},", mods.size());
  std::println(std::cout, "    \"total_size_bytes\": {}", total_size);
  std::println(std::cout, "  }}");
  std::println(std::cout, "}}");
}

// ── Summary line ────────────────────────────────────────────────────────────
void print_summary(const std::vector<Module>& mods) {
  if (mods.empty()) return;
  unsigned long long total = 0;
  unsigned long long max_size = 0;
  std::string_view largest_mod;
  int max_deps = 0;
  std::string_view most_dep;

  for (const auto& m : mods) {
    total += m.size_bytes;
    if (m.size_bytes > max_size) {
      max_size = m.size_bytes;
      largest_mod = m.name;
    }
    if (m.used_by_count > max_deps) {
      max_deps = m.used_by_count;
      most_dep = m.name;
    }
  }

  if (!g_no_color) std::print(std::cout, "{}", kDim);
  std::print(std::cout, "  {} modules · {} · largest: {} ({}) · most-depended: {} ({} users)",
    mods.size(), fmt_size(total), largest_mod, fmt_size(max_size),
    most_dep, max_deps);
  if (!g_no_color) std::print(std::cout, "{}", kReset);
  std::println(std::cout);
}

// ── Self-test ───────────────────────────────────────────────────────────────
int run_selftest() {
  unsigned pass = 0, fail = 0;

  auto check = [&](bool ok, const std::string& desc) {
    if (ok) {
      if (!g_no_color) std::print(std::cout, "  {}✓{} ", kGreen, kReset);
      else std::print(std::cout, "  PASS ");
      std::println(std::cout, "{}", desc);
      ++pass;
    } else {
      if (!g_no_color) std::print(std::cout, "  {}✗{} ", kRed, kReset);
      else std::print(std::cout, "  FAIL ");
      std::println(std::cout, "{}", desc);
      ++fail;
    }
  };

  // 1. Parse /proc/modules
  auto mods = parse_proc_modules();
  check(!mods.empty(), std::format("/proc/modules parsed: {} modules loaded", mods.size()));

  if (!mods.empty()) {
    // 2. Each module has name and size
    bool all_have_name = std::ranges::all_of(mods, [](const auto& m) { return !m.name.empty(); });
    bool all_have_size = std::ranges::all_of(mods, [](const auto& m) { return m.size_bytes > 0; });
    check(all_have_name, "All modules have a name");
    check(all_have_size, "All modules have a positive size");

    // 3. used_by_count >= used_by.size() (count may be larger due to anonymous references)
    bool consistent = std::ranges::all_of(mods, [](const auto& m) {
      return m.used_by_count >= static_cast<int>(m.used_by.size());
    });
    check(consistent, "used_by_count >= used_by.size() for all modules");

    // 4. at least one module has dependencies
    bool has_deps = std::ranges::any_of(mods, [](const auto& m) { return m.used_by_count > 0; });
    check(has_deps, "At least one module has dependencies");

    // 5. All are Live
    bool all_live = std::ranges::all_of(mods, [](const auto& m) { return m.live; });
    check(all_live, "All modules have Live status");

    // 6. Sort by size
    auto sorted = mods;
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.size_bytes > b.size_bytes; });
    bool sorted_desc = true;
    for (size_t i = 1; i < sorted.size(); ++i) {
      if (sorted[i-1].size_bytes < sorted[i].size_bytes) { sorted_desc = false; break; }
    }
    check(sorted_desc, "Sort by size descending works");

    // 7. Sort by users
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.used_by_count > b.used_by_count; });
    bool sorted_users = true;
    for (size_t i = 1; i < sorted.size(); ++i) {
      if (sorted[i-1].used_by_count < sorted[i].used_by_count) { sorted_users = false; break; }
    }
    check(sorted_users, "Sort by users descending works");

    // 8. JSON output valid
    std::stringstream js;
    auto old = std::cout.rdbuf(js.rdbuf());
    print_json(mods);
    std::cout.rdbuf(old);
    check(js.str().size() > 100, std::format("JSON output is non-trivial (>100 chars, got {})", js.str().size()));
    check(js.str().find("modules") != std::string::npos, "JSON contains 'modules' key");
    check(js.str().find("summary") != std::string::npos, "JSON contains 'summary' key");

    // 9. Size formatting
    check(fmt_size(512) == "512B", "fmt_size(512) == \"512B\"");
    check(fmt_size(2048) == "2.0K", "fmt_size(2048) == \"2.0K\"");
    check(fmt_size(1048576) == "1.0M", "fmt_size(1048576) == \"1.0M\"");
    check(fmt_size(1572864) == "1.5M", "fmt_size(1572864) == \"1.5M\"");

    // 10. Total size is reasonable (>0)
    unsigned long long total = 0;
    for (const auto& m : mods) total += m.size_bytes;
    check(total > 0, std::format("Total module size > 0 ({} = {} MB)", fmt_size(total), total / 1048576));
  }

  std::println(std::cout);
  std::string result_line = (fail == 0)
    ? std::format("  {} {}/{} passed",
        (g_no_color ? "" : kGreen), pass, pass + fail)
    : std::format("  {} {}/{} passed",
        (g_no_color ? "" : kRed), pass, pass + fail);
  if (fail == 0) result_line += g_no_color ? " ✅" : std::string(kReset) + " ✅";
  else result_line += g_no_color ? " ❌" : std::string(kReset) + " ❌";
  std::println(std::cout, "{}", result_line);

  return fail > 0 ? 1 : 0;
}

// ── Main ────────────────────────────────────────────────────────────────────
int app(int argc, char** argv) {
  // Parse args (stdlib-only)
  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    if (arg == "--help" || arg == "-h") {
      std::println(std::cout, "  modules — Linux kernel module viewer");
      std::println(std::cout);
      std::println(std::cout, "  Usage: ./modules [options]");
      std::println(std::cout);
      std::println(std::cout, "  Options:");
      std::println(std::cout, "    --json          JSON output");
      std::println(std::cout, "    --watch N       Watch mode, refresh every N seconds");
      std::println(std::cout, "    --sort mode     Sort: name, size, users (default: name)");
      std::println(std::cout, "    --search TERM   Filter modules by name");
      std::println(std::cout, "    --no-color      Plain text output");
      std::println(std::cout, "    --selftest      Run self-tests");
      std::println(std::cout, "    --help          This help");
      return 0;
    }
  }

  // Second pass for actual options
  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    if (arg == "--json") g_json = true;
    else if (arg == "--no-color") g_no_color = true;
    else if (arg == "--selftest") { g_selftest = true; }
    else if (arg == "--watch" && i + 1 < argc) { g_watch = std::stoi(argv[++i]); }
    else if (arg == "--sort" && i + 1 < argc) {
      ++i;
      std::string_view mode{argv[i]};
      if (mode == "size") g_sort_mode = 1;
      else if (mode == "users") g_sort_mode = 2;
      else g_sort_mode = 0;
    }
    else if (arg == "--search" && i + 1 < argc) { g_search = argv[++i]; }
  }

  if (g_selftest) {
    return run_selftest();
  }

  // Header
  if (!g_json) {
    std::println(std::cout, "{}", kClear);
    print_colored("  Kernel Module Viewer", kBold);
    std::println(std::cout, "  {}", std::string(32, '-'));
  }

  std::signal(SIGINT, signal_handler);

  do {
    auto mods = parse_proc_modules();

    if (!g_search.empty()) {
      std::vector<Module> filtered;
      for (const auto& m : mods) {
        if (m.name.find(g_search) != std::string::npos) {
          filtered.push_back(m);
        }
      }
      mods = std::move(filtered);
    }

    // Sort
    if (g_sort_mode == 1) {
      std::ranges::sort(mods, [](const auto& a, const auto& b) {
        return a.size_bytes > b.size_bytes;
      });
    } else if (g_sort_mode == 2) {
      std::ranges::sort(mods, [](const auto& a, const auto& b) {
        return a.used_by_count > b.used_by_count;
      });
    } else {
      std::ranges::sort(mods, [](const auto& a, const auto& b) {
        return a.name < b.name;
      });
    }

    if (g_json) {
      print_json(mods);
    } else {
      if (g_search.empty()) {
        print_table(mods);
      } else {
        print_table(mods);
        std::println(std::cout, "  {} (filter: \"{}\")",
          mods.size() > 1 ? std::format("{} results", mods.size()) : "1 result",
          g_search);
      }
    }

    if (g_watch > 0 && g_running) {
      std::this_thread::sleep_for(std::chrono::seconds(g_watch));
    }

  } while (g_watch > 0 && g_running);

  if (g_watch > 0 && !g_running) {
    std::println(std::cout);
    print_colored("  Interrupted.", kDim);
    std::println(std::cout);
  }

  return 0;
}

} // namespace modules

int main(int argc, char** argv) {
  return modules::app(argc, argv);
}
