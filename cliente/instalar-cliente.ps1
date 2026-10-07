<#
.SYNOPSIS
  Instala en un cliente WoW 3.3.5a todo lo de la carpeta cliente/ (parches y addons).

.DESCRIPTION
  Lee manifest.tsv (origen, destino, tipo, nota) y copia cada elemento sobre la
  carpeta del juego: los .MPQ se copian tal cual a Data/; los addons se
  sincronizan enteros (robocopy /MIR sobre SU carpeta, sin tocar el resto de
  AddOns/). Quita las copias sueltas de la antigua ARAC (Data/Patch-Arac.MPQ,
  Patch-X.MPQ, Patch-C.MPQ, Patch-A.MPQ): el Wow.exe nunca cargó ninguna de
  ellas (el cargador de Data/ exige un único carácter tras "patch-"; "Arac" no
  encaja) y desde el 21/09/2026 sus tres DBC van dentro de
  Data/<idioma>/patch-<idioma>-4.MPQ, que sí carga. Solo se borran si su MD5
  coincide con el de la ARAC vieja conocida (para no tocar un fichero de un
  jugador con ese mismo nombre por otro motivo). Con -Realm escribe "set
  realmlist <ip>" en el realmlist.wtf de cada idioma. Idempotente: se puede
  repetir.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\cliente\instalar-cliente.ps1 -Cliente "<cliente>" -Realm 192.168.1.100
#>
param(
    [Parameter(Mandatory = $true)]
    [string] $Cliente,
    [string] $Realm = "",
    [switch] $SoloMostrar
)

$ErrorActionPreference = "Stop"
$origenBase = Split-Path -Parent $MyInvocation.MyCommand.Path
$manifest = Join-Path $origenBase "manifest.tsv"

