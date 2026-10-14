#!/usr/bin/env python3
"""
Network test in Espressif's QEMU (no hardware): setup portal and OTA update.

The QEMU build (build-qemu, sdkconfig.qemu) has emulated Ethernet instead of
WiFi; the portal then serves its page and DNS on that interface, and the OTA
client trusts the test CA in components/net/test instead of the public CA
bundle (CONFIG_MS_OTA_TEST_CA, only possible in the QEMU build).

1. Portal: "wifi portal" on the console, then from the host: DNS answers every
   name with 192.168.4.1; the captive-portal checks of Android, iOS and Windows
   get a redirect; the page lists the form; invalid input is refused with a
   German message; a valid network is "tried" and fails with the message for
   "no WiFi" (QEMU); "wifi portal stop".
2. OTA: a local HTTPS server (cert for 10.0.2.2 = the host as seen from the
   guest, signed by the test CA) mimics GitHub: /latest/manifest.json
   redirects to the release manifest, image URLs redirect to /assets/.
   a) manifest with a wrong sha256: download is rejected, nothing switches;
   b) correct manifest for 1.0.1: download, SHA-256 check, restart into 1.0.1
      (other OTA slot), the application confirms it (rollback cancelled);
   c) 1.0.2, restarted before the application confirmed it: the bootloader
      goes back to 1.0.1, which reports the rollback.
   The newer images are the same build with another version string
   (tools/ota_set_version.py).

Usage: python tools/qemu_net_test.py   (ESP-IDF environment exported;
       builds build-qemu like tools/qemu_smoke_test.sh)
"""
import hashlib
import http.client
import http.server
import json
import os
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qemu_console_test as qct  # noqa: E402
import ota_set_version  # noqa: E402

FW = qct.FW
BUILD = os.path.join(FW, "build-qemu")
SERIAL_PORT = 5571
HTTP_PORT = 8971
DNS_PORT = 8953
OTA_IP = "10.0.2.2"          # the host, as seen from the guest (QEMU user networking)
OTA_HOST = OTA_IP             # + ":<port>" once the server runs
CA_DIR = os.path.join(FW, "components", "net", "test")


def log(msg):
    print(msg, flush=True)


def build():
    env = os.environ.copy()
    subprocess.run(["idf.py", "-B", BUILD, "-D", f"SDKCONFIG={BUILD}/sdkconfig",
                    "-D", "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.qemu", "build"],
                   cwd=FW, check=True, stdout=subprocess.DEVNULL, env=env)
    subprocess.run([sys.executable, "-m", "esptool", "--chip=esp32s3", "merge_bin", "--output=qemu_flash.bin",
                    "--fill-flash-size=16MB", "@flash_args"], cwd=BUILD, check=True, stdout=subprocess.DEVNULL)
    sys.path.insert(0, os.path.join(os.environ["IDF_PATH"], "tools"))
    from idf_py_actions.qemu_ext import QEMU_TARGETS
    with open(os.path.join(BUILD, "qemu_efuse.bin"), "wb") as f:
        f.write(QEMU_TARGETS["esp32s3"].default_efuse)


# ---- HTTPS server mimicking GitHub releases -------------------------------------------

class Release:
    manifest = b"{}"
    files = {}
    requests = []


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"       # keep-alive, like GitHub

    def log_message(self, fmt, *args):
        Release.requests.append(self.path)

    def do_GET(self):
        p = self.path
        if p == "/latest/manifest.json":
            self.send_response(302)
            self.send_header("Location", f"https://{OTA_HOST}/releases/v/manifest.json")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif p == "/releases/v/manifest.json":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(Release.manifest)))
            self.end_headers()
            self.wfile.write(Release.manifest)
        elif p.startswith("/download/"):
            # like github.com -> objects.githubusercontent.com: a long signed URL
            self.send_response(302)
            self.send_header("Location", "https://%s/assets/%s?X-Amz-Signature=%s" %
                             (OTA_HOST, p[len("/download/"):], "a" * 700))
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif p.startswith("/assets/") and p[len("/assets/"):].split("?")[0] in Release.files:
            data = Release.files[p[len("/assets/"):].split("?")[0]]
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        else:
            self.send_error(404)


