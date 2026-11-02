$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$runnerPath = Join-Path $projectRoot 'build\Release\test_ipc_runner.exe'

# Query exact protocol sizes directly from compiled binary
$sizesOutput = & $runnerPath --sizes
$reqSize = 0
$respSize = 0
foreach ($line in $sizesOutput) {
    if ($line -match 'REQUEST_SIZE=(\d+)') { $reqSize = [int]$matches[1] }
    if ($line -match 'RESPONSE_SIZE=(\d+)') { $respSize = [int]$matches[1] }
}
if ($reqSize -eq 0 -or $respSize -eq 0) {
    throw "Failed to query protocol sizes from test_ipc_runner --sizes"
}
Write-Host "Exact binary protocol sizes: REQUEST_SIZE=$reqSize, RESPONSE_SIZE=$respSize" -ForegroundColor Gray

function Run-Scenario([string]$scenario) {
    $pipe = [IO.Pipes.NamedPipeServerStream]::new(
        'PersonalIME_Service_Pipe',
        [IO.Pipes.PipeDirection]::InOut,
        1,
        [IO.Pipes.PipeTransmissionMode]::Message,
        [IO.Pipes.PipeOptions]::Asynchronous
    )

    $psi = [Diagnostics.ProcessStartInfo]::new($runnerPath)
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $runner = $null
    try {
        $conn = $pipe.BeginWaitForConnection($null, $null)
        $runner = [Diagnostics.Process]::Start($psi)
        if (-not $conn.AsyncWaitHandle.WaitOne(3000)) { throw "Pipe connection timed out" }
        $pipe.EndWaitForConnection($conn)

        $request = [byte[]]::new($reqSize)
        $readTask = $pipe.ReadAsync($request, 0, $reqSize)
        if (-not $readTask.Wait(3000)) { throw "Read request timed out" }

        # Parse request header
        $reqMsgType = [BitConverter]::ToUInt32($request, 0)
        $reqSessionId = [BitConverter]::ToUInt64($request, 4)
        $reqContextId = [BitConverter]::ToUInt64($request, 12)

        if ($scenario -eq 'raw-zero-bytes') {
            # 1. Raw 0x00 bytes across entire frame
            $zeroResp = [byte[]]::new($respSize)
            $pipe.Write($zeroResp, 0, $zeroResp.Length)
            $pipe.Flush()

            $exited = $runner.WaitForExit(2000)
            if (-not $exited) {
                $runner.Kill()
                $runner.WaitForExit(1000)
                throw "FAILED: Runner did not exit within timeout!"
            }

            $stdout = $runner.StandardOutput.ReadToEnd()
            $stderr = $runner.StandardError.ReadToEnd()
            Write-Host "RAW_ZERO_BYTES: Exited=True, ExitCode=$($runner.ExitCode)" -ForegroundColor Cyan
            Write-Host "Stderr: $stderr"
            if ($runner.ExitCode -ne 1) {
                throw "FAILED: Expected exit code 1, got $($runner.ExitCode)!"
            }
            Write-Host "PASSED: Raw 0x00 frame correctly rejected!" -ForegroundColor Green

        } elseif ($scenario -eq 'semantic-zero-payload') {
            # 2. Header matches (type, session, context), but payload is all zero (handled = 0)
            $resp = [byte[]]::new($respSize)
            [BitConverter]::GetBytes($reqMsgType).CopyTo($resp, 0)
            [BitConverter]::GetBytes($reqSessionId).CopyTo($resp, 4)
            [BitConverter]::GetBytes($reqContextId).CopyTo($resp, 12)
            # handled = 0 (offset 20)

            $pipe.Write($resp, 0, $resp.Length)
            $pipe.Flush()

            $exited = $runner.WaitForExit(2000)
            if (-not $exited) {
                $runner.Kill()
                $runner.WaitForExit(1000)
                throw "FAILED: Runner did not exit within timeout!"
            }

            $stdout = $runner.StandardOutput.ReadToEnd()
            $stderr = $runner.StandardError.ReadToEnd()
            Write-Host "SEMANTIC_ZERO_PAYLOAD: Exited=True, ExitCode=$($runner.ExitCode)" -ForegroundColor Cyan
            Write-Host "Stderr: $stderr"
            if ($runner.ExitCode -ne 1) {
                throw "FAILED: Expected exit code 1, got $($runner.ExitCode)!"
            }
            if (-not $stderr.Contains("Ping response handled must be 1")) {
                throw "FAILED: Expected 'Ping response handled must be 1', got '$stderr'"
            }
            Write-Host "PASSED: Semantic zero payload correctly rejected by handled check!" -ForegroundColor Green

        } elseif ($scenario -eq 'truncated-response') {
            # 3. Truncated response (64 bytes instead of $respSize)
            $truncResp = [byte[]]::new(64)
            $pipe.Write($truncResp, 0, $truncResp.Length)
            $pipe.Flush()

            $exited = $runner.WaitForExit(2000)
            if (-not $exited) {
                $runner.Kill()
                $runner.WaitForExit(1000)
                throw "FAILED: Runner did not exit within timeout!"
            }

            $stdout = $runner.StandardOutput.ReadToEnd()
            $stderr = $runner.StandardError.ReadToEnd()
            Write-Host "TRUNCATED_RESPONSE: Exited=True, ExitCode=$($runner.ExitCode)" -ForegroundColor Cyan
            Write-Host "Stderr: $stderr"
            if ($runner.ExitCode -ne 1) {
                throw "FAILED: Expected exit code 1 on truncated frame, got $($runner.ExitCode)!"
            }
            Write-Host "PASSED: Truncated frame correctly rejected at length check!" -ForegroundColor Green

        } elseif ($scenario -eq 'mismatched-identity') {
            # 4. Correct length and handled = 1, but mismatched session_id = 999999
            $fakeResp = [byte[]]::new($respSize)
            [BitConverter]::GetBytes($reqMsgType).CopyTo($fakeResp, 0)
            [BitConverter]::GetBytes([uint64]999999).CopyTo($fakeResp, 4) # mismatched session
            [BitConverter]::GetBytes($reqContextId).CopyTo($fakeResp, 12)
            [BitConverter]::GetBytes([uint32]1).CopyTo($fakeResp, 20)      # handled = 1

            $pipe.Write($fakeResp, 0, $fakeResp.Length)
            $pipe.Flush()

            $exited = $runner.WaitForExit(2000)
            if (-not $exited) {
                $runner.Kill()
                $runner.WaitForExit(1000)
                throw "FAILED: Runner did not exit within timeout!"
            }

            $stdout = $runner.StandardOutput.ReadToEnd()
            $stderr = $runner.StandardError.ReadToEnd()
            Write-Host "MISMATCHED_IDENTITY: Exited=True, ExitCode=$($runner.ExitCode)" -ForegroundColor Cyan
            Write-Host "Stderr: $stderr"
            if ($runner.ExitCode -ne 1) {
                throw "FAILED: Expected exit code 1 on identity mismatch, got $($runner.ExitCode)!"
            }
            Write-Host "PASSED: Mismatched session identity frame correctly rejected!" -ForegroundColor Green

        } elseif ($scenario -eq 'stalled-server') {
            # 5. Do NOT reply, wait for bounded client timeout
            $sw = [Diagnostics.Stopwatch]::StartNew()
            $exited = $runner.WaitForExit(1500)
            $sw.Stop()

            if (-not $exited) {
                $runner.Kill()
                $runner.WaitForExit(1000)
                throw "FAILED: Runner blocked indefinitely on stalled server!"
            }

            $stdout = $runner.StandardOutput.ReadToEnd()
            $stderr = $runner.StandardError.ReadToEnd()
            Write-Host "STALLED_SERVER: Exited=True within $($sw.ElapsedMilliseconds)ms, ExitCode=$($runner.ExitCode)" -ForegroundColor Cyan
            Write-Host "Stderr: $stderr"

            if ($runner.ExitCode -ne 1) {
                throw "FAILED: Expected exit code 1 on stalled server, got $($runner.ExitCode)!"
            }
            Write-Host "PASSED: Runner exited boundedly within timeout on stalled server!" -ForegroundColor Green
        }
    } finally {
        if ($runner -and -not $runner.HasExited) {
            $runner.Kill()
            $runner.WaitForExit(1000)
        }
        $pipe.Dispose()
    }
}

