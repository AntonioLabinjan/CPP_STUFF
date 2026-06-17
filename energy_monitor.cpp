/**
 * Smart Home Energy Monitor - IoT Simulator
 * ==========================================
 * Simulates a network of IoT smart plugs and sensors throughout a home.
 * Each device reports power draw every second. The monitor detects anomalies,
 * calculates running costs, estimates CO2 emissions, and provides alerts.
 *
 * Compile: g++ -std=c++17 -O2 -o energy_monitor energy_monitor.cpp
 * Run:     ./energy_monitor
 */

#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <map>
#include <random>
#include <chrono>
#include <thread>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <numeric>

// ─── ANSI color helpers ────────────────────────────────────────────────────
namespace Color {
    const std::string RESET   = "\033[0m";
    const std::string BOLD    = "\033[1m";
    const std::string RED     = "\033[31m";
    const std::string GREEN   = "\033[32m";
    const std::string YELLOW  = "\033[33m";
    const std::string BLUE    = "\033[34m";
    const std::string MAGENTA = "\033[35m";
    const std::string CYAN    = "\033[36m";
    const std::string WHITE   = "\033[37m";
    const std::string DIM     = "\033[2m";
    const std::string BG_DARK = "\033[48;5;234m";
}

// ─── Device types & rooms ─────────────────────────────────────────────────
enum class DeviceType {
    HVAC, WASHER, DRYER, DISHWASHER, REFRIGERATOR,
    OVEN, MICROWAVE, TV, COMPUTER, LIGHTING,
    EV_CHARGER, WATER_HEATER, IDLE
};

std::string deviceTypeName(DeviceType t) {
    switch(t) {
        case DeviceType::HVAC:         return "HVAC";
        case DeviceType::WASHER:       return "Washer";
        case DeviceType::DRYER:        return "Dryer";
        case DeviceType::DISHWASHER:   return "Dishwasher";
        case DeviceType::REFRIGERATOR: return "Refrigerator";
        case DeviceType::OVEN:         return "Oven";
        case DeviceType::MICROWAVE:    return "Microwave";
        case DeviceType::TV:           return "TV";
        case DeviceType::COMPUTER:     return "Computer";
        case DeviceType::LIGHTING:     return "Lighting";
        case DeviceType::EV_CHARGER:   return "EV Charger";
        case DeviceType::WATER_HEATER: return "Water Heater";
        default:                       return "Idle";
    }
}

// ─── IoT Smart Plug / Sensor ──────────────────────────────────────────────
struct Device {
    std::string   id;
    std::string   name;
    std::string   room;
    DeviceType    type;
    bool          isOn;

    double nominalWatts;       // rated power draw
    double currentWatts;       // real-time reading
    double energyWh;           // cumulative Wh consumed this session
    double peakWatts;

    std::vector<double> history; // last N readings for trend analysis
    int    anomalyCount;
    bool   anomalyFlag;
    std::string status;          // human-readable state

    // Sensor metadata
    double voltage;              // measured voltage (V)
    double powerFactor;          // PF (0–1)
    double temperature;          // device surface temp °C

    std::mt19937 rng;

    Device(const std::string& id_, const std::string& name_,
           const std::string& room_, DeviceType type_, double nominalW, bool on = true)
        : id(id_), name(name_), room(room_), type(type_),
          isOn(on), nominalWatts(nominalW), currentWatts(0),
          energyWh(0), peakWatts(0), anomalyCount(0), anomalyFlag(false),
          voltage(120.0), powerFactor(0.95), temperature(25.0)
    {
        rng.seed(std::hash<std::string>{}(id_) ^ std::random_device{}());
        status = isOn ? "Running" : "Standby";
    }

