FROM debian:bookworm-slim AS build
RUN dpkg --add-architecture armhf \
 && apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl git bzip2 xz-utils \
    build-essential g++-arm-linux-gnueabihf cmake ninja-build python3 pkg-config
WORKDIR /src
COPY . .
RUN bash tools/build-armhf.sh
FROM scratch AS artifact
COPY --from=build /src/dist/ /
