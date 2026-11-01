#pragma once
// ============================================================================
// Simulator: gemeinsamer Zustand von Laufzeit, Board und Hauptprogramm.
//
// Der Simulator trennt, was im Geraet getrennt ist:
//   - der Prozessor (RP2040) mit der Firmware - laeuft nur mit KNX-Bus; ein
//     Neustart setzt RAM und GPIOs zurueck (alle Pins hochohmig),
//   - die Relais - monostabil: angezogen nur, solange der Prozessor den Pin
//     auf OPENKNX_SWA_ACTIVE_ON treibt UND die Relaisversorgung anliegt,
//   - die Kontakte an den Binaereingaengen - von aussen geschaltet,
//   - der Parameterspeicher (ETS) und der Flash - ueberleben einen Neustart.
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "OpenKNX.h"

namespace sim
{
    // --- Zeit ----------------------------------------------------------------
    extern uint64_t now;    ///< Simulationszeit in ms seit Programmstart
    extern uint64_t bootAt; ///< Zeitpunkt des letzten Prozessorstarts

    // --- Geraetezustand -------------------------------------------------------
    extern std::vector<uint8_t> params; ///< wirksamer Parameterspeicher
    extern bool paramsValid;            ///< die ETS hat das Geraet programmiert
    extern bool downloading;            ///< ETS-Download laeuft gerade
    extern bool mcuRunning;             ///< Prozessor laeuft (KNX-Bus da)
    extern bool configured();           ///< knx.configured()

    extern std::vector<GroupObject> kos;
    extern uint64_t txCount; ///< gesendete Telegramme seit Programmstart

    // --- Flash, je Modul ein Block --------------------------------------------
    extern std::vector<std::vector<uint8_t>> flashBlocks;
    extern bool flashValid;

    // --- Board ----------------------------------------------------------------
    constexpr int kPins = 30; ///< GPIO 0..29 des RP2040
    constexpr int kModeUnset = -1;

    struct Pin
    {
        int mode = kModeUnset; ///< hochohmig nach dem Einschalten
        bool level = false;    ///< Ausgangspegel (nur bei OUTPUT wirksam)
        uint32_t writes = 0;   ///< digitalWrite-Aufrufe seit dem Start
    };
    extern Pin pins[kPins];

    constexpr int kInputs = OPENKNX_BI_GPIO_COUNT;
    constexpr int kRelays = OPENKNX_SWA_CHANNEL_COUNT;
    extern const uint16_t inputPins[kInputs];
    extern const uint16_t relayPins[kRelays];

    extern bool contactClosed[kInputs]; ///< Kontakt am Binaereingang geschlossen
    extern bool relaySupply;            ///< Versorgung der Relaisspulen

    /// Pegel, den der Prozessor an einem Pin liest.
    bool readPin(int pin);
    /// Relais angezogen (Spule bestromt).
    bool relayOn(int channel);
    /// Prozessor aus: alle Pins hochohmig.
    void pinsReset();

    /// Jede Aenderung eines Relais als Ereignis "relay" (fuer den Verlauf).
    void emitRelayChanges();

    /// delay() der Firmware: die Zeit laeuft weiter, der Prozessor steht.
    void blockingDelay(uint32_t ms);

    // --- Ausgabe ------------------------------------------------------------
    void emit(const std::string &json);
    std::string esc(const std::string &s);
    std::string num(double v, int decimals = 3);
    std::string hexOf(const uint8_t *data, int len);
    std::string koValueJson(GroupObject &ko, const Dpt &type);
    uint8_t lengthFor(const Dpt &type);
    Dpt parseDpt(const std::string &s);
    Dpt fallbackDpt(const Dpt &type);

    /// Inspektion (sim_inspect.cpp): Zustand der Module als JSON.
    std::string snapshotJson();
    /// Invariantenwaechter, nach jedem Durchlauf.
    void checkInvariants();
    void resetInvariants();
    /// Meldung an den Waechter, gleiche Meldung gedrosselt.
    void warn(const std::string &key, const std::string &inv, const std::string &text);
} // namespace sim
