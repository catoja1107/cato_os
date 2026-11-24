#!/usr/bin/env python3
"""Boots the image headless and tests the desktop, networking, and saved files.

Everything runs against private virtual disks and a local web server. This test
never touches the real USB drive and never contacts the internet.
"""
import hashlib
import http.server
import os
import pathlib
import re
import shutil
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
# Files QEMU writes while we read them stay on the Linux filesystem, because
# concurrent reads through the WSL /mnt/c mount can fail with ENODATA.
RUN_DIR = pathlib.Path(tempfile.mkdtemp(prefix="cato-smoke-"))
SERIAL = RUN_DIR / "serial.log"
SCREEN_PPM = RUN_DIR / "smoke.ppm"
DEBUG_LOG = RUN_DIR / "debugcon.log"
DESKTOP_PNG = BUILD / "desktop.png"
DESKTOP_MENU_PNG = BUILD / "desktop-menu.png"
DESKTOP_FILES_PNG = BUILD / "desktop-files.png"
DESKTOP_BROWSER_PNG = BUILD / "desktop-browser.png"
DESKTOP_RELOADED_PNG = BUILD / "desktop-reloaded.png"
DESKTOP_SECURE_PNG = BUILD / "desktop-secure.png"
TEST_IMAGE = "build/test/cato-test.img"
PKI_DIR = RUN_DIR / "pki"
CERT_NAME_ERROR = "web:error:The site's certificate was issued for a different name."
CERT_TRUST_ERROR = "web:error:The site's certificate is not from a trusted authority."
STORE_IMAGE = RUN_DIR / "store.img"
DECOY_USB = RUN_DIR / "decoy-usb.img"      # USB, but without the Cato marker.
DECOY_INTERNAL = RUN_DIR / "decoy-nvme.img"  # Has the marker, but is an internal drive.
GUEST_HOST = "10.0.2.2"  # QEMU's address for this machine.
EXPECTED_USER_AGENT = "SimpleBrowser/0.1"
BIG_PAGE_WORDS = 30000
UI_ACCENT = (232, 176, 75)
SERIAL_COPY = BUILD / "serial.log"
SCREEN_PNG = BUILD / "smoke.png"
SPLASH_PNG = BUILD / "splash.png"
SPLASH_REFERENCE = BUILD / "generated" / "splash.pgm"
MONITOR_LOG = BUILD / "smoke-monitor.log"

BOOT_TIMEOUT_SECONDS = 90
STEP_TIMEOUT_SECONDS = 15
KEY_WAIT_CHECK_SECONDS = 3
SPLASH_ADVANCE_SECONDS = 8
ANSI_ESCAPE = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
BARS = [
    ("red", (255, 0, 0)),
    ("green", (0, 255, 0)),
    ("blue", (0, 0, 255)),
    ("cyan", (0, 255, 255)),
    ("magenta", (255, 0, 255)),
    ("yellow", (255, 255, 0)),
    ("white", (255, 255, 255)),
]


class SmokeFailure(Exception):
    pass


def serial_text():
    if not SERIAL.exists():
        return b""
    return ANSI_ESCAPE.sub(b"", SERIAL.read_bytes())


