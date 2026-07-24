<#
.SYNOPSIS
    Prueba de insercion individual (SIN batch) para el nivel p02 (OMP_NUM_THREADS=2).

    IMPORTANTE: antes de correr este script, levante el contenedor con:
        OMP_NUM_THREADS=2 BACKEND_THREADS=1 CF_PARALLELISM_LEVEL=p02 docker compose up --build -d

    OMP_NUM_THREADS se lee una sola vez al iniciar el proceso -> no alcanza
    con cambiar la variable sin recrear el contenedor.
#>
param(
    [string]$BaseUrl = "http://localhost:8084",
    [int]$Requests = 2000,
    [int]$Run = 1,
    [string]$OutCsv = "resultado_single_insert.csv"
)

& "$PSScriptRoot\_single_insert_runner.ps1" -BaseUrl $BaseUrl -Requests $Requests -Level "p02" -Run $Run -OutCsv $OutCsv
