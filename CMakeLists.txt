cmake_minimum_required(VERSION 3.16)

# This repository only ever targets the one board in AGENTS.md. Pinning the
# target here means `idf.py build` works without a preceding `set-target`,
# which would otherwise write the (git-ignored) sdkconfig.
set(IDF_TARGET "esp32c6")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(amoledesp32)
