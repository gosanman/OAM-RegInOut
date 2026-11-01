# Release-Paket RegInOut, nach der Vorlage
# lib/OGM-Common/scripts/setup/templates/Build-Release.ps1.
#
#   scripts/Build-Release.ps1 Release   - Release-Paket
#   scripts/Build-Release.ps1           - Beta-Paket
#
# Ergebnis: release/RegInOut-<Release|Beta>-<Version>.zip mit knxprod-Daten,
# Firmware (.uf2), Upload-Skripten und Readme. Beide Pakete enthalten die
# Release-Firmware (Watchdog an, Debug aus). Die Produktdefinition kommt beim
# Release aus src/RegInOut-Release.xml, bei Beta aus src/RegInOut.xml mit "(Dev)"
# im Namen (scripts/OpenKNX-Build-Settings.ps1).
# Der Ordner release/ wird vorher geleert (steht in .gitignore).
#
# Die Vorbereitung schreibt include/knxprod.h aus der gebauten Definition. Nach
# einem Release-Paket traegt die naechste develop-Firmware deshalb den Namen ohne
# "(Dev)", bis "OpenKNXproducer" wieder laeuft. Die ETS-ID ist dieselbe.
#
# Abweichung von der OGM-Common-Vorlage: Build-Release-Postprocess.ps1 loescht
# dependencies.txt und schreibt restore/ neu. Beides ist eingecheckt. Deshalb wird
# es vor dem Nachlauf gesichert und danach unveraendert zurueckgelegt: ein
# Release-Build aendert nichts im Repository.

$keep = @("dependencies.txt", "restore/Restore-Dependencies.ps1", "restore/Restore-Dependencies-Branch.ps1", "restore/Restore-Dependencies-LinksOnly.ps1")
$backup = Join-Path ([System.IO.Path]::GetTempPath()) "reginout-release-keep"
function Save-Kept {
  if (Test-Path $backup) { Remove-Item -Recurse -Force $backup }
  foreach ($f in $keep) {
    if (Test-Path $f) {
      $target = Join-Path $backup $f
      New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
      Copy-Item $f $target
    }
  }
}
function Restore-Kept {
  foreach ($f in $keep) {
    $source = Join-Path $backup $f
    if (Test-Path $source) { Copy-Item -Force $source $f }
  }
}

# knxprod und Vorbereitung
lib/OGM-Common/scripts/setup/reusable/Build-Release-Preprocess.ps1 $args[0]
if (!$?) { exit 1 }

# Firmware REG1 mit RP2040, KNX-TP
lib/OGM-Common/scripts/setup/reusable/Build-Step.ps1 release_REG1_IO firmware-REG1-IO uf2
if (!$?) { exit 1 }

# Paket schnueren
Save-Kept
lib/OGM-Common/scripts/setup/reusable/Build-Release-Postprocess.ps1 $args[0]
$ok = $?
Restore-Kept
if (!$ok) { exit 1 }
