# Produktnamen fuer den Release-Build, nach der Vorlage
# lib/OGM-Common/scripts/setup/templates/OpenKNX-Build-Settings.ps1.
#
#   sourceName  "RegInOut"  - Name der Applikation im Projekt
#   targetName  "RegInOut"  - Name im Release-Paket (RegInOut-Release-x.y.z.zip)
#
# Welche Produktdefinition gebaut wird, folgt der OpenKNX-Regel:
#   Release  -> src/RegInOut-Release.xml  (ohne BuildSuffix, Name ohne "(Dev)")
#   Beta     -> src/RegInOut.xml          (BuildSuffix "-dev", Name mit "(Dev)")
# Beide haben dieselbe ApplicationNumber und -Version, also dieselbe ETS-ID.
$settings = lib/OGM-Common/scripts/build/OpenKNX-Build-Settings.ps1 $args[0] "RegInOut" "RegInOut"
Return $settings
