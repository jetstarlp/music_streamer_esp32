#!/usr/bin/env python3
"""Send a syntactically valid but FAKE Spotify Connect "addUser" request.

Test tool for the QEMU smoke test (tools/qemu_smoke_test.sh). It plays the
role of the Spotify app in the Zeroconf login:

  1. GET  /spotify_info?action=getInfo        -> device id + DH public key
  2. Diffie-Hellman (768 bit MODP group, generator 2) -> shared secret
  3. build a login blob the way the app does (the inverse of
     cspot::LoginBlob::decodeBlob/decodeBlobSecondary) with a made-up user
     name and made-up credentials
  4. POST /spotify_info  action=addUser&userName=..&blob=..&clientKey=..

The device accepts the blob (status 101) and then tries to log in at Spotify
with the made-up credentials. That login can not succeed: either the network
request fails (the emulated machine has no route to the internet by default)
or Spotify declines it. The firmware's handling of exactly that is what this
tool is meant to exercise.

Do not point it at a device that has internet access unless you really want
that device to send a bogus login to Spotify.

Needs the Python package "cryptography" (part of the ESP-IDF Python
environment). Usage:

    python zeroconf_fake_login.py [--host 127.0.0.1] [--port 7864] [--user qemu-test]
"""
import argparse
import base64
import hashlib
import hmac
import json
import os
import sys
import urllib.parse
import urllib.request

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

# Well-known Group 1, 768 bit (same constant as bell/main/utilities/include/Crypto.h)
DH_PRIME = int(
    "ffffffffffffffffc90fdaa22168c234c4c6628b80dc1cd129024e088a67cc74"
    "020bbea63b139b22514a08798e3404ddef9519b3cd3a431b302b0a6df25f1437"
    "4fe1356d6d51c245e485b576625e7ec6f44c42e9a63a3620ffffffffffffffff", 16)
DH_GENERATOR = 2
DH_KEY_SIZE = 96

AUTH_TYPE_STORED_SPOTIFY_CREDENTIALS = 1


def blob_int(value):
    """Variable-length integer as read by cspot::LoginBlob::readBlobInt()."""
    if value < 0x80:
        return bytes([value])
    return bytes([(value & 0x7F) | 0x80, value >> 7])


def build_blob(shared_key, device_id, username, auth_type, auth_data, corrupt_mac=False):
    # --- inner layer: AES-192-ECB, key derived from device id + user name ----
    login = (b"I" + blob_int(len(username)) + username.encode()
             + b"P" + blob_int(auth_type)
             + b"Q" + blob_int(len(auth_data)) + auth_data)
    login += bytes(-len(login) % 16)
    # the device undoes: data[j] ^= data[j - 16], from the end to the start
    chained = bytearray(login)
    for j in range(16, len(chained)):
        chained[j] ^= chained[j - 16]
    secret = hashlib.sha1(device_id.encode()).digest()
    base_key = hashlib.pbkdf2_hmac("sha1", secret, username.encode(), 0x100, 20)
    key = hashlib.sha1(base_key).digest() + b"\x00\x00\x00\x14"      # 24 bytes
    enc = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    inner = base64.b64encode(enc.update(bytes(chained)) + enc.finalize())

    # --- outer layer: AES-128-CTR + HMAC-SHA1, keys derived from the DH secret
    base = hashlib.sha1(shared_key).digest()[:16]
    checksum_key = hmac.new(base, b"checksum", hashlib.sha1).digest()
    encryption_key = hmac.new(base, b"encryption", hashlib.sha1).digest()[:16]
    iv = os.urandom(16)
    enc = Cipher(algorithms.AES(encryption_key), modes.CTR(iv)).encryptor()
    encrypted = enc.update(inner) + enc.finalize()
    mac = hmac.new(checksum_key, encrypted, hashlib.sha1).digest()
    if corrupt_mac:
        mac = bytes(b ^ 0xFF for b in mac)
    return base64.b64encode(iv + encrypted + mac).decode()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7864)
    ap.add_argument("--path", default="/spotify_info")
    ap.add_argument("--user", default="qemu-test")
    ap.add_argument("--corrupt-mac", action="store_true",
                    help="negative test: send a wrong checksum; the device then logs \"Mac doesn't match!\"")
    args = ap.parse_args()

    url = "http://%s:%d%s" % (args.host, args.port, args.path)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))   # never via a proxy

    info = json.load(opener.open(url + "?action=getInfo", timeout=10))
    device_id = info["deviceID"]
    device_key = int.from_bytes(base64.b64decode(info["publicKey"]), "big")
    print("device  : %s (%s)" % (info.get("remoteName"), device_id))

    private = int.from_bytes(os.urandom(DH_KEY_SIZE - 1), "big")
    client_key = pow(DH_GENERATOR, private, DH_PRIME).to_bytes(DH_KEY_SIZE, "big")
    shared_key = pow(device_key, private, DH_PRIME).to_bytes(DH_KEY_SIZE, "big")

    fake_credentials = b"not-a-real-credential-" + os.urandom(8).hex().encode()
    blob = build_blob(shared_key, device_id, args.user,
                      AUTH_TYPE_STORED_SPOTIFY_CREDENTIALS, fake_credentials,
                      corrupt_mac=args.corrupt_mac)
    body = urllib.parse.urlencode({
        "action": "addUser",
        "userName": args.user,
        "blob": blob,
        "clientKey": base64.b64encode(client_key).decode(),
        "tokenType": "default",
        "loginId": os.urandom(8).hex(),
        "deviceName": "fake-login-test",
    }).encode()
    answer = json.load(opener.open(urllib.request.Request(url, data=body), timeout=20))
    print("addUser :", json.dumps(answer))
    if answer.get("status") != 101:
        sys.exit("device did not accept the blob")

    info = json.load(opener.open(url + "?action=getInfo", timeout=10))
    print("getInfo : availability=%r" % info.get("availability"))
    if info.get("availability") != args.user:
        sys.exit("device does not report the user name of the blob")


if __name__ == "__main__":
    main()
