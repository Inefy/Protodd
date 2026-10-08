Set-StrictMode -Version Latest

$script:PrivateUsageBudgetBytes = [long]256 * 1024 * 1024
$script:PeakWorkingSetBudgetBytes = [long]512 * 1024 * 1024

function Get-MatchMemoryBudgetAssessment {
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Samples,
        [int]$SampleFailures = 0
    )

    $issues = [Collections.Generic.List[string]]::new()
    $privateSamples = [Collections.Generic.List[long]]::new()
    $workingSetSamples = [Collections.Generic.List[long]]::new()
    if ($Samples.Count -eq 0) { $issues.Add('no-memory-samples') }
    if ($SampleFailures -gt 0) { $issues.Add("memory-sample-failures:$SampleFailures") }

    foreach ($sample in $Samples) {
        $privateProperty = $sample.PSObject.Properties['private_usage']
        $workingSetProperty = $sample.PSObject.Properties['peak_working_set']
        if ($null -eq $privateProperty -or $null -eq $workingSetProperty) {
            $issues.Add('malformed-memory-sample')
            break
        }
        try {
            $privateBytes = [long]$privateProperty.Value
            $workingSetBytes = [long]$workingSetProperty.Value
        } catch {
            $issues.Add('malformed-memory-sample')
            break
        }
        if ($privateBytes -lt 0 -or $workingSetBytes -lt 0) {
            $issues.Add('negative-memory-sample')
            break
        }
        $privateSamples.Add($privateBytes)
        $workingSetSamples.Add($workingSetBytes)
    }

    $peakPrivateUsageBytes = if ($privateSamples.Count) {
        ($privateSamples | Measure-Object -Maximum).Maximum
    } else { 0L }
    $peakWorkingSetBytes = if ($workingSetSamples.Count) {
        ($workingSetSamples | Measure-Object -Maximum).Maximum
    } else { 0L }
    if ($peakPrivateUsageBytes -gt $script:PrivateUsageBudgetBytes) {
        $issues.Add('private-usage-budget-exceeded')
    }
    if ($peakWorkingSetBytes -gt $script:PeakWorkingSetBudgetBytes) {
        $issues.Add('working-set-budget-exceeded')
    }

    return [pscustomobject][ordered]@{
        schema = 'protodd-memory-budget-v1'
        sample_count = $Samples.Count
        sample_failures = $SampleFailures
        peak_private_usage_bytes = [long]$peakPrivateUsageBytes
        private_usage_budget_bytes = $script:PrivateUsageBudgetBytes
        peak_working_set_bytes = [long]$peakWorkingSetBytes
        peak_working_set_budget_bytes = $script:PeakWorkingSetBudgetBytes
        within_budget = ($issues.Count -eq 0)
        issues = @($issues.ToArray())
    }
}

Export-ModuleMember -Function 'Get-MatchMemoryBudgetAssessment'
