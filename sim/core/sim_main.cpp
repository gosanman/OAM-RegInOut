// ============================================================================
// Simulator: Hauptprogramm des Kerns (iosim.exe).
//
// Liest Befehle zeilenweise von stdin, schreibt Ereignisse als JSON-Zeilen nach
// stdout. Jeder Befehl endet mit {"ev":"ok"}. Damit laesst sich der Kern von
// der Python-Oberflaeche steuern - oder mit einer Szenario-Datei direkt:
//
//     iosim.exe < szenario.txt
//
// Befehle:
//   load <hex>          Parameterspeicher setzen, als waere das Geraet schon
//                       programmiert (ohne Download, ohne Neustart)
//   pimg <hex>          Parameterspeicher fuer den naechsten ETS-Download
//   ets [ms]            ETS-Download: knx.configured() faellt fuer [ms] (Vorgabe
//                       3000) auf false, dann processBeforeRestart, Flash
//                       sichern, neue Parameter, Neustart
//   unload              Applikation entladen (Geraet wie neu), Neustart
//   restart             Neustart (processBeforeRestart, Flash, setup)
//   bus 0|1             KNX-Bus aus/ein. Aus: SAVE (savePower, Flash), Prozessor
//                       aus. Ein: Neustart.
//   busdip <ms>         kurzer Einbruch: SAVE, <ms> warten, 1 s nach SAVE
//                       restorePower() ohne Neustart (wie OpenKNX Common)
//   supply 0|1          Versorgung der Relaisspulen aus/ein
//   bi <n> 0|1          Kontakt am Binaereingang n (1..) offen/geschlossen
//   ko <nr> <dpt> <wert> Telegramm vom Bus an Objekt <nr>, z. B. ko 110 1.1 1
//   btn short|long|double  Funktionstaste
//   fp <obj> <prop> <hex>  Function Property (die Module haben keine)
//   run <ms>            Zeit fortschreiben, loop() in Schritten von stepms
//   stepms <ms>         Schrittweite (Vorgabe 10 ms)
//   cmd <text>          Konsolenbefehl, z. B. cmd sa switch a 1
//   diag <text>         Befehl ueber das Diagnose-KO (Antwort in 14 Zeichen)
//   state               Zustand als JSON
// ============================================================================

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <new>
#include <sstream>
#include <string>

#include "sim.h"

#include "GpioBinaryInputModule.h"
#include "SwitchActuatorModule.h"

namespace
{
    std::vector<uint8_t> pending;
    uint32_t stepMs = 10;
    bool busOn = false;

    // Reihenfolge und Nummern wie src/main.cpp: Logik (1) und FileTransfer (4)
    // baut der Simulator nicht.
    OpenKNX::Module *modules[] = {&openknxGpioBinaryInputModule, &openknxSwitchActuatorModule};
    constexpr int kModules = 2;

    void event(const std::string &name, const std::string &extra = "")
    {
        sim::emit("{\"ev\":\"" + name + "\",\"t\":" + std::to_string(sim::now) +
                  (extra.empty() ? "" : "," + extra) + "}");
    }

    bool parseHex(const std::string &hex, std::vector<uint8_t> &out)
    {
        if (hex.size() % 2 != 0)
            return false;
        out.assign(hex.size() / 2, 0);
        for (size_t i = 0; i < out.size(); i++)
            out[i] = static_cast<uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
        return true;
    }

    // ------------------------------------------------------------ Flash

    void flashSave()
    {
        sim::flashBlocks.assign(kModules, {});
        for (int i = 0; i < kModules; i++)
        {
            openknx.flash.writeTarget = &sim::flashBlocks[i];
            modules[i]->writeFlash();
            sim::flashBlocks[i].resize(modules[i]->flashSize(), 0);
        }
        openknx.flash.writeTarget = nullptr;
        sim::flashValid = true;
        event("flash", "\"action\":\"save\"");
    }

    void flashLoad()
    {
        for (int i = 0; i < kModules; i++)
        {
            const uint16_t size = modules[i]->flashSize();
            if (sim::flashValid && i < static_cast<int>(sim::flashBlocks.size()) && size > 0 &&
                sim::flashBlocks[i].size() == size)
            {
                openknx.flash.readPtr = sim::flashBlocks[i].data();
                openknx.flash.readEnd = sim::flashBlocks[i].data() + size;
                modules[i]->readFlash(sim::flashBlocks[i].data(), size);
            }
            else
            {
                openknx.flash.readPtr = nullptr;
                modules[i]->readFlash(nullptr, 0);
            }
        }
        openknx.flash.readPtr = nullptr;
    }

