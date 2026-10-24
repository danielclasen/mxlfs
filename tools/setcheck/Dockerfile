ARG GO_MXL_TAG=1.1.0-rc.7
FROM ghcr.io/qvest-digital/go-mxl-builder:${GO_MXL_TAG} AS builder
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY main.go ./
RUN go build -o /out/setcheck .

FROM ghcr.io/qvest-digital/go-mxl-runtime:${GO_MXL_TAG}
COPY --from=builder /out/setcheck /usr/local/bin/setcheck
ENTRYPOINT ["/usr/local/bin/setcheck"]
