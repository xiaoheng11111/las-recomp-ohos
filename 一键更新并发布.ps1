# 一键：提交并推送仓库 + 生成 FFmpeg 包 + 创建/更新 GitHub Release（含截图）
# 需要 PowerShell 7（双击即可；不含任何密码）
$ErrorActionPreference = 'Stop'
$repoDir = $PSScriptRoot
Set-Location $repoDir

function Say($msg, $color = 'Cyan') { Write-Host $msg -ForegroundColor $color }
function Pause-Exit { Read-Host '按回车退出' | Out-Null; exit }

if ($PSVersionTable.PSVersion.Major -lt 7) {
    Say '[!] 建议用 PowerShell 7 运行（双击本文件即可）' 'Yellow'
}
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Say '未找到 git' 'Red'; Pause-Exit }

$origin = git remote get-url origin 2>$null
if ([string]::IsNullOrWhiteSpace($origin)) {
    Say '还没有 origin 远程仓库。请先运行 publish-ohos.ps1 选 2 完成首次上传，再来运行本脚本。' 'Red'
    Pause-Exit
}

# ---------- 代理检测（git 不读 Windows 系统代理，需要显式设置） ----------
function Test-GitHub443 {
    $c = New-Object Net.Sockets.TcpClient
    try {
        $r = $c.BeginConnect('github.com', 443, $null, $null)
        if ($r.AsyncWaitHandle.WaitOne(4000)) { $c.EndConnect($r); return $true }
    } catch { } finally { $c.Close() }
    return $false
}
$proxy = ''
$ie = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings' -ErrorAction SilentlyContinue
if ($ie -and $ie.ProxyEnable -eq 1 -and $ie.ProxyServer) {
    $p = [string]$ie.ProxyServer
    if ($p -notmatch '^https?://') { $p = 'http://' + $p }
    $proxy = $p
}
if (-not $proxy -and -not (Test-GitHub443)) {
    Say '检测到无法直连 github.com:443。' 'Yellow'
    $proxy = Read-Host '请输入代理地址（例如 http://127.0.0.1:7877；直接回车跳过）'
}
if ($proxy) {
    $env:HTTP_PROXY = $proxy
    $env:HTTPS_PROXY = $proxy
    Say "使用代理：$proxy" 'Yellow'
}

# ---------- 1. 提交并推送仓库（截图/文档更新） ----------
$dirty = git status --porcelain
if ($dirty) {
    Say '检测到本地改动，正在提交…'
    git add -A
    git commit -m ("update: docs/screenshots " + (Get-Date -Format 'yyyy-MM-dd HH:mm'))
}
Say '正在推送到 GitHub…'
if ($proxy) {
    $out = git -c "http.proxy=$proxy" -c "https.proxy=$proxy" push origin main 2>&1
} else {
    $out = git push origin main 2>&1
}
$out | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) {
    $txt = ($out | Out-String)
    if ($txt -match 'Could not connect|Failed to connect|curl 28|Connection timed out|timed out|hung up') {
        Say '推送失败：无法连接 github.com:443。请确认代理/VPN 已开启（脚本已尝试系统代理）。' 'Red'
    } else {
        Say '推送失败：可能是登录过期或权限问题。若弹出登录窗口，请用浏览器登录后重试。' 'Red'
    }
    Pause-Exit
}

# ---------- 2. 生成 FFmpeg 8.1.2 包 ----------
$ffDefault = Join-Path (Split-Path $repoDir -Parent) 'repo\letsgo-recomp-android-master\platform\ohos\ffmpeg-arm64'
$ffDir = $ffDefault
if (-not (Test-Path (Join-Path $ffDir 'lib'))) {
    Say "默认 FFmpeg 目录不存在：$ffDefault" 'Yellow'
    $ffDir = Read-Host '请输入 FFmpeg arm64 目录（里面应有 lib/ 和 include/；回车退出）'
    if ([string]::IsNullOrWhiteSpace($ffDir) -or -not (Test-Path (Join-Path $ffDir 'lib'))) {
        Say '找不到 FFmpeg lib 目录。' 'Red'; Pause-Exit
    }
}
$licDir = Join-Path $repoDir 'third_party\ffmpeg'
if (-not (Test-Path $licDir)) { Say '缺少 third_party/ffmpeg 许可证目录。' 'Red'; Pause-Exit }

Say '正在打包 FFmpeg（lib + include + licenses + SOURCE/SHA256）…'
$pkgRoot = Join-Path $env:TEMP ('ffmpeg-pkg-' + [guid]::NewGuid().ToString('N'))
$pkg = Join-Path $pkgRoot 'ffmpeg-ohos-universal-8.1.2'
New-Item -ItemType Directory -Path (Join-Path $pkg 'lib'), (Join-Path $pkg 'include'), (Join-Path $pkg 'licenses') -Force | Out-Null
Copy-Item (Join-Path $ffDir 'lib\*.a') (Join-Path $pkg 'lib') -Force
Copy-Item (Join-Path $ffDir 'include\*') (Join-Path $pkg 'include') -Recurse -Force
Copy-Item (Join-Path $licDir '*') (Join-Path $pkg 'licenses') -Force
@'
FFmpeg 8.1.2 — OpenHarmony (OHOS) universal prebuilt static libraries
====================================================================
THIRD-PARTY build outputs, not code authored by the launcher project.