    // Simulate a realistic power reading based on device type
    void tick(double elapsedSeconds) {
        if (!isOn) {
            currentWatts = 0.5 + noise(0.3); // standby draw
            status = "Standby";
            logReading();
            return;
        }

        double base = nominalWatts;
        double w = 0;

        switch (type) {
            case DeviceType::HVAC: {
                // Cycles on/off, ramps up
                double cycle = std::sin(elapsedSeconds / 120.0) * 0.3 + 0.7;
                w = base * cycle + noise(base * 0.05);
                status = cycle > 0.5 ? "Cooling" : "Idle";
                break;
            }
            case DeviceType::WASHER: {
                // Wash phases: fill, agitate, spin
                int phase = (int)(elapsedSeconds / 40) % 3;
                if      (phase == 0) { w = base * 0.3 + noise(20); status = "Filling"; }
                else if (phase == 1) { w = base * 0.8 + noise(50); status = "Agitating"; }
                else                 { w = base * 1.1 + noise(80); status = "Spinning"; }
                break;
            }
            case DeviceType::DRYER: {
                // Heating element cycles
                double heatCycle = (std::fmod(elapsedSeconds, 30) < 22) ? 1.0 : 0.1;
                w = base * heatCycle + noise(base * 0.03);
                status = heatCycle > 0.5 ? "Heating" : "Tumbling";
                break;
            }
            case DeviceType::REFRIGERATOR: {
                // Compressor cycles every ~15 min
                double comp = (std::fmod(elapsedSeconds, 900) < 300) ? 1.0 : 0.08;
                w = base * comp + noise(15);
                status = comp > 0.5 ? "Compressor On" : "Idle";
                break;
            }
            case DeviceType::OVEN: {
                // Preheat then maintain
                double preheat = std::min(1.0, elapsedSeconds / 60.0);
                double maint = (std::fmod(elapsedSeconds, 20) < 6) ? 0.4 : 0.0;
                w = base * (preheat < 1.0 ? preheat : maint) + noise(30);
                status = preheat < 1.0 ? "Preheating" : "Maintaining Temp";
                break;
            }
            case DeviceType::TV:
            case DeviceType::COMPUTER: {
                w = base + noise(base * 0.15);
                status = "Active";
                break;
            }
            case DeviceType::EV_CHARGER: {
                // Level 2 charger, constant draw with small fluctuation
                w = base + noise(base * 0.02);
                status = "Charging";
                break;
            }
            case DeviceType::WATER_HEATER: {
                double element = (std::fmod(elapsedSeconds, 600) < 180) ? 1.0 : 0.0;
                w = base * element + noise(20);
                status = element > 0 ? "Heating" : "Standby";
                break;
            }
            default:
                w = base + noise(base * 0.1);
                status = "Running";
        }

        // Clamp to realistic bounds
        w = std::max(0.0, w);
        currentWatts = w;
        peakWatts = std::max(peakWatts, w);

        // Accumulate energy (Wh = W * h)
        energyWh += w * (1.0 / 3600.0);

        // Voltage sag under heavy load
        voltage = 120.0 - (w / nominalWatts) * 2.5 + noise(0.5);
        powerFactor = 0.92 + noise(0.04);
        temperature = 25.0 + (w / nominalWatts) * 20.0 + noise(2.0);

        logReading();
        checkAnomaly();
    }

    void logReading() {
        history.push_back(currentWatts);
        if (history.size() > 60) history.erase(history.begin());
    }

    void checkAnomaly() {
        if (history.size() < 10) return;
        double mean = std::accumulate(history.begin(), history.end(), 0.0) / history.size();
        double sq_sum = std::inner_product(history.begin(), history.end(), history.begin(), 0.0);
        double stdev = std::sqrt(sq_sum / history.size() - mean * mean);
        // Anomaly: current draw is > 2.5 sigma above rolling mean
        if (stdev > 0 && std::abs(currentWatts - mean) > 2.5 * stdev && currentWatts > mean * 1.3) {
            anomalyCount++;
            anomalyFlag = true;
        } else {
            anomalyFlag = false;
        }
    }

    double avgWatts() const {
        if (history.empty()) return 0;
        return std::accumulate(history.begin(), history.end(), 0.0) / history.size();
    }

    double trend() const {
        if (history.size() < 6) return 0;
        double first = std::accumulate(history.end()-6, history.end()-3, 0.0) / 3.0;
        double last  = std::accumulate(history.end()-3, history.end(),   0.0) / 3.0;
        return last - first;
    }

    double noise(double scale) {
        std::normal_distribution<double> dist(0, scale);
        return dist(rng);
    }
};

// ─── Energy Monitor Hub ───────────────────────────────────────────────────
class EnergyMonitor {
public:
    std::vector<Device> devices;
    double electricityRate;   // $ per kWh
    double co2PerKwh;         // kg CO2 per kWh (grid average)
    double sessionStart;
    int    tickCount;

    EnergyMonitor(double rate = 0.14, double co2 = 0.386)
        : electricityRate(rate), co2PerKwh(co2), tickCount(0)
    {
        sessionStart = 0;
        setupHome();
    }

