cmake_minimum_required(VERSION 3.16)

# TFLite Micro uses TF_LITE_STATIC_MEMORY to keep every buffer in the arenas and
# to pick the static versions of its structures. It changes the layout of types
# in the public headers, so the whole project has to be built with the same
# definition, exactly as ESPHome does for its micro_wake_word component.
add_compile_definitions(TF_LITE_STATIC_MEMORY)

# This repository only ever targets the one board in AGENTS.md. Pinning the
# target here means `idf.py build` works without a preceding `set-target`,
# which would otherwise write the (git-ignored) sdkconfig.
set(IDF_TARGET "esp32c6")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stavvy)
