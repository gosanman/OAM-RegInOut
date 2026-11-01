// ============================================================================
// Simulator: Laufzeit fuer die OpenKNX-Nachbildung (OpenKNX.h).
// ============================================================================

#include <cmath>
#include <cstdio>

#include "sim.h"

#include "knxprod.h"

SimKnx knx;
OpenKNX::SimOpenKnx openknx;

namespace sim
{
    uint64_t now = 0;
    uint64_t bootAt = 0;
    std::vector<uint8_t> params(MAIN_ParameterSize, 0);
    bool paramsValid = false;
    bool downloading = false;
    bool mcuRunning = false;
    std::vector<GroupObject> kos(MAIN_MaxKoNumber + 1);
    uint64_t txCount = 0;
    std::vector<std::vector<uint8_t>> flashBlocks;
    bool flashValid = false;

    bool configured() { return paramsValid && !downloading; }

    uint8_t lengthFor(const Dpt &type)
    {
        const uint8_t n = type.dataLength();
        return n == 0 ? 1 : (n > 14 ? 14 : n);
    }

    void emit(const std::string &json)
    {
        std::fputs(json.c_str(), stdout);
        std::fputc('\n', stdout);
    }

    std::string esc(const std::string &s)
    {
        std::string o = "\"";
        for (unsigned char c : s)
        {
            switch (c)
            {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': break;
                case '\t': o += "\\t"; break;
                default:
                    if (c < 0x20)
                    {
                        char b[8];
                        std::snprintf(b, sizeof(b), "\\u%04x", c);
                        o += b;
                    }
                    else
                        o += static_cast<char>(c);
            }
        }
        return o + "\"";
    }

    std::string num(double v, int decimals)
    {
        if (!std::isfinite(v))
            return "null";
        char b[64];
        std::snprintf(b, sizeof(b), "%.*f", decimals, v);
        // ueberfluessige Nullen weg, damit das JSON schlank bleibt
        std::string s = b;
        if (s.find('.') != std::string::npos)
        {
            while (!s.empty() && s.back() == '0')
                s.pop_back();
            if (!s.empty() && s.back() == '.')
                s.pop_back();
        }
        if (s == "-0")
            s = "0";
        return s;
    }

    std::string hexOf(const uint8_t *data, int len)
    {
        std::string o;
        char b[4];
        for (int i = 0; i < len; i++)
        {
            std::snprintf(b, sizeof(b), "%02X", data[i]);
            o += b;
        }
        return o;
    }

    Dpt parseDpt(const std::string &s)
    {
        int main = 0, sub = 0;
        std::sscanf(s.c_str(), "%d.%d", &main, &sub);
        return Dpt(main, sub);
    }

    /// Gleich grosser Ersatztyp, wenn die KNX-Bibliothek einen DPT nicht kennt:
    /// am Bus ist es dasselbe Byte, die Firmware liest es mit ihrem eigenen DPT.
    Dpt fallbackDpt(const Dpt &type)
    {
        switch (type.mainGroup)
        {
            case 1: return Dpt(1, 1);
            case 5: return Dpt(5, 10);
            case 6: return Dpt(6, 10);
            case 7: return Dpt(7, 1);
            case 8: return Dpt(8, 1);
            case 9: return Dpt(9, 1);
            default: return type;
        }
    }

    std::string koValueJson(GroupObject &ko, const Dpt &requested)
    {
        // Anzeige: so dekodieren, wie es ein Busmonitor taete. Kennt die
        // Bibliothek den DPT nicht, das gleich grosse Ersatzformat.
        Dpt type = requested;
        KNXValue v = "";
        if (!KNX_Decode_Value(const_cast<uint8_t *>(ko.simData()), lengthFor(type), type, v))
        {
            type = fallbackDpt(type);
            v = "";
            KNX_Decode_Value(const_cast<uint8_t *>(ko.simData()), lengthFor(type), type, v);
        }
        if (type.mainGroup == 1)
            return static_cast<bool>(v) ? "true" : "false";
        if (type.mainGroup == 16)
        {
            // DPT 16.001 ist ISO 8859-1, das JSON zur Oberflaeche ist UTF-8.
            std::string utf8;
            for (const unsigned char c : std::string(static_cast<const char *>(v)))
            {
                if (c < 0x80)
                    utf8 += static_cast<char>(c);
                else
                {
                    utf8 += static_cast<char>(0xC0 | (c >> 6));
                    utf8 += static_cast<char>(0x80 | (c & 0x3F));
                }
            }
            return esc(utf8);
        }
        return num(static_cast<double>(v), 3);
    }
} // namespace sim

