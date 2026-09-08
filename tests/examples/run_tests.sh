#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
proto_dir=$(mktemp -d)
trap 'rm -rf "$proto_dir"' EXIT
protoc -I "$repo/protos" --python_out="$proto_dir" "$repo/protos/messages.proto"
export PYTHONPATH="$proto_dir${PYTHONPATH:+:$PYTHONPATH}"
python -c 'import messages_pb2'
python -m unittest discover -s "$repo/tests/examples" -v
