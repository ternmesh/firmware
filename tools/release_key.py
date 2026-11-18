#!/usr/bin/env python3
"""Release keys and signed images (draft/companion.md, "Signed images", in ternmesh/spec).

An image's signature is Ed25519 (RFC 8032) by a release key over the 16 ASCII bytes
"TERN-FIRMWARE-V1" and the SHA-256 of the file exactly as published. A release lists them all in
one file, SIGNATURES, beside its files: a line for each, the signature as 128 lower-case hex digits,
two spaces and the file's name, as SHA256SUMS lists digests. A file may instead have <file>.sig
beside it, the 64 bytes and nothing else; verify reads either. docs/signing.md says who holds the
key and how.

    python3 tools/release_key.py generate KEYFILE   make a new key; write its private half to KEYFILE
    python3 tools/release_key.py public [KEYFILE]   print a key's public half, and its line for
                                                    ports/node/release_keys.h
    python3 tools/release_key.py listed             fail unless the signing key is in release_keys.h
    python3 tools/release_key.py sign FILE...       write SIGNATURES beside the FILEs, one line each
    python3 tools/release_key.py verify FILE...     fail unless each FILE's signature, in SIGNATURES
                                                    or FILE.sig, verifies with a key in release_keys.h
    python3 tools/release_key.py selftest           check this against the specification's vectors

The private key is 64 hex digits, the Ed25519 seed. sign, public and listed read it from KEYFILE if
given with --key, else from the environment variable TERN_RELEASE_SIGNING_KEY, which is how the
release workflow gives it. With --test they use the specification's published TEST KEY instead
(tests/vectors/companion.json): anyone can sign with it, so no release is ever signed with it and it
is never in release_keys.h; verify --test checks against it alone.

Needs the `cryptography` package (pip install "cryptography>=42").
"""

import argparse
import hashlib
import json
import os
import re
import sys
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

ROOT = Path(__file__).resolve().parent.parent
KEYS_H = ROOT / "ports" / "node" / "release_keys.h"
VECTORS = ROOT / "tests" / "vectors" / "companion.json"
PREFIX = b"TERN-FIRMWARE-V1"
ENV = "TERN_RELEASE_SIGNING_KEY"
LIST = "SIGNATURES"


def read_list(text):
    """SIGNATURES's lines, by file name. Raises ValueError on a line that is not a signature and a
    name, or a name listed twice."""
    out = {}
    for line in text.splitlines():
        if not line:
            continue
        m = re.fullmatch(r"([0-9a-f]{128})  (\S+)", line)
        if not m:
            raise ValueError(f"{LIST} has a line that is not a signature and a name: {line!r}")
        if m.group(2) in out:
            raise ValueError(f"{LIST} lists {m.group(2)} twice")
        out[m.group(2)] = bytes.fromhex(m.group(1))
    return out


def signature_for(path):
    """A file's signature: its .sig beside it if there is one, else its line in SIGNATURES beside
    it; None if neither has one."""
    path = Path(path)
    sig = path.with_name(path.name + ".sig")
    if sig.exists():
        return sig.read_bytes()
    listed = path.with_name(LIST)
    if listed.exists():
        return read_list(listed.read_text(encoding="utf-8")).get(path.name)
    return None


def message(data):
    """What a release key signs for a file: the prefix and the file's SHA-256."""
    assert len(PREFIX) == 16
    return PREFIX + hashlib.sha256(data).digest()


def public_of(private):
    return private.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)


def parse_seed(text, where):
    text = text.strip()
    if not re.fullmatch(r"[0-9a-fA-F]{64}", text):
        sys.exit(f"{where}: not a release key's private half (64 hex digits)")
    return Ed25519PrivateKey.from_private_bytes(bytes.fromhex(text))


def test_seed():
    return json.loads(VECTORS.read_text(encoding="utf-8"))["release_seed"]


def signing_key(args):
    if args.test:
        return parse_seed(test_seed(), "the test key")
    if args.key:
        return parse_seed(Path(args.key).read_text(encoding="ascii"), args.key)
    text = os.environ.get(ENV, "")
    if not text.strip():
        sys.exit(f"{ENV} is not set, and no --key given: nothing to sign with. See docs/signing.md.")
    return parse_seed(text, ENV)


def c_line(public):
    """The key as release_keys.h lists it, laid out as clang-format lays it out: eleven bytes to a
    line."""
    rows = [", ".join(f"0x{b:02x}" for b in public[i:i + 11]) for i in range(0, 32, 11)]
    return "    {" + ",\n     ".join(rows) + "},"


def listed_keys():
    """The release keys ports/node/release_keys.h compiles into the firmware."""
    text = KEYS_H.read_text(encoding="utf-8")
    m = re.search(r"node_release_keys\[\]\[32\] = \{(.*?)\n\};", text, re.S)
    if not m:
        sys.exit(f"{KEYS_H}: no node_release_keys")
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    keys = []
    groups = re.findall(r"\{([^{}]*)\}", body)
    if not groups or groups[-1].strip() != "0":
        sys.exit(f"{KEYS_H}: the list does not end with {{0}}")
    for group in groups[:-1]:
        values = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", group)]
        if len(values) != 32:
            sys.exit(f"{KEYS_H}: a key that is not 32 bytes")
        keys.append(bytes(values))
    return keys