Source / build project : https://github.com/debugly/MRFFToolChainBuildShell
FFmpeg version          : 8.1.2
FFmpeg source           : https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz
Target                  : aarch64-linux-ohos (arm64)
License                 : LGPL-2.1-or-later (see licenses/COPYING.LGPLv2.1)
                          If GPL components were enabled, GPL v2/v3 applies.

Libraries: libavcodec libavformat libavutil libswscale libswresample libavfilter libavdevice
           libdav1d libopus libxml2 libbluray libSoundTouch libuavs3d libcrypto libssl

Keep this SOURCE.txt and licenses/ with any redistribution.
'@ | Set-Content -LiteralPath (Join-Path $pkg 'SOURCE.txt') -Encoding utf8
Get-ChildItem (Join-Path $pkg 'lib\*.a') | Get-FileHash -Algorithm SHA256 |
    ForEach-Object { "{0}  {1}" -f $_.Hash.ToLower(), (Split-Path $_.Path -Leaf) } |
    Set-Content -LiteralPath (Join-Path $pkg 'SHA256SUMS') -Encoding ascii

$dist = Join-Path $repoDir 'dist'
New-Item -ItemType Directory -Path $dist -Force | Out-Null
$zip = Join-Path $dist 'ffmpeg-ohos-universal-8.1.2.zip'
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($pkgRoot, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $true)
Remove-Item $pkgRoot -Recurse -Force -ErrorAction SilentlyContinue
Say ("FFmpeg 包已生成：" + $zip + "  (" + [math]::Round((Get-Item $zip).Length/1MB,2) + " MB)") 'Green'

# ---------- 3. GitHub CLI 认证 ----------
$gh = 'C:\Program Files\GitHub CLI\gh.exe'
if (-not (Test-Path $gh)) { $gh = (Get-Command gh -ErrorAction SilentlyContinue).Source }
if (-not $gh) { Say '未找到 GitHub CLI（gh）。请安装后重试，或在 GitHub 网页手动上传 Release 附件。' 'Red'; Pause-Exit }

& $gh auth status 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    Say '需要登录 GitHub CLI。浏览器会打开，复制屏幕显示的一次性代码并授权即可。' 'Yellow'
    & $gh auth login --hostname github.com --git-protocol https --web
    if ($LASTEXITCODE -ne 0) { Say 'GitHub CLI 登录失败。' 'Red'; Pause-Exit }
}
$fullName = (& $gh repo view --json nameWithOwner -q .nameWithOwner) 2>$null
if ([string]::IsNullOrWhiteSpace($fullName)) { Say '无法识别当前仓库（origin）。' 'Red'; Pause-Exit }

# ---------- 4. 创建/更新 Release ----------
$tag = 'v0.1.0'
$notesPath = Join-Path $dist 'release-notes.md'
@'
# HarmonyOS Let's Go 移植 v0.1.0

- 源码（不含游戏数据、重编译游戏代码）
- 新截图：封面 / 启动开场 / 游戏内 / 户外植被场景（示例 18 fps / 104 ms）
- 修复文字不可见：完整字体指向 + 游戏语言（详见 README）
- 附件：FFmpeg 8.1.2 OHOS universal 预编译库（第三方，来源 debugly/MRFFToolChainBuildShell，LGPL；见包内 licenses/SOURCE.txt）

> 游戏本体/数据/重编译游戏代码不随本项目分发。
'@ | Set-Content -LiteralPath $notesPath -Encoding utf8

$tagExists = git tag -l $tag
if (-not $tagExists) {
    git tag -a $tag -m "v0.1.0"
    git push origin $tag
}
$assets = @($zip)
$assets += (Get-ChildItem (Join-Path $repoDir 'screenshots\*.jpeg') | ForEach-Object { $_.FullName })

& $gh release view $tag 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) {
    Say "Release $tag 已存在，正在覆盖上传附件…"
    & $gh release upload $tag @assets --clobber
} else {
    Say "正在创建 Release $tag…"
    & $gh release create $tag @assets --title "HarmonyOS Let's Go 移植 v0.1.0" --notes-file $notesPath
}
if ($LASTEXITCODE -eq 0) {
    Say "完成：https://github.com/$fullName/releases/tag/$tag" 'Green'
    Start-Process "https://github.com/$fullName/releases/tag/$tag"
} else {
    Say 'Release 创建/上传失败。也可以打开仓库 Release 页面手动拖入 dist 里的 zip。' 'Red'
}
Say ''
Pause-Exit
