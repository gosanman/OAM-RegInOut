// ============================================================================
// Simulator: Inspektion und Invariantenwaechter.
//
// Liest den inneren Zustand der Module fuer die Anzeige. Dafuer werden in
// DIESER Uebersetzungseinheit private Felder geoeffnet - ein Kniff fuer
// Testwerkzeuge, der den Firmware-Code unveraendert laesst. Alle
// Standard-Header muessen VOR dem #define stehen.
//
// Kanalparameter werden ueber die Makros aus knxprod.h gelesen, genau wie im
// Kanal selbst: Sie rechnen mit einer Variablen _channelIndex im Gueltigkeits-
// bereich.
// ============================================================================

#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sim.h"

#define private public
#define protected public
#include "GpioBinaryInputModule.h"
#include "SwitchActuatorModule.h"
#undef private
#undef protected

namespace sim
{
    static std::string b(bool v) { return v ? "true" : "false"; }

    static uint8_t visibleRelays()
    {
        return MIN(ParamSWA_VisibleChannels, OPENKNX_SWA_CHANNEL_COUNT);
    }

    /// Restzeit eines Verzoegerungs-Timers der Firmware in ms (0 = keiner).
    static uint32_t remaining(uint32_t timer, uint32_t duration)
    {
        if (timer == 0)
            return 0;
        const uint32_t elapsed = millis() - timer;
        return elapsed >= duration ? 0 : duration - elapsed;
    }

    std::string snapshotJson()
    {
        SwitchActuatorModule &swa = openknxSwitchActuatorModule;
        GpioBinaryInputModule &bi = openknxGpioBinaryInputModule;
        const bool cfg = configured() && mcuRunning;
        std::string o = "{";

        // --- Relais ----------------------------------------------------------
        o += "\"relays\":[";
        for (uint8_t _channelIndex = 0; _channelIndex < kRelays; _channelIndex++)
        {
            const Pin &p = pins[relayPins[_channelIndex]];
            const SwitchActuatorChannel *c = mcuRunning ? swa.channel[_channelIndex] : nullptr;
            const bool visible = cfg && _channelIndex < visibleRelays();
            if (_channelIndex)
                o += ",";
            o += "{\"no\":" + std::to_string(_channelIndex + 1) +
                 ",\"pin\":" + std::to_string(relayPins[_channelIndex]) +
                 ",\"on\":" + b(relayOn(_channelIndex)) +
                 ",\"level\":" + b(p.level) + ",\"output\":" + b(p.mode == OUTPUT) +
                 ",\"writes\":" + std::to_string(p.writes) +
                 ",\"visible\":" + b(visible);
            if (visible)
            {
                o += ",\"active\":" + b(ParamSWA_ChActive == 1) +
                     ",\"opener\":" + b(ParamSWA_ChOperationMode) +
                     ",\"status\":" + b((bool)KoSWA_ChStatus.value(DPT_Switch)) +
                     ",\"locked\":" + b((bool)KoSWA_ChLockStatus.value(DPT_Switch));
                if (c != nullptr)
                    o += ",\"onDelay\":" + std::to_string(remaining(c->turnOnDelayTimer, ParamSWA_ChTurnOnDelayTimeMS)) +
                         ",\"offDelay\":" + std::to_string(remaining(c->turnOffDelayTimer, ParamSWA_ChTurnOffDelayTimeMS));
            }
            o += "}";
        }

        // --- Binaereingaenge -------------------------------------------------
        o += "],\"inputs\":[";
        for (uint8_t _channelIndex = 0; _channelIndex < kInputs; _channelIndex++)
        {
            const Pin &p = pins[inputPins[_channelIndex]];
            const BinaryInputChannel *c =
                (cfg && _channelIndex < BI_ChannelCount) ? bi._channels[_channelIndex] : nullptr;
            if (_channelIndex)
                o += ",";
            o += "{\"no\":" + std::to_string(_channelIndex + 1) +
                 ",\"pin\":" + std::to_string(inputPins[_channelIndex]) +
                 ",\"closed\":" + b(contactClosed[_channelIndex]) +
                 ",\"read\":" + b(readPin(inputPins[_channelIndex])) +
                 ",\"pull\":" + std::to_string(p.mode);
            if (c != nullptr)
                o += ",\"active\":" + b(ParamBI_ChannelActive) +
                     ",\"state\":" + std::to_string(c->_currentState) +
                     ",\"hw\":" + std::to_string(c->_currentHardwareState);
            o += "}";
        }

        // --- LED-Funktionen --------------------------------------------------
        o += "],\"leds\":[";
        bool first = true;
        for (auto *g : openknx.ledFunctions.all())
        {
            if (!first)
                o += ",";
            first = false;
            o += "[" + std::to_string(g->id()) + "," + std::to_string(static_cast<int>(g->mode())) + "," +
                 std::to_string(g->activities()) + "]";
        }

        o += "],\"supply\":" + b(relaySupply) + ",\"tx\":" + std::to_string(txCount) + "}";
        return o;
    }

