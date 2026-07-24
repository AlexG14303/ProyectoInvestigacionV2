<#
.SYNOPSIS
    Prueba POST /followups/batch/validate: genera un lote sintetico y mide
    cuanto tarda el microservicio en validar/normalizar/clasificar TODO el
    lote (fase 100% CPU, sin tocar la base de datos).

.PARAMETER BaseUrl
    URL base del microservicio (por defecto http://localhost:8084).

.PARAMETER BatchSize
    Cantidad de registros sinteticos a generar (por defecto 1000).
    Pensado para 1000, 5000 o 10000 segun lo pedido.

.EXAMPLE
    ./test_batch_validate.ps1 -BatchSize 5000
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
        record_number        = "TEST-$i-$([guid]::NewGuid().ToString('N').Substring(0,8))"
        risk_description     = "Registro sintetico de prueba #$i"
        scheduled_activities = "Visita de control comunitario"
        compliance_status    = $statuses[$i % 3]
        analysis_date        = (Get-Date).ToString("yyyy-MM-dd")
        evaluation_date      = (Get-Date).AddDays(15).ToString("yyyy-MM-dd")
    }
}

Write-Host "Generando $BatchSize registros sinteticos..." -ForegroundColor Cyan
$records = 1..$BatchSize | ForEach-Object { New-SyntheticRecord $_ }

Write-Host "Serializando a JSON..." -ForegroundColor Cyan
$body = @{ records = $records } | ConvertTo-Json -Depth 6 -Compress

Write-Host "Enviando POST $BaseUrl/followups/batch/validate ..." -ForegroundColor Cyan
$clientStopwatch = [System.Diagnostics.Stopwatch]::StartNew()
try {
    $response = Invoke-RestMethod -Uri "$BaseUrl/followups/batch/validate" -Method Post `
        -ContentType "application/json" -Body $body -TimeoutSec 300
}
catch {
    Write-Error "Fallo la llamada al endpoint: $_"
    exit 1
}
$clientStopwatch.Stop()

Write-Host ""
Write-Host "=== Resultado ===" -ForegroundColor Green
Write-Host ("Tiempo total cliente (red + servidor) : {0} ms" -f $clientStopwatch.ElapsedMilliseconds)
Write-Host ("Tiempo de procesamiento reportado (CPU): {0} ms" -f $response.processing_time_ms)
Write-Host ("Registros recibidos                    : {0}" -f $response.records_received)
Write-Host ("Registros validos                      : {0}" -f $response.records_valid)
Write-Host ("Registros invalidos                     : {0}" -f $response.records_invalid)
Write-Host ("Paralelismo activo                      : {0}" -f $response.parallel_enabled)
Write-Host ("Hilos usados                            : {0}" -f $response.threads_used)
Write-Host ("Etiqueta de paralelismo                 : {0}" -f $response.parallelism_label)
Write-Host ("Registros por segundo                   : {0:N1}" -f $response.records_per_second)
Write-Host ("Resumen de riesgo                       : Alto={0} Medio={1} Bajo={2}" -f `
    $response.risk_summary.alto, $response.risk_summary.medio, $response.risk_summary.bajo)

if ($response.sample_errors -and $response.sample_errors.Count -gt 0) {
    Write-Host ""
    Write-Host "Muestra de errores de validacion (hasta 10):" -ForegroundColor Yellow
    $response.sample_errors | ForEach-Object { Write-Host "  - $_" }
}
