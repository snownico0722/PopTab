"""Smoke-test native messaging framing without a browser UI (Windows only)."""
import json
import struct
import subprocess
import sys

executable = sys.argv[1]
messages = [
    {"type": "sync", "windows": []},
    {"type": "transfer", "from": 1, "to": 2},
    {"type": "sync", "windows": [
        {"id": 100, "left": 120, "top": 80, "width": 700, "height": 400}
    ]},
    {"type": "sync", "windows": []},
]
request = b"".join(
    struct.pack("<I", len(payload := json.dumps(msg, separators=(",", ":")).encode("utf-8")))
    + payload for msg in messages
)
result = subprocess.run(
    [executable, "--host-test"],
    input=request,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    timeout=15,
    check=True,
)
output = result.stdout
assert len(output) >= 4, "native host sent no framed output"
size = struct.unpack("<I", output[:4])[0]
assert size <= 65536 and len(output) >= 4 + size
first = json.loads(output[4:4 + size].decode("utf-8"))
assert first == {"type": "ready"}, first
print("Native Messaging smoke test passed")
