param(
    [Parameter(Mandatory=$true)][string]$Serial,
    [string]$Adb='adb',
    [int]$UserId=0,
    [ValidateRange(1,10)][int]$Repetitions=1
)
$ErrorActionPreference='Stop'
if ($Serial -notmatch '^[a-zA-Z0-9._:-]+$' -or $UserId -lt 0) { throw 'Invalid device/user selection.' }
function Test-AcceptanceOutput([string]$Output) {
    # am instrument can exit zero even when its test failed; require both the
    # specific test result and successful instrumentation completion.
    return $Output -match '(?m)^INSTRUMENTATION_RESULT: pcsx5=PASS: 5 real UI/JNI tests; repeat, recreate, rotate, background/resume, finish\r?$' -and
        $Output -match '(?m)^INSTRUMENTATION_CODE: -1\r?$' -and
        $Output -notmatch 'FAIL:|INSTRUMENTATION_FAILED|INSTRUMENTATION_ABORTED'
}
$good="INSTRUMENTATION_RESULT: pcsx5=PASS: 5 real UI/JNI tests; repeat, recreate, rotate, background/resume, finish`nINSTRUMENTATION_CODE: -1"
if (!(Test-AcceptanceOutput $good)) { throw 'Runner success-parser self-test failed.' }
foreach ($bad in @('', 'INSTRUMENTATION_CODE: -1', $good.Replace('PASS:','FAIL:'),
        $good.Replace('CODE: -1','CODE: 0'), ($good+"`nINSTRUMENTATION_FAILED"))) {
    if (Test-AcceptanceOutput $bad) { throw 'Runner failure-parser self-test failed.' }
}
for ($iteration=1; $iteration -le $Repetitions; ++$iteration) {
    $result=@(& $Adb -s $Serial shell am instrument --user $UserId -w -r org.pcsx5.experimental/org.pcsx5.experimental.DeviceAcceptance 2>&1)
    $exitCode=$LASTEXITCODE
    $output=$result -join "`n"
    Write-Output $output
    if ($exitCode -ne 0 -or !(Test-AcceptanceOutput $output)) { throw "Device app acceptance failed, repetition $iteration." }
}
Write-Host "PASS: $Repetitions app acceptance runs. Existing app/data retained; no installation or permissions changed."
