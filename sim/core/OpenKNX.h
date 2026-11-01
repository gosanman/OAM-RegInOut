#pragma once
// ============================================================================
// Simulator: Ersatz fuer OGM-Common/OpenKNX.h.
//
// Bildet genau die Teile von OpenKNX und dem KNX-Stack nach, die die Module
// OFM-BinaryInput und OFM-SwitchActuator benutzen - nicht mehr. Der
// Firmware-Code selbst wird UNVERAENDERT uebersetzt; nur was er von aussen
// aufruft, kommt von hier: Parameterspeicher (knx.paramByte ...),
// Gruppenobjekte, GPIO, Logger, Konsole, Flash und LED-Funktionen.
//
// Die DPT-Kodierung ist die ECHTE der KNX-Bibliothek (dptconvert.cpp,
// knx_value.cpp): ein Wert wird beim Senden so kodiert und beim Empfangen so
// dekodiert wie am Bus. Ein falscher DPT faellt damit im Simulator auf.
//
// Die Arduino-Grundlagen (HIGH/LOW, INPUT_PULLUP, MIN/MAX, bitRead, delay,
// millis) kommen aus lib/knx/src/knx/bits.h, Zweig "Non-Arduino platforms".
// ============================================================================

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "bits.h"

#include "hardware.h" // Board-Header des Geraets
#include "versions.h" // vom Firmware-Build erzeugt (MODULE_..._Version)

#include "dpt.h"
#include "dptconvert.h"
#include "knx_value.h"

// ---------------------------------------------------------------- Arduino-Rest

uint32_t micros();

#ifndef bitWrite
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? ((value) |= (1UL << (bit))) : ((value) &= ~(1UL << (bit))))
#endif

// min/max wie im arduino-pico-Kern (api/Common.h): auch fuer gemischte Typen
template <class T, class L>
inline auto max(const T &a, const L &b) -> decltype((b < a) ? b : a) { return (a < b) ? b : a; }

// wie OGM-Common/src/OpenKNX/Helper.h
#define delayCheckMillis(last, duration) (millis() - last >= duration)
#define delayCheck(last, duration) delayCheckMillis(last, duration)
#define delayTimerInit() (max(millis(), 1UL))

// ---------------------------------------------------------------- KNX-Stack
// ParameterFloatEncodings steht in bits.h.

/// Ein Gruppenobjekt. Haelt die Nutzdaten wie im echten Stack als Bytes.
class GroupObject
{
  public:
    uint16_t asap() const { return mNumber; }

    /// Empfangener oder zuletzt gesendeter Wert, dekodiert mit `type`.
    KNXValue value(const Dpt &type);

    /// Wie im Stack: false, wenn der DPT den Wert nicht dekodieren kann.
    bool tryValue(KNXValue &value, const Dpt &type);

    /// Wert setzen UND senden - wie GroupObject::value() im Stack.
    void value(const KNXValue &value, const Dpt &type);

    /// Wert setzen, ohne zu senden. false: DPT nicht kodierbar.
    bool valueNoSend(const KNXValue &value, const Dpt &type);

    // --- nur Simulator ------------------------------------------------------
    void simInit(uint16_t number)
    {
        mNumber = number;
        std::memset(mData, 0, sizeof(mData));
    }
    /// Telegramm vom Bus: Bytes so ablegen, wie sie ankommen.
    bool simReceive(const KNXValue &value, const Dpt &type);
    const uint8_t *simData() const { return mData; }

  private:
    uint16_t mNumber = 0;
    uint8_t mData[16] = {};
};

class SimKnx
{
  public:
    uint8_t paramByte(uint32_t addr);
    uint16_t paramWord(uint32_t addr);
    uint32_t paramInt(uint32_t addr);
    double paramFloat(uint32_t addr, ParameterFloatEncodings enc);
    uint8_t *paramData(uint32_t addr);
    char *paramString(uint32_t addr, uint32_t length);
    GroupObject &getGroupObject(uint16_t number);
    bool configured();
};

extern SimKnx knx;

// ---------------------------------------------------------------- OpenKNX

#define CONSOLE_HEADLINE_COLOR 33

namespace OpenKNX
{
    /// Wie OGM-Common Base: setup(bool)/loop(bool) rufen setup()/loop() nur
    /// bei parametriertem Geraet auf. Ein Modul, das nur loop() ueberschreibt,
    /// laeuft unparametriert also nicht.
    class Base
    {
      public:
        virtual ~Base() = default;
        virtual const std::string name() = 0;
        virtual void init() {}
        virtual void setup(bool configured)
        {
            if (configured)
                setup();
        }
        virtual void setup() {}
        virtual void loop(bool configured)
        {
            if (configured)
                loop();
        }
        virtual void loop() {}
        virtual void processInputKo(GroupObject &ko) { (void)ko; }

      protected:
        virtual const std::string logPrefix() { return name(); }
    };

    class Channel : public Base
    {
      protected:
        uint8_t _channelIndex = 0;
        const std::string logPrefix() override
        {
            return name() + "<" + std::to_string(_channelIndex + 1) + ">";
        }

      public:
        virtual uint8_t channelIndex() { return _channelIndex; }
    };

