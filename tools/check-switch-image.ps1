# 只读检查：Switch 游戏镜像（XCI/NSP/NCA/NSZ/XCZ）是否加密/是否已解密
# 不解密、不提取、不修改文件；只读取文件头和扫描魔数。
# 用法：双击运行后粘贴文件路径；或：
#   pwsh -File check-switch-image.ps1 "D:\xxx.xci" [-Full] [-Sha256]
param(
    [string]$Path,
    [int]$MaxScanMB = 64,
    [switch]$Full,
    [switch]$Sha256
)
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($Path)) {
    Write-Host '请把 XCI/NSP/NCA 文件路径粘贴到这里（可直接把文件拖进本窗口），回车：' -ForegroundColor Cyan
    $Path = Read-Host
}
$Path = $Path.Trim().Trim('"').Trim("'")
if (-not (Test-Path -LiteralPath $Path)) {
    Write-Host "文件不存在：$Path" -ForegroundColor Red
    Read-Host '按回车退出' | Out-Null
    exit 1
}
$fi = Get-Item -LiteralPath $Path
Write-Host ''
Write-Host '==== 镜像检查 ====' -ForegroundColor Cyan
Write-Host ("文件 : " + $fi.FullName)
Write-Host ("大小 : " + [math]::Round($fi.Length/1GB, 3) + " GB (" + $fi.Length + " 字节)")
Write-Host ("修改 : " + $fi.LastWriteTime)

$fs = [IO.File]::OpenRead($fi.FullName)
function ReadAt([int64]$off, [int]$n) {
    $fs.Position = $off
    $b = New-Object byte[] $n
    $got = 0
    while ($got -lt $n) {
        $r = $fs.Read($b, $got, $n - $got)
        if ($r -le 0) { break }
        $got += $r
    }
    return $b
}