def wait_for_serial(process, marker, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if marker.encode() in serial_text():
            return
        if process.poll() is not None:
            raise SmokeFailure(f"QEMU exited with {process.returncode} before '{marker}'")
        time.sleep(0.2)
    raise SmokeFailure(f"timed out waiting for '{marker}' on serial")


def monitor(process, command):
    process.stdin.write((command + "\n").encode())
    process.stdin.flush()


def parse_ppm(data):
    match = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not match:
        return None
    width, height = int(match[1]), int(match[2])
    pixels = data[match.end():]
    if len(pixels) < width * height * 3:
        return None
    return width, height, pixels


def capture_screen(process):
    SCREEN_PPM.unlink(missing_ok=True)
    monitor(process, f"screendump {SCREEN_PPM}")
    deadline = time.monotonic() + STEP_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if SCREEN_PPM.exists():
            image = parse_ppm(SCREEN_PPM.read_bytes())
            if image:
                return image
        time.sleep(0.2)
    raise SmokeFailure("timed out waiting for the QEMU screendump")


def read_splash_reference():
    data = SPLASH_REFERENCE.read_bytes()
    match = re.match(rb"P5\s+(\d+)\s+(\d+)\s+255\s", data)
    if not match:
        raise SmokeFailure(f"{SPLASH_REFERENCE} is not a valid PGM")
    return int(match[1]), int(match[2]), data[match.end():]


def splash_fit(src_width, src_height, screen_width, screen_height):
    """Mirrors splash_fit in boot/splash.h."""
    max_width = screen_width * 92 // 100
    max_height = screen_height * 92 // 100
    width = max_width
    height = width * src_height // src_width
    if height > max_height:
        height = max_height
        width = height * src_width // src_height
    return (screen_width - width) // 2, (screen_height - height) // 2, width, height


def splash_coord(dest, dest_size, src_size):
    """Mirrors splash_coord in boot/splash.h."""
    center = ((dest * 2 + 1) * (src_size << 16)) // (dest_size * 2)
    return min(max(center - 0x8000, 0), (src_size - 1) << 16)


def splash_sample(source, fx, fy):
    """Mirrors splash_sample in boot/splash.h."""
    src_width, src_height, data = source
    x0, y0 = fx >> 16, fy >> 16
    x1 = x0 + 1 if x0 + 1 < src_width else x0
    y1 = y0 + 1 if y0 + 1 < src_height else y0
    tx, ty = (fx >> 8) & 0xff, (fy >> 8) & 0xff
    top = data[y0 * src_width + x0] * (256 - tx) + data[y0 * src_width + x1] * tx
    bottom = data[y1 * src_width + x0] * (256 - tx) + data[y1 * src_width + x1] * tx
    return (top * (256 - ty) + bottom * ty + 32768) >> 16


def splash_problem(width, height, pixels, source):
    """Returns None if the screen shows the splash, otherwise a description."""
    x, y, rect_width, rect_height = splash_fit(source[0], source[1], width, height)
    stride = width * 3

    top_band = pixels[:y * stride]
    bottom_start = (y + rect_height) * stride
    bottom_band = pixels[bottom_start:height * stride]
    if top_band.count(0) != len(top_band) or bottom_band.count(0) != len(bottom_band):
        return "the area above or below the image is not black"
    for row in range(y, y + rect_height, 8):
        left = pixels[row * stride:(row * width + x) * 3]
        right = pixels[(row * width + x + rect_width) * 3:(row + 1) * stride]
        if left.count(0) != len(left) or right.count(0) != len(right):
            return f"the area beside the image is not black at row {row}"

    for grid_y in range(48):
        dest_y = (grid_y * 2 + 1) * rect_height // 96
        fy = splash_coord(dest_y, rect_height, source[1])
        for grid_x in range(64):
            dest_x = (grid_x * 2 + 1) * rect_width // 128
            fx = splash_coord(dest_x, rect_width, source[0])
            expected = splash_sample(source, fx, fy)
            offset = ((y + dest_y) * width + x + dest_x) * 3
            actual = tuple(pixels[offset:offset + 3])
            if actual != (expected, expected, expected):
                return f"pixel ({x + dest_x},{y + dest_y}) is {actual}, expected {expected}"
    return None


def wait_for_splash(process, source):
    deadline = time.monotonic() + BOOT_TIMEOUT_SECONDS
    problem = "no screen captured"
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise SmokeFailure(f"QEMU exited with {process.returncode} before the splash")
        image = capture_screen(process)
        problem = splash_problem(*image, source)
        if problem is None:
            return image
        time.sleep(0.5)
    raise SmokeFailure(f"splash never appeared: {problem}")


def check_bars(width, height, pixels):
    """Samples three rows per bar. A wrong pitch would shear the bars across rows."""
    margin = width // 20
    top = height - height // 4
    bottom = height - height // 20
    bar_width = (width - 2 * margin) // 7
    rows = [top + 2, (top + bottom) // 2, bottom - 3]

    for index, (name, expected) in enumerate(BARS):
        x = margin + index * bar_width + bar_width // 2
        for y in rows:
            offset = (y * width + x) * 3
            actual = tuple(pixels[offset:offset + 3])
            if actual != expected:
                raise SmokeFailure(f"{name} bar at ({x},{y}) is {actual}, expected {expected}")


def gui_events():
    if not DEBUG_LOG.exists():
        return []
    return DEBUG_LOG.read_text(errors="replace").splitlines()


def wait_for_gui(process, prefix, timeout=STEP_TIMEOUT_SECONDS):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for event in gui_events():
            if event.startswith(prefix):
                return event
        if process.poll() is not None:
            raise SmokeFailure(f"QEMU exited with {process.returncode} before '{prefix}'")
        time.sleep(0.05)
    raise SmokeFailure(f"timed out waiting for GUI event '{prefix}'. Saw: {gui_events()}")


def send_key(process, key):
    monitor(process, f"sendkey {key} 40")
    time.sleep(0.15)


def key_name(character):
    special = {" ": "spc", ".": "dot", "/": "slash", "-": "minus", ":": "shift-semicolon"}
    if character in special:
        return special[character]
    return f"shift-{character.lower()}" if character.isupper() else character


def type_text(process, text):
    for character in text:
        send_key(process, key_name(character))


def type_line(process, text):
    type_text(process, text)
    send_key(process, "ret")


def frame_from_event(event):
    return [int(value) for value in event.rsplit(":", 1)[1].split(",")]


def check_desktop_chrome(width, height, pixels):
    """The taskbar's cat icon is drawn in the solid accent color."""
    bar = pixels[(height - 36) * width * 3:height * width * 3]
    accent_pixels = sum(
        1 for offset in range(0, len(bar), 3) if tuple(bar[offset:offset + 3]) == UI_ACCENT
    )
    if accent_pixels < 40:
        raise SmokeFailure(f"desktop taskbar not drawn ({accent_pixels} accent pixels)")


class TestSite(http.server.BaseHTTPRequestHandler):
    """A small local website for the browser to load."""

    protocol_version = "HTTP/1.1"
    user_agents = []
    big_page = (
        "<title>Smoke Big</title><p>" + " ".join(f"word{i}" for i in range(BIG_PAGE_WORDS)) + "</p>"
    ).encode()
    pages = {
        "/": b"<!doctype html><title>Smoke Home</title><h1>Local test site</h1>"
             b"<p>This page came over <b>TCP</b> from the host.</p>"
             b"<ul><li><a href=\"/next\">Next page</a></li><li>Second item</li></ul>"
             b"<script>document.write('never shown')</script>",
        "/next": b"<title>Smoke Next</title><p>The link worked.</p>",
        "/secure": b"<title>Smoke Secure</title><h1>Encrypted</h1>"
                   b"<p>This page arrived over <b>TLS</b>.</p>"
                   b"<blockquote>Certificates are checked.</blockquote>"
                   b"<pre>code sample</pre><a href=\"/next\">A link</a>",
        "/landing": b"<title>Smoke Landing</title><p>The redirect worked.</p>",
    }

    def log_message(self, *args):
        pass

    def do_GET(self):
        TestSite.user_agents.append(self.headers.get("User-Agent", ""))
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/landing")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
        elif self.path == "/big":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            for start in range(0, len(self.big_page), 7001):
                chunk = self.big_page[start:start + 7001]
                self.wfile.write(b"%x\r\n" % len(chunk) + chunk + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            body = self.pages.get(self.path)
            self.send_response(200 if body else 404)
            body = body or b"<title>Not found</title>"
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        self.close_connection = True


class QuietServer(http.server.ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        pass  # Rejected TLS handshakes are expected.


def start_site(certificate=None):
    server = QuietServer(("127.0.0.1", 0), TestSite)
    if certificate:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(PKI_DIR / f"{certificate}.pem", PKI_DIR / f"{certificate}.key")
        server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def make_test_pki():
    """Creates a throwaway CA and server certificates, then builds the test image."""
    PKI_DIR.mkdir()

    def openssl(*args):
        subprocess.run(["openssl", *args], cwd=PKI_DIR, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    ec = ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes"]
    openssl("req", "-x509", *ec, "-keyout", "ca.key", "-out", "ca.pem", "-days", "2",
            "-subj", "/CN=Cato Smoke Test CA",
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    for name, alt_name in (("good", f"DNS:{GUEST_HOST}"), ("wrong", "DNS:wrong.example")):
        openssl("req", *ec, "-keyout", f"{name}.key", "-out", f"{name}.csr", "-subj", f"/CN={name}")
        (PKI_DIR / f"{name}.ext").write_text(
            f"subjectAltName={alt_name}\nbasicConstraints=CA:FALSE\n"
            "keyUsage=digitalSignature\nextendedKeyUsage=serverAuth\n")
        openssl("x509", "-req", "-in", f"{name}.csr", "-CA", "ca.pem", "-CAkey", "ca.key",
                "-CAcreateserial", "-out", f"{name}.pem", "-days", "2", "-extfile", f"{name}.ext")
    # Correct name, but signed by nobody the test image trusts.
    openssl("req", "-x509", *ec, "-keyout", "untrusted.key", "-out", "untrusted.pem", "-days", "2",
            "-subj", f"/CN={GUEST_HOST}", "-addext", f"subjectAltName=DNS:{GUEST_HOST}")

    (BUILD / "test").mkdir(parents=True, exist_ok=True)
    subprocess.run(["python3", "scripts/gen_anchors.py", str(PKI_DIR / "ca.pem"),
                    str(BUILD / "test" / "tls_anchors_test.c")],
                   cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["make", "-s", "test-image"], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)


def wait_for_page(process, title, timeout=30):
    """Waits for a page load and returns its [status, byte count]."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for event in gui_events():
            if event.startswith("web:loaded:") and event.endswith(":" + title):
                return [int(value) for value in event.split(":")[2:4]]
        if process.poll() is not None:
            raise SmokeFailure(f"QEMU exited with {process.returncode} before page '{title}'")
        time.sleep(0.05)
    raise SmokeFailure(f"timed out loading page '{title}'. Saw: {gui_events()[-8:]}")


def exercise_browser(process, ports):
    port = ports["plain"]
    send_key(process, "f1")
    for _ in range(4):
        send_key(process, "down")
    send_key(process, "ret")
    wait_for_gui(process, "open:browser")

    type_line(process, f"{GUEST_HOST}:{port}/")
    status, _ = wait_for_page(process, "Smoke Home")
    if status != 200:
        raise SmokeFailure(f"home page returned {status}")
    print("ok: DHCP, TCP, and HTTP loaded a page")
    time.sleep(0.3)
    width, height, pixels = capture_screen(process)
    write_png(DESKTOP_BROWSER_PNG, width, height, pixels)

    send_key(process, "right")
    send_key(process, "ret")
    wait_for_page(process, "Smoke Next")
    print("ok: keyboard followed a link")

    type_line(process, f"{GUEST_HOST}:{port}/redirect")
    wait_for_page(process, "Smoke Landing")
    print("ok: browser followed a redirect")

    type_line(process, f"{GUEST_HOST}:{port}/big")
    _, length = wait_for_page(process, "Smoke Big", timeout=60)
    if length != len(TestSite.big_page):
        raise SmokeFailure(f"big page was {length} bytes, expected {len(TestSite.big_page)}")
    print(f"ok: received a {length // 1024} KB chunked page intact")

    # The pointer rests over the browser, so the wheel scrolls the page.
    # In QEMU's monitor, a negative wheel value means scrolling down.
    monitor(process, "mouse_move 0 0 -2")
    event = wait_for_gui(process, "web:wheel:")
    if int(event.split(":")[2]) <= 0:
        raise SmokeFailure(f"the wheel did not scroll the page: {event}")
    print("ok: mouse wheel scrolled the page")

    type_line(process, f"https://{GUEST_HOST}:{ports['good']}/secure")
    wait_for_page(process, "Smoke Secure", timeout=40)
    wait_for_gui(process, f"web:secure:{GUEST_HOST}")
    print("ok: HTTPS page loaded with a verified certificate")
    time.sleep(0.3)
    width, height, pixels = capture_screen(process)
    write_png(DESKTOP_SECURE_PNG, width, height, pixels)

    send_key(process, "ctrl-d")
    wait_for_gui(process, "web:bookmarks-changed")
    print("ok: bookmarked the page")

    send_key(process, "ctrl-t")
    wait_for_gui(process, "web:tab:1")
    type_line(process, f"https://{GUEST_HOST}:{ports['wrong']}/secure")
    wait_for_gui(process, CERT_NAME_ERROR, 40)
    print("ok: refused a certificate for the wrong name")

    type_line(process, f"https://{GUEST_HOST}:{ports['untrusted']}/secure")
    wait_for_gui(process, CERT_TRUST_ERROR, 40)
    print("ok: refused a certificate from an untrusted authority")
    send_key(process, "ctrl-w")

    if set(TestSite.user_agents) != {EXPECTED_USER_AGENT}:
        raise SmokeFailure(f"unexpected User-Agent values: {set(TestSite.user_agents)}")
    print("ok: requests carried only the generic client name")

    send_key(process, "f1")
    send_key(process, "down")
    send_key(process, "down")
    send_key(process, "ret")
    type_line(process, f"ping {GUEST_HOST}")
    wait_for_gui(process, "net:ping-ok")
    print("ok: ping received a reply")


def make_test_disks():
    subprocess.run(["sh", "scripts/mkstore.sh", str(STORE_IMAGE)], cwd=ROOT, check=True)
    subprocess.run(["sh", "scripts/mkstore.sh", str(DECOY_USB)], cwd=ROOT, check=True)
    subprocess.run(["sh", "scripts/mkstore.sh", str(DECOY_INTERNAL)], cwd=ROOT, check=True)
    subprocess.run(
        ["mdel", "-i", f"{DECOY_USB}@@1M", "::/CATOOS/STORE.ID"],
        env={**os.environ, "MTOOLS_SKIP_CHECK": "1"}, check=True,
    )
    return {path: file_hash(path) for path in (DECOY_USB, DECOY_INTERNAL)}


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def read_slots():
    """Returns the bytes of both saved-file slots on the virtual drive."""
    combined = b""
    for name in ("SLOTA.DAT", "SLOTB.DAT"):
        target = RUN_DIR / name
        target.unlink(missing_ok=True)
        subprocess.run(
            ["mcopy", "-n", "-i", f"{STORE_IMAGE}@@1M", f"::/CATOOS/{name}", str(target)],
            env={**os.environ, "MTOOLS_SKIP_CHECK": "1"},
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
        )
        if target.exists():
            combined += target.read_bytes()
    return combined


def check_saved_files(decoys):
    slots = read_slots()
    for expected in (b"Hello from Cato", b"Projects", b"Bookmarks.txt"):
        if expected not in slots:
            raise SmokeFailure(f"{expected!r} was not saved to the USB drive")
    print("ok: files were written to the approved USB drive")

    for path, digest in decoys.items():
        if file_hash(path) != digest:
            raise SmokeFailure(f"SAFETY: {path.name} was modified")
    print("ok: unmarked USB and marked internal drives were not modified")


def check_reload():
    """Boots again and confirms that the saved files come back."""
    saved = [e for e in gui_events() if e.startswith("storage:saved:")]
    if not saved:
        raise SmokeFailure("the desktop never reported saving files")
    expected = saved[-1].split(":", 2)[2]

    DEBUG_LOG.unlink(missing_ok=True)
    SERIAL.unlink(missing_ok=True)
    with (BUILD / "smoke-reload-monitor.log").open("wb") as monitor_log:
        process = start_qemu(monitor_log)
        try:
            event = wait_for_gui(process, "storage:loaded:", BOOT_TIMEOUT_SECONDS)
            if event.split(":", 2)[2] != expected:
                raise SmokeFailure(f"reloaded {event}, but saved {expected}")
            wait_for_gui(process, "ready")
            print("ok: saved files loaded after a reboot")

            send_key(process, "f1")
            send_key(process, "down")
            send_key(process, "down")
            send_key(process, "ret")
            wait_for_gui(process, "open:terminal")
            type_line(process, "cat /Documents/Notes.txt")
            type_line(process, "ls /")
            time.sleep(0.3)
            width, height, pixels = capture_screen(process)
            write_png(DESKTOP_RELOADED_PNG, width, height, pixels)
            monitor(process, "quit")
            process.wait(timeout=STEP_TIMEOUT_SECONDS)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


def exercise_desktop(process, ports):
    wait_for_gui(process, "ready", BOOT_TIMEOUT_SECONDS)
    print("ok: desktop started")

    send_key(process, "f1")
    wait_for_gui(process, "menu:open")
    time.sleep(0.2)
    width, height, pixels = capture_screen(process)
    write_png(DESKTOP_MENU_PNG, width, height, pixels)
    check_desktop_chrome(width, height, pixels)
    print("ok: start menu opened from the keyboard")

    send_key(process, "ret")
    files = frame_from_event(wait_for_gui(process, "open:files"))
    print("ok: start menu opened Files")

    send_key(process, "ctrl-n")
    wait_for_gui(process, "fs:new:New Folder")
    type_line(process, "Projects")
    wait_for_gui(process, "fs:rename:Projects")
    print("ok: created and named a folder")

    send_key(process, "ctrl-c")
    wait_for_gui(process, "clip:copy:Projects")
    send_key(process, "ctrl-v")
    wait_for_gui(process, "fs:paste:Projects (2)")
    send_key(process, "delete")
    wait_for_gui(process, "fs:trash:Projects (2)")
    print("ok: copied, pasted, and moved a folder to the Trash")

    # The pointer starts at the screen center. Drag the Files window by its title bar.
    pointer_x, pointer_y = width // 2, height // 2
    target_x, target_y = files[0] + 40, files[1] + 12
    monitor(process, f"mouse_move {target_x - pointer_x} {target_y - pointer_y}")
    time.sleep(0.5)
    monitor(process, "mouse_button 1")
    wait_for_gui(process, "drag:files")
    monitor(process, "mouse_move -48 36")
    time.sleep(0.5)
    monitor(process, "mouse_button 0")
    moved = frame_from_event(wait_for_gui(process, "moved:files"))
    if moved[:2] != [files[0] - 48, files[1] + 36]:
        raise SmokeFailure(f"mouse drag moved the window to {moved[:2]}, started at {files[:2]}")
    print("ok: mouse dragged a window")

    # Right-click an empty part of the file list.
    pointer_x, pointer_y = target_x - 48, target_y + 36
    blank_x, blank_y = moved[0] + 500, moved[1] + 400
    monitor(process, f"mouse_move {blank_x - pointer_x} {blank_y - pointer_y}")
    time.sleep(0.5)
    monitor(process, "mouse_button 2")
    wait_for_gui(process, "context:open")
    monitor(process, "mouse_button 0")
    time.sleep(0.2)
    width, height, pixels = capture_screen(process)
    write_png(DESKTOP_FILES_PNG, width, height, pixels)
    send_key(process, "esc")
    print("ok: right-click opened a context menu")

    send_key(process, "f1")
    send_key(process, "down")
    send_key(process, "down")
    send_key(process, "ret")
    wait_for_gui(process, "open:terminal")
    type_line(process, "edit /Documents/Notes.txt")
    wait_for_gui(process, "open:editor")
    type_text(process, "Hello from Cato")
    send_key(process, "ctrl-s")
    wait_for_gui(process, "fs:save:Notes.txt")
    print("ok: terminal opened a file, and the editor saved it")

    time.sleep(0.2)
    width, height, pixels = capture_screen(process)
    write_png(DESKTOP_PNG, width, height, pixels)
    check_desktop_chrome(width, height, pixels)

    exercise_browser(process, ports)

    send_key(process, "f1")
    send_key(process, "up")
    send_key(process, "ret")
    time.sleep(0.2)
    send_key(process, "ret")
    wait_for_gui(process, "exit")


def write_png(path, width, height, pixels):
    def chunk(kind, payload):
        crc = zlib.crc32(kind + payload) & 0xffffffff
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", crc)

    stride = width * 3
    raw = b"".join(b"\0" + pixels[y * stride:(y + 1) * stride] for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b"")
    )


def start_qemu(monitor_log):
    return subprocess.Popen(
        ["sh", "scripts/qemu.sh", "headless", TEST_IMAGE],
        cwd=ROOT,
        env={
            **os.environ,
            "CATO_SERIAL_LOG": str(SERIAL),
            "CATO_DEBUG_LOG": str(DEBUG_LOG),
            "CATO_STORE_IMG": str(STORE_IMAGE),
            # Decoys prove that only the approved drive is ever written.
            "CATO_QEMU_ARGS": (
                f"-drive if=none,id=decoyusb,format=raw,file={DECOY_USB} "
                "-device usb-storage,bus=xhci.0,drive=decoyusb,removable=on "
                f"-drive if=none,id=decoynvme,format=raw,file={DECOY_INTERNAL} "
                "-device nvme,drive=decoynvme,serial=decoy"
            ),
        },
        stdin=subprocess.PIPE,
        stdout=monitor_log,
        stderr=subprocess.STDOUT,
    )


def check_splash_auto_advance():
    """With no input at all, the splash must give way to the desktop."""
    with MONITOR_LOG.open("wb") as monitor_log:
        process = start_qemu(monitor_log)
        try:
            wait_for_splash(process, read_splash_reference())
            wait_for_gui(process, "ready", SPLASH_ADVANCE_SECONDS)
            monitor(process, "quit")
            process.wait(timeout=STEP_TIMEOUT_SECONDS)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    DEBUG_LOG.unlink(missing_ok=True)
    SERIAL.unlink(missing_ok=True)
    print("ok: splash advanced to the desktop with no input")


def main():
    sites = []
    try:
        make_test_pki()
        sites = {name: start_site(certificate) for name, certificate in (
            ("plain", None), ("good", "good"), ("wrong", "wrong"), ("untrusted", "untrusted"))}
        ports = {name: server.server_address[1] for name, server in sites.items()}
        decoys = make_test_disks()
        check_splash_auto_advance()
    except (SmokeFailure, subprocess.TimeoutExpired, subprocess.CalledProcessError) as error:
        print(f"SMOKE TEST FAILED: {error}", file=sys.stderr)
        for server in (sites.values() if sites else []):
            server.shutdown()
        shutil.rmtree(RUN_DIR, ignore_errors=True)
        return 1

    with MONITOR_LOG.open("wb") as monitor_log:
        process = start_qemu(monitor_log)
        try:
            width, height, pixels = wait_for_splash(process, read_splash_reference())
            # The splash times out, so press D before doing anything slower.
            monitor(process, "sendkey d")
            write_png(SPLASH_PNG, width, height, pixels)
            print(f"ok: splash image correct at {width}x{height}")

            wait_for_serial(process, "CATO_M1_BOOT_OK", STEP_TIMEOUT_SECONDS)
            print("ok: D opened the diagnostics screen")

            width, height, pixels = capture_screen(process)
            write_png(SCREEN_PNG, width, height, pixels)
            check_bars(width, height, pixels)
            print(f"ok: all color bars correct at {width}x{height}")

            time.sleep(KEY_WAIT_CHECK_SECONDS)
            if b"CATO_M1_EXIT" in serial_text():
                raise SmokeFailure("loader exited before any key was pressed")
            print("ok: loader waits for a key")

            monitor(process, "sendkey ret")
            exercise_desktop(process, ports)

            wait_for_serial(process, "CATO_M1_EXIT", STEP_TIMEOUT_SECONDS)
            print("ok: exit command returned control to firmware")

            monitor(process, "quit")
            process.wait(timeout=STEP_TIMEOUT_SECONDS)

            check_saved_files(decoys)
            check_reload()
            check_saved_files(decoys)
        except (SmokeFailure, subprocess.TimeoutExpired) as error:
            print(f"SMOKE TEST FAILED: {error}", file=sys.stderr)
            print(f"See {SERIAL_COPY} and {MONITOR_LOG}", file=sys.stderr)
            return 1
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            if SERIAL.exists():
                shutil.copyfile(SERIAL, SERIAL_COPY)
            for server in sites.values():
                server.shutdown()
            shutil.rmtree(RUN_DIR, ignore_errors=True)

    print("smoke test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
