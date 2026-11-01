# Simulator

Der Simulator führt den **unveränderten** Firmware-Code der Module
OFM-BinaryInput und OFM-SwitchActuator auf dem PC aus. Ersetzt wird nur, was die
Firmware von außen aufruft: OpenKNX (Logger, Konsole, Flash, GPIO,
LED-Funktionen), der KNX-Stack (Parameterspeicher, Gruppenobjekte) und das
Board (Kontakte an den Eingängen, Relais, Versorgung). Die DPT-Kodierung ist die
echte aus `lib/knx`.

## Starten

1. ETS-Applikation erzeugen (Task „OpenKNXproducer“) und die Firmware einmal
   bauen (Task „Firmware bauen“): Der Simulator braucht `include/knxprod.h`,
   `include/versions.h` und `src/RegInOut.debug.xml`.
2. Task „Simulator bauen“ oder `pwsh -File sim/build.ps1`. Gebraucht wird ein
   MinGW-g++ (`winget install BrechtSanders.WinLibs.POSIX.UCRT`).
3. Task „Simulator starten“ oder
   `~/.platformio/penv/Scripts/python.exe sim/iosim.py`. Die Oberfläche öffnet
   sich unter <http://127.0.0.1:8767>.
   Optionen: `--port`, `--no-browser`, `--project "<Name>"`.
4. Task „Simulator-Selbsttest“ oder `python sim/selftest.py`: alle Abläufe ohne
   Oberfläche, am Ende „ALLES OK“ oder die Liste der Fehlschläge.

## Bedienung

- **Kopfleiste:** Simulationszeit (anhalten, 1× bis 600×, Einzelschritte),
  KNX-Bus und Relaisversorgung schalten, Busdip, Programmieren (ETS-Download mit
  Neustart), Neustart, Entladen, Funktionstaste, Projekte laden und speichern.
- **Rechte Spalte:** je Binäreingang der Kontakt (Klick schaltet ihn), der Pegel
  am Pin und der Zustand in der Firmware; je Relais die Spule, das Statusobjekt
  und laufende Verzögerungen. Darunter der Verlauf der Relais.
- **Bedienung:** die sichtbaren Eingangsobjekte je Kanal zum Senden, die
  Ausgangsobjekte mit ihrem letzten Wert.
- **Parameter:** die ETS-Ansicht der Kanäle OpenKNX, Binäreingänge und
  Schaltaktor. Änderungen wirken erst nach „Programmieren“.
- **Objekte, Telegramme, Konsole:** alle sichtbaren Objekte, jedes Telegramm mit
  Rohdaten, die Konsolenbefehle der Module (`sa switch a 1`, `sa test mode` …).
- **Hardware:** Relaisversorgung, Dauer des Busdips, Pins mit Modus und Pegel.
- **Prüfung:** Meldungen des Wächters, Neustarts, Downloads, Busdips.

Projekte liegen in `sim/projects/*.json` (geänderte Parameter und
Hardware-Einstellungen). Laden heißt „so programmiert und eingeschaltet“.

## Aufbau

```
sim/
  build.ps1        Kern übersetzen, gen_meta.py aufrufen
  gen_meta.py      src/RegInOut.debug.xml (+ .knxprod) -> build/meta.json, build/baggage/
  iocore.py        ParamImage (ETS-Abbild), Core (Kern-Prozess), ETS-Sichtbarkeit
  iosim.py         App: HTTP-Server, Simulationszeit, Projekte, Aktionen
  selftest.py      Abläufe ohne Oberfläche
  projects/        mitgelieferte Projekte
  core/
    OpenKNX.h      Ersatz für OGM-Common und KNX-Stack, soweit die Module ihn nutzen
    OpenKNX/Module.h   leitet auf OpenKNX.h um
    sim.h          gemeinsamer Zustand: Zeit, Parameter, Objekte, Flash, Board
    sim_runtime.cpp    Parameterspeicher, Gruppenobjekte, Logger, Flash, GPIO, LED-Funktionen
    sim_board.cpp      Board-Modell: Pins, Kontakte, monostabile Relais
    sim_inspect.cpp    Zustand als JSON, Wächter
    sim_main.cpp       Befehlsschleife, Start, ETS-Download, Bus, Busdip
    sim_platform.cpp   print()-Stubs für lib/knx/bits.cpp
  web/             Oberfläche (app.js, ets.js für die ETS-Ansicht)
```

Der Kern liest Befehle zeilenweise von stdin und schreibt Ereignisse als
JSON-Zeilen nach stdout; jeder Befehl endet mit `{"ev":"ok"}`. Die Befehle
stehen im Kopf von `core/sim_main.cpp`. Ohne App:
`sim/build/iosim.exe < szenario.txt`.

Die Module laufen in derselben Reihenfolge wie in `src/main.cpp`
(BinaryInput, SwitchActuator).

## Modell

- **Prozessor:** läuft nur mit KNX-Bus. Ein Neustart setzt RAM und Pins zurück
  (alle Pins hochohmig); Parameter und Flash bleiben.
- **Relais:** monostabil. Die Spule ist bestromt, solange der Prozessor den Pin
  als Ausgang auf `OPENKNX_SWA_ACTIVE_ON` treibt und die Relaisversorgung
  anliegt. Ohne Bus fallen deshalb alle Relais ab, unabhängig von „bei
  Busspannungsverlust“.
- **Binäreingänge:** ein geschlossener Kontakt zieht den Pin auf
  `OPENKNX_BI_ONLEVEL`, offen liegt der Pull-up an.
- **Busdip:** SAVE (`savePower()`, Flash sichern), nach 1 s `restorePower()`
  ohne Neustart, wie OpenKNX Common.
- **`delay()`:** die Simulationszeit läuft weiter, der Prozessor steht. Ab
  100 ms meldet der Wächter eine Blockierung.

Der Wächter prüft nach jedem Durchlauf:

- Das Statusobjekt eines aktiven Relais passt zur Spule (beim Öffner invertiert).
- Unparametriert ist kein Relais angezogen.
- Die Firmware blockiert nicht mit `delay()`.
- Jeder gesendete oder gelesene DPT ist kodierbar.

## Grenzen

- Logikmodul, FileTransfer und Konfigurationstransfer laufen nicht mit; ihre
  Objekte und Parameter fehlen in der Oberfläche.
- Kein Gruppenadressen-Modell: Gesendet wird auch auf Objekte, die die ETS
  ausblendet (in „Telegramme“ markiert). Am Gerät gehen sie nicht auf den Bus.
- Ein Neustart dauert im Simulator 0 ms. Am Gerät fallen die Relais während
  des Starts kurz ab; der Simulator meldet das als Hinweis, die Dauer ist nicht
  modelliert.
- Lesetelegramme (GroupValueRead) und die Zeit vom Bus werden nicht nachgebildet.
- Die Relaisversorgung ist als eigener Schalter modelliert; ob sie auf der
  Platine am KNX-Bus hängt, ist im Modell nicht festgelegt.
- Die Strommessung des Schaltaktors (BL0942) ist auf dieser Platine nicht
  bestückt und wird nicht übersetzt.
