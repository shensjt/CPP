FROM ubuntu:24.04 AS builder

ARG APT_MIRROR=archive.ubuntu.com
ARG BUILD_COMMIT=dev
ENV DEBIAN_FRONTEND=noninteractive

RUN sed -i \
        -e "s|http://archive.ubuntu.com/ubuntu/|http://${APT_MIRROR}/ubuntu/|g" \
        -e "s|http://security.ubuntu.com/ubuntu/|http://${APT_MIRROR}/ubuntu/|g" \
        /etc/apt/sources.list.d/ubuntu.sources \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential cmake python3-yaml python3-pygments \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_COMMIT="${BUILD_COMMIT}" \
    && cmake --build build/release --target web_server -j"$(nproc)" \
    && python3 blog/scripts/build.py --content blog/demo --out /out/www

FROM ubuntu:24.04

RUN useradd --create-home --home-dir /app --uid 10001 app

WORKDIR /app
COPY --from=builder /src/build/release/web_server /app/web_server
COPY --from=builder /out/www /app/www

RUN mkdir -p /app/logs && chown -R app:app /app

USER app
EXPOSE 8080

ENTRYPOINT ["/app/web_server"]
CMD ["--host", "0.0.0.0", "--port", "8080", "--root", "/app/www", "--threads", "4", "--timeout", "60"]