uint32_t millis() { return static_cast<uint32_t>(sim::now - sim::bootAt); }
uint32_t micros() { return millis() * 1000; }
void delay(uint32_t ms) { sim::blockingDelay(ms); }
void delayMicroseconds(unsigned int) {}

// ------------------------------------------------------------ Gruppenobjekte

static std::string dptText(const Dpt &t)
{
    return std::to_string(t.mainGroup) + "." + std::to_string(t.subGroup);
}

// Verhalten wie lib/knx/src/knx/group_object.cpp - auch im Fehlerfall:
//  - value(type): scheitert die Dekodierung, kommt KNXValue("") zurueck, also 0.
//  - value(v, type): scheitert die Kodierung, bleiben die Daten unveraendert,
//    und es wird NICHT gesendet.
// Beides meldet der Simulator, weil es am Geraet unbemerkt passiert.
KNXValue GroupObject::value(const Dpt &type)
{
    KNXValue v = "";
    if (!KNX_Decode_Value(mData, sim::lengthFor(type), type, v))
        sim::warn("DPT-R" + std::to_string(mNumber), "DPT",
                  "Firmware liest Objekt " + std::to_string(mNumber) + " als DPT " + dptText(type) +
                      " - die KNX-Bibliothek kann diesen DPT nicht dekodieren, Ergebnis 0");
    return v;
}

bool GroupObject::tryValue(KNXValue &value, const Dpt &type)
{
    return KNX_Decode_Value(mData, sim::lengthFor(type), type, value);
}

bool GroupObject::valueNoSend(const KNXValue &value, const Dpt &type)
{
    return KNX_Encode_Value(value, mData, sim::lengthFor(type), type);
}

void GroupObject::value(const KNXValue &value, const Dpt &type)
{
    if (!valueNoSend(value, type))
    {
        sim::warn("DPT-W" + std::to_string(mNumber), "DPT",
                  "Firmware sendet Objekt " + std::to_string(mNumber) + " als DPT " + dptText(type) +
                      " - die KNX-Bibliothek kann diesen DPT nicht kodieren, das Telegramm entfaellt");
        return;
    }
    // Jeder Aufruf ist ein Telegramm - genau wie im Stack.
    sim::txCount++;
    sim::emit(std::string("{\"ev\":\"ko\",\"t\":") + std::to_string(sim::now) +
              ",\"nr\":" + std::to_string(mNumber) + ",\"dpt\":\"" + dptText(type) +
              "\",\"val\":" + sim::koValueJson(*this, type) +
              ",\"raw\":\"" + sim::hexOf(mData, sim::lengthFor(type)) + "\"}");
}

bool GroupObject::simReceive(const KNXValue &value, const Dpt &type)
{
    // Ein Telegramm vom Bus: so kodiert, wie es ein korrekter Sender tut.
    std::memset(mData, 0, sizeof(mData));
    if (KNX_Encode_Value(value, mData, sim::lengthFor(type), type))
        return true;
    std::memset(mData, 0, sizeof(mData));
    const Dpt alt = sim::fallbackDpt(type);
    return alt != type && KNX_Encode_Value(value, mData, sim::lengthFor(alt), alt);
}

// ------------------------------------------------------------ Parameter

uint8_t SimKnx::paramByte(uint32_t addr)
{
    return addr < sim::params.size() ? sim::params[addr] : 0;
}

uint16_t SimKnx::paramWord(uint32_t addr)
{
    return static_cast<uint16_t>((paramByte(addr) << 8) | paramByte(addr + 1));
}

uint32_t SimKnx::paramInt(uint32_t addr)
{
    return (static_cast<uint32_t>(paramByte(addr)) << 24) |
           (static_cast<uint32_t>(paramByte(addr + 1)) << 16) |
           (static_cast<uint32_t>(paramByte(addr + 2)) << 8) | paramByte(addr + 3);
}

double SimKnx::paramFloat(uint32_t addr, ParameterFloatEncodings enc)
{
    if (enc == Float_Enc_IEEE754Single)
    {
        uint32_t raw = paramInt(addr);
        float f;
        std::memcpy(&f, &raw, sizeof(f));
        return f;
    }
    if (enc == Float_Enc_DPT9)
    {
        const uint16_t raw = paramWord(addr);
        const int exponent = (raw >> 11) & 0x0F;
        int mantissa = raw & 0x07FF;
        if (raw & 0x8000)
            mantissa -= 2048;
        return 0.01 * mantissa * (1 << exponent);
    }
    return 0.0;
}