    void setupHome() {
        // Living Room
        devices.emplace_back("LR-TV",   "Smart TV 65\"",    "Living Room", DeviceType::TV,           120.0);
        devices.emplace_back("LR-LAMP", "Floor Lamp",        "Living Room", DeviceType::LIGHTING,      15.0);
        // Kitchen
        devices.emplace_back("KT-REF",  "Refrigerator",      "Kitchen",     DeviceType::REFRIGERATOR, 150.0);
        devices.emplace_back("KT-OVN",  "Electric Oven",     "Kitchen",     DeviceType::OVEN,        2400.0, false);
        devices.emplace_back("KT-MWV",  "Microwave",         "Kitchen",     DeviceType::MICROWAVE,   1100.0, false);
        // Laundry
        devices.emplace_back("LN-WSH",  "Washing Machine",   "Laundry",     DeviceType::WASHER,       500.0);
        devices.emplace_back("LN-DRY",  "Dryer",             "Laundry",     DeviceType::DRYER,       5000.0);
        // Garage
        devices.emplace_back("GR-EV",   "EV Charger (L2)",   "Garage",      DeviceType::EV_CHARGER,  7200.0);
        // Bedroom
        devices.emplace_back("BR-PC",   "Desktop Computer",  "Bedroom",     DeviceType::COMPUTER,     200.0);
        devices.emplace_back("BR-LAMP", "Bedside Lamp",      "Bedroom",     DeviceType::LIGHTING,       8.0, false);
        // Utility
        devices.emplace_back("UT-HVAC", "Central HVAC",      "Utility",     DeviceType::HVAC,        3500.0);
        devices.emplace_back("UT-WHT",  "Water Heater",      "Utility",     DeviceType::WATER_HEATER,4500.0);
    }

    void tick(double elapsed) {
        for (auto& d : devices) d.tick(elapsed);
        tickCount++;
    }

    double totalWatts() const {
        double sum = 0;
        for (auto& d : devices) sum += d.currentWatts;
        return sum;
    }

    double totalEnergyWh() const {
        double sum = 0;
        for (auto& d : devices) sum += d.energyWh;
        return sum;
    }

    double sessionCost() const {
        return (totalEnergyWh() / 1000.0) * electricityRate;
    }

    double sessionCO2() const {
        return (totalEnergyWh() / 1000.0) * co2PerKwh;
    }

    // Group watts by room
    std::map<std::string, double> byRoom() const {
        std::map<std::string, double> m;
        for (auto& d : devices) m[d.room] += d.currentWatts;
        return m;
    }

    int anomalyCount() const {
        int n = 0;
        for (auto& d : devices) if (d.anomalyFlag) n++;
        return n;
    }
};

// ─── Terminal UI ──────────────────────────────────────────────────────────
class UI {
public:
    static void clearScreen() { std::cout << "\033[2J\033[H"; }

    static std::string bar(double val, double max, int width, const std::string& col) {
        int filled = (int)std::round((val / max) * width);
        filled = std::clamp(filled, 0, width);
        std::string b = col + std::string(filled, '#') + Color::DIM
                      + std::string(width - filled, '.') + Color::RESET;
        return b;
    }

    static std::string trendArrow(double t) {
        if      (t >  20) return Color::RED    + "↑↑" + Color::RESET;
        else if (t >   5) return Color::YELLOW + "↑"  + Color::RESET;
        else if (t <  -5) return Color::GREEN  + "↓"  + Color::RESET;
        else              return Color::DIM    + "→"  + Color::RESET;
    }

    static std::string wattsColor(double w, double nominal) {
        double ratio = w / nominal;
        if      (ratio > 1.2) return Color::RED;
        else if (ratio > 0.8) return Color::WHITE;
        else if (ratio > 0.3) return Color::CYAN;
        else                  return Color::DIM;
    }

    static void render(EnergyMonitor& mon, double elapsed) {
        clearScreen();
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);

        double totalW   = mon.totalWatts();
        double totalWh  = mon.totalEnergyWh();
        double cost     = mon.sessionCost();
        double co2      = mon.sessionCO2();
        auto   rooms    = mon.byRoom();
        int    alerts   = mon.anomalyCount();

        // ── Header ──────────────────────────────────────────────────
        std::cout << Color::BOLD << Color::CYAN;
        std::cout << "╔══════════════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║          🏠  SMART HOME ENERGY MONITOR  ·  IoT Sensor Dashboard         ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════════════════╝\n";
        std::cout << Color::RESET;

