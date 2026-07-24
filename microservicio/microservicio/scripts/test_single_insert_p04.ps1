<#
.SYNOPSIS
    Prueba de insercion individual (SIN batch) para el nivel p04 (OMP_NUM_THREADS=4).

    IMPORTANTE: antes de correr este script, levante el contenedor con:
        OMP_NUM_THREADS=4 BACKEND_THREADS=1 CF_PARALLELISM_LEVEL=p04 docker compose up --build -d

    OMP_NUM_THREADS se lee una sola vez al iniciar el proceso -> no alcanza
    con cambiar la variable sin recrear el contenedor.
#>
param(
    [string]$BaseUrl = "http://localhost:8084",
    [int]$Requests = 2000,
    [int]$Run = 1,
    [string]$OutCsv = "resultado_single_insert.csv"
)

& "$PSScriptRoot\_single_insert_runner.ps1" -BaseUrl $BaseUrl -Requests $Requests -Level "p04" -Run $Run -OutCsv $OutCsv
