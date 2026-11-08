#!/bin/sh
# Regenerates esphome_api.pb.c and esphome_api.pb.h from esphome_api.proto.
#
# The ESP-IDF Docker image has neither protobuf nor the nanopb generator, so the
# generated files are committed and the normal build never runs this. The
# nanopb PyPI package bundles a matching protoc, which keeps the only host
# prerequisite down to python3.
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
proto_directory="$project_root/main/proto"
# A throwaway virtualenv keeps the generator's protobuf dependency out of the
# host Python installation; every invocation starts from a known-good version.
virtualenv_directory="${TMPDIR:-/tmp}/stavvy-nanopb-venv"

python3 -m venv "$virtualenv_directory"
"$virtualenv_directory/bin/pip" install --quiet --upgrade 'nanopb==0.4.9.1'
"$virtualenv_directory/bin/nanopb_generator" -I "$proto_directory" \
    --output-dir="$proto_directory" "$proto_directory/esphome_api.proto"
