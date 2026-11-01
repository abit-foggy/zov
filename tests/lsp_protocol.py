# Copyright (c) 2026 foggy (abit-foggy)
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

# Drives zov-lsp through a real JSON-RPC 2.0 session: initialize,
# didOpen over a document with a borrow error, completion, shutdown, exit.
import json
import re
import subprocess
import sys
import os

tmpdir = sys.argv[1]
lsp_bin = sys.argv[2]
fixture = sys.argv[3]


def frame(obj):
    body = json.dumps(obj).encode()
    return b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body


# broken doc: assignment to an immutable binding (borrow error)
broken_uri = "file://" + os.path.join(tmpdir, "lsp_broken.zov").replace(os.sep, "/")
broken_text = (
    "export fn main() i32 = {\n"
    "    let x: i32 = 1;\n"
    "    x = 2;\n"
    "    return x;\n"
    "};\n"
)

session = b""
session += frame({"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}})
session += frame({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": broken_uri, "languageId": "zov", "version": 1, "text": broken_text}}})
session += frame({"jsonrpc": "2.0", "id": 1, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": broken_uri}, "position": {"line": 2, "character": 4}}})
session += frame({"jsonrpc": "2.0", "id": 2, "method": "shutdown"})
session += frame({"jsonrpc": "2.0", "method": "exit"})

proc = subprocess.run([lsp_bin], input=session, capture_output=True, timeout=60)
if proc.returncode != 0:
    print("lsp exited %d" % proc.returncode)
    sys.exit(1)

msgs = []
pos = 0
while True:
    m = re.compile(rb"Content-Length: (\d+)\r\n\r\n").match(proc.stdout, pos)
    if not m:
        break
    n = int(m.group(1))
    start = m.end()
    msgs.append(json.loads(proc.stdout[start:start + n]))
    pos = start + n

# 1: initialize result with capabilities
init = next((m for m in msgs if m.get("id") == 0), None)
if not init or "capabilities" not in init.get("result", {}):
    print("missing initialize capabilities:", init)
    sys.exit(1)
caps = init["result"]["capabilities"]
if caps.get("textDocumentSync") != 1 or not caps.get("hoverProvider"):
    print("wrong capabilities:", caps)
    sys.exit(1)

# 2: publishDiagnostics carrying the borrow error on the assignment line
diag = next((m for m in msgs if m.get("method") == "textDocument/publishDiagnostics"
             and m["params"]["uri"] == broken_uri), None)
if not diag:
    print("no publishDiagnostics for", broken_uri)
    sys.exit(1)
entries = diag["params"]["diagnostics"]
if len(entries) != 1:
    print("expected 1 diagnostic, got", len(entries))
    sys.exit(1)
d = entries[0]
if "immutable binding" not in d["message"]:
    print("wrong diagnostic message:", d["message"])
    sys.exit(1)
if d["range"]["start"]["line"] != 2:
    print("expected 0-based line 2, got", d["range"]["start"]["line"])
    sys.exit(1)

# 3: completion items include the spec'd keywords and modules
comp = next((m for m in msgs if m.get("id") == 1), None)
if not comp or not isinstance(comp.get("result"), list):
    print("missing completion result:", comp)
    sys.exit(1)
labels = {item["label"] for item in comp["result"]}
for want in ("orelse", "resource", "std::io", "std::vec", "std::map",
             "push", "getref", "findref", "free", "i32", "defer",
             "hashval", "fromowned", "std::proc"):
    if want not in labels:
        print("completion missing", want)
        sys.exit(1)

# 4: shutdown ack
bye = next((m for m in msgs if m.get("id") == 2), None)
if not bye or bye.get("result") is not None:
    print("missing shutdown ack:", bye)
    sys.exit(1)

sys.exit(0)
