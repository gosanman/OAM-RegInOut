# OAM-RegInOut

OpenKNX-Firmware für ein REG-Gerät auf Basis der REG1-Platine (RP2040, KNX-TP):

- 2 Binäreingänge (OFM-BinaryInput)
- 2 monostabile Relais (OFM-SwitchActuator)
- Logikmodul, FileTransfer, Konfigurationstransfer

Pins und Gerätekennung stehen in `include/hardware.h`.

## Bauen

Die Abhängigkeiten liegen als Geschwister-Repos neben diesem Repo und sind unter
`lib/` verlinkt; der Stand steht in `dependencies.txt`
(`restore/Restore-Dependencies.ps1` stellt ihn her).

| Schritt | VS-Code-Task |
|---|---|
| ETS-Applikation (`src/RegInOut.knxprod`, `include/knxprod.h`) | OpenKNXproducer |
| Firmware | Firmware bauen (`develop_REG1_IO`) |
| beides nacheinander | knxprod + Firmware |
| Release-Paket | Build-Release |

`src/RegInOut.xml` ist die Entwicklungsfassung, `src/RegInOut-Release.xml` die
Release-Fassung mit derselben ETS-ID.

## Simulator

`sim/` übersetzt den Code der Binäreingänge und des Schaltaktors für den PC und
bietet eine Browser-Oberfläche mit ETS-Ansicht und einen Selbsttest. Bedienung
und Grenzen: [sim/README.md](sim/README.md).
