$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Import-Module (Join-Path $repo 'scripts/MatchResourceBudget.psm1') -Force

$privateBudget = [long]256 * 1024 * 1024
$workingSetBudget = [long]512 * 1024 * 1024
$within = Get-MatchMemoryBudgetAssessment -Samples @(
    [pscustomobject]@{ private_usage = $privateBudget; peak_working_set = $workingSetBudget }
)
if (-not $within.within_budget -or $within.issues.Count -ne 0) {
    throw 'Memory values exactly at the documented budgets must pass'
}

$privateExceeded = Get-MatchMemoryBudgetAssessment -Samples @(
    [pscustomobject]@{ private_usage = $privateBudget + 1; peak_working_set = 1 }
)
if ($privateExceeded.within_budget -or $privateExceeded.issues -notcontains 'private-usage-budget-exceeded') {
    throw 'Private usage above the documented budget must fail'
}

$workingSetExceeded = Get-MatchMemoryBudgetAssessment -Samples @(
    [pscustomobject]@{ private_usage = 1; peak_working_set = $workingSetBudget + 1 }
)
if ($workingSetExceeded.within_budget -or $workingSetExceeded.issues -notcontains 'working-set-budget-exceeded') {
    throw 'Peak working set above the documented budget must fail'
}

$noSamples = Get-MatchMemoryBudgetAssessment -Samples @()
if ($noSamples.within_budget -or $noSamples.issues -notcontains 'no-memory-samples') {
    throw 'Missing process memory samples must fail closed'
}

$sampleFailure = Get-MatchMemoryBudgetAssessment -Samples @(
    [pscustomobject]@{ private_usage = 1; peak_working_set = 1 }
) -SampleFailures 1
if ($sampleFailure.within_budget -or $sampleFailure.issues -notcontains 'memory-sample-failures:1') {
    throw 'Failed process memory samples must fail closed'
}

Write-Output 'Runtime process memory budget boundary checks passed'