    // ------------------------------------------------------------ Prozessor

    /// Kaltstart des Prozessors: RAM und GPIOs weg, Flash und Parameter bleiben.
    void boot()
    {
        bool wasOn[sim::kRelays];
        for (int ch = 0; ch < sim::kRelays; ch++)
            wasOn[ch] = sim::relayOn(ch);

        // Waehrend des Neustarts sind alle Pins hochohmig: die Relais fallen ab.
        sim::pinsReset();
        sim::emitRelayChanges();

        // RAM zuruecksetzen: die Modulobjekte neu aufbauen. Die Kanaele (new
        // ...Channel) bleiben als Leck stehen - im Simulator egal.
        openknxGpioBinaryInputModule.~GpioBinaryInputModule();
        new (&openknxGpioBinaryInputModule) GpioBinaryInputModule();
        openknxSwitchActuatorModule.~SwitchActuatorModule();
        new (&openknxSwitchActuatorModule) SwitchActuatorModule();

        for (size_t i = 0; i < sim::kos.size(); i++)
            sim::kos[i].simInit(static_cast<uint16_t>(i));
        openknx.ledFunctions.simReset();
        openknx.func1Button = OpenKNX::SimButton{};

        sim::bootAt = sim::now;
        sim::mcuRunning = true;
        sim::downloading = false;
        sim::resetInvariants();

        const bool cfg = sim::configured();
        event("boot", std::string("\"configured\":") + (cfg ? "true" : "false"));

        // Reihenfolge wie OpenKNX Common::setup(): init() und setup() aller
        // Module, danach den Flash lesen.
        for (auto *m : modules)
            m->init();
        for (auto *m : modules)
            m->setup(cfg);
        if (cfg)
            flashLoad();
        sim::emitRelayChanges();

        for (int ch = 0; ch < sim::kRelays; ch++)
            if (wasOn[ch])
                event("info", "\"text\":" + sim::esc("Neustart: K" + std::to_string(ch + 1) +
                                                     " fiel ab (Pins hochohmig), danach " +
                                                     (sim::relayOn(ch) ? "aus dem Flash wieder angezogen"
                                                                       : "aus geblieben")));
    }

    void loopOnce()
    {
        const bool cfg = sim::configured();
        for (auto *m : modules)
            m->loop(cfg);
    }

    void run(uint64_t ms)
    {
        const uint64_t end = sim::now + ms;
        while (sim::now < end)
        {
            const uint64_t step = std::min<uint64_t>(stepMs, end - sim::now);
            sim::now += step;
            if (sim::mcuRunning)
                loopOnce();
            sim::checkInvariants();
            sim::emitRelayChanges();
        }
    }

    void beforeRestart()
    {
        if (!sim::mcuRunning)
            return;
        for (auto *m : modules)
            m->processBeforeRestart();
        flashSave();
    }

    /// SAVE-Interrupt wie Common::processSavePin: nur bei parametriertem Geraet.
    bool savePowerAll()
    {
        if (!sim::mcuRunning || !sim::configured())
            return false;
        for (auto *m : modules)
            m->savePower();
        flashSave();
        return true;
    }

    void setBus(bool on)
    {
        if (on == busOn)
            return;
        busOn = on;
        if (!on)
        {
            savePowerAll();
            sim::mcuRunning = false;
            sim::pinsReset();
            sim::emitRelayChanges();
            event("mcu", "\"running\":false");
        }
        else
        {
            boot();
        }
    }

    void busDip(uint64_t ms)
    {
        if (!sim::mcuRunning)
        {
            event("error", "\"text\":\"Busdip ohne laufenden Prozessor nicht moeglich\"");
            return;
        }
        event("busdip", "\"phase\":\"start\",\"ms\":" + std::to_string(ms));
        const bool saved = savePowerAll();
        run(ms);
        // Common::processRestoreSavePin: 1 s nach dem SAVE, unabhaengig davon,
        // wann die Spannung zurueckkam.
        if (ms < 1000)
            run(1000 - ms);
        bool reboot = false;
        if (saved)
            for (auto *m : modules)
                if (!m->restorePower())
                {
                    reboot = true;
                    break;
                }
        event("busdip", std::string("\"phase\":\"end\",\"reboot\":") + (reboot ? "true" : "false"));
        if (reboot)
            boot();
        sim::emitRelayChanges();
    }

    // ------------------------------------------------------------ Befehle

    std::string rest(std::istringstream &in)
    {
        std::string r;
        std::getline(in, r);
        const size_t p = r.find_first_not_of(' ');
        return p == std::string::npos ? "" : r.substr(p);
    }

