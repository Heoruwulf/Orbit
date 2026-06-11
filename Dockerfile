# ==========================================
# Stage 1: Builder
# ==========================================
FROM gcc:14 AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    clang \
    cmake \
    liburing-dev \
    libhiredis-dev \
    librdkafka-dev \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /src

# Copy only the necessary files for the build
COPY CMakeLists.txt .
COPY include/ ./include/
COPY src/ ./src/
COPY scripts/generate_version.cmake ./scripts/

ARG ENABLE_EVENT_REDIS=ON
ARG ENABLE_EVENT_KAFKA=ON
ARG ENABLE_EVENT_UDP=ON

# Build the project
# - Release mode optimizes the binary.
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release \
             -DENABLE_EVENT_REDIS=${ENABLE_EVENT_REDIS} \
             -DENABLE_EVENT_KAFKA=${ENABLE_EVENT_KAFKA} \
             -DENABLE_EVENT_UDP=${ENABLE_EVENT_UDP} && \
    make -j$(nproc) orbit

# Collect runtime dependencies into a staging directory
RUN mkdir -p /dist/usr/local/bin && \
    cp /src/build/src/orbit /dist/usr/local/bin/orbit && \
    mkdir -p /dist/usr/lib/x86_64-linux-gnu && \
    for lib in $(ldd /dist/usr/local/bin/orbit | grep "=> /" | awk '{print $3}' | grep -E -v "/(libc\.so|libm\.so|libpthread\.so|libdl\.so|librt\.so|libgcc_s\.so|libstdc\+\+\.so|ld-linux|libssl\.so|libcrypto\.so)"); do \
        cp -L "$lib" /dist/usr/lib/x86_64-linux-gnu/; \
    done

# ==========================================
# Stage 2: Runtime
# ==========================================
FROM gcr.io/distroless/cc-debian13:nonroot

# Copy everything from the staging directory
COPY --from=builder /dist /

# Explicitly expose commonly used ports for clarity
EXPOSE 5060/udp
EXPOSE 8080/tcp
EXPOSE 9000/udp

# Execute the binary directly, as orbit uses environment variables rather than CLI arguments.
ENTRYPOINT ["/usr/local/bin/orbit"]
