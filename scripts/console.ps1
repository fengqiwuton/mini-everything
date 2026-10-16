$script:MiniConsoleApp = Join-Path (Split-Path -Parent $PSScriptRoot) 'build/Release/mini-everything.exe'

# 包装终端显示：彩色区分命令、诊断和退出状态；普通搜索自动转为表格，其他命令保持流式显示。
# 仅用于人工查看；需要取得原始 JSON/TSV 的脚本直接调用 mini-everything.exe。
function mini {
    $appArguments = @($args)
    if (-not (Test-Path -LiteralPath $script:MiniConsoleApp)) {
        throw "Build Release first: $script:MiniConsoleApp"
    }
    $asTable = $appArguments.Count -gt 0 -and $appArguments[0] -eq 'search' -and
        -not ($appArguments -contains '--format')
    Write-Host ("`n>>> mini-everything " + ($appArguments -join ' ')) -ForegroundColor Cyan
    if ($asTable) { $appArguments += @('--format', 'json') }
    $ErrorActionPreference = 'Continue'
    # 原生 stderr 只显示消息正文，避免 Windows PowerShell 的 ErrorRecord 装饰淹没诊断。
    $payload = & $script:MiniConsoleApp @appArguments 2>&1 | ForEach-Object {
        $line = $_.ToString()
        if ($line -match '^(Error:|Usage error:|Cancelled:)') {
            Write-Host $line -ForegroundColor Red
        } elseif ($_ -is [System.Management.Automation.ErrorRecord] -or
                  $line -match '^(Elapsed:|Returned |Profile:|Changes:|Progress:|Watch:)') {
            Write-Host $line -ForegroundColor DarkGray
        } elseif ($asTable) {
            $line
        } else {
            Write-Host $line -ForegroundColor White
        }
    }
    $resultCode = $LASTEXITCODE
    if ($asTable -and $resultCode -eq 0) {
        $nodes = @(($payload -join "`n") | ConvertFrom-Json)
        Write-Host '--- Results ---' -ForegroundColor Yellow
        if ($nodes.Count -eq 0) { Write-Host '(0 results)' -ForegroundColor DarkGray }
        else {
            $columns = if ($appArguments -contains '--show-ids') {
                @('id','parent_id','name','size','path')
            } else { @('name','size','path') }
            $nodes | Format-Table -Property $columns -AutoSize -Wrap | Out-Host
        }
    }
    $statusColor = if ($resultCode -eq 0) { 'Green' } else { 'Red' }
    Write-Host ("<<< Exit code: " + $resultCode + "`n") -ForegroundColor $statusColor
}