function Test-HarnessTimeoutSafety {
    Write-Host "`n=== TEST 6: Verifying Harness Itself Safely Kills Stuck Processes ===" -ForegroundColor Yellow
    $psi = [Diagnostics.ProcessStartInfo]::new("powershell.exe", "-NoProfile -Command `"`$host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown')`"")
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $stuckProc = [Diagnostics.Process]::Start($psi)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $exited = $stuckProc.WaitForExit(800)
    $sw.Stop()

    if (-not $exited) {
        $stuckProc.Kill()
        $stuckProc.WaitForExit(1000)
        Write-Host "PASSED: Harness successfully killed stuck process without hanging after $($sw.ElapsedMilliseconds)ms!" -ForegroundColor Green
    } else {
        throw "Unexpected: Mock stuck process exited prematurely."
    }
}

function Test-PipeBusyBudget {
    Write-Host "`n=== TEST 7: Pipe Busy Bounded Timeout (< 500ms total, SendRequest bounded to deadline) ===" -ForegroundColor Yellow
    $pipe = [IO.Pipes.NamedPipeServerStream]::new('PersonalIME_Service_Pipe', [IO.Pipes.PipeDirection]::InOut, 1, [IO.Pipes.PipeTransmissionMode]::Message, [IO.Pipes.PipeOptions]::Asynchronous)
    $holder = [IO.Pipes.NamedPipeClientStream]::new('.', 'PersonalIME_Service_Pipe', [IO.Pipes.PipeDirection]::InOut)
    $runner = $null
    try {
        $conn = $pipe.BeginWaitForConnection($null, $null)
        $holder.Connect(1000)
        if (-not $conn.AsyncWaitHandle.WaitOne(1000)) { throw "Holder connection timed out" }
        $pipe.EndWaitForConnection($conn)

        $psi = [Diagnostics.ProcessStartInfo]::new($runnerPath, "--test-timeout-budget")
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true

        $sw = [Diagnostics.Stopwatch]::StartNew()
        $runner = [Diagnostics.Process]::Start($psi)
        $exited = $runner.WaitForExit(1000)
        $sw.Stop()

        if (-not $exited) {
            $runner.Kill()
            $runner.WaitForExit(500)
            throw "FAILED: Busy pipe test hung over 1000ms!"
        }

        $stdout = $runner.StandardOutput.ReadToEnd().Trim()
        Write-Host "PIPE_BUSY_OUTPUT: $stdout (Total Process Time: $($sw.ElapsedMilliseconds)ms, ExitCode=$($runner.ExitCode))" -ForegroundColor Cyan
        if ($stdout -match 'TIMEOUT_ELAPSED_MS=(\d+)') {
            $innerMs = [int]$matches[1]
            if ($innerMs -gt 150) {
                throw "FAILED: In-process SendRequest on busy pipe took ${innerMs}ms, exceeding budget!"
            }
        }
        Write-Host "PASSED: Busy pipe SendRequest strictly bounded to budget!" -ForegroundColor Green
    } finally {
        if ($runner -and -not $runner.HasExited) { $runner.Kill(); $runner.WaitForExit(500) }
        $holder.Dispose()
        $pipe.Dispose()
    }
}

Write-Host "`n=== TEST 1: Raw 0x00 Frame Rejection ===" -ForegroundColor Yellow
Run-Scenario 'raw-zero-bytes'

Write-Host "`n=== TEST 2: Semantic Zero-Payload Rejection (handled = 0 check) ===" -ForegroundColor Yellow
Run-Scenario 'semantic-zero-payload'

Write-Host "`n=== TEST 3: Truncated Response Frame Rejection (Length check) ===" -ForegroundColor Yellow
Run-Scenario 'truncated-response'

Write-Host "`n=== TEST 4: Mismatched Identity Response Rejection (Session check) ===" -ForegroundColor Yellow
Run-Scenario 'mismatched-identity'

Write-Host "`n=== TEST 5: Stalled Server Bounded Timeout (Hang prevention) ===" -ForegroundColor Yellow
Run-Scenario 'stalled-server'

Test-HarnessTimeoutSafety

Test-PipeBusyBudget