    // ------------------------------------------------------------ Waechter

    struct WarnState
    {
        uint64_t last = 0;
        uint32_t count = 0;
    };
    static std::map<std::string, WarnState> warned;
    static int mismatchSteps[kRelays] = {};

    void resetInvariants()
    {
        for (auto &m : mismatchSteps)
            m = 0;
    }

    void warn(const std::string &key, const std::string &inv, const std::string &text)
    {
        // gleiche Meldung hoechstens alle 5 s, mit Zaehler
        WarnState &w = warned[key];
        w.count++;
        if (w.last != 0 && now - w.last < 5000)
            return;
        w.last = now == 0 ? 1 : now;
        emit(std::string("{\"ev\":\"warn\",\"t\":") + std::to_string(now) + ",\"inv\":" + esc(inv) +
             ",\"text\":" + esc(text) + ",\"count\":" + std::to_string(w.count) + "}");
        w.count = 0;
    }

    void checkInvariants()
    {
        if (!mcuRunning)
        {
            resetInvariants();
            return;
        }

        // Unparametriert ist die Firmware sicher: kein Relais angezogen. Waehrend
        // eines ETS-Downloads halten die Relais ihren Zustand - das ist gewollt.
        if (!paramsValid)
        {
            for (int ch = 0; ch < kRelays; ch++)
                if (relayOn(ch))
                    warn("I-unconf-" + std::to_string(ch), "Unparametriert",
                         "Relais K" + std::to_string(ch + 1) + " angezogen, obwohl das Geraet nicht parametriert ist");
            resetInvariants();
            return;
        }
        if (!configured())
        {
            resetInvariants();
            return;
        }

        // Status = Relais: das Statusobjekt meldet, was der Kontakt tut. Bei
        // "Oeffner" ist die Spule invertiert. Ohne Relaisversorgung kann die
        // Firmware den Kontakt nicht kennen - dann keine Pruefung.
        SwitchActuatorModule &swa = openknxSwitchActuatorModule;
        for (uint8_t _channelIndex = 0; _channelIndex < kRelays; _channelIndex++)
        {
            int &steps = mismatchSteps[_channelIndex];
            if (!relaySupply || _channelIndex >= visibleRelays() || ParamSWA_ChActive != 1 ||
                swa.channel[_channelIndex] == nullptr)
            {
                steps = 0;
                continue;
            }
            const bool status = (bool)KoSWA_ChStatus.value(DPT_Switch);
            const bool coil = status != ParamSWA_ChOperationMode;
            if (relayOn(_channelIndex) == coil)
            {
                steps = 0;
                continue;
            }
            // ein Durchlauf Toleranz: Pin und Status setzt doSwitchInternal
            // in einem Zug, dazwischen liegt kein loop()
            if (++steps >= 2)
                warn("I-status-" + std::to_string(_channelIndex), "Status = Relais",
                     "K" + std::to_string(_channelIndex + 1) + ": Statusobjekt meldet " +
                         (status ? "Ein" : "Aus") + ", die Spule ist " +
                         (relayOn(_channelIndex) ? "bestromt" : "stromlos"));
        }
    }
} // namespace sim