try {
    $head = ReadAt 0 0x400
    $magic0 = [Text.Encoding]::ASCII.GetString($head, 0, 4)
    $magic100 = [Text.Encoding]::ASCII.GetString($head, 0x100, 4)

    $kind = '未知'
    if ($magic100 -eq 'HEAD') { $kind = 'XCI（卡带镜像）' }
    elseif ($magic0 -eq 'PFS0') { $kind = 'NSP（PFS0 包）' }
    elseif ($magic0 -eq 'NCA0' -or $magic0 -eq 'NCA1') { $kind = 'NCA（单文件）' }
    elseif ($magic0 -eq 'NCZ0') { $kind = 'NSZ/XCZ（NCZ 压缩镜像）' }
    elseif ($magic0 -eq 'HFS0') { $kind = 'HFS0 分区' }

    Write-Host ("容器 : " + $kind + "   (0x000='" + $magic0 + "', 0x100='" + $magic100 + "')")
    if ($kind -like 'XCI*') {
        # XCI 头字段（仅打印，不解密）
        $romSize = [BitConverter]::ToUInt32($head, 0x104)
        $ver = $head[0x108]
        Write-Host ("XCI  : version=0x{0:X2}  rom_area_size=0x{1:X} (仅参考)" -f $ver, $romSize)
    }

    # ---- 扫描明文魔数（分块，默认 64MB；-Full 扫全文件）----
    $len = [int64]$fi.Length
    $scanLen = if ($Full) { $len } else { [Math]::Min($len, [int64]$MaxScanMB * 1MB) }
    Write-Host ("扫描 : " + [math]::Round($scanLen/1MB, 1) + " MB" + $(if ($Full) { '（全文件）' } else { "（前 $MaxScanMB MB，可加 -Full 全扫）" }))

    $tokens = @('NCA0', 'NCA1', 'HFS0', 'PFS0', 'HEAD', 'GAME', 'NCZ0')
    $counts = @{}
    $first = @{}
    foreach ($t in $tokens) { $counts[$t] = 0 }
    $bufSize = 8MB
    $pos = [int64]0
    while ($pos -lt $scanLen) {
        $n = [int][Math]::Min([int64]$bufSize, $scanLen - $pos)
        $b = ReadAt $pos $n
        $s = [Text.Encoding]::ASCII.GetString($b)
        foreach ($t in $tokens) {
            $idx = 0
            while (($idx = $s.IndexOf($t, $idx, [StringComparison]::Ordinal)) -ge 0) {
                $counts[$t] = [int]$counts[$t] + 1
                if (-not $first.ContainsKey($t)) { $first[$t] = $pos + $idx }
                $idx += $t.Length
            }
        }
        $pos += $n
    }
    Write-Host ''
    Write-Host '明文字符串命中：' -ForegroundColor Cyan
    foreach ($t in $tokens) {
        if ($counts[$t] -gt 0) {
            Write-Host ("  {0}: {1} 次，首个偏移 0x{2:X}" -f $t, $counts[$t], $first[$t])
        } else {
            Write-Host ("  {0}: 0" -f $t)
        }
    }

    # ---- 熵（采样 256KB，判断高熵/加密）----
    $sampleOff = [int64][Math]::Min([int64]0x10000, [Math]::Max([int64]0, $len - 262144))
    $sample = ReadAt $sampleOff 262144
    $freq = New-Object int[] 256
    foreach ($by in $sample) { $freq[$by]++ }
    $ent = 0.0
    foreach ($c in $freq) { if ($c -gt 0) { $p = $c / $sample.Length; $ent -= $p * [Math]::Log($p, 2) } }
    Write-Host ''
    Write-Host ("采样熵 : {0:N2} bits/byte  (接近 8 说明数据接近随机/加密或强压缩)" -f $ent)

    # ---- 若找到 NCA 头，读一下 crypto_type ----
    $plainNca = ($counts['NCA0'] + $counts['NCA1']) -gt 0
    if ($plainNca) {
        $ncaOff = if ($counts['NCA0'] -gt 0) { [int64]$first['NCA0'] } else { [int64]$first['NCA1'] }
        $nh = ReadAt $ncaOff 0x208
        $cryptoType = $nh[0x200]
        Write-Host ("第一个 NCA 头 : @0x{0:X}  crypto_type=0x{1:X2}" -f $ncaOff, $cryptoType)
    }

    # ---- 结论 ----
    Write-Host ''
    Write-Host '==== 结论 ====' -ForegroundColor Cyan
    $hasFs = ($counts['HFS0'] + $counts['PFS0']) -gt 0
    $ncz = ($counts['NCZ0'] -gt 0) -or ($kind -like 'NSZ*')
    if ($ncz) {
        Write-Host '检测到 NCZ 压缩格式（NSZ/XCZ）：它不是“加密”，而是压缩镜像；需要先解压（本项目不提供工具）。' -ForegroundColor Yellow
    } elseif ($plainNca) {
        Write-Host '发现明文 NCA 头（NCA0/NCA1）：这个镜像看起来是已解密/明文的（至少 NCA 头是明文）。' -ForegroundColor Green
        Write-Host '注意：只是“头可见”，不代表所有内容都完整明文；仍建议用你自己的合法流程确认。'
    } elseif ($hasFs) {
        Write-Host '分区表（HFS0/PFS0）是明文，但没有发现 NCA 明文头：内部 NCA 仍是加密的（零售 dump 的常见状态）。' -ForegroundColor Yellow
    } else {
        Write-Host '未识别到明文 NCA/分区结构：文件可能加密、压缩或非标准格式。' -ForegroundColor Yellow
    }

    if ($Sha256) {
        Write-Host ''
        Write-Host '正在计算 SHA256（大文件较慢）…' -ForegroundColor Cyan
        $h = (Get-FileHash -LiteralPath $fi.FullName -Algorithm SHA256).Hash
        Write-Host ("SHA256: " + $h)
    }
}
finally {
    $fs.Close()
}

Write-Host ''
if ($Host.Name -eq 'ConsoleHost' -and -not $env:NO_PAUSE) {
    Read-Host '按回车退出' | Out-Null
}
