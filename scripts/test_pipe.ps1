$pipeName = "PersonalIME_Service_Pipe"
try {
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", $pipeName, [System.IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect(1000)
    Write-Host "[PIPE TEST] Successfully connected to pipe: $pipeName" -ForegroundColor Green
    $pipe.Close()
} catch {
    Write-Host "[PIPE TEST] Failed to connect: $_" -ForegroundColor Red
}