    class Module : public Base
    {
      public:
        virtual const std::string version() { return ""; }
        virtual uint16_t flashSize() { return 0; }
        virtual void writeFlash() {}
        virtual void readFlash(const uint8_t *data, const uint16_t size)
        {
            (void)data;
            (void)size;
        }
        virtual void processAfterStartupDelay() {}
        virtual void processBeforeRestart() {}
        virtual void savePower() {}
        virtual bool restorePower() { return true; }
        virtual void showHelp() {}
        virtual bool processCommand(const std::string cmd, bool diagnoseKo)
        {
            (void)cmd;
            (void)diagnoseKo;
            return false;
        }
    };

    namespace Led
    {
        /// Zustand einer LED-Funktion, so wie das Modul ihn setzt.
        enum class SimMode : uint8_t
        {
            Off,
            On,
            Blinking,
            Activity
        };

        class FunctionGroup
        {
          public:
            explicit FunctionGroup(uint32_t id) : mId(id) {}
            void on(bool state = true) { mMode = state ? SimMode::On : SimMode::Off; }
            void off() { mMode = SimMode::Off; }
            void blinking(uint16_t frequency = 1000)
            {
                (void)frequency;
                mMode = SimMode::Blinking;
            }
            void activity(uint32_t &lastActivity, bool inverted = false)
            {
                (void)inverted;
                lastActivity = millis();
                mMode = SimMode::Activity;
                mActivities++;
            }

            // --- nur Simulator ---------------------------------------------
            uint32_t id() const { return mId; }
            SimMode mode() const { return mMode; }
            uint32_t activities() const { return mActivities; }
            void simReset()
            {
                mMode = SimMode::Off;
                mActivities = 0;
            }

          private:
            uint32_t mId;
            SimMode mMode = SimMode::Off;
            uint32_t mActivities = 0;
        };
    } // namespace Led

    class SimLedFunctions
    {
      public:
        Led::FunctionGroup *get(uint32_t id);
        std::vector<Led::FunctionGroup *> all();
        void simReset();

      private:
        std::vector<Led::FunctionGroup *> mGroups;
    };

    class SimConsole
    {
      public:
        void writeDiagnoseKo(const char *fmt, ...);
        /// Schreibweise mit Tippfehler, in OGM-Common noch vorhanden und vom
        /// Schaltaktor benutzt.
        void writeDiagenoseKo(const char *fmt, ...);
    };

    class SimLogger
    {
      public:
        void logMacroWrapper(int color, const char *prefix, const char *fmt, ...);
        void log(const char *text);
        void color(int c) { (void)c; }
        void indentUp() { mIndent++; }
        void indentDown()
        {
            if (mIndent > 0)
                mIndent--;
        }
        int indent() const { return mIndent; }

      private:
        int mIndent = 0;
    };

    /// Flash wie OpenKNX: je Modul ein Block, sequentiell geschrieben/gelesen.
    class SimFlash
    {
      public:
        void writeByte(uint8_t v);
        void writeWord(uint16_t v);
        void writeInt(uint32_t v);
        uint8_t readByte();
        uint16_t readWord();
        uint32_t readInt();

        // --- nur Simulator -------------------------------------------------
        std::vector<uint8_t> *writeTarget = nullptr;
        const uint8_t *readPtr = nullptr;
        const uint8_t *readEnd = nullptr;
    };

    /// GPIO wie OpenKNX::GPIO::Manager mit dem eingebauten Treiber des RP2040.
    /// Die Pegel kommen aus dem Board-Modell (sim_board.cpp).
    class SimGpio
    {
      public:
        void pinMode(uint16_t pin, int mode, bool preset = false, int status = 0);
        void digitalWrite(uint16_t pin, int status);
        bool digitalRead(uint16_t pin);
    };

    class SimButton
    {
      public:
        void onShortClick(std::function<void()> cb) { shortCb = cb; }
        void onLongClick(std::function<void()> cb) { longCb = cb; }
        void onDoubleClick(std::function<void()> cb) { doubleCb = cb; }
        std::function<void()> shortCb, longCb, doubleCb;
    };

    struct SimOpenKnx
    {
        SimConsole console;
        SimLogger logger;
        SimFlash flash;
        SimLedFunctions ledFunctions;
        SimGpio gpio;
        SimButton func1Button;
    };
} // namespace OpenKNX

extern OpenKNX::SimOpenKnx openknx;

#define logInfoP(...) openknx.logger.logMacroWrapper(0, logPrefix().c_str(), __VA_ARGS__)
#define logErrorP(...) openknx.logger.logMacroWrapper(31, logPrefix().c_str(), __VA_ARGS__)
#define logDebugP(...) openknx.logger.logMacroWrapper(90, logPrefix().c_str(), __VA_ARGS__)
#define logTraceP(...)
#define logInfo(prefix, ...) openknx.logger.logMacroWrapper(0, prefix, __VA_ARGS__)
#define logIndentUp() openknx.logger.indentUp()
#define logIndentDown() openknx.logger.indentDown()

// wie OGM-Common: die Module sehen die Parameter- und KO-Makros ueber OpenKNX.h
#include "knxprod.h"