if (-not (Test-Path (Join-Path $Cliente "Wow.exe"))) {
    Write-Host "No encuentro Wow.exe en '$Cliente'. Indica la carpeta donde esta el juego." -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $manifest)) {
    Write-Host "No existe $manifest" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Cliente:  $Cliente"
Write-Host "Origen:   $origenBase"
if ($SoloMostrar) { Write-Host "(solo mostrar: no se copia nada)" -ForegroundColor Yellow }
Write-Host ""

$lineas = Get-Content $manifest -Encoding UTF8 | Select-Object -Skip 1 | Where-Object { $_.Trim() -ne "" }
$errores = 0
$mpqInstalado = $false

foreach ($linea in $lineas) {
    $campos = $linea -split "`t"
    if ($campos.Count -lt 3) { continue }
    $origen  = Join-Path $origenBase ($campos[0] -replace '/', '\')
    $destino = Join-Path $Cliente    ($campos[1] -replace '/', '\')
    $tipo    = $campos[2].Trim()
    $nota    = ""
    if ($campos.Count -ge 4) { $nota = $campos[3] }

    if (-not (Test-Path $origen)) {
        if ($campos[2].Trim() -eq "mpq") {
            # La edicion publica no lleva los MPQ (salen de TU cliente): los genera el panel.
            Write-Host "[FALTA]  $($campos[0]) no esta en cliente/. Se genera desde tu cliente en el panel (menu Addons) y se instala desde alli." -ForegroundColor Yellow
        } else {
            Write-Host "[FALTA]  $($campos[0]) no esta en cliente/ (se omite)" -ForegroundColor Yellow
        }
        continue
    }

    if ($tipo -eq "mpq") {
        Write-Host "[MPQ]    $($campos[1])  - $nota"
        if (-not $SoloMostrar) {
            $carpeta = Split-Path -Parent $destino
            if (-not (Test-Path $carpeta)) { New-Item -ItemType Directory -Force $carpeta | Out-Null }
            Copy-Item -Force $origen $destino
            $mpqInstalado = $true
        }
    }
    elseif ($tipo -eq "addon") {
        Write-Host "[ADDON]  $($campos[1])  - $nota"
        if (-not $SoloMostrar) {
            if (-not (Test-Path $destino)) { New-Item -ItemType Directory -Force $destino | Out-Null }
            # /MIR sincroniza SOLO la carpeta del addon; /NFL /NDL /NJH /NJS silencian el listado.
            & robocopy $origen $destino /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
            if ($LASTEXITCODE -ge 8) {
                Write-Host "         robocopy devolvio $LASTEXITCODE" -ForegroundColor Red
                $errores++
            }
        }
    }
    else {
        Write-Host "[?]      tipo desconocido '$tipo' en $linea" -ForegroundColor Yellow
    }
}

# Limpieza de la antigua ARAC suelta (Data/Patch-Arac.MPQ y las copias con
# otro nombre que se probaron antes, Patch-X/C/A.MPQ): el Wow.exe nunca las
# cargó (el cargador de Data/ exige un único carácter tras "patch-"; "Arac"
# no encaja) y desde el 21/09/2026 sus tres DBC van dentro de
# Data/<idioma>/patch-<idioma>-4.MPQ, que sí se carga (ver INSTALL_ES.md,
# parte 5). Solo se borran si su MD5 coincide con el de esa ARAC vieja
# conocida, para no tocar un archivo de otro origen que se llame igual.
$MD5_ARAC_VIEJA = "864C5D27E11D4F366DF8F9A6E42ECB42"
$dataDirLimpieza = Join-Path $Cliente "Data"
if (Test-Path $dataDirLimpieza) {
    foreach ($viejo in @("Patch-Arac.MPQ", "Patch-X.MPQ", "Patch-C.MPQ", "Patch-A.MPQ")) {
        $rutaVieja = Join-Path $dataDirLimpieza $viejo
        if (Test-Path $rutaVieja) {
            $md5Viejo = (Get-FileHash -Algorithm MD5 $rutaVieja).Hash
            if ($md5Viejo -eq $MD5_ARAC_VIEJA) {
                Write-Host "[LIMPIEZA] quitando la ARAC suelta $viejo (ya no se carga; sus DBC van en patch-<idioma>-4.MPQ)" -ForegroundColor DarkGray
                if (-not $SoloMostrar) {
                    try {
                        Remove-Item -Force $rutaVieja -ErrorAction Stop
                    } catch {
                        # Con el WoW abierto el fichero esta en uso; no molesta, nunca se cargo.
                        Write-Host "           no se pudo borrar $viejo (¿WoW abierto?). Cierra el juego y repite; mientras, no molesta (nunca se cargaba)." -ForegroundColor Yellow
                    }
                }
            } else {
                Write-Host "[AVISO]  $viejo no es la ARAC suelta conocida (MD5 distinto); no se toca." -ForegroundColor Yellow
            }
        }
    }
}

if ($Realm -ne "") {
    $dataDir = Join-Path $Cliente "Data"
    $escritos = 0
    Get-ChildItem -Path $dataDir -Directory | ForEach-Object {
        $rl = Join-Path $_.FullName "realmlist.wtf"
        if (Test-Path $rl) {
            Write-Host "[REALM]  $($_.Name)\realmlist.wtf -> set realmlist $Realm"
            if (-not $SoloMostrar) {
                Set-Content -Path $rl -Value "set realmlist $Realm" -Encoding ASCII
            }
            $escritos++
        }
    }
    if ($escritos -eq 0) {
        Write-Host "[REALM]  no hay ningun Data\<idioma>\realmlist.wtf que escribir" -ForegroundColor Yellow
    }
}

# Los parches cambian DBC. Si no se borra Cache/ (consultas cacheadas al
# servidor), el cliente sigue enseñando datos viejos (icono "?", nombres de
# otro servidor). Se borra sola tras instalar un MPQ, para no tener que hacerlo
# a mano.
if ($mpqInstalado -and -not $SoloMostrar) {
    $cache = Join-Path $Cliente "Cache"
    if (Test-Path $cache) {
        try {
            Remove-Item -Recurse -Force $cache -ErrorAction Stop
            Write-Host "[CACHE]  Cache\ borrada (se regenera al entrar)"
        } catch {
            Write-Host "[CACHE]  no se pudo borrar Cache\ (¿WoW abierto?). Cierra el juego y bórrala a mano." -ForegroundColor Yellow
        }
    }
}

Write-Host ""
if ($errores -eq 0) {
    Write-Host "Hecho. Reinicia el WoW. En AddOns: ServerHelp y MultiBot tienen que estar marcados." -ForegroundColor Green
} else {
    Write-Host "Terminado con $errores errores." -ForegroundColor Red
    exit 1
}
