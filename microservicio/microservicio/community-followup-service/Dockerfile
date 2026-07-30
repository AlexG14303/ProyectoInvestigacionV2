FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    cmake ninja-build build-essential \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY vendor/ vendor/
COPY shared/ shared/
COPY community-followup-service/ community-followup-service/
COPY CMakeLists.txt CMakeLists.txt

RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target community-followup-service

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    libgomp1 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Usuario no-root: si el proceso llegara a comprometerse, el atacante
# queda limitado a los permisos de "appuser" en vez de tener root dentro
# del contenedor (menos superficie para escapes de contenedor y para
# tocar archivos/recursos que no le corresponden).
RUN groupadd --system appgroup && useradd --system --gid appgroup --no-create-home appuser

COPY --from=builder /app/build/community-followup-service/community-followup-service /usr/local/bin/community-followup-service

USER appuser

EXPOSE 8084
CMD ["community-followup-service"]
