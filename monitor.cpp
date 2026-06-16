/**
 * hwmon.cpp — Linux Hardware Monitor
 *
 * Direktno čita kernel sysfs interface (/sys/class/hwmon) bez ikakvih
 * eksternih biblioteka. Prikazuje CPU temp, fan speeds, voltages i
 * druge senzore u real-time terminal UI-u.
 *
 * Kompajliranje:
 *   g++ -O2 -std=c++17 -o hwmon hwmon.cpp
 *
 * Pokretanje:
 *   sudo ./hwmon           (sudo za pristup svim senzorima)
 *   ./hwmon --once         (ispiši jednom i izađi)
 *   ./hwmon --interval 2   (refresh svake 2 sekunde, default: 1)
 */

#include <algorithm>
#include <chrono>
#include <unistd.h>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ─── ANSI escape codes ───────────────────────────────────────────────────────
namespace ansi {
    const std::string RESET   = "\033[0m";
    const std::string BOLD    = "\033[1m";
    const std::string DIM     = "\033[2m";

    const std::string RED     = "\033[31m";
    const std::string GREEN   = "\033[32m";
    const std::string YELLOW  = "\033[33m";
    const std::string BLUE    = "\033[34m";
    const std::string MAGENTA = "\033[35m";
    const std::string CYAN    = "\033[36m";
    const std::string WHITE   = "\033[37m";

    const std::string BG_RED  = "\033[41m";

    const std::string CLEAR_SCREEN = "\033[2J\033[H";
    const std::string HIDE_CURSOR  = "\033[?25l";
    const std::string SHOW_CURSOR  = "\033[?25h";

    std::string move(int row, int col) {
        return "\033[" + std::to_string(row) + ";" + std::to_string(col) + "H";
    }
    std::string color_256(int n) {
        return "\033[38;5;" + std::to_string(n) + "m";
    }
}

// ─── Helper: read single-value sysfs file ────────────────────────────────────
std::optional<std::string> read_sysfs(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return std::nullopt;
    std::string val;
    std::getline(f, val);
    if (val.empty()) return std::nullopt;
    return val;
}

std::optional<long long> read_sysfs_int(const fs::path& path) {
    auto s = read_sysfs(path);
    if (!s) return std::nullopt;
    try { return std::stoll(*s); }
    catch (...) { return std::nullopt; }
}

// ─── Sensor structs ──────────────────────────────────────────────────────────
struct TempSensor {
    std::string label;
    double value_c;           // current °C
    std::optional<double> max_c;
    std::optional<double> crit_c;
    std::optional<double> hyst_c;
};

struct FanSensor {
    std::string label;
    long long rpm;
    std::optional<long long> min_rpm;
    std::optional<long long> max_rpm;
    int pwm = -1;             // 0–255, -1 = unknown
};

struct VoltageSensor {
    std::string label;
    double value_v;
    std::optional<double> min_v;
    std::optional<double> max_v;
    std::optional<double> lcrit_v;
    std::optional<double> crit_v;
};

struct PowerSensor {
    std::string label;
    double value_w;
    std::optional<double> max_w;
};

struct CurrentSensor {
    std::string label;
    double value_a;
};

struct HwmonChip {
    std::string name;
    fs::path    path;

    std::vector<TempSensor>    temps;
    std::vector<FanSensor>     fans;
    std::vector<VoltageSensor> voltages;
    std::vector<PowerSensor>   powers;
    std::vector<CurrentSensor> currents;
};

