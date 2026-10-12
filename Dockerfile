# --- build stage ---
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc make libc6-dev libcjson-dev libsqlite3-dev libssl-dev libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile .
COPY src ./src
RUN make

# --- runtime stage ---
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        libcjson1 libsqlite3-0 libssl3 libcurl4 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 mpesa \
    && mkdir /data && chown mpesa /data
COPY --from=build /src/mpesa_webhook /usr/local/bin/mpesa_webhook
USER mpesa
WORKDIR /data
ENV DB_PATH=/data/mpesa.db BIND_ADDR=0.0.0.0 PORT=8080
VOLUME /data
EXPOSE 8080
CMD ["mpesa_webhook"]
