<#
.SYNOPSIS
    Barre los niveles p01, p02, p04, p06 y p08: reinicia el contenedor con
    OMP_NUM_THREADS distinto en cada paso, espera a que /health responda,
    envia el mismo lote sintetico a /followups/batch/validate y guarda un
    resumen comparativo en CSV.

    OMP_NUM_THREADS es una variable de entorno leida UNA sola vez al iniciar
    el proceso (ver parallel_config.cpp) -> es indispensable recrear el
    contenedor en cada nivel; no alcanza con cambiar la variable en caliente.

.PARAMETER BatchSize
    Tamano del lote de prueba en cada nivel (por defecto 5000).

.PARAMETER BaseUrl
    URL base del microservicio.

.PARAMETER ComposeFile
    Ruta al docker-compose.yml (por defecto ./docker-compose.yml, ejecutar
    el script desde la raiz del proyecto o ajustar esta ruta).

.PARAMETER BackendThreads
    Valor fijo para BACKEND_THREADS durante toda la barrida. Dejar en 1
    para medir el paralelismo OpenMP de forma "limpia" (ver
    README_OPENMP_OPTIMIZATION.md, seccion de configuracion para pruebas).

.EXAMPLE
    ./test_parallel_levels.ps1 -BatchSize 10000 -BackendThreads 1
#>
param(
    [int]$BatchSize = 5000,
    [string]$BaseUrl = "http://localhost:8084",
    [string]$ComposeFile = "docker-compose.yml",
    [int]$BackendThreads = 1
)

$ErrorActionPreference = "Stop"
$levels = @(1, 2, 4, 6, 8)
$results = @()
$scriptDir = $PSScriptRoot

foreach ($n in $levels) {
    $label = "p{0:D2}" -f $n
    Write-Host ""
    Write-Host "===================================================" -ForegroundColor Magenta
    Write-Host " Nivel $label  ->  OMP_NUM_THREADS=$n  BACKEND_THREADS=$BackendThreads" -ForegroundColor Magenta
    Write-Host "===================================================" -ForegroundColor Magenta

    $env:OMP_NUM_THREADS      = "$n"
    $env:BACKEND_THREADS      = "$BackendThreads"
    $env:CF_PARALLELISM_LEVEL = $label

    Write-Host "Reconstruyendo y reiniciando el contenedor..." -ForegroundColor Cyan
    docker compose -f $ComposeFile up --build -d community-followup-service

    Write-Host "Esperando a que /health responda con la configuracion esperada..." -ForegroundColor Cyan
    $health = $null
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Seconds 2
        try {
            $candidate = Invoke-RestMethod -Uri "$BaseUrl/health" -Method Get -TimeoutSec 3
            if ($candidate.status -eq "ok") { $health = $candidate; break }
        }
        catch { }
    }

    if ($null -eq $health) {
        Write-Warning "El servicio no respondio a tiempo para el nivel $label. Se omite esta corrida."
        continue
    }

    $confirmedThreads = $health.openmp.omp_num_threads_configured
    if ($confirmedThreads -ne $n) {
        Write-Warning "El servicio reporta omp_num_threads_configured=$confirmedThreads, se esperaba $n. Revise las variables de entorno del contenedor."
    }
    Write-Host "Config confirmada por /health -> omp_num_threads_configured=$confirmedThreads, backend_threads=$($health.backend_threads)" -ForegroundColor Green

    $statuses = @("SI_CUMPLE", "PARCIAL", "NO_CUMPLE")
    $records = 1..$BatchSize | ForEach-Object {
        [pscustomobject]@{
            family_id         = ($_ % 5000) + 1
            record_number     = "SWEEP-$n-$_-$([guid]::NewGuid().ToString('N').Substring(0,8))"
            compliance_status = $statuses[$_ % 3]
        }
    }
    $body = @{ records = $records } | ConvertTo-Json -Depth 6 -Compress

    try {
        $resp = Invoke-RestMethod -Uri "$BaseUrl/followups/batch/validate" -Method Post `
            -ContentType "application/json" -Body $body -TimeoutSec 300
    }
    catch {
        Write-Warning "Fallo la llamada de validacion en el nivel $label -> $_"
        continue
    }

    $results += [pscustomobject]@{
        Nivel                 = $label
        OmpNumThreads         = $confirmedThreads
        BackendThreads        = $health.backend_threads
        RegistrosEnviados     = $resp.records_received
        RegistrosValidos      = $resp.records_valid
        HilosUsados           = $resp.threads_used
        ParalelismoActivo     = $resp.parallel_enabled
        TiempoProcesamientoMs = $resp.processing_time_ms
        RegistrosPorSegundo   = [math]::Round($resp.records_per_second, 1)
    }

    Write-Host ("Nivel {0}: {1} ms  ({2:N0} registros/seg)" -f $label, $resp.processing_time_ms, $resp.records_per_second) -ForegroundColor Green
}

Write-Host ""
Write-Host "=== Resumen de la barrida p01 -> p08 ===" -ForegroundColor Green
$results | Format-Table -AutoSize

$outFile = Join-Path $scriptDir "resultado_barrida_openmp.csv"
$results | Export-Csv -Path $outFile -NoTypeInformation -Encoding UTF8
Write-Host "Guardado en $outFile" -ForegroundColor Green
Write-Host ""
Write-Host "Recuerde: si p08 no gano mucho (o perdio) frente a p04/p06, no es" -ForegroundColor Yellow
Write-Host "necesariamente un error -- puede ser overhead u overhead de" -ForegroundColor Yellow
Write-Host "sobreparalelismo. Vea README_OPENMP_OPTIMIZATION.md." -ForegroundColor Yellow
