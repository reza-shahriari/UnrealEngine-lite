#!/bin/bash

set -eu -x

SCRIPT_DIR=$(cd "$(dirname "$BASH_SOURCE")" ; pwd)

# purge old LibC++ files from tree
rm -rf ${SCRIPT_DIR}/../../../../Source/ThirdParty/Unix/LibCxx/include/*
find ${SCRIPT_DIR}/../../../../Source/ThirdParty/Unix/LibCxx/lib -type f -delete

# copy new LibC++ files into place
# grab aarch64 includes because the x86_64 ones are identical, but get built in a weird place due to how they're built
cp -rf build/install-libc++-aarch64-unknown-linux-gnueabi/include ${SCRIPT_DIR}/../../../../Source/ThirdParty/Unix/LibCxx
cp build/install-libc++-x86_64-unknown-linux-gnu/lib/{libc++.a,libc++abi.a} ${SCRIPT_DIR}/../../../../Source/ThirdParty/Unix/LibCxx/lib/Unix/x86_64-unknown-linux-gnu
cp build/install-libc++-aarch64-unknown-linux-gnueabi/lib/{libc++.a,libc++abi.a} ${SCRIPT_DIR}/../../../../Source/ThirdParty/Unix/LibCxx/lib/Unix/aarch64-unknown-linux-gnueabi