def start_server(tmp):
    key = os.path.join(tmp, "server.key")
    csr = os.path.join(tmp, "server.csr")
    crt = os.path.join(tmp, "server.pem")
    ext = os.path.join(tmp, "ext.cnf")
    with open(ext, "w") as f:
        f.write(f"subjectAltName=IP:{OTA_IP}\nextendedKeyUsage=serverAuth\n")
    q = dict(check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", key], **q)
    subprocess.run(["openssl", "req", "-new", "-key", key, "-subj", f"/CN={OTA_IP}", "-out", csr], **q)
    subprocess.run(["openssl", "x509", "-req", "-in", csr, "-CA", os.path.join(CA_DIR, "ota_test_ca.pem"),
                    "-CAkey", os.path.join(CA_DIR, "ota_test_ca.key"), "-CAcreateserial",
                    "-CAserial", os.path.join(tmp, "ca.srl"), "-days", "2",
                    "-sha256", "-extfile", ext, "-out", crt], **q)
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(crt, key)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    global OTA_HOST
    OTA_HOST = f"{OTA_IP}:{httpd.server_address[1]}"
    return httpd, httpd.server_address[1]


def manifest(version, image, sha=None, notes=""):
    return json.dumps({
        "version": version,
        "url": f"https://{OTA_HOST}/download/ms-qemu-{version}.bin",
        "sha256": sha or hashlib.sha256(image).hexdigest(),
        "size": len(image),
        "notes": notes,
    }).encode()


# ---- QEMU ---------------------------------------------------------------------------

class LiveConsole(qct.Console):
    """Console that also writes everything to build-qemu/qemu_net_test.log as it arrives."""

    def __init__(self, sock, path):
        super().__init__(sock)
        self.file = open(path, "w", encoding="utf-8")

    def read_until(self, token, timeout, start=0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                data = self.sock.recv(4096)
                if data:
                    text = data.decode("utf-8", "replace")
                    self.log += text
                    self.file.write(text)
                    self.file.flush()
            except socket.timeout:
                pass
            if self.log.find(token, start) >= 0:
                return True
        return False


def boot(flash, efuse):
    # Not restricted: the guest reaches the test server on the host as
    # 10.0.2.2. (A guestfwd would need a helper program per connection.)
    nic = (f"user,model=open_eth,"
           f"hostfwd=tcp:127.0.0.1:{HTTP_PORT}-:80,hostfwd=udp:127.0.0.1:{DNS_PORT}-:53")
    qemu = subprocess.Popen(
        ["qemu-system-xtensa", "-M", "esp32s3", "-m", "8M",
         "-drive", f"file={flash},if=mtd,format=raw",
         "-drive", f"file={efuse},if=none,format=raw,id=efuse",
         "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
         "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
         "-nic", nic, "-display", "none", "-monitor", "none",
         "-serial", f"tcp:127.0.0.1:{SERIAL_PORT},server=on,wait=on"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sock = None
    for _ in range(50):
        try:
            sock = socket.create_connection(("127.0.0.1", SERIAL_PORT), timeout=1)
            break
        except OSError:
            time.sleep(0.2)
    if sock is None:
        qemu.kill()
        print("FAIL: could not connect to the QEMU serial port")
        sys.exit(1)
    sock.settimeout(0.5)
    return qemu, LiveConsole(sock, os.path.join(BUILD, "qemu_net_test.log"))


def wait(c, token, timeout, start):
    if not c.read_until(token, timeout, start=start):
        qct.fail(f"'{token}' not seen within {timeout} s", c.log)
    return c.log.find(token, start)


def warm_up(c):
    """The first console line after a boot is sometimes lost (see
    qemu_console_test.py): send empty lines until the prompt answers."""
    for _ in range(5):
        mark = len(c.log)
        c.sock.sendall(b"\n")
        if c.read_until("streamer>", 3, start=mark):
            c.read_until("\x00", 1)      # drain late output before the next command
            return
    qct.fail("console does not answer", c.log)


def no_crash(c):
    for bad in ("Guru Meditation", "abort()", "Backtrace:", "stack overflow"):
        if bad in c.log:
            qct.fail(f"crash in the log ({bad})", c.log)


# ---- portal ---------------------------------------------------------------------------

def dns_query(name):
    q = struct.pack(">HHHHHH", 0x4242, 0x0100, 1, 0, 0, 0)
    for label in name.split("."):
        q += bytes([len(label)]) + label.encode()
    q += b"\0" + struct.pack(">HH", 1, 1)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    try:
        s.sendto(q, ("127.0.0.1", DNS_PORT))
        r, _ = s.recvfrom(512)
    finally:
        s.close()
    return r


def web(method, path, host="192.168.4.1", body=None):
    conn = http.client.HTTPConnection("127.0.0.1", HTTP_PORT, timeout=10)
    headers = {"Host": host}
    if body is not None:
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    conn.request(method, path, body=body, headers=headers)
    r = conn.getresponse()
    data = r.read().decode("utf-8", "replace")
    loc = r.getheader("Location")
    conn.close()
    return r.status, loc, data


def test_portal(c):
    mark = len(c.log)
    c.cmd("wifi portal", "WIFI:T:nopass;S:Music-Streamer;;")
    wait(c, "setup page and DNS on the emulated Ethernet", 20, mark)
    time.sleep(1)

    # (With "restrict=on" QEMU does not deliver the UDP replies of a hostfwd
    # back to the host, hence the unrestricted network in boot().) The device
    # also counts its answers, checked when the portal stops.
    for name in ("connectivitycheck.gstatic.com", "captive.apple.com", "www.msftconnecttest.com"):
        r = None
        for _ in range(3):
            try:
                r = dns_query(name)
                break
            except socket.timeout:
                pass
        if r is None or r[:2] != b"\x42\x42" or r[-4:] != bytes([192, 168, 4, 1]):
            qct.fail(f"DNS answer for {name} wrong or missing: {r!r}", c.log)
        log(f"  DNS: {name} -> 192.168.4.1")
    checks = [("/generate_204", "connectivitycheck.gstatic.com", "Android"),
              ("/hotspot-detect.html", "captive.apple.com", "iOS"),
              ("/connecttest.txt", "www.msftconnecttest.com", "Windows"),
              ("/", "example.com", "any other host")]
    for path, host, who in checks:
        st, loc, body = web("GET", path, host)
        if st != 302 or loc != "http://192.168.4.1/" or "Music Streamer einrichten" not in body:
            qct.fail(f"captive check {who}: {st} {loc} {body[:80]}", c.log)
        log(f"  {who}: http://{host}{path} -> 302 {loc}")

    st, _, page = web("GET", "/")
    if st != 200 or "<form" not in page or "Verbinden" not in page or "#C9A45C" not in page:
        qct.fail(f"portal page: {st} {page[:200]}", c.log)
    log(f"  GET / -> 200, form page ({len(page)} bytes)")

    st, _, page = web("POST", "/connect", body="s=Mein+WLAN&p=kurz")
    if st != 200 or "8 bis 63 Zeichen" not in page or 'value="Mein WLAN"' not in page:
        qct.fail(f"short password not refused: {st} {page[:300]}", c.log)
    log("  POST /connect with a 4-character password -> form with German error, SSID kept")

    mark = len(c.log)
    st, loc, _ = web("POST", "/connect", body="s=Mein+WLAN&p=geheim%2B123")
    if st != 303 or loc != "/status":
        qct.fail(f"valid form not accepted: {st} {loc}", c.log)
    st, _, page = web("GET", "/status")
    if "WLAN ist auf diesem Ger" not in page:
        qct.fail(f"status page without the expected error: {page[:300]}", c.log)
    wait(c, 'network "Mein WLAN" entered on the setup page', 5, mark)
    if "geheim+123" in c.log:
        qct.fail("password printed in the log", c.log)
    log("  POST /connect valid -> 303 /status -> \"WLAN ist auf diesem Gerät nicht verfügbar\" (QEMU)")
    st, loc, _ = web("GET", "/scan")
    if st != 303 or loc != "/":
        qct.fail(f"/scan: {st} {loc}", c.log)

    mark = len(c.log)
    c.cmd("wifi portal stop", "stops")
    wait(c, "DNS answers sent)", 10, mark)
    try:
        web("GET", "/")
        qct.fail("portal still answers after stop", c.log)
    except (ConnectionError, OSError, http.client.HTTPException):
        pass
    no_crash(c)
    log("PASS (1/2): setup portal (DNS, captive checks, form, validation, stop)")


# ---- OTA ------------------------------------------------------------------------------

def test_ota(c, v101, v102):
    mark = len(c.log)
    c.cmd(f"ota url https://{OTA_HOST}/latest/manifest.json", f"manifest URL: https://{OTA_HOST}")

    # a) wrong sha256
    Release.manifest = manifest("1.0.1", v101, sha="00" * 32, notes="Neu: Einrichtung per Hotspot")
    c.cmd("ota check", "checking")
    wait(c, "update available: 1.0.0 -> 1.0.1", 120, mark)
    out = c.cmd("ota", "offered 1.0.1")
    if "Neu: Einrichtung per Hotspot" not in out:
        qct.fail("release notes not shown", c.log)
    log("  check: 1.0.1 offered (scheduled checks only report, see 'ota')")
    mark = len(c.log)
    c.cmd("ota install", "installing")
    wait(c, "SHA-256 of the downloaded image does not match the manifest", 900, mark)
    i = c.log.find("download 95 %", mark)
    if i < 0:
        qct.fail("image was not downloaded completely before the check", c.log)
    log("  wrong sha256 in the manifest: whole image downloaded, then rejected, no restart")

    # b) correct manifest
    Release.manifest = manifest("1.0.1", v101)
    t0 = time.time()
    mark = len(c.log)
    c.cmd("ota install", "installing")
    wait(c, "SHA-256 ok", 900, mark)
    wait(c, "restarting into the new firmware", 30, mark)
    log("  download + verify of %d bytes took %.0f s (emulated, software crypto)" % (len(v101), time.time() - t0))
    wait(c, "firmware 1.0.1, updates from", 120, mark)
    wait(c, "first start of firmware 1.0.1 after an update", 5, mark)
    if "ota_1" not in c.log[mark:] and "partition at offset 0x520000" not in c.log[mark:]:
        log("  (note: boot log does not name the partition)")
    wait(c, "serial console ready", 60, mark)
    wait(c, "firmware 1.0.1 confirmed, rollback cancelled", 180, mark)
    warm_up(c)
    c.cmd("ota", "firmware:    1.0.1")
    log("  restarted into 1.0.1 (second OTA slot), application confirmed it")
    for p in ("/latest/manifest.json", "/releases/v/manifest.json", "/download/ms-qemu-1.0.1.bin"):
        if p not in Release.requests:
            qct.fail(f"server never saw {p} (redirects not followed?)", c.log)
    if not any(r.startswith("/assets/ms-qemu-1.0.1.bin?X-Amz-Signature=") for r in Release.requests):
        qct.fail("image was not fetched through the redirect", c.log)
    log("  redirects followed: /latest -> /releases, /download -> /assets?<700-char signature>")

    # c) rollback: 1.0.2 restarted before it was confirmed
    Release.manifest = manifest("1.0.2", v102)
    mark = len(c.log)
    c.cmd("ota install", "installing")
    wait(c, "first start of firmware 1.0.2 after an update", 900, mark)
    wait(c, "serial console ready", 60, mark)
    mark2 = len(c.log)
    for _ in range(5):                  # before the application confirms it (30 s)
        c.sock.sendall(b"reboot\n")
        if c.read_until("rebooting...", 3, start=mark2):
            break
    wait(c, "firmware 1.0.2 did not work and was rolled back", 120, mark2)
    wait(c, "firmware 1.0.1, updates from", 5, mark2)
    if "firmware 1.0.2 confirmed" in c.log[mark:]:
        qct.fail("1.0.2 was confirmed before the restart, test invalid", c.log)
    wait(c, "serial console ready", 60, mark2)
    time.sleep(1)
    warm_up(c)
    c.cmd("ota", "1.0.2 was rolled back")
    no_crash(c)
    log("  1.0.2 restarted before confirmation -> bootloader went back to 1.0.1, rollback reported")
    log("PASS (2/2): OTA over HTTPS (redirects, SHA-256, slot switch, confirmation, rollback)")


def main():
    for tool in ("qemu-system-xtensa", "openssl"):
        if shutil.which(tool) is None:
            print(f"{tool} not found")
            return 2
    log("building build-qemu ...")
    build()
    tmp = tempfile.mkdtemp(prefix="ms_net_")
    try:
        flash = os.path.join(tmp, "flash.bin")
        shutil.copy(os.path.join(BUILD, "qemu_flash.bin"), flash)
        efuse = os.path.join(BUILD, "qemu_efuse.bin")
        with open(os.path.join(BUILD, "music_streamer.bin"), "rb") as f:
            v100 = f.read()
        v101 = ota_set_version.set_version(v100, "1.0.1")
        v102 = ota_set_version.set_version(v100, "1.0.2")
        Release.files = {"ms-qemu-1.0.1.bin": v101, "ms-qemu-1.0.2.bin": v102}
        httpd, _ = start_server(tmp)

        qemu, c = boot(flash, efuse)
        try:
            wait(c, "serial console ready", 120, 0)
            if "App version:      1.0.0" not in c.log:
                qct.fail("running version is not 1.0.0 (firmware/version.txt)", c.log)
            time.sleep(1)
            warm_up(c)
            test_portal(c)
            test_ota(c, v101, v102)
        finally:
            c.file.close()
            c.sock.close()
            qemu.kill()
            qemu.wait()
            httpd.shutdown()
        log(f"console log: {os.path.join(BUILD, 'qemu_net_test.log')}")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
