# Installs ready-to-use neural voice models for DyVocalizer.
# Creates %APPDATA%\DyVocalizer\models and downloads:
#   content.onnx       - ContentVec-768 content encoder (vec-768-layer-12)
#   voice_HuTao.onnx   - BASE voice (RVC v2 48k, bundled with the project)
#   voice_woman1.onnx  - RVC v2 40k female voice (w-okada realtime convention)
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\install_models.ps1
# After it finishes, restart FL Studio (or reload the plugin) - the neural
# engine picks the models up automatically.

$ErrorActionPreference = "Stop"

$dst = Join-Path $env:APPDATA "DyVocalizer\models"
New-Item -ItemType Directory -Force -Path $dst | Out-Null
Write-Host "Model folder: $dst"

$base = "https://github.com/saikodev-ru/voicefem/releases/download/models-v1"
$files = @(
    @{ Name = "content.onnx";     Url = "https://huggingface.co/ozada/onnx_rvc/resolve/main/vec-768-layer-12.onnx"; MinMB = 300 },
    @{ Name = "voice_woman1.onnx"; Url = "https://huggingface.co/ozada/onnx_rvc/resolve/main/woman_1.onnx";         MinMB = 90 },
    @{ Name = "voice_woman2.onnx"; Url = "https://huggingface.co/ozada/onnx_rvc/resolve/main/woman_2.onnx";         MinMB = 90 }
)

foreach ($f in $files) {
    $out = Join-Path $dst $f.Name
    if ((Test-Path $out) -and ((Get-Item $out).Length -gt $f.MinMB * 1MB)) {
        Write-Host "SKIP  $($f.Name) (already present)"
        continue
    }
    Write-Host "GET   $($f.Name) ..."
    & curl.exe -L --fail --retry 3 -o "$out.part" $f.Url
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "download failed: $($f.Url) (skipping)"
        Remove-Item -ErrorAction SilentlyContinue "$out.part"
        continue
    }
    $size = (Get-Item "$out.part").Length
    if ($size -lt $f.MinMB * 1MB) {
        Write-Warning "downloaded file too small: $($f.Name) ($size bytes), skipping"
        Remove-Item -ErrorAction SilentlyContinue "$out.part"
        continue
    }
    Move-Item -Force "$out.part" $out
    Write-Host ("OK    {0}  ({1:N1} MB)" -f $f.Name, ($size / 1MB))
}

# base voice HuTao is added to the release once the source .pth (Yandex.Disk)
# becomes downloadable again; it is optional until then
$hutao = Join-Path $dst "voice_HuTao.onnx"
if (-not (Test-Path $hutao)) {
    Write-Host "GET   voice_HuTao.onnx (base voice) ..."
    & curl.exe -L --fail --retry 2 -o "$hutao.part" "$base/voice_HuTao.onnx" 2>$null
    if ($LASTEXITCODE -eq 0 -and (Get-Item "$hutao.part").Length -gt 90MB) {
        Move-Item -Force "$hutao.part" $hutao
        Write-Host ("OK    voice_HuTao.onnx  ({0:N1} MB)" -f ((Get-Item $hutao).Length / 1MB))
    } else {
        Write-Host "SKIP  voice_HuTao.onnx (not published yet - the other voices still work)"
        Remove-Item -ErrorAction SilentlyContinue "$hutao.part"
    }
}

Write-Host ""
Write-Host "Done. Open the plugin and select Engine = 'Neural (model)'."
Write-Host "Your own RVC .pth voices: drag the .pth onto the plugin window (or use Import model...),"
Write-Host "or convert manually:  python tools\pth_to_onnx.py voice.pth voice_xxx.onnx"
