# ==========================================
# Stage 1: Builder
# ==========================================
FROM alpine:3.22.4 AS builder

# Install build dependencies
# Note: build-base includes make, clang is explicitly requested in CMakeLists.txt
RUN apk add --no-cache \
    build-base \
    clang \
    cmake \
    pkgconf \
    git \
    linux-headers \
    liburing-dev

# Set working directory
WORKDIR /src

# Copy only the necessary files for the build
COPY CMakeLists.txt .
COPY include/ ./include/
COPY src/ ./src/

# Build the project
# - Release mode optimizes the binary.
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc) orbit

# ==========================================
# Stage 2: Runtime
# ==========================================
FROM alpine:3.22.4

# Install runtime dependencies ONLY
RUN apk add --no-cache \
    liburing

# Security best practice: Create a non-root user to run the daemon
RUN addgroup -S orbit && adduser -S orbit -G orbit

# Copy the compiled binary from the builder stage
COPY --from=builder /src/build/src/orbit /usr/local/bin/orbit

# Drop privileges
USER orbit

# Explicitly expose commonly used ports for clarity
EXPOSE 5060/udp
EXPOSE 8080/tcp
EXPOSE 9000/udp

# Execute the binary directly, as orbit uses environment variables rather than CLI arguments.
ENTRYPOINT ["/usr/local/bin/orbit"]