        // ── Summary strip ───────────────────────────────────────────
        char tbuf[32];
        std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", std::localtime(&t));
        std::cout << Color::DIM << "  Sim time: " << Color::RESET
                  << Color::YELLOW << std::setw(6) << (int)elapsed << "s" << Color::RESET
                  << Color::DIM << "  |  Wall clock: " << Color::RESET
                  << Color::WHITE << tbuf << Color::RESET
                  << Color::DIM << "  |  Tick #" << Color::RESET
                  << mon.tickCount << "\n\n";

        // ── Power overview ───────────────────────────────────────────
        std::string powerCol = totalW > 8000 ? Color::RED :
                               totalW > 5000 ? Color::YELLOW : Color::GREEN;

        std::cout << Color::BOLD << "  ⚡ TOTAL LOAD    " << Color::RESET
                  << powerCol << Color::BOLD
                  << std::setw(7) << std::fixed << std::setprecision(0) << totalW << " W"
                  << Color::RESET << "   "
                  << bar(totalW, 12000, 30, powerCol) << "\n";

        std::cout << "  💰 SESSION COST  " << Color::GREEN << Color::BOLD
                  << "$" << std::fixed << std::setprecision(4) << cost << Color::RESET
                  << "   (" << std::setprecision(3) << totalWh/1000.0 << " kWh)\n";

        std::cout << "  🌿 CO₂ EMITTED   " << Color::CYAN
                  << std::setprecision(3) << co2 << " kg" << Color::RESET
                  << Color::DIM << "  (@ $" << mon.electricityRate << "/kWh, "
                  << mon.co2PerKwh << " kg/kWh grid)" << Color::RESET << "\n";

        if (alerts > 0)
            std::cout << "  " << Color::RED << Color::BOLD << "⚠  " << alerts
                      << " ANOMALY ALERT(S) — see device list below" << Color::RESET << "\n";

        std::cout << "\n";

        // ── Room breakdown ───────────────────────────────────────────
        std::cout << Color::BOLD << "  BY ROOM\n" << Color::RESET;
        std::cout << Color::DIM << "  " << std::string(72, '-') << Color::RESET << "\n";
        for (auto& [room, w] : rooms) {
            std::string rc = w > 3000 ? Color::RED : w > 1000 ? Color::YELLOW : Color::CYAN;
            std::cout << "  " << Color::BOLD << std::left << std::setw(14) << room
                      << Color::RESET << " " << rc << std::right << std::setw(6)
                      << std::setprecision(0) << w << " W  " << Color::RESET
                      << bar(w, 8000, 20, rc) << "\n";
        }
        std::cout << "\n";

        // ── Device table ─────────────────────────────────────────────
        std::cout << Color::BOLD
                  << "  ID        DEVICE              ROOM           STATUS          W(now)   W(avg) PK ALERT\n"
                  << Color::RESET
                  << Color::DIM << "  " << std::string(90, '-') << Color::RESET << "\n";

        // Sort: anomalies first, then by power desc
        std::vector<int> idx(mon.devices.size());
        std::iota(idx.begin(), idx.end(), 0);
        std::sort(idx.begin(), idx.end(), [&](int a, int b){
            if (mon.devices[a].anomalyFlag != mon.devices[b].anomalyFlag)
                return mon.devices[a].anomalyFlag > mon.devices[b].anomalyFlag;
            return mon.devices[a].currentWatts > mon.devices[b].currentWatts;
        });

        for (int i : idx) {
            auto& d = mon.devices[i];
            std::string wc = wattsColor(d.currentWatts, d.nominalWatts);
            std::string alert = d.anomalyFlag
                ? Color::RED + Color::BOLD + " ⚠ SPIKE" + Color::RESET
                : (d.anomalyCount > 0 ? Color::YELLOW + " " + std::to_string(d.anomalyCount) + " past" + Color::RESET
                                      : Color::DIM + "  —" + Color::RESET);
            std::string onOff = d.isOn
                ? Color::GREEN + "●" + Color::RESET
                : Color::DIM   + "○" + Color::RESET;

            std::cout << "  " << onOff << " "
                      << Color::DIM << std::left << std::setw(8)  << d.id    << Color::RESET << "  "
                      << Color::BOLD << std::left << std::setw(18) << d.name  << Color::RESET
                      << Color::DIM  << std::left << std::setw(13) << d.room  << Color::RESET
                      << "  " << std::left << std::setw(16) << d.status
                      << "  " << wc << std::right << std::setw(7) << std::setprecision(0)
                                                    << d.currentWatts << Color::RESET
                      << "  " << Color::DIM << std::setw(6) << (int)d.avgWatts() << Color::RESET
                      << " " << trendArrow(d.trend())
                      << alert << "\n";
        }

