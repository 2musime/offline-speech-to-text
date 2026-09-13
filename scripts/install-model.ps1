<#
.SYNOPSIS
    Downloads a Whisper model into a directory the application will accept.

.DESCRIPTION
    This is the only step that touches the network, and it is deliberately
    separate from transcription, which never does.

    Models come from the whisper.cpp project's own distribution at
    https://huggingface.co/ggerganov/whisper.cpp

.PARAMETER Model
    base.en (default), small.en, tiny.en, medium.en or large-v3.

.PARAMETER Destination
    Where to put it. Defaults to %LOCALAPPDATA%\audio-to-text\models, which is
    one of the directories the application accepts a model from.

.EXAMPLE
    .\install-model.ps1 base.en

.EXAMPLE
    .\install-model.ps1 small.en -Destination D:\models
#>
[CmdletBinding()]
param(
    [ValidateSet('tiny.en', 'base.en', 'small.en', 'medium.en', 'large-v3')]
    [string]$Model = 'base.en',

    [string]$Destination
)

$ErrorActionPreference = 'Stop'

if (-not $Destination) {
    if (-not $env:LOCALAPPDATA) {
        throw 'LOCALAPPDATA is not set. Pass -Destination to say where the model should go.'
    }
    $Destination = Join-Path $env:LOCALAPPDATA 'audio-to-text\models'
}

New-Item -ItemType Directory -Force -Path $Destination | Out-Null

$source = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-$Model.bin"
$file = Join-Path $Destination "ggml-$Model.bin"
$partial = "$file.part"

Write-Host "Downloading $Model into $Destination"

# Invoke-WebRequest renders a progress bar by redrawing the console on every
# chunk, which on Windows PowerShell costs more time than the download itself.
$previousProgress = $ProgressPreference
$ProgressPreference = 'SilentlyContinue'
try {
    if (Test-Path $partial) { Remove-Item $partial -Force }
    # Written to a partial file first, so an interrupted download cannot be
    # mistaken for a model, and an existing good model survives a failure.
    Invoke-WebRequest -Uri $source -OutFile $partial -UseBasicParsing
    Move-Item -Path $partial -Destination $file -Force
}
catch {
    if (Test-Path $partial) { Remove-Item $partial -Force }
    throw "Download failed: $($_.Exception.Message)"
}
finally {
    $ProgressPreference = $previousProgress
}

if (-not (Test-Path $file)) {
    throw "Download did not produce $file"
}

$size = '{0:N0} MB' -f ((Get-Item $file).Length / 1MB)
$hash = (Get-FileHash -Path $file -Algorithm SHA256).Hash.ToLower()

Write-Host ''
Write-Host "Installed: $file"
Write-Host "Size:      $size"
Write-Host "SHA-256:   $hash"
Write-Host ''
Write-Host 'Record that checksum now and compare it later to detect a changed file.'
Write-Host ''
Write-Host 'Verify it loads:'
Write-Host "  audio_to_text_cli.exe `"$file`" --model-dir `"$Destination`" --version"
