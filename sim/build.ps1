# ============================================================================
# Simulator bauen: Kern (iosim.exe) und Metadaten (meta.json).
#
# Voraussetzungen:
#   - include/knxprod.h und src/RegInOut.debug.xml existieren, also
#       OpenKNXproducer create --Debug -h include/knxprod.h src/RegInOut
#     ist gelaufen,
#   - include/versions.h existiert, also die Firmware wurde einmal gebaut.
# Alle drei sind generiert und nicht im Repository.
#
# Aufruf aus dem Projektordner:  pwsh -File sim/build.ps1
# ============================================================================
param(
    # Anderer Name fuer den Kern, z. B. wenn die App laeuft und iosim.exe sperrt:
    #   pwsh -File sim/build.ps1 -Exe iosim_test.exe
    # Die App nimmt ihn mit der Umgebungsvariable IOSIM_EXE.
    [string]$Exe = 'iosim.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$sim = $PSScriptRoot
$out = Join-Path $sim 'build'
New-Item -ItemType Directory -Force $out | Out-Null

foreach ($f in @('include/knxprod.h', 'src/RegInOut.debug.xml')) {
    if (-not (Test-Path (Join-Path $root $f))) {
        throw "$f fehlt - zuerst den OpenKNXproducer laufen lassen (siehe Kopf dieser Datei)."
    }
}
if (-not (Test-Path (Join-Path $root 'include/versions.h'))) {
    throw 'include/versions.h fehlt - zuerst einmal die Firmware bauen (Task "Firmware bauen").'
}

# --- Compiler: MinGW-GCC aus winget, sonst g++ im PATH ----------------------
$gpp = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter g++.exe -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $gpp) { $gpp = (Get-Command g++ -ErrorAction SilentlyContinue).Source }
if (-not $gpp) { throw 'Kein g++ gefunden (winget install BrechtSanders.WinLibs.POSIX.UCRT).' }

$bi = Join-Path $root 'lib/OFM-BinaryInput/src'
$swa = Join-Path $root 'lib/OFM-SwitchActuator/src'
$knx = Join-Path $root 'lib/knx/src/knx'

# Firmware-Quellen der beiden Module - UNVERAENDERT. Logikmodul und
# FileTransfer baut der Simulator nicht.
$modSources = @(
    (Join-Path $bi 'BinaryInputChannel.cpp'), (Join-Path $bi 'GpioBinaryInputModule.cpp'),
    (Join-Path $swa 'SwitchActuatorChannel.cpp'), (Join-Path $swa 'SwitchActuatorModule.cpp'))
$knxSources = @('dpt', 'dptconvert', 'knx_value', 'bits') | ForEach-Object { Join-Path $knx "$_.cpp" }
$simSources = Get-ChildItem (Join-Path $sim 'core') -Filter *.cpp | ForEach-Object FullName

$gppArgs = @('-std=gnu++17', '-O2', '-static', '-Wall',
    # aus unveraendertem Fremdcode (lib/knx): ungenutzte statische Funktionen
    '-Wno-unused-function',
    # aus unveraendertem Fremdcode (OFM-BinaryInput): Abfrage "_gpioPins == nullptr"
    # auf ein Array, das nie NULL ist - harmlos, nur toter Code
    '-Wno-address',
    # wie platformio.custom.ini
    '-DOPENKNX_LOOPTIME_WARNING=100',
    '-DIO_SIMULATOR', '-D_POSIX_THREAD_SAFE_FUNCTIONS',
    # gmtime_r fuer knx_value.cpp unter MinGW
    "-I$(Join-Path $sim 'core')", "-I$(Join-Path $root 'include')", "-I$bi", "-I$swa", "-I$knx",
    '-o', (Join-Path $out $Exe)) + $simSources + $modSources + $knxSources

Write-Host "g++: $gpp"
& $gpp @gppArgs
if ($LASTEXITCODE -ne 0) { throw "Uebersetzen fehlgeschlagen ($LASTEXITCODE)" }
Write-Host "Kern gebaut: $(Join-Path $out $Exe)"

# --- Metadaten aus der ETS-Applikation ----------------------------------------
$py = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
if (-not (Test-Path $py)) { $py = 'python' }
& $py (Join-Path $sim 'gen_meta.py') (Join-Path $root 'src/RegInOut.debug.xml') (Join-Path $out 'meta.json')
if ($LASTEXITCODE -ne 0) { throw "Metadaten fehlgeschlagen ($LASTEXITCODE)" }