        // ── Sensor detail for top consumer ──────────────────────────
        auto topIt = std::max_element(mon.devices.begin(), mon.devices.end(),
            [](const Device& a, const Device& b){ return a.currentWatts < b.currentWatts; });
        if (topIt != mon.devices.end() && topIt->isOn) {
            std::cout << "\n";
            std::cout << Color::DIM << "  " << std::string(72, '-') << Color::RESET << "\n";
            std::cout << Color::BOLD << "  📡 SENSOR DETAIL — " << topIt->name << Color::RESET
                      << Color::DIM << " (highest load)\n" << Color::RESET;
            std::cout << std::fixed << std::setprecision(1);
            std::cout << "     Voltage:      " << Color::CYAN << topIt->voltage << " V\n" << Color::RESET;
            std::cout << "     Power Factor: " << Color::CYAN << topIt->powerFactor << "\n" << Color::RESET;
            std::cout << "     Surface Temp: ";
            std::string tc = topIt->temperature > 60 ? Color::RED :
                             topIt->temperature > 40 ? Color::YELLOW : Color::CYAN;
            std::cout << tc << topIt->temperature << " °C\n" << Color::RESET;
            std::cout << "     Energy used:  " << Color::GREEN
                      << std::setprecision(2) << topIt->energyWh << " Wh ("
                      << std::setprecision(4) << topIt->energyWh/1000.0 << " kWh)\n" << Color::RESET;
        }

        // ── Projections ──────────────────────────────────────────────
        double hourlyRate    = totalW / 1000.0 * mon.electricityRate;
        double dailyEst      = hourlyRate * 24;
        double monthlyEst    = dailyEst * 30;

        std::cout << "\n";
        std::cout << Color::DIM << "  " << std::string(72, '-') << Color::RESET << "\n";
        std::cout << Color::BOLD << "  📊 PROJECTIONS" << Color::RESET
                  << Color::DIM << " (based on current load)\n" << Color::RESET;
        std::cout << "     Hourly cost:   " << Color::GREEN << "$" << std::setprecision(3)
                  << hourlyRate  << "/hr\n" << Color::RESET;
        std::cout << "     Daily est.:    " << Color::YELLOW << "$" << std::setprecision(2)
                  << dailyEst    << "/day\n" << Color::RESET;
        std::cout << "     Monthly est.:  " << Color::RED << Color::BOLD << "$"
                  << std::setprecision(0) << monthlyEst << "/month\n" << Color::RESET;

        std::cout << "\n" << Color::DIM << "  Press Ctrl+C to exit\n" << Color::RESET;
        std::cout.flush();
    }
};

// ─── Main ─────────────────────────────────────────────────────────────────
int main() {
    // Hide cursor
    std::cout << "\033[?25l";

    EnergyMonitor mon(0.14, 0.386);
    UI ui;

    auto startTime = std::chrono::steady_clock::now();

    std::cout << Color::CYAN << Color::BOLD
              << "\n  Initializing IoT sensor network...\n" << Color::RESET;
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    while (true) {
        auto now     = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - startTime).count();

        // Occasionally toggle devices on/off to make it interesting
        static std::mt19937 rng(42);
        if (mon.tickCount % 30 == 0) {
            // Oven comes on after 60s, goes off after 180s
            if (elapsed > 60  && elapsed < 180) mon.devices[3].isOn = true;
            if (elapsed > 180)                  mon.devices[3].isOn = false;
            // Microwave on briefly at 120s
            if (elapsed > 120 && elapsed < 150) mon.devices[4].isOn = true;
            if (elapsed > 150)                  mon.devices[4].isOn = false;
            // Bedroom lamp on after 90s
            if (elapsed > 90)                   mon.devices[9].isOn = true;
        }

        mon.tick(elapsed);
        UI::render(mon, elapsed);

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Restore cursor (unreachable in normal operation, but good practice)
    std::cout << "\033[?25h";
    return 0;
}
