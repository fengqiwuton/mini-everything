param(
    [Parameter(Mandatory)][string]$App
)
$ErrorActionPreference = 'Stop'
$OutputEncoding = [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
function Invoke-App {
    # Windows PowerShell 5.1 represents native stderr as ErrorRecord objects.
    $ErrorActionPreference = 'Continue'
    $result = & $App @args 2>&1 | ForEach-Object { "$_" }
    $script:NativeExit = $LASTEXITCODE
    return $result
}
# The system temporary volume is more likely to support 8.3 aliases than a source volume.
$fixture = Join-Path ([System.IO.Path]::GetTempPath()) ('mini-everything-windows-' + [Guid]::NewGuid().ToString('N'))
# Use code points so Windows PowerShell 5.1 also reads this UTF-8 script correctly.
$chinese = [string][char]0x62a5 + [char]0x544a
$root = Join-Path $fixture "$chinese space"
$db = Join-Path $fixture "$chinese.db"
New-Item -ItemType Directory -Path $root -Force | Out-Null
Set-Content -LiteralPath (Join-Path $root "$chinese.txt") -Value 'hello' -Encoding UTF8
(Get-Item -LiteralPath (Join-Path $root "$chinese.txt")).LastWriteTimeUtc = [DateTime]::new(2020, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)
$junction = Join-Path $root 'cycle'
try {
    New-Item -ItemType Junction -Path $junction -Target $root | Out-Null
    $scan = Invoke-App scan $root --db $db
    if ($NativeExit -ne 0 -or ($scan -join "`n") -notmatch 'skipped 1') {
        throw "Junction scan failed: $scan"
    }
    $result = Invoke-App search $chinese --db $db --ext txt
    if ($NativeExit -ne 0 -or ($result -join "`n") -notmatch "$chinese.txt") {
        throw "Unicode CLI search failed: $result"
    }
    if (($result -join "`n") -notmatch '1577836800') { throw 'File timestamp is not in Unix seconds' }
    # Alternate casing and trailing separators must resolve to the same root.
    # A forward separator avoids PowerShell 5.1's native quoting bug for trailing backslashes.
    Invoke-App scan ($root.ToUpperInvariant() + '/') --db $db
    if ($NativeExit -ne 0) { throw 'Case-insensitive root rescan failed' }
    $result = Invoke-App search $chinese --db $db --ext txt
    if ($NativeExit -ne 0 -or ($result -join "`n") -notmatch 'Returned 1 result') {
        throw "Case variant root duplicated results: $result"
    }
    Add-Type -TypeDefinition @'
using System.Text;
using System.Runtime.InteropServices;
public static class MiniTestPaths {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint GetShortPathName(string path, StringBuilder output, uint length);
}
'@
    $buffer = [System.Text.StringBuilder]::new(32768)
    $length = [MiniTestPaths]::GetShortPathName($root, $buffer, 32768)
    if ($length -eq 0) { throw 'GetShortPathName failed' }
    $shortRoot = $buffer.ToString()
    Invoke-App scan $shortRoot --db $db
    if ($NativeExit -ne 0) { throw 'Short-path root rescan failed' }
    $result = Invoke-App search $chinese --db $db --ext txt
    if ($NativeExit -ne 0 -or ($result -join "`n") -notmatch 'Returned 1 result') {
        throw "Short-path root duplicated results: $result"
    }
    $insideDb = Join-Path $root 'inside.db'
    Invoke-App scan $shortRoot --db $insideDb | Out-Null
    if ($NativeExit -ne 2 -or (Test-Path -LiteralPath $insideDb)) {
        throw 'Short-path alias bypassed database-inside-root rejection'
    }
} finally {
    # Remove the junction itself without following its target before recursive cleanup.
    if (Test-Path -LiteralPath $junction) { [System.IO.Directory]::Delete($junction) }
    $resolvedFixture = [System.IO.Path]::GetFullPath($fixture)
    $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolvedFixture.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolvedFixture) -notmatch '^mini-everything-windows-[a-f0-9]{32}$') {
        throw 'Refusing to clean up an unexpected fixture path'
    }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
