FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make libpq-dev ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel 2
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libpq5 ca-certificates && rm -rf /var/lib/apt/lists/* && useradd -r -u 10001 ledger
COPY --from=build /src/build/ledger_server /usr/local/bin/ledger_server
USER ledger
ENV HOST=0.0.0.0 PORT=8080
EXPOSE 8080
ENTRYPOINT ["ledger_server"]