// ─── Discovery + parsing ─────────────────────────────────────────────────────
HwmonChip parse_chip(const fs::path& chip_path) {
    HwmonChip chip;
    chip.path = chip_path;

    // Chip name
    auto name = read_sysfs(chip_path / "name");
    chip.name = name.value_or(chip_path.filename().string());

    // Collect all files once into a map for quick lookup
    std::map<std::string, fs::path> files;
    for (auto& entry : fs::directory_iterator(chip_path)) {
        if (entry.is_regular_file() || entry.is_symlink())
            files[entry.path().filename().string()] = entry.path();
    }

    // ── Temperature sensors (tempN_*) ──────────────────────────────────────
    // Find all unique N values
    std::map<int, TempSensor> temp_map;
    for (auto& [fname, fpath] : files) {
        if (fname.rfind("temp", 0) != 0) continue;
        // Extract index
        size_t us = fname.find('_');
        if (us == std::string::npos) continue;
        int idx;
        try { idx = std::stoi(fname.substr(4, us - 4)); }
        catch (...) { continue; }

        auto& ts = temp_map[idx];
        std::string key = fname.substr(us + 1);

        if (key == "input") {
            auto v = read_sysfs_int(fpath);
            if (v) ts.value_c = *v / 1000.0;
        } else if (key == "label") {
            auto v = read_sysfs(fpath);
            if (v) ts.label = *v;
        } else if (key == "max") {
            auto v = read_sysfs_int(fpath);
            if (v) ts.max_c = *v / 1000.0;
        } else if (key == "crit") {
            auto v = read_sysfs_int(fpath);
            if (v) ts.crit_c = *v / 1000.0;
        } else if (key == "hyst" || key == "crit_hyst") {
            auto v = read_sysfs_int(fpath);
            if (v) ts.hyst_c = *v / 1000.0;
        }
    }
    for (auto& [idx, ts] : temp_map) {
        if (ts.label.empty())
            ts.label = "temp" + std::to_string(idx);
        if (ts.value_c != 0.0 || temp_map.size() == 1)
            chip.temps.push_back(ts);
    }
    std::sort(chip.temps.begin(), chip.temps.end(),
        [](auto& a, auto& b){ return a.label < b.label; });

    // ── Fan sensors (fanN_*) ───────────────────────────────────────────────
    std::map<int, FanSensor> fan_map;
    for (auto& [fname, fpath] : files) {
        if (fname.rfind("fan", 0) != 0) continue;
        size_t us = fname.find('_');
        if (us == std::string::npos) continue;
        int idx;
        try { idx = std::stoi(fname.substr(3, us - 3)); }
        catch (...) { continue; }

        auto& fs_ref = fan_map[idx];
        std::string key = fname.substr(us + 1);

        if (key == "input") {
            auto v = read_sysfs_int(fpath);
            if (v) fs_ref.rpm = *v;
        } else if (key == "label") {
            auto v = read_sysfs(fpath);
            if (v) fs_ref.label = *v;
        } else if (key == "min") {
            auto v = read_sysfs_int(fpath);
            if (v) fs_ref.min_rpm = *v;
        } else if (key == "max") {
            auto v = read_sysfs_int(fpath);
            if (v) fs_ref.max_rpm = *v;
        }
    }
    // Check for PWM (pwmN files correspond to fanN)
    for (auto& [fname, fpath] : files) {
        if (fname.rfind("pwm", 0) != 0) continue;
        if (fname.size() < 4) continue;
        // pwmN (no underscore = raw pwm value)
        if (fname.find('_') != std::string::npos) continue;
        int idx;
        try { idx = std::stoi(fname.substr(3)); }
        catch (...) { continue; }
        auto v = read_sysfs_int(fpath);
        if (v && fan_map.count(idx))
            fan_map[idx].pwm = (int)*v;
    }
    for (auto& [idx, fs_ref] : fan_map) {
        if (fs_ref.label.empty())
            fs_ref.label = "fan" + std::to_string(idx);
        chip.fans.push_back(fs_ref);
    }
    std::sort(chip.fans.begin(), chip.fans.end(),
        [](auto& a, auto& b){ return a.label < b.label; });

    // ── Voltage sensors (inN_*) ────────────────────────────────────────────
    std::map<int, VoltageSensor> volt_map;
    for (auto& [fname, fpath] : files) {
        if (fname.rfind("in", 0) != 0) continue;
        size_t us = fname.find('_');
        if (us == std::string::npos) continue;
        int idx;
        try { idx = std::stoi(fname.substr(2, us - 2)); }
        catch (...) { continue; }

        auto& vs = volt_map[idx];
        std::string key = fname.substr(us + 1);

        if (key == "input") {
            auto v = read_sysfs_int(fpath);
            if (v) vs.value_v = *v / 1000.0;
        } else if (key == "label") {
            auto v = read_sysfs(fpath);
            if (v) vs.label = *v;
        } else if (key == "min") {
            auto v = read_sysfs_int(fpath);
            if (v) vs.min_v = *v / 1000.0;
        } else if (key == "max") {
            auto v = read_sysfs_int(fpath);
            if (v) vs.max_v = *v / 1000.0;
        } else if (key == "lcrit") {
            auto v = read_sysfs_int(fpath);
            if (v) vs.lcrit_v = *v / 1000.0;
        } else if (key == "crit") {
            auto v = read_sysfs_int(fpath);
            if (v) vs.crit_v = *v / 1000.0;
        }
    }
    for (auto& [idx, vs] : volt_map) {
        if (vs.label.empty())
            vs.label = "in" + std::to_string(idx);
        if (vs.value_v > 0.0)
            chip.voltages.push_back(vs);
    }
    std::sort(chip.voltages.begin(), chip.voltages.end(),
        [](auto& a, auto& b){ return a.label < b.label; });

    // ── Power sensors (powerN_*) ───────────────────────────────────────────
    std::map<int, PowerSensor> pwr_map;
    for (auto& [fname, fpath] : files) {
        if (fname.rfind("power", 0) != 0) continue;
        size_t us = fname.find('_');
        if (us == std::string::npos) continue;
        int idx;
        try { idx = std::stoi(fname.substr(5, us - 5)); }
        catch (...) { continue; }

        auto& ps = pwr_map[idx];
        std::string key = fname.substr(us + 1);

        if (key == "input") {
            auto v = read_sysfs_int(fpath);
            if (v) ps.value_w = *v / 1'000'000.0;   // µW → W
        } else if (key == "label") {
            auto v = read_sysfs(fpath);
            if (v) ps.label = *v;
        } else if (key == "max") {
            auto v = read_sysfs_int(fpath);
            if (v) ps.max_w = *v / 1'000'000.0;
        }
    }
    for (auto& [idx, ps] : pwr_map) {
        if (ps.label.empty())
            ps.label = "power" + std::to_string(idx);
        if (ps.value_w >= 0.0)
            chip.powers.push_back(ps);
    }

    // ── Current sensors (currN_*) ──────────────────────────────────────────
    std::map<int, CurrentSensor> curr_map;
    for (auto& [fname, fpath] : files) {
        if (fname.rfind("curr", 0) != 0) continue;
        size_t us = fname.find('_');
        if (us == std::string::npos) continue;
        int idx;
        try { idx = std::stoi(fname.substr(4, us - 4)); }
        catch (...) { continue; }

        auto& cs = curr_map[idx];
        std::string key = fname.substr(us + 1);

        if (key == "input") {
            auto v = read_sysfs_int(fpath);
            if (v) cs.value_a = *v / 1000.0;
        } else if (key == "label") {
            auto v = read_sysfs(fpath);
            if (v) cs.label = *v;
        }
    }
    for (auto& [idx, cs] : curr_map) {
        if (cs.label.empty())
            cs.label = "curr" + std::to_string(idx);
        chip.currents.push_back(cs);
    }

    return chip;
}

