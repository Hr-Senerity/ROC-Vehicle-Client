FROM scratch
ARG VEHICLE_REVISION
LABEL org.opencontainers.image.title="ROC Vehicle TEST-ONLY instrumented probe" \
      org.opencontainers.image.revision="${VEHICLE_REVISION}" \
      org.roc.system.revision="a9c601d6977d89000438854a24e3199573004a2f" \
      org.roc.vehicle.test-instrumentation="sqlite-boundary-sigkill-and-synthetic-telemetry"
COPY rootfs/ /
USER 1000:1000
WORKDIR /
ENV SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
ENV SSL_CERT_DIR=/etc/ssl/certs
ENTRYPOINT ["/opt/roc-vehicle/roc_vehicle_driver_crash_probe"]
CMD ["--help"]
