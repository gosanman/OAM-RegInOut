# CLAUDE.md - OAM-RegInOut

Diese Datei gilt in jeder Sitzung. Lies sie, bevor du Code änderst.

## Was dieses Projekt ist

OpenKNX-Firmware für ein REG-Gerät auf Basis der REG1-Platine (RP2040, KNX-TP):

- 2 Binäreingänge (GPIO 16/17, aktiv LOW) über OFM-BinaryInput
- 2 monostabile Relais (GPIO 26/27, aktiv HIGH) über OFM-SwitchActuator
- Logikmodul, FileTransfer, ConfigTransfer

Das Gerät hat keine eigene Logik und keine eigenen Parameterseiten. Es setzt die
Module zusammen und legt die Hardware im Board-Header fest. Fehler im Verhalten
werden im jeweiligen OFM-Repo behoben, nicht hier.

## Invarianten

1. **Pins nur in `include/hardware.h`.** Das gilt auch für die OpenKNX-Systempins
   (KNX-UART, PROG, SAVE, LEDs, Funktionstaste).
2. **Monostabile Relais: Der Pegel ist der Zustand.** `OPENKNX_SWA_PINS` und
   `OPENKNX_SWA_ACTIVE_ON` wählen im Schaltaktor die Einzelpin-Ansteuerung. Die
   Defines für bistabile Relais (`OPENKNX_SWA_SET_PINS`/`_RESET_PINS`) dürfen nicht
   zusätzlich gesetzt sein, sonst bricht der Build mit `#error` ab.
3. **Sicherer Ausgangszustand beim Start: Relais aus.** `SwitchActuatorChannel::setup()`
   legt jeden Relais-Pin mit `!OPENKNX_SWA_ACTIVE_ON` als Ausgang an. Erst danach
   stellt `readFlash()` den zuletzt gespeicherten Zustand wieder her.
4. **Modulreihenfolge in `src/main.cpp` bleibt fest.** Sie bestimmt die
   Flash-Aufteilung (Logik 1, BinaryInput 2, SwitchActuator 3, FileTransfer 4).
5. **KO-Belegung ist im Kopf von `src/RegInOut.xml` festgehalten.** Neue Objekte
   nur in freien Bereichen; KoOffsets nicht verschieben.

## Build

```
~/bin/OpenKNXproducer.exe create --Debug -h include/knxprod.h src/RegInOut
pwsh -NoProfile -ExecutionPolicy Bypass -File lib/OGM-Common/scripts/build/OpenKNX-Build.ps1 -env develop_REG1_IO
pwsh -NoProfile -ExecutionPolicy Bypass -File sim/build.ps1
~/.platformio/penv/Scripts/python.exe sim/selftest.py
```

`src/RegInOut.xml` ist die Entwicklungsfassung (BuildSuffix "-dev"),
`src/RegInOut-Release.xml` die Release-Fassung mit derselben ETS-ID. Versionen
stehen in `src/RegInOut.conf.xml`.

## Was du nicht tust

- Kein Commit, Branch, Tag oder Push ohne ausdrückliche Aufforderung.
- Kein Flashen, kein Upload, keine Kommandos an die Hardware ohne Aufforderung in
  derselben Nachricht.
- Keine Änderung der Modulreihenfolge in `src/main.cpp`.