std::vector<HwmonChip> discover_chips() {
    std::vector<HwmonChip> chips;
    fs::path base = "/sys/class/hwmon";
    if (!fs::exists(base)) return chips;

    for (auto& entry : fs::directory_iterator(base)) {
        // Follow symlink to real path
        fs::path real = fs::canonical(entry.path());
        try {
            chips.push_back(parse_chip(real));
        } catch (...) {}
    }

    // Sort by chip name for stable ordering
    std::sort(chips.begin(), chips.end(),
        [](auto& a, auto& b){ return a.name < b.name; });
    return chips;
}

// ─── Also read /proc/cpuinfo for CPU model name ──────────────────────────────
std::string cpu_model_name() {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("model name", 0) == 0) {
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string val = line.substr(colon + 2);
                // Trim
                while (!val.empty() && std::isspace(val.back())) val.pop_back();
                return val;
            }
        }
    }
    return "Unknown CPU";
}

// Read CPU freq from /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
std::optional<double> cpu_freq_mhz() {
    auto v = read_sysfs_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    if (!v) return std::nullopt;
    return *v / 1000.0;   // kHz → MHz
}

// ─── Display helpers ─────────────────────────────────────────────────────────

// Draw a bar [████████░░░░] of given width, fill 0.0–1.0
std::string bar(double fill, int width, const std::string& color) {
    fill = std::clamp(fill, 0.0, 1.0);
    int filled = static_cast<int>(std::round(fill * width));
    int empty  = width - filled;
    std::string result = "[";
    result += color;
    result += std::string(filled, '\xe2') + std::string(filled * 2, '\x96') ;
    // Use Unicode block ▓ for filled, ░ for empty
    // We'll do it in ASCII-safe way:
    result = "[" + color;
    for (int i = 0; i < filled; ++i) result += "█";
    result += ansi::DIM;
    for (int i = 0; i < empty; ++i)  result += "░";
    result += ansi::RESET + "]";
    return result;
}

