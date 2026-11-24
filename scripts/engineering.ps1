[CmdletBinding()]
param(
    [ValidateSet('State', 'Artifacts', 'Focused', 'Comprehensive', 'Usage')]
    [string]$Mode = 'State',
    [string]$OpenCode = 'opencode',
    [string]$ResultsDir = 'app/build/test-results/testDebugUnitTest'
)

$ErrorActionPreference = 'Stop'
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}
function Save-Command($Executable, [string[]]$Arguments, $Log) {
    # PowerShell 5 wraps redirected native stderr as errors; use the native exit
    # code and retain both streams, even when diagnostics go to stderr.
    $ErrorActionPreference = 'Continue'
    & $Executable @Arguments 2>&1 | Out-File -LiteralPath $Log -Encoding utf8
    return $LASTEXITCODE
}
function Show-TestResults {
    $tests = 0; $skipped = 0; $failures = 0; $errors = 0
    foreach ($file in Get-ChildItem -LiteralPath $ResultsDir -Filter 'TEST-*.xml' -File) {
        [xml]$xml = Get-Content -LiteralPath $file.FullName -Raw
        $suite = $xml.SelectSingleNode('/testsuite')
        if (!$suite) { throw "Missing testsuite in $($file.Name)" }
        foreach ($test in $suite.SelectNodes('testcase')) {
            $tests++
            if ($test.SelectSingleNode('skipped')) { $skipped++ }
            if ($test.SelectSingleNode('failure')) { $failures++ }
            if ($test.SelectSingleNode('error')) { $errors++ }
        }
    }
    Write-Host "JUnit XML: $($tests - $skipped) executed; $failures failed; $errors errors; $skipped skipped."
    if ($tests -le $skipped -or $failures -or $errors) {
        throw 'JUnit evidence missing, all skipped or unsuccessful.'
    }
}

Push-Location -LiteralPath (Join-Path $PSScriptRoot '..')
try {
    if ($Mode -eq 'State') {
        foreach ($argsList in @('branch --show-current', 'rev-parse HEAD', 'status -sb -uall')) {
            & git @($argsList.Split(' '))
            if ($LASTEXITCODE) { exit $LASTEXITCODE }
        }
        Write-Host 'Checkpoint and authorization: docs/SESSION_HANDOFF.md'
        exit 0
    }
    if ($Mode -eq 'Artifacts') {
        Write-Host 'Existing artifacts only; no tests requested by this command.'
        Show-TestResults
        exit 0
    }

    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
    $dir = New-Item -ItemType Directory -Path "captures/workflow/$stamp-$PID"
    if ($Mode -eq 'Usage') {
        $exe = (Get-Command $OpenCode -CommandType Application).Source
        $log = Join-Path $dir.FullName 'usage.json'
        $stderr = Join-Path $dir.FullName 'usage.stderr.log'
        $ErrorActionPreference = 'Continue'
        & $exe stats --project . --all --models --cost --json 2> $stderr |
            Out-File -LiteralPath $log -Encoding utf8
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        Write-Host "OpenCode stats exit=$code; JSON=$log; stderr=$stderr"
        if ($code) { exit $code }
        $s = Get-Content -LiteralPath $log -Raw | ConvertFrom-Json
        if (!$s.tokens -or !$s.tokens.cache -or $null -eq $s.steps) { throw 'Missing usage fields.' }
        Write-Host "Steps=$($s.steps); input=$($s.tokens.input); output=$($s.tokens.output); reasoning=$($s.tokens.reasoning); cache read=$($s.tokens.cache.read); write=$($s.tokens.cache.write); reported cost=$($s.cost)."
        exit 0
    }

    if ($PSBoundParameters.ContainsKey('ResultsDir')) { throw '-ResultsDir is only for Artifacts mode.' }
    $log = Join-Path $dir.FullName 'gradle.log'
    $tasks = @('--console=plain', ':app:testDebugUnitTest')
    if ($Mode -eq 'Comprehensive') {
        $tasks += @(':app:assembleDebug', ':app:externalNativeBuildDebug', ':app:lintDebug')
    }
    $code = Save-Command '.\gradlew.bat' $tasks $log
    Write-Host "Gradle exit=$code; full log=$log"
    if ($code) { Get-Content -LiteralPath $log -Tail 20; exit $code }
    if (!(Select-String -LiteralPath $log -Pattern '^> Task :app:testDebugUnitTest( UP-TO-DATE| FROM-CACHE)?$' -Quiet)) {
        throw 'Gradle did not execute or reuse the debug unit-test task.'
    }
    Select-String -LiteralPath $log -Pattern '^> Task :app:testDebugUnitTest\b' | ForEach-Object { Write-Host $_.Line }
    Show-TestResults
    if ($Mode -eq 'Comprehensive') {
        $lint = 'app/build/reports/lint-results-debug.xml'
        [xml]$xml = Get-Content -LiteralPath $lint -Raw
        if (!$xml.SelectSingleNode('/issues')) { throw 'Missing lint issues document.' }
        $errorCount = $xml.SelectNodes('/issues/issue[@severity="Error" or @severity="Fatal"]').Count
        $warningCount = $xml.SelectNodes('/issues/issue[@severity="Warning"]').Count
        Write-Host "Lint XML: $errorCount errors, $warningCount warnings; $lint"
        if ($errorCount) { throw 'Lint reports errors.' }
        $diff = Join-Path $dir.FullName 'diff-check.log'
        $code = Save-Command 'git' @('diff','--check','HEAD') $diff
        Write-Host "Tracked diff check exit=$code; log=$diff. Add any intended untracked paths before the final staged check."
        if ($code) { Get-Content -LiteralPath $diff; exit $code }
    }
    exit 0
} catch {
    Write-Error -ErrorAction Continue $_
    exit 1
} finally {
    Pop-Location
}
