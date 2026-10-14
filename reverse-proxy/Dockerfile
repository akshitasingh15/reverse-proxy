# Multi-stage build: compile in a full toolchain image, ship a slim runtime image.
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ make && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY Makefile .
COPY src ./src
RUN make proxy

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /app/proxy /app/proxy
COPY config ./config
RUN useradd --no-create-home --shell /usr/sbin/nologin proxyuser
USER proxyuser
EXPOSE 8080
ENTRYPOINT ["/app/proxy"]
CMD ["config/docker.conf"]