std::string temp_color(double c, std::optional<double> crit) {
    double limit = crit.value_or(100.0);
    double ratio = c / limit;
    if (ratio > 0.90) return ansi::BG_RED + ansi::WHITE + ansi::BOLD;
    if (ratio > 0.75) return ansi::RED + ansi::BOLD;
    if (ratio > 0.55) return ansi::YELLOW;
    return ansi::GREEN;
}

std::string fan_color(long long rpm, std::optional<long long> max_rpm) {
    if (rpm == 0) return ansi::DIM + ansi::WHITE;
    if (!max_rpm) return ansi::CYAN;
    double ratio = static_cast<double>(rpm) / *max_rpm;
    if (ratio > 0.85) return ansi::RED;
    if (ratio > 0.60) return ansi::YELLOW;
    return ansi::CYAN;
}

std::string volt_color(double v, std::optional<double> min_v, std::optional<double> max_v) {
    if (!min_v || !max_v) return ansi::WHITE;
    double range  = *max_v - *min_v;
    double center = (*max_v + *min_v) / 2.0;
    double dev    = std::abs(v - center) / (range / 2.0);
    if (dev > 0.90) return ansi::RED + ansi::BOLD;
    if (dev > 0.60) return ansi::YELLOW;
    return ansi::GREEN;
}

// Pad / truncate string to fixed width
std::string fixed_w(const std::string& s, int w) {
    if ((int)s.size() >= w) return s.substr(0, w);
    return s + std::string(w - s.size(), ' ');
}

