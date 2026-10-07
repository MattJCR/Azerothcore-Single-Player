# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
# tools/revisar-actualizaciones.ps1 — ¿hay versiones nuevas río arriba?, desde
# Windows. Sube tools/revisar-actualizaciones.sh al instalador de la VM
# (~/azerothcore-installer/tools/) y lo ejecuta por plink (PuTTY). En el
# servidor sólo hace `git fetch` y escribe el aviso de mod-update-notice
# (env/dist/bin/updates-pending.txt); no actualiza nada.
#
# Uso (desde cualquier PowerShell):
#   & ".\tools\revisar-actualizaciones.ps1"
#   & "...\revisar-actualizaciones.ps1" -SoloNovedades        # sólo los repos con commits nuevos
#   & "...\revisar-actualizaciones.ps1" -SinFichero           # no escribir el aviso de mod-update-notice
#   & "...\revisar-actualizaciones.ps1" -Log mod-transmog     # los commits nuevos de ese repo y los ficheros tocados
#
# Para actualizar de verdad, después de leer los cambios (en la VM):
#   ./install.sh --only 3 ; sudo systemctl stop ac-worldserver ; ./install.sh --from 4 ; ./install.sh --freeze
#
# Conexión a la VM: variables de entorno AC_VM_HOST, AC_VM_USER (por defecto
# acore) y AC_VM_PASS, o los parámetros -Host_, -Usuario y -Pass. Sin contraseña
# se pide por teclado. No hay valores por defecto en este fichero.
# =============================================================================
param(
    [switch]$SoloNovedades,
    [switch]$SinFichero,
    [string]$Log = "",
    [string]$Host_ = $env:AC_VM_HOST,
    [string]$Usuario = $(if ($env:AC_VM_USER) { $env:AC_VM_USER } else { "acore" }),
    [string]$Pass = $env:AC_VM_PASS
)

if (-not $Host_) { Write-Error "Falta la IP de la VM: define AC_VM_HOST o usa -Host_."; exit 1 }
if (-not $Pass) {
    $segura = Read-Host "Contraseña de $Usuario@$Host_" -AsSecureString
    $Pass = [Runtime.InteropServices.Marshal]::PtrToStringBSTR([Runtime.InteropServices.Marshal]::SecureStringToBSTR($segura))
}

$putty = "C:\Program Files\PuTTY"
$sh = Join-Path $PSScriptRoot "revisar-actualizaciones.sh"
if (-not (Test-Path $sh)) { Write-Error "No encuentro $sh"; exit 1 }

# LF y UTF-8 sin BOM, que bash no traga ni CRLF ni el BOM en la primera línea
$tmp = Join-Path $env:TEMP "revisar-actualizaciones.sh"
$texto = (Get-Content $sh -Raw) -replace "`r`n", "`n"
[System.IO.File]::WriteAllText($tmp, $texto, (New-Object System.Text.UTF8Encoding($false)))
try { chcp 65001 | Out-Null } catch {}
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$args_ = @()
if ($SoloNovedades) { $args_ += "--solo-novedades" }
if ($SinFichero)    { $args_ += "--sin-fichero" }
if ($Log -ne "")    { $args_ += "--log"; $args_ += $Log }
$argumentos = ($args_ -join " ")

# El script vive dentro del instalador de la VM: necesita config.sh, lib/ y versions.lock de ahí
& "$putty\pscp.exe" -batch -pw $Pass $tmp "$Usuario@${Host_}:/home/acore/azerothcore-installer/tools/revisar-actualizaciones.sh" | Out-Null
& "$putty\plink.exe" -batch -ssh "$Usuario@$Host_" -pw $Pass "cd /home/acore/azerothcore-installer && bash tools/revisar-actualizaciones.sh $argumentos"
