<#
.SYNOPSIS
    Barre p01, p02, p04, p06 y p08 usando SOLO POST /followups (inserción
    individual, sin batch). Reinicia el contenedor con cada OMP_NUM_THREADS,
    confirma la config via /health, repite cada nivel varias veces, y
    consolida todo en un único CSV.

.PARAMETER RequestsPerRun
    Peticiones individuales por corrida (por defecto 2000). La variable que
    simula la cantidad de usuarios/hilos de JMeter es independiente de esto
    y se define en la herramienta de carga, no aquí — este script sirve
    para pruebas rápidas de sanidad entre niveles; la prueba grande con
    JMeter se corre aparte con la configuración fija que indicó el profesor.

.PARAMETER Repeats
    Repeticiones por nivel (3 a 5, según lo pedido).

.PARAMETER BackendThreads
    Fijo en 1 durante toda la barrida, para medir OpenMP de forma limpia
    (ver README_OPENMP_SINGLE_INSERT.md).

.EXAMPLE
    ./test_parallel_levels_single_insert.ps1 -RequestsPerRun 2000 -Repeats 5
#>
param(
    [int]$RequestsPerRun = 2000,
    [int]$Repeats = 3,
    [string]$BaseUrl = "http://localhost:8084",
    [string]$ComposeFile = "docker-compose.yml",
    [int]$BackendThreads = 1,
    [string]$OutCsv = "resultado_single_insert.csv"
)

$ErrorActionPreference = "Stop"
$levels = @(
    @{ Label = "p01"; Threads = 1 },
    @{ Label = "p02"; Threads = 2 },
    @{ Label = "p04"; Threads = 4 },
    @{ Label = "p06"; Threads = 6 },
    @{ Label = "p08"; Threads = 8 }
)

if (Test-Path $OutCsv) {
    Write-Host "Se encontro un $OutCsv previo; se le van a AGREGAR filas nuevas (no se borra)." -ForegroundColor Yellow
}

foreach ($lvl in $levels) {
    $label = $lvl.Label
    $n = $lvl.Threads

    Write-Host ""
    Write-Host "===================================================" -ForegroundColor Magenta
    Write-Host " Nivel $label  ->  OMP_NUM_THREADS=$n  BACKEND_THREADS=$BackendThreads (fijo)" -ForegroundColor Magenta
    Write-Host "===================================================" -ForegroundColor Magenta

    $env:OMP_NUM_THREADS      = "$n"
    $env:BACKEND_THREADS      = "$BackendThreads"
    $env:CF_PARALLELISM_LEVEL = $label

    Write-Host "Reconstruyendo y reiniciando el contenedor..." -ForegroundColor Cyan
    docker compose -f $ComposeFile up --build -d community-followup-service

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
        Write-Warning "El servicio no respondio a tiempo para el nivel $label. Se omite."
        continue
    }

    $confirmed = $health.openmp.omp_num_threads_configured
    if ($confirmed -ne $n) {
        Write-Warning "El servicio reporta omp_num_threads_configured=$confirmed, se esperaba $n."
    }
    Write-Host "Config confirmada -> omp_num_threads_configured=$confirmed backend_threads=$($health.backend_threads)" -ForegroundColor Green

    for ($run = 1; $run -le $Repeats; $run++) {
        Write-Host ""
        Write-Host "-- $label, corrida $run de $Repeats --" -ForegroundColor Cyan
        & "$PSScriptRoot\_single_insert_runner.ps1" -BaseUrl $BaseUrl -Requests $RequestsPerRun `
            -Level $label -Run $run -OutCsv $OutCsv
    }
}

Write-Host ""
Write-Host "=== Barrida completa. Resumen final ===" -ForegroundColor Green
if (Test-Path $OutCsv) {
    Import-Csv $OutCsv | Format-Table -AutoSize
    Write-Host "CSV completo en $OutCsv" -ForegroundColor Green
}
Write-Host ""
Write-Host "Recuerde: esta barrida rapida usa RequestsPerRun=$RequestsPerRun por corrida," -ForegroundColor Yellow
Write-Host "distinto de la prueba grande con JMeter (250k/500k/1M) que exige la consigna." -ForegroundColor Yellow
Write-Host "Sirve para confirmar que la config y el motor de evaluacion funcionan en cada" -ForegroundColor Yellow
Write-Host "nivel ANTES de lanzar la corrida grande." -ForegroundColor Yellow
