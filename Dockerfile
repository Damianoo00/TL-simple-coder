# syntax=docker/dockerfile:1
FROM node:22-bookworm AS web
WORKDIR /app
COPY frontend/package*.json frontend/
RUN npm ci --prefix frontend
COPY frontend/ frontend/
RUN npm test --prefix frontend && npm run build --prefix frontend
COPY overlay-renderer/package*.json overlay-renderer/
RUN npm ci --prefix overlay-renderer && npm run setup --prefix overlay-renderer
COPY overlay-renderer/ overlay-renderer/

FROM debian:bookworm AS backend
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake python3 python3-venv pkg-config nasm yasm \
    autoconf automake libtool ca-certificates git ffmpeg \
    && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /opt/conan && /opt/conan/bin/pip install --no-cache-dir 'conan>=2,<3'
ENV PATH="/opt/conan/bin:${PATH}"
WORKDIR /app
COPY backend/conanfile.txt backend/conanfile.txt
RUN conan profile detect && conan install backend --output-folder=build/conan \
    --build=missing -s build_type=Release -c tools.build:jobs=2
COPY CMakeLists.txt ./
COPY backend/ backend/
RUN cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=conan/conan_toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j2 \
    && ctest --test-dir build --output-on-failure

FROM node:22-bookworm-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    xvfb xauth libgtk-3-0 libnss3 libasound2 libgbm1 libdrm2 \
    libatk-bridge2.0-0 libcups2 libxss1 libxtst6 libxkbcommon0 \
    libxcomposite1 libxdamage1 libxrandr2 ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=backend /app/build/supervisor /app/build/graph_worker ./build/
COPY --from=web /app/frontend/dist ./frontend/dist/
COPY --from=web /app/overlay-renderer ./overlay-renderer/
COPY examples/ examples/
COPY config.json ./
# Chromium uses its setuid helper; keep the sandbox enabled and run as non-root.
RUN chown root:root overlay-renderer/node_modules/electron/dist/chrome-sandbox \
    && chmod 4755 overlay-renderer/node_modules/electron/dist/chrome-sandbox \
    && mkdir -p /data/graphs /data/media \
    && chown -R node:node /data
ENV ASYNC_NODES_BIND_ADDRESS=0.0.0.0 \
    ASYNC_NODES_GRAPH_DIR=/data/graphs
USER node
EXPOSE 8080
CMD ["xvfb-run", "-a", "--server-args=-screen 0 1920x1080x24 -nolisten tcp", "/app/build/supervisor", "/app/build/graph_worker"]
