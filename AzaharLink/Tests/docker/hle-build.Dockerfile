# The image the PC tests run in (name: azl-hle-build): the devkitARM image plus a 32-bit C++ toolchain (the tests that drive the plugin's IPC front-end
# are built as 32-bit, as size_t is on the console) and the libctru sources for its headers.
#   docker build -t azl-hle-build -f AzaharLink/Tests/docker/hle-build.Dockerfile .
FROM devkitpro/devkitarm
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq g++-multilib git \
 && git clone --depth 1 https://github.com/devkitPro/libctru.git /tmp/libctru
