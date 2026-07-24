<#
.SYNOPSIS
    Prueba POST /followups/batch: genera un lote sintetico, lo valida/normaliza
    en paralelo y lo INSERTA de verdad en la base de datos via PostgREST.

    ADVERTENCIA: este script SI escribe filas nuevas en followup_records.
    Use test_batch_validate.ps1 si solo quiere medir CPU sin tocar datos.

.PARAMETER BaseUrl
    URL base del microservicio (por defecto http://localhost:8084).

.PARAMETER BatchSize
    Cantidad de registros sinteticos a generar e insertar (por defecto 1000).

.EXAMPLE
    ./test_batch_insert.ps1 -BatchSize 10000
#>
param(
    [string]$BaseUrl = "http://localhost:8084",
    [int]$BatchSize = 1000
)

$ErrorActionPreference = "Stop"
$statuses = @("SI_CUMPLE", "PARCIAL", "NO_CUMPLE")

function New-SyntheticRecord([int]$i) {
    [pscustomobject]@{
        family_id            = ($i % 5000) + 1
        record_number        = "INS-$i-$([guid]::NewGuid().ToString('N').Substring(0,10))"
        risk_description     = "Registro sintetico de ingreso por lote #$i"
        scheduled_activities = "Visita de control comunitario"
        compliance_status    = $statuses[$i % 3]
        analysis_date        = (Get-Date).ToString("yyyy-MM-dd")
        evaluation_date      = (Get-Date).AddDays(15).ToString("yyyy-MM-dd")
    }
}

Write-Host "Este script va a INSERTAR $BatchSize registros nuevos en la base de datos." -ForegroundColor Yellow
Write-Host "record_number se genera con un sufijo aleatorio para evitar choques con la" -ForegroundColor Yellow
Write-Host "restriccion UNIQUE de la tabla al correr el script varias veces." -ForegroundColor Yellow
Write-Host ""

Write-Host "Generando $BatchSize registros sinteticos..." -ForegroundColor Cyan
$records = 1..$BatchSize | ForEach-Object { New-SyntheticRecord $_ }
$body = @{ records = $records } | ConvertTo-Json -Depth 6 -Compress

Write-Host "Enviando POST $BaseUrl/followups/batch ..." -ForegroundColor Cyan
$clientStopwatch = [System.Diagnostics.Stopwatch]::StartNew()
try {
    $response = Invoke-RestMethod -Uri "$BaseUrl/followups/batch" -Method Post `
        -ContentType "application/json" -Body $body -TimeoutSec 600
}
catch {
    Write-Error "Fallo la llamada al endpoint: $_"
    exit 1
}
$clientStopwatch.Stop()

Write-Host ""
Write-Host "=== Resultado ===" -ForegroundColor Green
Write-Host ("Tiempo total cliente (red + servidor)   : {0} ms" -f $clientStopwatch.ElapsedMilliseconds)
Write-Host ("Fase CPU (validar/normalizar/clasificar) : {0} ms" -f $response.cpu_phase_time_ms)
Write-Host ("Fase de persistencia (PostgREST)         : {0} ms" -f $response.persist_phase_time_ms)
Write-Host ("Tiempo total reportado por el servidor   : {0} ms" -f $response.total_time_ms)
Write-Host ("Registros recibidos                      : {0}" -f $response.records_received)
Write-Host ("Registros validos                        : {0}" -f $response.records_valid)
Write-Host ("Registros invalidos                      : {0}" -f $response.records_invalid)
Write-Host ("Registros insertados                     : {0}" -f $response.records_inserted)
Write-Host ("Paralelismo activo                       : {0}" -f $response.parallel_enabled)
Write-Host ("Hilos usados                              : {0}" -f $response.threads_used)
Write-Host ("Etiqueta de paralelismo                  : {0}" -f $response.parallelism_label)
Write-Host ("Registros por segundo (extremo a extremo): {0:N1}" -f $response.records_per_second)

if ($response.insert_errors -and $response.insert_errors.Count -gt 0) {
    Write-Host ""
    Write-Host "Errores durante la persistencia:" -ForegroundColor Red
    $response.insert_errors | ForEach-Object { Write-Host "  - $_" }
}
