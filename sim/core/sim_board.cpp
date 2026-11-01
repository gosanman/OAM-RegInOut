// ============================================================================
// Simulator: Board-Modell der REG1-Platine (include/hardware.h).
//
// Was die Firmware sehen kann, ist nur der Pegel an ihren Pins:
//   - Binaereingaenge: ein geschlossener Kontakt zieht den Pin auf
//     OPENKNX_BI_ONLEVEL. Offen liegt der Pull-up (bzw. Pull-down) an; ohne
//     internen Pull wird ein externer Widerstand angenommen.
//   - Relais: monostabil. Angezogen nur, solange der Prozessor laeuft, den
//     Pin als Ausgang auf OPENKNX_SWA_ACTIVE_ON treibt und die Relais-
//     versorgung anliegt. Faellt der Prozessor aus, sind die Pins hochohmig
//     und das Relais faellt ab.
// ============================================================================

#include "sim.h"

namespace sim
{
    Pin pins[kPins];
    const uint16_t inputPins[kInputs] = {OPENKNX_BI_GPIO_PINS};
    const uint16_t relayPins[kRelays] = {OPENKNX_SWA_PINS};
    bool contactClosed[kInputs] = {};
    bool relaySupply = true;

    void pinsReset()
    {
        for (auto &p : pins)
            p = Pin{};
    }

    bool readPin(int pin)
    {
        if (pin < 0 || pin >= kPins)
            return false;
        const Pin &p = pins[pin];
        if (p.mode == OUTPUT)
            return p.level;
        for (int i = 0; i < kInputs; i++)
        {
            if (inputPins[i] != pin)
                continue;
            if (contactClosed[i])
                return OPENKNX_BI_ONLEVEL != 0;
            if (p.mode == INPUT_PULLUP)
                return true;
            if (p.mode == INPUT_PULLDOWN)
                return false;
            return OPENKNX_BI_ONLEVEL == 0;
        }
        return false;
    }

    bool relayOn(int channel)
    {
        if (channel < 0 || channel >= kRelays || !mcuRunning || !relaySupply)
            return false;
        const Pin &p = pins[relayPins[channel]];
        return p.mode == OUTPUT && p.level == (OPENKNX_SWA_ACTIVE_ON != 0);
    }

    void emitRelayChanges()
    {
        static int last[kRelays];
        static bool first = true;
        bool changed = first;
        int v[kRelays];
        for (int ch = 0; ch < kRelays; ch++)
        {
            v[ch] = relayOn(ch) ? 1 : 0;
            if (!first && v[ch] != last[ch])
                changed = true;
        }
        first = false;
        if (!changed)
            return;
        std::string o = "{\"ev\":\"relay\",\"t\":" + std::to_string(now) + ",\"on\":[";
        for (int ch = 0; ch < kRelays; ch++)
        {
            last[ch] = v[ch];
            o += (ch ? "," : "") + std::to_string(v[ch]);
        }
        emit(o + "]}");
    }

    void blockingDelay(uint32_t ms)
    {
        // Die Pins bleiben waehrend der Wartezeit stehen: erst den Zustand
        // davor melden, dann die Zeit weiterlaufen lassen.
        emitRelayChanges();
        now += ms;
#ifdef OPENKNX_LOOPTIME_WARNING
        if (ms >= OPENKNX_LOOPTIME_WARNING)
            warn("delay", "Blockierung",
                 "Firmware wartet mit delay(" + std::to_string(ms) + ") - der Prozessor steht so lange");
#endif
    }
} // namespace sim