uint8_t *SimKnx::paramData(uint32_t addr)
{
    return addr < sim::params.size() ? &sim::params[addr] : nullptr;
}

char *SimKnx::paramString(uint32_t addr, uint32_t length)
{
    static std::string buffer;
    buffer.assign(reinterpret_cast<const char *>(paramData(addr)), length);
    return buffer.data();
}

GroupObject &SimKnx::getGroupObject(uint16_t number)
{
    if (number >= sim::kos.size())
        return sim::kos[0];
    return sim::kos[number];
}

bool SimKnx::configured() { return sim::configured(); }

// ------------------------------------------------------------ Logger, Konsole

namespace OpenKNX
{
    static void emitLog(const char *level, const std::string &prefix, const std::string &text,
                        int indent)
    {
        std::string line(static_cast<size_t>(indent) * 2, ' ');
        line += text;
        sim::emit(std::string("{\"ev\":\"log\",\"t\":") + std::to_string(sim::now) +
                  ",\"level\":\"" + level + "\",\"src\":" + sim::esc(prefix) +
                  ",\"text\":" + sim::esc(line) + "}");
    }

    void SimLogger::logMacroWrapper(int color, const char *prefix, const char *fmt, ...)
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        emitLog(color == 31 ? "error" : (color == 90 ? "debug" : "info"), prefix, buf, mIndent);
    }

    void SimLogger::log(const char *text) { emitLog("info", "", text, mIndent); }

    static void diagnoseKo(const char *fmt, va_list ap)
    {
        char buf[64];
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        buf[14] = '\0'; // DPT 16: 14 Zeichen
#ifdef BASE_KoDiagnose
        knx.getGroupObject(BASE_KoDiagnose).value(buf, Dpt(16, 1));
#endif
    }

    void SimConsole::writeDiagnoseKo(const char *fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        diagnoseKo(fmt, ap);
        va_end(ap);
    }

    void SimConsole::writeDiagenoseKo(const char *fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        diagnoseKo(fmt, ap);
        va_end(ap);
    }

    // ---------------------------------------------------------------- Flash

    void SimFlash::writeByte(uint8_t v)
    {
        if (writeTarget != nullptr)
            writeTarget->push_back(v);
    }
    void SimFlash::writeWord(uint16_t v)
    {
        writeByte(static_cast<uint8_t>(v >> 8));
        writeByte(static_cast<uint8_t>(v));
    }
    void SimFlash::writeInt(uint32_t v)
    {
        writeWord(static_cast<uint16_t>(v >> 16));
        writeWord(static_cast<uint16_t>(v));
    }
    uint8_t SimFlash::readByte()
    {
        if (readPtr == nullptr || readPtr >= readEnd)
            return 0;
        return *readPtr++;
    }
    uint16_t SimFlash::readWord()
    {
        const uint16_t hi = readByte();
        return static_cast<uint16_t>((hi << 8) | readByte());
    }
    uint32_t SimFlash::readInt()
    {
        const uint32_t hi = readWord();
        return (hi << 16) | readWord();
    }

    // ---------------------------------------------------------------- GPIO

    void SimGpio::pinMode(uint16_t pin, int mode, bool preset, int status)
    {
        if (pin >= sim::kPins)
            return;
        // Wie DriverEmbedded: bei preset erst den Pegel setzen, dann die
        // Richtung - ein Ausgang zuckt beim Umschalten nicht.
        if (preset)
            sim::pins[pin].level = status != 0;
        sim::pins[pin].mode = mode;
    }

    void SimGpio::digitalWrite(uint16_t pin, int status)
    {
        if (pin >= sim::kPins)
            return;
        sim::pins[pin].level = status != 0;
        sim::pins[pin].writes++;
    }

    bool SimGpio::digitalRead(uint16_t pin) { return sim::readPin(pin); }

    // ---------------------------------------------------------------- LED

    Led::FunctionGroup *SimLedFunctions::get(uint32_t id)
    {
        for (auto *g : mGroups)
            if (g->id() == id)
                return g;
        mGroups.push_back(new Led::FunctionGroup(id));
        return mGroups.back();
    }

    std::vector<Led::FunctionGroup *> SimLedFunctions::all() { return mGroups; }

    void SimLedFunctions::simReset()
    {
        for (auto *g : mGroups)
            g->simReset();
    }
} // namespace OpenKNX