    void receiveKo(int nr, const std::string &dptText, const std::string &valueText)
    {
        if (nr < 0 || nr >= static_cast<int>(sim::kos.size()))
        {
            event("error", "\"text\":" + sim::esc("unbekanntes Objekt " + std::to_string(nr)));
            return;
        }
        const Dpt dpt = sim::parseDpt(dptText);
        GroupObject &ko = sim::kos[nr];
        bool ok;
        if (dpt.mainGroup == 1)
        {
            const bool v = valueText == "1" || valueText == "true" || valueText == "ein";
            ok = ko.simReceive(KNXValue(v), dpt);
        }
        else if (dpt.mainGroup == 16)
            ok = ko.simReceive(KNXValue(valueText.c_str()), dpt);
        else if (dpt.mainGroup == 18)
        {
            // Szenensteuerung: Wert = Szenennummer 0..63, "L" davor = lernen
            const bool learn = !valueText.empty() && (valueText[0] == 'L' || valueText[0] == 'l');
            const int scene = std::stoi(learn ? valueText.substr(1) : valueText);
            ok = ko.simReceive(KNXValue(static_cast<uint8_t>(scene & 0x3F)), Dpt(18, 1, 1));
            if (ok)
                ok = KNX_Encode_Value(KNXValue(learn), const_cast<uint8_t *>(ko.simData()), 1, Dpt(18, 1, 0));
        }
        else
            ok = ko.simReceive(KNXValue(std::stod(valueText)), dpt);
        if (!ok)
        {
            event("error", "\"text\":" + sim::esc("Wert " + valueText + " passt nicht zu DPT " +
                                                   dptText + " - Objekt " + std::to_string(nr)));
            return;
        }

        // Ein unkonfiguriertes Geraet hat keine Zuordnungstabelle - es kommt
        // nichts an. Ohne Bus laeuft der Prozessor nicht.
        const bool delivered = sim::mcuRunning && sim::configured() && busOn;
        event("rx", "\"nr\":" + std::to_string(nr) + ",\"dpt\":\"" + dptText +
                        "\",\"val\":" + sim::koValueJson(ko, dpt) +
                        ",\"raw\":\"" + sim::hexOf(ko.simData(), sim::lengthFor(dpt)) +
                        "\",\"delivered\":" + (delivered ? "true" : "false"));
        if (delivered)
            for (auto *m : modules)
                m->processInputKo(ko);
        sim::emitRelayChanges();
    }

    void console(const std::string &text, bool debugKo)
    {
        if (!sim::mcuRunning)
        {
            event("error", "\"text\":\"Prozessor laeuft nicht (kein KNX-Bus)\"");
            return;
        }
        if (text == "help" || text == "?")
        {
            for (auto *m : modules)
                m->showHelp();
            return;
        }
        for (auto *m : modules)
            if (m->processCommand(text, debugKo))
            {
                sim::emitRelayChanges();
                return;
            }
        event("error", "\"text\":" + sim::esc("unbekannter Befehl: " + text));
    }

    void state()
    {
        std::string flags = std::string("\"uptime\":") + std::to_string(millis()) +
                            ",\"mcu\":" + (sim::mcuRunning ? "true" : "false") +
                            ",\"bus\":" + (busOn ? "true" : "false") +
                            ",\"supply\":" + (sim::relaySupply ? "true" : "false") +
                            ",\"paramsValid\":" + (sim::paramsValid ? "true" : "false") +
                            ",\"downloading\":" + (sim::downloading ? "true" : "false") +
                            ",\"configured\":" + (sim::configured() ? "true" : "false");
        sim::emit("{\"ev\":\"state\",\"t\":" + std::to_string(sim::now) + "," + flags +
                  ",\"sim\":" + sim::snapshotJson() + "}");
    }