// ─── Render ──────────────────────────────────────────────────────────────────
void render(const std::vector<HwmonChip>& chips, int refresh_s) {
    std::ostringstream out;

    // Header
    auto now     = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::string ts(32, '\0');
    strftime(ts.data(), ts.size(), "%Y-%m-%d %H:%M:%S", localtime(&now));
    ts.resize(strlen(ts.c_str()));

    std::string model = cpu_model_name();
    auto freq = cpu_freq_mhz();

    out << ansi::BOLD << ansi::CYAN
        << "╔══════════════════════════════════════════════════════════════╗\n"
        << "║  " << ansi::WHITE << "⚡ hwmon — Linux Hardware Monitor"
        << ansi::CYAN << "                            ║\n"
        << "║  " << ansi::DIM << fixed_w(model, 55) << ansi::CYAN << "  ║\n"
        << "╚══════════════════════════════════════════════════════════════╝"
        << ansi::RESET << "\n";

    out << ansi::DIM << "  Updated: " << ts;
    if (freq) {
        out << "  │  CPU freq: " << ansi::WHITE << std::fixed
            << std::setprecision(0) << *freq << " MHz" << ansi::RESET;
    }
    out << ansi::DIM << "  │  refresh: " << refresh_s << "s  │  q = quit"
        << ansi::RESET << "\n\n";

    for (auto& chip : chips) {
        bool has_data = !chip.temps.empty() || !chip.fans.empty()
                     || !chip.voltages.empty() || !chip.powers.empty()
                     || !chip.currents.empty();
        if (!has_data) continue;

        // Chip header
        out << ansi::BOLD << ansi::BLUE
            << "  ▶ " << chip.name
            << ansi::DIM << "  (" << chip.path.string() << ")"
            << ansi::RESET << "\n";

        // ── Temperatures ──────────────────────────────────────────────────
        if (!chip.temps.empty()) {
            out << ansi::BOLD << "    Temperatures:\n" << ansi::RESET;
            for (auto& ts : chip.temps) {
                std::string col = temp_color(ts.value_c, ts.crit_c);
                double limit    = ts.crit_c.value_or(ts.max_c.value_or(100.0));
                double fill     = ts.value_c / limit;

                out << "      " << fixed_w(ts.label, 20)
                    << col << std::fixed << std::setprecision(1)
                    << std::setw(6) << ts.value_c << " °C" << ansi::RESET
                    << "  " << bar(fill, 20, col);

                if (ts.max_c)
                    out << ansi::DIM << "  max "
                        << std::setprecision(0) << *ts.max_c << "°C";
                if (ts.crit_c)
                    out << "  crit " << *ts.crit_c << "°C";
                out << ansi::RESET << "\n";
            }
        }

        // ── Fans ──────────────────────────────────────────────────────────
        if (!chip.fans.empty()) {
            out << ansi::BOLD << "    Fans:\n" << ansi::RESET;
            for (auto& fs : chip.fans) {
                std::string col = fan_color(fs.rpm, fs.max_rpm);
                double fill = 0.0;
                if (fs.max_rpm && *fs.max_rpm > 0)
                    fill = static_cast<double>(fs.rpm) / *fs.max_rpm;
                else if (fs.rpm > 0)
                    fill = std::min(1.0, fs.rpm / 3000.0);

                out << "      " << fixed_w(fs.label, 20)
                    << col << std::setw(6) << fs.rpm << " RPM" << ansi::RESET
                    << "  " << bar(fill, 20, col);

                if (fs.pwm >= 0) {
                    int pct = static_cast<int>(fs.pwm * 100.0 / 255.0 + 0.5);
                    out << ansi::DIM << "  PWM " << std::setw(3) << pct << "%";
                }
                if (fs.min_rpm) out << ansi::DIM << "  min " << *fs.min_rpm;
                if (fs.max_rpm) out << "  max " << *fs.max_rpm;
                out << ansi::RESET << "\n";
            }
        }

        // ── Voltages ──────────────────────────────────────────────────────
        if (!chip.voltages.empty()) {
            out << ansi::BOLD << "    Voltages:\n" << ansi::RESET;
            for (auto& vs : chip.voltages) {
                std::string col = volt_color(vs.value_v, vs.min_v, vs.max_v);

                // Deviation bar (center = good, edge = bad)
                double fill = 0.5;
                if (vs.min_v && vs.max_v && *vs.max_v > *vs.min_v) {
                    fill = (vs.value_v - *vs.min_v) / (*vs.max_v - *vs.min_v);
                    fill = std::clamp(fill, 0.0, 1.0);
                }

                out << "      " << fixed_w(vs.label, 20)
                    << col << std::fixed << std::setprecision(3)
                    << std::setw(7) << vs.value_v << " V " << ansi::RESET
                    << "  " << bar(fill, 20, col);

                if (vs.min_v) out << ansi::DIM << "  min " << std::setprecision(3)
                                  << *vs.min_v << "V";
                if (vs.max_v) out << "  max " << *vs.max_v << "V";
                out << ansi::RESET << "\n";
            }
        }

        // ── Power ─────────────────────────────────────────────────────────
        if (!chip.powers.empty()) {
            out << ansi::BOLD << "    Power:\n" << ansi::RESET;
            for (auto& ps : chip.powers) {
                double fill = ps.max_w ? std::min(1.0, ps.value_w / *ps.max_w) : 0.5;
                std::string col = fill > 0.85 ? ansi::RED
                                : fill > 0.60 ? ansi::YELLOW
                                : ansi::MAGENTA;
                out << "      " << fixed_w(ps.label, 20)
                    << col << std::fixed << std::setprecision(2)
                    << std::setw(7) << ps.value_w << " W " << ansi::RESET
                    << "  " << bar(fill, 20, col);
                if (ps.max_w) out << ansi::DIM << "  max "
                                  << std::setprecision(1) << *ps.max_w << "W";
                out << ansi::RESET << "\n";
            }
        }

        // ── Currents ──────────────────────────────────────────────────────
        if (!chip.currents.empty()) {
            out << ansi::BOLD << "    Currents:\n" << ansi::RESET;
            for (auto& cs : chip.currents) {
                out << "      " << fixed_w(cs.label, 20)
                    << ansi::YELLOW << std::fixed << std::setprecision(3)
                    << std::setw(7) << cs.value_a << " A"
                    << ansi::RESET << "\n";
            }
        }

        out << "\n";
    }

    if (chips.empty()) {
        out << ansi::YELLOW
            << "  No hwmon chips found. Try running with sudo.\n"
            << "  Expected: /sys/class/hwmon/hwmon*\n"
            << ansi::RESET;
    }

    // Footer
    out << ansi::DIM
        << "  ──────────────────────────────────────────────────────────────\n"
        << "  Kernel sysfs interface: /sys/class/hwmon  │  "
        << "Sensors: lm-sensors compatible\n"
        << ansi::RESET;

    // Atomic write: clear screen, then dump buffer
    std::cout << ansi::CLEAR_SCREEN << out.str() << std::flush;
}

