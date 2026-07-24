<#
.SYNOPSIS
    Envía N peticiones individuales (una por seguimiento, SIN batch) a
    POST /followups y mide el tiempo de cada una. Pensado para usarse desde
    los scripts test_single_insert_p0X.ps1, o directamente con -Level.

.PARAMETER BaseUrl
    URL base del microservicio.

.PARAMETER Requests
    Cantidad de peticiones individuales a enviar en esta corrida.

.PARAMETER Level
    Etiqueta del nivel (p01, p02, p04, p06, p08) — solo para el nombre del
    CSV de salida y para mostrarla en pantalla; no cambia ningún hilo por sí
    misma (eso lo controla OMP_NUM_THREADS en el contenedor).

.PARAMETER Run
    Número de repetición de esta corrida (para cuando se repite 3-5 veces
    por nivel, como pide la consigna).

.PARAMETER OutCsv
    Ruta del CSV donde se acumulan los resultados (se agrega una fila por
    corrida, no se sobreescribe).
#>
param(
    [string]$BaseUrl = "http://localhost:8084",
    [int]$Requests = 2000,
    [string]$Level = "p01",
    [int]$Run = 1,
    [string]$OutCsv = "resultado_single_insert.csv"
)

$ErrorActionPreference = "Stop"
$statuses = @("SI_CUMPLE", "PARCIAL", "NO_CUMPLE")
$times = New-Object System.Collections.Generic.List[double]
$errors = 0

Write-Host "Nivel $Level, corrida $Run: enviando $Requests peticiones individuales a $BaseUrl/followups ..." -ForegroundColor Cyan

# Config confirmada antes de la corrida.
try {
    $health = Invoke-RestMethod -Uri "$BaseUrl/health" -Method Get -TimeoutSec 5
    Write-Host ("Config confirmada -> omp_num_threads_configured={0}  backend_threads={1}" -f `
        $health.openmp.omp_num_threads_configured, $health.backend_threads) -ForegroundColor Green
}
catch {
    Write-Warning "No se pudo leer /health antes de la corrida: $_"
}

$totalStopwatch = [System.Diagnostics.Stopwatch]::StartNew()

for ($i = 1; $i -le $Requests; $i++) {
    $payload = @{
        family_id            = ($i % 5000) + 1
        record_number        = "SGL-$Level-$Run-$i-$([guid]::NewGuid().ToString('N').Substring(0,8))"
        risk_description     = "Registro individual de prueba #$i"
        scheduled_activities = "Visita de control comunitario"
        compliance_status    = $statuses[$i % 3]
        analysis_date        = (Get-Date).ToString("yyyy-MM-dd")
        evaluation_date      = (Get-Date).AddDays(15).ToString("yyyy-MM-dd")
    } | ConvertTo-Json -Compress

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        Invoke-RestMethod -Uri "$BaseUrl/followups" -Method Post -ContentType "application/json" `
            -Body $payload -TimeoutSec 30 | Out-Null
        $sw.Stop()
        $times.Add($sw.Elapsed.TotalMilliseconds)
    }
    catch {
        $sw.Stop()
        $errors++
    }

    if ($i % 500 -eq 0) { Write-Host "  ... $i / $Requests" }
}

$totalStopwatch.Stop()

$success = $times.Count
$avg = if ($success -gt 0) { ($times | Measure-Object -Average).Average } else { 0 }
$min = if ($success -gt 0) { ($times | Measure-Object -Minimum).Minimum } else { 0 }
$max = if ($success -gt 0) { ($times | Measure-Object -Maximum).Maximum } else { 0 }
$throughput = if ($totalStopwatch.Elapsed.TotalSeconds -gt 0) { $Requests / $totalStopwatch.Elapsed.TotalSeconds } else { 0 }

$row = [pscustomobject]@{
    level             = $Level
    omp_threads       = $health.openmp.omp_num_threads_configured
    backend_threads   = $health.backend_threads
    run               = $Run
    requests          = $Requests
    total_time_ms     = [math]::Round($totalStopwatch.Elapsed.TotalMilliseconds, 2)
    avg_time_ms       = [math]::Round($avg, 3)
    min_time_ms       = [math]::Round($min, 3)
    max_time_ms       = [math]::Round($max, 3)
    success           = $success
    error             = $errors
    throughput_req_s  = [math]::Round($throughput, 2)
}

Write-Host ""
Write-Host "=== Resultado $Level / corrida $Run ===" -ForegroundColor Green
$row | Format-List

$existing = @()
if (Test-Path $OutCsv) {
    $existing = Import-Csv $OutCsv
}
$existing += $row
$existing | Export-Csv -Path $OutCsv -NoTypeInformation -Encoding UTF8
Write-Host "Guardado en $OutCsv" -ForegroundColor Green
