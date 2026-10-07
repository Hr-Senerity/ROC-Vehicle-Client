FROM scratch
ARG VEHICLE_REVISION
LABEL org.opencontainers.image.title="ROC Vehicle isolated test runtime" \
      org.opencontainers.image.revision="${VEHICLE_REVISION}" \
      org.roc.system.revision="a9c601d6977d89000438854a24e3199573004a2f"
COPY rootfs/ /
USER 1000:1000
WORKDIR /
ENV SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
ENV SSL_CERT_DIR=/etc/ssl/certs
ENTRYPOINT ["/opt/roc-vehicle/roc_vehicle_driver"]
CMD ["--help"]