// ─── main ────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    int  refresh_s  = 1;
    bool once       = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--once" || arg == "-1") {
            once = true;
        } else if ((arg == "--interval" || arg == "-i") && i + 1 < argc) {
            try { refresh_s = std::stoi(argv[++i]); }
            catch (...) {}
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: hwmon [--once] [--interval N]\n"
                << "  --once       Print once and exit\n"
                << "  --interval N Refresh interval in seconds (default: 1)\n";
            return 0;
        }
    }

    if (!once) {
        std::cout << ansi::HIDE_CURSOR;
        // Disable canonical mode so we can read 'q' without Enter
        system("stty -icanon -echo min 1 time 0 2>/dev/null");
    }

    auto cleanup = [&]() {
        if (!once) {
            system("stty icanon echo 2>/dev/null");
            std::cout << ansi::SHOW_CURSOR << ansi::RESET << "\n";
        }
    };

    try {
        while (true) {
            auto chips = discover_chips();
            render(chips, refresh_s);

            if (once) break;

            // Wait for refresh_s seconds, checking for 'q' each 100ms
            for (int ms = 0; ms < refresh_s * 1000; ms += 100) {
                // Non-blocking stdin check via select
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(STDIN_FILENO, &fds);
                struct timeval tv = { 0, 100'000 };  // 100ms
                if (select(1, &fds, nullptr, nullptr, &tv) > 0) {
                    char c = 0;
                    if (read(STDIN_FILENO, &c, 1) == 1
                            && (c == 'q' || c == 'Q' || c == 3 /* Ctrl-C */)) {
                        cleanup();
                        return 0;
                    }
                }
            }
        }
    } catch (std::exception& e) {
        cleanup();
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    cleanup();
    return 0;
}
