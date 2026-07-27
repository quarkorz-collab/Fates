param(
    [Parameter(Mandatory = $true)]
    [string]$DenseExecutable,
    [Parameter(Mandatory = $true)]
    [string]$StdExecutable,
    [ValidateRange(3, 21)]
    [int]$Runs = 7,
    [ValidateSet('standard', 'heavy', 'constrained')]
    [string]$Case = 'standard'
)

$commonArguments = @(
    '520.82418', '--value-bits', '48', '--threads', '16', '--no-stop',
    '--results', '20', '--mode', 'nearest', '--json', '--no-stats'
)

switch ($Case) {
    'standard' {
        $caseArguments = @('--beam', '5000', '--pairs', '2000000', '--max-cost', '12')
    }
    'heavy' {
        $caseArguments = @('--beam', '12000', '--pairs', '8000000', '--max-cost', '14')
    }
    'constrained' {
        $caseArguments = @(
            '--beam', '5000', '--pairs', '2000000', '--max-cost', '12',
            '--symbol-count', 'pi=1:2'
        )
    }
}
$searchArguments = $commonArguments + $caseArguments

function Invoke-FatesBenchmarkRun {
    param([string]$Executable, [string[]]$Arguments)
    return (& $Executable @Arguments | ConvertFrom-Json)
}

$denseTimes = @()
$stdTimes = @()
$identical = $true
for ($runIndex = 0; $runIndex -lt $Runs; ++$runIndex) {
    if (($runIndex % 2) -eq 0) {
        $stdResult = Invoke-FatesBenchmarkRun $StdExecutable $searchArguments
        $denseResult = Invoke-FatesBenchmarkRun $DenseExecutable $searchArguments
    } else {
        $denseResult = Invoke-FatesBenchmarkRun $DenseExecutable $searchArguments
        $stdResult = Invoke-FatesBenchmarkRun $StdExecutable $searchArguments
    }

    $denseTimes += [double]$denseResult.stats.seconds
    $stdTimes += [double]$stdResult.stats.seconds
    $identical = $identical -and
        (($denseResult.results | ConvertTo-Json -Depth 4 -Compress) -ceq
         ($stdResult.results | ConvertTo-Json -Depth 4 -Compress)) -and
        ($denseResult.stats.attempted -eq $stdResult.stats.attempted) -and
        ($denseResult.stats.valid -eq $stdResult.stats.valid) -and
        ($denseResult.stats.kept -eq $stdResult.stats.kept)
}

$denseSorted = $denseTimes | Sort-Object
$stdSorted = $stdTimes | Sort-Object
$middle = [int][math]::Floor($Runs / 2)
$denseMedian = $denseSorted[$middle]
$stdMedian = $stdSorted[$middle]
$summary = [pscustomobject]@{
    Case = $Case
    Runs = $Runs
    DenseMedianSeconds = $denseMedian
    StdMedianSeconds = $stdMedian
    Speedup = $stdMedian / $denseMedian
    ResultsAndStatsIdentical = $identical
    DenseRuns = $denseTimes -join ','
    StdRuns = $stdTimes -join ','
}
$summary | Format-List

if (-not $identical) {
    throw 'Container backends produced different search results or counters.'
}
if ($denseMedian -ge $stdMedian) {
    throw 'unordered_dense did not improve the median runtime in this run.'
}