    void handle(const std::string &line)
    {
        std::istringstream in(line);
        std::string op;
        in >> op;

        if (op.empty() || op[0] == '#')
            return;
        if (op == "load" || op == "pimg")
        {
            std::string hex;
            in >> hex;
            std::vector<uint8_t> img;
            if (!parseHex(hex, img))
            {
                event("error", "\"text\":\"Parameterabbild ist kein Hex\"");
                return;
            }
            img.resize(MAIN_ParameterSize, 0);
            if (op == "load")
            {
                sim::params = img;
                sim::paramsValid = true;
            }
            pending = img;
        }
        else if (op == "ets")
        {
            uint64_t ms = 3000;
            in >> ms;
            if (!sim::mcuRunning)
            {
                event("error", "\"text\":\"ETS-Download ohne KNX-Bus nicht moeglich\"");
                return;
            }
            if (pending.empty())
                pending = sim::params;
            sim::downloading = true;
            event("download", "\"phase\":\"start\"");
            run(ms);
            beforeRestart();
            sim::params = pending;
            sim::paramsValid = true;
            event("download", "\"phase\":\"done\"");
            boot();
        }
        else if (op == "unload")
        {
            beforeRestart();
            sim::paramsValid = false;
            if (sim::mcuRunning)
                boot();
        }
        else if (op == "restart")
        {
            if (sim::mcuRunning)
            {
                beforeRestart();
                boot();
            }
        }
        else if (op == "bus")
        {
            int v = 0;
            in >> v;
            setBus(v != 0);
        }
        else if (op == "busdip")
        {
            uint64_t ms = 200;
            in >> ms;
            busDip(ms);
        }
        else if (op == "supply")
        {
            int v = 0;
            in >> v;
            sim::relaySupply = v != 0;
            event("supply", std::string("\"on\":") + (v ? "true" : "false"));
            sim::emitRelayChanges();
        }
        else if (op == "bi")
        {
            int n = 0, v = 0;
            in >> n >> v;
            if (n < 1 || n > sim::kInputs)
            {
                event("error", "\"text\":" + sim::esc("Binaereingang " + std::to_string(n) + " gibt es nicht"));
                return;
            }
            sim::contactClosed[n - 1] = v != 0;
            event("contact", "\"no\":" + std::to_string(n) + ",\"closed\":" + (v ? "true" : "false"));
        }
        else if (op == "ko")
        {
            int nr = -1;
            std::string dpt;
            in >> nr >> dpt;
            receiveKo(nr, dpt, rest(in));
        }
        else if (op == "btn")
        {
            std::string kind;
            in >> kind;
            auto &b = openknx.func1Button;
            std::function<void()> cb =
                kind == "short" ? b.shortCb : (kind == "long" ? b.longCb : b.doubleCb);
            if (sim::mcuRunning && cb)
                cb();
            else
                event("info", "\"text\":" + sim::esc("Funktionstaste " + kind + ": kein Modul belegt sie"));
        }
        else if (op == "fp")
        {
            int obj = 0, prop = 0;
            in >> obj >> prop;
            event("fp", "\"obj\":" + std::to_string(obj) + ",\"prop\":" + std::to_string(prop) +
                            ",\"handled\":false,\"res\":\"\"");
        }
        else if (op == "run")
        {
            uint64_t ms = 0;
            in >> ms;
            run(ms);
        }
        else if (op == "stepms")
        {
            in >> stepMs;
            if (stepMs == 0)
                stepMs = 1;
        }
        else if (op == "cmd")
            console(rest(in), false);
        else if (op == "diag")
            console(rest(in), true);
        else if (op == "state")
            state();
        else if (op == "quit")
            std::exit(0);
        else
            event("error", "\"text\":" + sim::esc("unbekannter Befehl: " + op));
    }
} // namespace

int main()
{
    std::ios::sync_with_stdio(false);
    static char outbuf[1 << 16];
    std::setvbuf(stdout, outbuf, _IOFBF, sizeof(outbuf));

    // Ausgangslage: Bus aus, nie programmiert, Relaisversorgung da.
    for (size_t i = 0; i < sim::kos.size(); i++)
        sim::kos[i].simInit(static_cast<uint16_t>(i));
    sim::emitRelayChanges();
    std::string pinList;
    for (int i = 0; i < sim::kInputs; i++)
        pinList += (i ? "," : "") + std::to_string(sim::inputPins[i]);
    std::string relayList;
    for (int i = 0; i < sim::kRelays; i++)
        relayList += (i ? "," : "") + std::to_string(sim::relayPins[i]);
    event("hello", "\"paramSize\":" + std::to_string(MAIN_ParameterSize) +
                       ",\"maxKo\":" + std::to_string(MAIN_MaxKoNumber) +
                       ",\"device\":" + sim::esc(DEVICE_NAME) +
                       ",\"inputPins\":[" + pinList + "],\"relayPins\":[" + relayList + "]");
    std::fflush(stdout);

    std::string line;
    while (std::getline(std::cin, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        try
        {
            handle(line);
        }
        catch (const std::exception &e)
        {
            event("error", "\"text\":" + sim::esc(std::string("Ausnahme: ") + e.what()));
        }
        sim::emit("{\"ev\":\"ok\"}");
        std::fflush(stdout);
    }
    return 0;
}