def verifies(keys, data, sig):
    if len(sig) != 64:
        return False
    for key in keys:
        try:
            Ed25519PublicKey.from_public_bytes(key).verify(sig, message(data))
            return True
        except InvalidSignature:
            pass
    return False


def generate(args):
    path = Path(args.keyfile)
    if path.exists():
        sys.exit(f"{path} exists: a release key is never overwritten")
    private = Ed25519PrivateKey.generate()
    seed = private.private_bytes_raw()
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="ascii") as f:
        f.write(seed.hex() + "\n")
    public = public_of(private)
    print(f"wrote the private key to {path} (mode 0600). Keep it offline; it is the release key.")
    print(f"public key: {public.hex()}")
    print(f"for ports/node/release_keys.h, above the list's end:\n{c_line(public)}")


def public(args):
    if args.keyfile:
        args.key = args.keyfile
    key = public_of(signing_key(args))
    print(key.hex())
    print(c_line(key))


def listed(args):
    key = public_of(signing_key(args))
    keys = listed_keys()
    if key not in keys:
        sys.exit(f"the signing key {key.hex()} is not in {KEYS_H.relative_to(ROOT)}: a release "
                 "signed with it would be refused by every node built from this tree. "
                 "See docs/signing.md.")
    print(f"the signing key {key.hex()} is release key {keys.index(key) + 1} of {len(keys)}")


def sign(args):
    private = signing_key(args)
    paths = [Path(name) for name in args.files]
    dirs = {p.resolve().parent for p in paths}
    if len(dirs) != 1:
        sys.exit("sign: the files must all be in one directory, where SIGNATURES is written")
    if len({p.name for p in paths}) != len(paths):
        sys.exit("sign: a file named twice")
    lines = [f"{private.sign(message(p.read_bytes())).hex()}  {p.name}\n"
             for p in sorted(paths, key=lambda p: p.name)]
    out = dirs.pop() / LIST
    out.write_text("".join(lines), encoding="utf-8")
    print(f"{out}: {len(lines)} signatures")


def verify(args):
    keys = [public_of(parse_seed(test_seed(), "the test key"))] if args.test else listed_keys()
    if not keys:
        sys.exit(f"{KEYS_H.relative_to(ROOT)} lists no release key: nothing can verify")
    bad = 0
    for name in args.files:
        try:
            sig = signature_for(name)
        except ValueError as e:
            sys.exit(str(e))
        ok = sig is not None and verifies(keys, Path(name).read_bytes(), sig)
        if not ok:
            print(f"{name}: {'no signature' if sig is None else 'signature does not verify'}")
            bad += 1
    if bad:
        sys.exit(f"{bad} of {len(args.files)} files are not signed by a release key")
    print(f"{len(args.files)} files signed by a release key")


def selftest(args):
    """Signs and checks as the specification's vectors do, with their test key."""
    v = json.loads(VECTORS.read_text(encoding="utf-8"))
    private = parse_seed(v["release_seed"], "the test key")
    assert public_of(private).hex() == v["release_key"]
    image = bytes.fromhex(v["image"])
    assert message(image) == PREFIX + bytes.fromhex(v["image_digest"])
    assert private.sign(message(image)).hex() == v["image_signature"]
    key = bytes.fromhex(v["release_key"])
    for case in v["signatures"]:
        assert bytes.fromhex(case["message"]) == PREFIX + bytes.fromhex(case["digest"])
        try:
            Ed25519PublicKey.from_public_bytes(key).verify(bytes.fromhex(case["signature"]),
                                                           bytes.fromhex(case["message"]))
            got = len(case["signature"]) == 128
        except InvalidSignature:
            got = False
        assert got == case["verifies"], case["why"]
    assert verifies([key], image, bytes.fromhex(v["image_signature"]))
    assert not verifies([key], image + b"\0", bytes.fromhex(v["image_signature"]))
    assert key not in listed_keys(), "the test key must never be a release key"
    print(f"release_key.py agrees with the specification's vectors; "
          f"{len(listed_keys())} release keys in {KEYS_H.relative_to(ROOT)}")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--key", help="the private key's file, rather than $" + ENV)
    p.add_argument("--test", action="store_true", help="the specification's published test key")
    sub = p.add_subparsers(dest="command", required=True)
    g = sub.add_parser("generate")
    g.add_argument("keyfile")
    g.set_defaults(run=generate)
    pub = sub.add_parser("public")
    pub.add_argument("keyfile", nargs="?")
    pub.set_defaults(run=public)
    sub.add_parser("listed").set_defaults(run=listed)
    s = sub.add_parser("sign")
    s.add_argument("files", nargs="+")
    s.set_defaults(run=sign)
    ver = sub.add_parser("verify")
    ver.add_argument("files", nargs="+")
    ver.set_defaults(run=verify)
    sub.add_parser("selftest").set_defaults(run=selftest)
    args = p.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
