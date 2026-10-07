FROM ubuntu@sha256:b8b6ee6aa931ecd9d0d952abc34dc0e5f7c6a30c6bb71b079fe399fde0329c02

ARG DEBIAN_FRONTEND=noninteractive
ARG DEV_UID=1000
ARG DEV_GID=1000
ARG BOOST_VERSION=1.92.0
ARG BOOST_VERSION_UNDERSCORE=1_92_0
ARG BOOST_SHA256=5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c
ARG JSONCONS_VERSION=1.7.0
ARG JSONCONS_SHA256=5a2aad4e791a1c93b0b0b326973459753a96a4c48d06d3035cd0ea0d262198d4
ARG OPENSSL_VERSION=3.5.8
ARG OPENSSL_SHA256=a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2
ARG SQLITE_VERSION_NUMBER=3530400
ARG SQLITE_SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        build-essential \
        bzip2 \
        ca-certificates \
        clang \
        cmake \
        curl \
        git \
        ninja-build \
        perl \
        pkg-config \
        python3 \
        unzip \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/roc-deps/include /opt/roc-deps/lib \
    && curl --fail --location --silent --show-error \
        "https://archives.boost.io/release/${BOOST_VERSION}/source/boost_${BOOST_VERSION_UNDERSCORE}.tar.bz2" \
        --output /tmp/boost.tar.bz2 \
    && echo "${BOOST_SHA256}  /tmp/boost.tar.bz2" | sha256sum --check --strict \
    && tar --extract --bzip2 --file /tmp/boost.tar.bz2 --directory /tmp \
    && cp -R "/tmp/boost_${BOOST_VERSION_UNDERSCORE}/boost" /opt/roc-deps/include/ \
    && curl --fail --location --silent --show-error \
        "https://github.com/danielaparker/jsoncons/archive/refs/tags/v${JSONCONS_VERSION}.tar.gz" \
        --output /tmp/jsoncons.tar.gz \
    && echo "${JSONCONS_SHA256}  /tmp/jsoncons.tar.gz" | sha256sum --check --strict \
    && tar --extract --gzip --file /tmp/jsoncons.tar.gz --directory /tmp \
    && cp -R "/tmp/jsoncons-${JSONCONS_VERSION}/include/." /opt/roc-deps/include/ \
    && curl --fail --location --silent --show-error \
        "https://www.sqlite.org/2026/sqlite-amalgamation-${SQLITE_VERSION_NUMBER}.zip" \
        --output /tmp/sqlite.zip \
    && echo "${SQLITE_SHA256}  /tmp/sqlite.zip" | sha256sum --check --strict \
    && unzip -q /tmp/sqlite.zip -d /tmp \
    && cc -O2 -fPIC -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=2 \
        -c "/tmp/sqlite-amalgamation-${SQLITE_VERSION_NUMBER}/sqlite3.c" \
        -o /tmp/sqlite3.o \
    && ar rcs /opt/roc-deps/lib/libsqlite3.a /tmp/sqlite3.o \
    && install -m 0644 "/tmp/sqlite-amalgamation-${SQLITE_VERSION_NUMBER}/sqlite3.h" \
        /opt/roc-deps/include/sqlite3.h \
    && install -m 0644 "/tmp/sqlite-amalgamation-${SQLITE_VERSION_NUMBER}/sqlite3ext.h" \
        /opt/roc-deps/include/sqlite3ext.h \
    && curl --fail --location --silent --show-error \
        "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz" \
        --output /tmp/openssl.tar.gz \
    && echo "${OPENSSL_SHA256}  /tmp/openssl.tar.gz" | sha256sum --check --strict \
    && tar --extract --gzip --file /tmp/openssl.tar.gz --directory /tmp \
    && cd "/tmp/openssl-${OPENSSL_VERSION}" \
    && ./Configure linux-x86_64 no-shared no-tests \
        --prefix=/opt/roc-deps --openssldir=/opt/roc-deps/ssl --libdir=lib \
    && make -j2 \
    && make install_sw install_ssldirs \
    && rm -rf /tmp/boost.tar.bz2 /tmp/boost_* /tmp/jsoncons.tar.gz \
        /tmp/jsoncons-* /tmp/sqlite.zip /tmp/sqlite-amalgamation-* \
        /tmp/sqlite3.o /tmp/openssl.tar.gz /tmp/openssl-*

RUN groupadd --gid "${DEV_GID}" rocdev \
    && useradd --uid "${DEV_UID}" --gid "${DEV_GID}" --create-home --shell /bin/bash rocdev \
    && mkdir -p /workspace /build \
    && chown -R rocdev:rocdev /workspace /build

USER rocdev
WORKDIR /workspace

ENV HOME=/home/rocdev
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8
ENV CMAKE_PREFIX_PATH=/opt/roc-deps
ENV SSL_CERT_DIR=/etc/ssl/certs
ENV SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt

CMD ["bash"]
