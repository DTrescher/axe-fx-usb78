param([ValidateRange(1,3600)][int]$DurationSeconds=180)
$timer=[System.Diagnostics.Stopwatch]::StartNew()
$value=0.0
while($timer.Elapsed.TotalSeconds -lt $DurationSeconds) {
    for($index=0; $index -lt 10000; $index++) {$value=[Math]::Sqrt($index+1)+[Math]::Sin($value)}
}
