#!/bin/bash
# The requests of the update, on a PC, with the libraries of the console: libcurl 8.4.0 (the version of devkitPro's 3ds-curl), built with mbedTLS 2.28 and without a threaded resolver, as
# there. Run from AzaharLink/. It needs Docker and, for the part against GitHub, internet (--no-github leaves that out). The first run builds an image with that curl.
#   1. against Tests/update_mock_server.py: redirects, broken answers, a stop, silences and bad certificates (made here, with a CA of this run)
#   2. against GitHub with the list of roots the app carries (ConfigApp/certs/cacert.pem): the question, the CIA, the 3DSX, the summary
#   3. against GitHub with the console's clock wrong, in 2011 and in 2040: it must say that it is the clock
set -euo pipefail

if [ "${AZL_UPDATE_HTTP_INNER:-}" != "1" ]; then
  IMAGE=azl-update-test:1
  if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build -q -t "$IMAGE" - <<'DOCKERFILE'
FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends g++ make libmbedtls-dev zlib1g-dev python3 openssl faketime ca-certificates curl xz-utils >/dev/null \
 && curl -fsSLO https://curl.se/download/curl-8.4.0.tar.xz && tar xf curl-8.4.0.tar.xz && cd curl-8.4.0 \
 && ./configure --prefix=/opt/curl --disable-shared --with-mbedtls --with-zlib --disable-threaded-resolver --disable-ldap --disable-manual --without-libpsl --without-brotli --without-zstd --without-nghttp2 --without-libidn2 >/dev/null \
 && make -j8 install >/dev/null && cd .. && rm -rf curl-8.4.0 curl-8.4.0.tar.xz
DOCKERFILE
  fi
  MSYS_NO_PATHCONV=1 docker run --rm -e AZL_UPDATE_HTTP_INNER=1 -v "$(pwd -W 2>/dev/null || pwd):/src:ro" -w /src "$IMAGE" bash Tests/update_http_test.sh "$@"
  exit $?
fi

WORK=$(mktemp -d)
g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -pthread -fsanitize=address,undefined -I/opt/curl/include -IConfigApp/source \
  Tests/update_http_test.cpp ConfigApp/source/update_http.cpp /opt/curl/lib/libcurl.a -lmbedtls -lmbedx509 -lmbedcrypto -lz -o "$WORK/test"
# (without the sanitizers for the run under faketime: it replaces functions of libc the address sanitizer wants for itself)
g++ -std=c++17 -O1 -g -pthread -I/opt/curl/include -IConfigApp/source \
  Tests/update_http_test.cpp ConfigApp/source/update_http.cpp /opt/curl/lib/libcurl.a -lmbedtls -lmbedx509 -lmbedcrypto -lz -o "$WORK/test_plain"

# ---- the certificates of this run
cd "$WORK"
make_ca() { # make_ca <name>
  openssl ecparam -name prime256v1 -genkey -noout -out "$1.key" 2>/dev/null
  openssl req -x509 -new -key "$1.key" -subj "/CN=$1" -days 7300 -out "$1.pem" -sha256 2>/dev/null
}
make_cert() { # make_cert <name> <ca> <host> <when: "" or a date for faketime>
  openssl ecparam -name prime256v1 -genkey -noout -out "$1.key" 2>/dev/null
  openssl req -new -key "$1.key" -subj "/CN=$3" -out "$1.csr" 2>/dev/null
  printf 'subjectAltName=DNS:%s\n' "$3" > "$1.ext"
  local sign=(openssl x509 -req -in "$1.csr" -CA "$2.pem" -CAkey "$2.key" -CAcreateserial -days 30 -sha256 -extfile "$1.ext" -out "$1.crt")
  if [ -n "$4" ]; then
    faketime "$4" "${sign[@]}" 2>/dev/null
  else
    "${sign[@]}" 2>/dev/null
  fi
}
make_ca testca
make_ca strangerca
# (the CA of the test is itself valid from long before to long after every date used below)
faketime '2000-01-01' openssl req -x509 -new -key testca.key -subj "/CN=testca" -days 18000 -out testca.pem -sha256 2>/dev/null
make_cert good testca localhost ""
make_cert expired testca localhost "2020-01-01"
make_cert future testca localhost "2040-01-01"
make_cert otherhost testca other.example ""
make_cert otherca strangerca localhost ""
cd /src

# ---- 1. the mock
PORT=18443
python3 Tests/update_mock_server.py "$PORT" "$WORK" > "$WORK/server.log" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null || true' EXIT
for _ in $(seq 1 50); do
  grep -q ready "$WORK/server.log" 2>/dev/null && break
  sleep 0.1
done
ASAN_OPTIONS=detect_leaks=1 "$WORK/test" mock "$WORK/testca.pem" "$PORT"

if [ "${1:-}" = "--no-github" ]; then
  echo "update_http_test: GitHub left out"
  exit 0
fi

# ---- 2. and 3. GitHub
"$WORK/test" github ConfigApp/certs/cacert.pem ok
faketime '2011-06-01 12:00:00' "$WORK/test_plain" github ConfigApp/certs/cacert.pem clock
faketime '2040-06-01 12:00:00' "$WORK/test_plain" github ConfigApp/certs/cacert.pem clock
echo "update_http_test: all passed"
