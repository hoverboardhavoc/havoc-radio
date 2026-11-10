// Replays test/scenarios.txt through the firmware UI and compares every rendered frame with the golden
// rendered by the prototype (tools/gen_goldens.py).
#include <sys/stat.h>

#include <vector>

#include "sim.h"
#include "util.h"

namespace {

std::string readPbm(const std::string& path) {
    std::stringstream ss(readFile(path));
    std::string magic, tok, bits;
    int w = 0, h = 0;
    ss >> magic >> w >> h;
    if (magic != "P1" || w != gfx::W || h != gfx::H) return "";
    while (ss >> tok) bits += tok;
    return bits;
}

void writePbm(const std::string& path, const std::string& bits) {
    std::string out = "P1\n128 32\n";
    for (int y = 0; y < gfx::H; y++) out += bits.substr(y * gfx::W, gfx::W) + "\n";
    std::ofstream(path) << out;
}

}  // namespace

TEST(goldens) {
    std::stringstream file(readFile(t::root() + "/radio/test/scenarios.txt"));
    const std::string outDir = t::root() + "/radio/build/host/actual";
    mkdir(outDir.c_str(), 0755);
    std::string line, name;
    Sim* sim = nullptr;
    long tm = -1;
    int renders = 0;
    std::vector<Sim*> sims;
    while (std::getline(file, line)) {
        line = line.substr(0, line.find('#'));
        std::stringstream ls(line);
        std::vector<std::string> w;
        for (std::string x; ls >> x;) w.push_back(x);
        if (w.empty()) continue;
        if (w[0] == "scenario") {
            name = w[1];
            sim = new Sim();
            sims.push_back(sim);
            tm = -1;
            continue;
        }
        if (w[0][0] == '@') {
            tm = std::stol(w[0].substr(1));
            w.erase(w.begin());
        } else {
            tm = tm < 0 ? 3000 : tm + 100;
        }
        const uint32_t now = static_cast<uint32_t>(tm);
        const std::string& op = w[0];
        if (op == "turn") sim->ev(now, ui::EventType::Turn, std::stoi(w[1]));
        else if (op == "hold") sim->ev(now, ui::EventType::HoldTurn, std::stoi(w[1]));
        else if (op == "press") sim->ev(now, ui::EventType::Press);
        else if (op == "long") sim->ev(now, ui::EventType::LongPress);
        else if (op == "side") sim->ev(now, ui::EventType::Side);
        else if (op == "sidehold") sim->ev(now, ui::EventType::SideHold);
        else if (op == "ota") {
            sim->tick(now);
            sim->ui.confirmInstall(Sim::kBoot + now);
        } else if (op == "empty") {
            sim->tick(now);
            sim->ui.message(i18n::tr(sim->s.lang, i18n::Str::battery_empty));
        } else if (op == "set") {
            if (w[1] == "bars") sim->st.bars = static_cast<uint8_t>(std::stoi(w[2]));
            else if (w[1] == "ssb") sim->st.ssb = w[2] == "1";
            else if (w[1] == "wifi") sim->wifiOff();
            else if (w[1] == "lang") sim->setLang(w[2].c_str());
            else if (w[1] == "server") std::snprintf(sim->s.ntpServer, sizeof sim->s.ntpServer, "%s", w[2].c_str());
            else t::fail(__FILE__, __LINE__, "unknown set " + w[1]);
        } else if (op == "ntp") {
            // An NTP sync at HH:MM UTC on 2026-11-10, as tools/gen_goldens.py.
            const int64_t day = 1794268800000LL;
            const int h = std::stoi(w[1].substr(0, 2)), m = std::stoi(w[1].substr(3, 2));
            sim->tick(now);
            sim->ntp(now, day + (h * 60 + m) * 60000LL);
        } else if (op == "rds") {
            std::string text;
            for (size_t i = 2; i < w.size(); i++) text += (i > 2 ? " " : "") + w[i];
            if (w[1] == "sync") sim->st.rdsSync = text == "1";
            else if (w[1] == "pty") sim->st.pty = static_cast<uint8_t>(std::stoi(text));
            else if (w[1] == "ps") sim->setPs(text);
            else if (w[1] == "rt") sim->setRt(text);
            else if (w[1] == "ab") {
                sim->st.rtSeq++;
                sim->setRt("");
            } else t::fail(__FILE__, __LINE__, "unknown rds " + w[1]);
        } else if (op == "render") {
            const std::string label = name + (w.size() > 1 ? "_" + w[1] : "");
            const std::string actual = frameBits(sim->render(now));
            const std::string golden = readPbm(t::root() + "/radio/test/golden/" + label + ".pbm");
            renders++;
            if (golden.empty()) {
                t::fail(__FILE__, __LINE__, "no golden " + label);
            } else if (actual != golden) {
                int diff = 0;
                for (size_t i = 0; i < actual.size(); i++) diff += actual[i] != golden[i];
                writePbm(outDir + "/" + label + ".pbm", actual);
                t::fail(__FILE__, __LINE__, label + ": " + std::to_string(diff) + " pixels differ (radio/build/host/actual)");
            }
        } else {
            t::fail(__FILE__, __LINE__, "unknown op " + op);
        }
    }
    for (Sim* s : sims) delete s;
    CHECK(renders > 100);
}
