param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$Generator = 'Ninja'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $repo "build/$Configuration"
$downloads = Join-Path $repo 'build/downloads'
New-Item -ItemType Directory -Path $downloads -Force | Out-Null
$archive = Join-Path $downloads 'sqlite-amalgamation-3530400.zip'
$expectedHash = '1E71DDF93849C6A6ECF58B827C0692073D2DD7EE40196158068F7B29F422E87D'
if (-not (Test-Path -LiteralPath $archive)) {
    # Use the Windows certificate store; never disable TLS verification.
    Invoke-WebRequest -UseBasicParsing -Uri 'https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip' -OutFile $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedHash) {
    throw "SQLite checksum mismatch: $archive. Remove this archive and retry."
}
& cmake -S $repo -B $buildDirectory -G $Generator "-DCMAKE_BUILD_TYPE=$Configuration" "-DMINI_SQLITE_ARCHIVE=$archive"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& cmake --build $buildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
& ctest --test-dir $buildDirectory -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
