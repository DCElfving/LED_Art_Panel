#!/usr/bin/env python3
"""
setup_panel.py — get a computer ready to program the LED Art Panel.

Students run this once, at home, before the first class. It installs
everything the Arduino IDE needs for this project, so in class they can open
LED_Art_Panel.ino, plug in their (already-flashed) panel and click Upload:

  1. The Arduino IDE itself
  2. Rosetta (Apple Silicon Macs only; one of the IDE's tools needs it)
  3. Apple's Command Line Developer Tools (Macs only; the ESP32 build and
     upload run python3, which a Mac only has once these are installed)
  4. USB drivers for the ESP32's USB chip (or a check that they're there)
  5. The ESP32 board package (only the parts the original ESP32 needs)
  6. The LED matrix libraries
  7. The "Upload LittleFS" plugin (for uploading changes to the web page)
  8. A test compile, to prove it all works

It never uploads anything to a panel, and it doesn't need the project code:
run it from anywhere. (If it's inside an LED_Art_Panel folder, the test
compile uses that project; otherwise it compiles a small built-in test sketch.)

Everything it prints also goes into a log file on the Desktop, so a student who
gets stuck can email it to their instructor.

Usage (Python 3.8+, no extra packages needed):

    python3 setup_panel.py           # set everything up (Windows: py setup_panel.py)
    python3 setup_panel.py check     # only report what's installed; change nothing

Options: --yes (don't ask questions), --no-verify (skip the test compile).

Running it again is safe: anything already installed is skipped, and a
download that failed part-way carries on where it stopped.

Instructors: Windows USB drivers can't be downloaded by a script. Put the
driver installers in a folder called drivers/ before building the executable
and they're packed inside it (see drivers/README.txt).
"""

import argparse
import json
import os
import platform
import shutil
import ssl
import subprocess
import sys
import tarfile
import tempfile
import time
import traceback
import urllib.error
import urllib.request
from pathlib import Path

# ---- Versions this project was tested with -----------------------------------
# Pinned so every student in a class gets the same setup. Bump them together,
# after checking the sketch still compiles and runs.
IDE_VERSION = "2.3.10"
ESP32_CORE_VERSION = "3.3.12"
LIBRARIES = [
    "ESP32 HUB75 LED MATRIX PANEL DMA Display@3.0.14",
    "Adafruit GFX Library",          # the HUB75 library draws through this
]
ARDUINO_CLI_VERSION = "1.5.1"        # Linux only: the IDE's own copy isn't reachable there
LITTLEFS_PLUGIN_URL = ("https://github.com/earlephilhower/arduino-littlefs-upload/"
                       "releases/download/1.6.3/arduino-littlefs-upload-1.6.3.vsix")

# The ESP32 package normally brings compilers and libraries for every ESP32
# variant (S2, S3, C3, C6, H2, P4...), about 6 GB. The original ESP32 on our
# panels only needs these tools, about 1.5 GB:
ESP32_TOOLS_NEEDED = {"esp-x32", "esp32-libs", "esptool_py", "mklittlefs", "mkspiffs"}

ESP32_INDEX_URL = ("https://raw.githubusercontent.com/espressif/arduino-esp32/"
                   "gh-pages/package_esp32_index.json")
# Board for the test compile: "ESP32 Dev Module" plus its partition scheme. A
# sketch can override it with "default_fqbn:" in a sketch.yaml next to the .ino
# (the Alpha version does, because it uses the "No OTA" scheme).
FQBN = "esp32:esp32:esp32:PartitionScheme=default"
PARTITION_NAMES = {      # how the IDE's Tools -> Partition Scheme menu names them
    "default":  "Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)",
    "noota_3g": "No OTA (1MB APP/3MB SPIFFS)",
}

# When packaged as an executable (PyInstaller), __file__ is inside a temporary
# folder, so look next to the executable instead.
FROZEN = getattr(sys, "frozen", False)
HERE = os.path.dirname(os.path.abspath(sys.executable if FROZEN else __file__))
# The project to test-compile, if we were run from inside it; else None.
PROJECT_DIR = HERE if os.path.isfile(os.path.join(HERE, "LED_Art_Panel.ino")) else None
# Windows drivers: a drivers/ folder next to us, or one packed into the executable.
DRIVERS_DIRS = [os.path.join(HERE, "drivers"),
                os.path.join(getattr(sys, "_MEIPASS", HERE), "drivers")]
HOME = os.path.expanduser("~")
TOOLS_DIR = os.path.join(HOME, ".led-art-panel")
IDE_DIR = os.path.join(HOME, ".arduinoIDE")
IDE_CONFIG = os.path.join(IDE_DIR, "arduino-cli.yaml")   # shared by the IDE and us

SYSTEM = platform.system()           # "Darwin", "Windows" or "Linux"
MACHINE = platform.machine().lower() # "arm64", "x86_64", "amd64", "aarch64"
if SYSTEM == "Darwin" and MACHINE == "x86_64":
    # An Intel program running on an Apple Silicon Mac (through Rosetta) is told
    # it's on Intel. Ask the Mac itself, so we install the right IDE.
    try:
        if subprocess.run(["sysctl", "-n", "sysctl.proc_translated"],
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                          universal_newlines=True).stdout.strip() == "1":
            MACHINE = "arm64"
    except OSError:
        pass

# IDE ~0.6 GB + ESP32 package 1.4 GB + downloads and a build; on a Mac also
# Apple's Command Line Developer Tools (~3 GB).
DISK_NEEDED_GB = 6 if SYSTEM == "Darwin" else 3

# Every server the setup downloads from, checked before we start.
SERVERS = {
    "https://downloads.arduino.cc": "the Arduino IDE and libraries",
    "https://raw.githubusercontent.com": "the ESP32 package list",
    "https://github.com": "the ESP32 package and the LittleFS plugin",
}

# USB-to-serial chips found on ESP32 boards, by USB vendor ID.
USB_CHIPS = {
    "10c4": ("Silicon Labs CP210x",
             "https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers"),
    "1a86": ("WCH CH340 / CH9102",
             "https://www.wch-ic.com/downloads/category/30.html"),
}

ASSUME_YES = False
problems = []    # things the student still has to fix, printed at the end


# ---- The log file ----------------------------------------------------------------
def desktop():
    """The Desktop folder (on Windows it may have been moved into OneDrive)."""
    for d in (os.path.join(HOME, "OneDrive", "Desktop"), os.path.join(HOME, "Desktop")):
        if os.path.isdir(d):
            return d
    return HOME

LOG_PATH = os.path.join(desktop(), "LED Art Panel setup log.txt")
log_file = None

def log(text):
    """Write to the log file only."""
    if log_file:
        log_file.write(text + "\n")
        log_file.flush()

def open_log():
    global log_file
    try:
        log_file = open(LOG_PATH, "w", encoding="utf-8")
    except OSError:
        return
    log("LED Art Panel setup, " + time.strftime("%Y-%m-%d %H:%M"))
    log("%s %s (%s), Python %s, %s" % (SYSTEM, platform.release(), MACHINE,
                                       platform.python_version(),
                                       "executable" if FROZEN else "script"))
    log("Run from: " + HERE)


# ---- Trusted certificates for HTTPS downloads -------------------------------------
def use_system_certificates():
    """Make every download trust the same websites the computer's browser does.

    Python's own list of trusted certificates lives wherever it was installed
    from; a packaged copy of it run on another computer may find none, and then
    every HTTPS download fails. So also trust the system's list (on a Mac,
    Apple's built-in one) and, if it was packed in, certifi's."""
    context = ssl.create_default_context()        # Windows: includes the system's list
    try:
        import certifi                            # packed into the executables
        context.load_verify_locations(certifi.where())
    except ImportError:
        pass
    if SYSTEM == "Darwin":
        roots = subprocess.run(
            ["security", "find-certificate", "-a", "-p",
             "/System/Library/Keychains/SystemRootCertificates.keychain"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, universal_newlines=True).stdout
        if roots:
            context.load_verify_locations(cadata=roots)
    elif SYSTEM == "Linux":
        for bundle in ("/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt"):
            if os.path.isfile(bundle):
                context.load_verify_locations(bundle)
    urllib.request.install_opener(
        urllib.request.build_opener(urllib.request.HTTPSHandler(context=context)))


# ---- Small helpers -------------------------------------------------------------
def say(msg=""):
    print(msg, flush=True)
    log(msg)

def step(msg):
    say("\n== " + msg + " " + "=" * max(0, 60 - len(msg)))

def ok(msg):
    say("  [ok]   " + msg)

def warn(msg):
    say("  [!!]   " + msg)

def problem(msg):
    warn(msg)
    problems.append(msg)

def fail(msg):
    say("\n  [FAIL] " + msg)
    finish(1)

def finish(code=0):
    """Exit. A double-clicked executable's window would close at once, so wait."""
    if code and log_file:
        say("\n  A record of everything above is saved in:")
        say("    " + LOG_PATH)
        say("  If you can't fix it, email that file to your instructor.")
    if FROZEN:
        input("\n  Press Enter to close this window. ")
    sys.exit(code)

def ask(question, default=True):
    """Yes/no question. With --yes, always takes the default."""
    if ASSUME_YES:
        return default
    hint = "[Y/n]" if default else "[y/N]"
    answer = input("  " + question + " " + hint + " ").strip().lower()
    log("  > " + answer)
    return default if answer == "" else answer.startswith("y")

def run(cmd, **kw):
    log("  $ " + " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, universal_newlines=True, **kw)

def run_logged(cmd):
    """Run a command, showing its output and copying it into the log."""
    log("  $ " + " ".join(str(c) for c in cmd))
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            universal_newlines=True, errors="replace")
    for line in proc.stdout:
        print(line, end="", flush=True)
        log(line.rstrip("\n"))
    return proc.wait()

def output(cmd):
    try:
        return run(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
    except OSError:
        return ""

def download(url, dest, tries=3):
    """Download to dest, trying again if the connection drops."""
    say("  downloading " + url.rsplit("/", 1)[-1] + " ...")
    for attempt in range(1, tries + 1):
        try:
            with urllib.request.urlopen(url, timeout=60) as r, open(dest + ".part", "wb") as f:
                shutil.copyfileobj(r, f)
            os.replace(dest + ".part", dest)
            return
        except Exception as e:
            error = e
            if attempt < tries:
                say("  the download was interrupted (%s); trying again ..." % e)
                time.sleep(5)
    hint = ""
    if "CERTIFICATE" in str(error).upper():
        hint = "\n         (A security certificate couldn't be checked; tell your instructor.)"
    fail("Download failed: %s%s\n         Check your internet connection and run this again."
         % (error, hint))

def open_page(url):
    if ask("Open the download page in your browser?"):
        import webbrowser
        webbrowser.open(url)


# ---- 1. Arduino IDE ----------------------------------------------------------------
def mac_app_dirs():
    return ["/Applications", os.path.join(HOME, "Applications")]

def windows_ide_dirs():
    return [os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "Arduino IDE"),
            os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "Arduino IDE")]

LINUX_APPIMAGE = os.path.join(HOME, "Applications", "arduino-ide.AppImage")

def find_ide():
    """Path of the installed Arduino IDE, or None."""
    if SYSTEM == "Darwin":
        candidates = [os.path.join(d, "Arduino IDE.app") for d in mac_app_dirs()]
    elif SYSTEM == "Windows":
        candidates = [os.path.join(d, "Arduino IDE.exe") for d in windows_ide_dirs()]
    else:
        candidates = [LINUX_APPIMAGE, shutil.which("arduino-ide") or ""]
    for c in candidates:
        if c and os.path.exists(c):
            return c
    return None

def ide_bundled_cli():
    """The arduino-cli that ships inside the Arduino IDE (macOS and Windows)."""
    ide = find_ide()
    if not ide or SYSTEM == "Linux":
        return None
    if SYSTEM == "Darwin":
        cli = os.path.join(ide, "Contents", "Resources", "app", "lib", "backend",
                           "resources", "arduino-cli")
    else:
        cli = os.path.join(os.path.dirname(ide), "resources", "app", "lib", "backend",
                           "resources", "arduino-cli.exe")
    return cli if os.path.isfile(cli) else None

def install_ide():
    base = "https://downloads.arduino.cc/arduino-ide/arduino-ide_%s_" % IDE_VERSION
    tmp = tempfile.mkdtemp()
    if SYSTEM == "Darwin":
        name = "macOS_arm64.dmg" if MACHINE == "arm64" else "macOS_64bit.dmg"
        dmg = os.path.join(tmp, name)
        download(base + name, dmg)
        # /Applications if we can write there (admin accounts can), else ~/Applications.
        dest_dir = "/Applications" if os.access("/Applications", os.W_OK) \
            else os.path.join(HOME, "Applications")
        os.makedirs(dest_dir, exist_ok=True)
        mount = os.path.join(tmp, "mnt")
        say("  copying Arduino IDE.app into " + dest_dir + " ...")
        run(["hdiutil", "attach", "-nobrowse", "-readonly", "-quiet",
             "-mountpoint", mount, dmg], check=True)
        try:
            run(["ditto", os.path.join(mount, "Arduino IDE.app"),
                 os.path.join(dest_dir, "Arduino IDE.app")], check=True)
        finally:
            run(["hdiutil", "detach", "-quiet", mount])
    elif SYSTEM == "Windows":
        name = "Windows_64bit.exe"
        exe = os.path.join(tmp, name)
        download(base + name, exe)
        say("  installing (this takes a minute; no window will appear) ...")
        # Silent install for this user only, so no administrator password is needed.
        run([exe, "/S", "/currentuser"], check=True)
    else:
        name = "Linux_64bit.AppImage"
        os.makedirs(os.path.dirname(LINUX_APPIMAGE), exist_ok=True)
        download(base + name, LINUX_APPIMAGE)
        os.chmod(LINUX_APPIMAGE, 0o755)
        apps = os.path.join(HOME, ".local", "share", "applications")
        os.makedirs(apps, exist_ok=True)
        with open(os.path.join(apps, "arduino-ide.desktop"), "w") as f:
            f.write("[Desktop Entry]\nType=Application\nName=Arduino IDE\n"
                    "Exec=%s %%F\nCategories=Development;\n" % LINUX_APPIMAGE)
    shutil.rmtree(tmp, ignore_errors=True)
    if not find_ide():
        fail("The Arduino IDE didn't install. Get it from https://www.arduino.cc/en/software")

def step_ide():
    step("1/8  Arduino IDE")
    ide = find_ide()
    if ide:
        ok("Already installed: " + ide)
    else:
        say("  Installing Arduino IDE %s (about 200 MB)." % IDE_VERSION)
        install_ide()
        ok("Installed: " + find_ide())
    if SYSTEM == "Linux" and "libfuse.so.2" not in output(["ldconfig", "-p"]):
        problem("The IDE's AppImage needs libfuse2: sudo apt install libfuse2")


# ---- 2. Rosetta -------------------------------------------------------------------
def needs_rosetta():
    return SYSTEM == "Darwin" and MACHINE == "arm64"

def rosetta_installed():
    r = run(["/usr/bin/arch", "-x86_64", "/usr/bin/true"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return r.returncode == 0

def step_rosetta():
    step("2/8  Rosetta (Apple Silicon Macs)")
    if not needs_rosetta():
        ok("Not needed on this computer")
        return
    if rosetta_installed():
        ok("Already installed")
        return
    say("  The IDE's code-scanning tool (ctags) only runs with Rosetta. Without it,")
    say("  every Verify and Upload fails with 'bad CPU type in executable'.")
    if ask("Install Rosetta now? (asks for your Mac password)"):
        say("  Type your Mac password and press Enter. Nothing appears while you")
        say("  type; that's normal.")
        run(["sudo", "softwareupdate", "--install-rosetta", "--agree-to-license"])
    if rosetta_installed():
        ok("Installed")
    else:
        problem("Rosetta isn't installed. Run: softwareupdate --install-rosetta --agree-to-license")


# ---- 3. Apple's Command Line Developer Tools ---------------------------------------
# On a Mac, the ESP32 package runs python3 on every build (to make the
# partition table) and every upload (flasher.py). /usr/bin/python3 is only a
# placeholder until Apple's developer tools are installed: without them,
# Verify and Upload fail and macOS pops up an "install developer tools" window.
def apple_tools_installed():
    return run(["xcode-select", "-p"], stdout=subprocess.DEVNULL,
               stderr=subprocess.DEVNULL).returncode == 0

def apple_installer_open():
    return run(["pgrep", "-f", "Install Command Line Developer Tools"],
               stdout=subprocess.DEVNULL).returncode == 0

def step_apple_tools():
    step("3/8  Apple Command Line Developer Tools (Macs)")
    if SYSTEM != "Darwin":
        ok("Not needed on this computer")
        return
    if apple_tools_installed():
        ok("Already installed")
        return
    say("  The ESP32 build tools need Apple's Command Line Developer Tools (they")
    say("  include Python). A window from Apple will open: click Install, then")
    say("  Agree. It downloads about 1 GB and takes 5-20 minutes. This setup waits")
    say("  here and carries on by itself when it's done.")
    if not ask("Open Apple's installer now?"):
        problem("Apple's Command Line Developer Tools aren't installed. Run this setup "
                "again to install them.")
        return
    run(["xcode-select", "--install"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(10)                       # give the window time to appear
    waited = 0
    while not apple_tools_installed():
        if not apple_installer_open():
            warn("Apple's installer window was closed before it finished.")
            if not ask("Open it again?"):
                problem("Apple's Command Line Developer Tools aren't installed. Run this "
                        "setup again to install them.")
                return
            run(["xcode-select", "--install"], stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL)
            time.sleep(10)
            continue
        time.sleep(15)
        waited += 15
        if waited % 120 == 0:
            say("  still installing (%d minutes so far) ..." % (waited // 60))
    ok("Installed")


# ---- 4. USB drivers -------------------------------------------------------------------
def plugged_in_chips():
    """USB vendor IDs of the USB-serial chips plugged in right now (best effort)."""
    text = ""
    try:
        if SYSTEM == "Darwin":
            for line in output(["ioreg", "-p", "IOUSB", "-l", "-w0"]).splitlines():
                if '"idVendor" =' in line:
                    text += "vid_%04x\n" % int(line.split("=")[1].strip())
        elif SYSTEM == "Windows":
            text = output(["powershell", "-NoProfile", "-Command",
                           "Get-PnpDevice -PresentOnly | "
                           "Select-Object -ExpandProperty InstanceId"]).lower()
        else:
            for d in os.listdir("/sys/bus/usb/devices"):
                p = os.path.join("/sys/bus/usb/devices", d, "idVendor")
                if os.path.isfile(p):
                    text += "vid_" + open(p).read().strip() + "\n"
    except Exception:
        pass
    return [v for v in USB_CHIPS if "vid_" + v in text]

def windows_drivers_installed():
    """Which of our two driver families Windows has (from its driver store)."""
    store = output(["pnputil", "/enum-drivers"]).lower()
    return {"10c4": "silabser.inf" in store,
            "1a86": "ch341ser.inf" in store or "ch343ser.inf" in store}

def walk_drivers():
    """os.walk() over each drivers folder that exists (each one only once)."""
    for folder in sorted(set(DRIVERS_DIRS)):
        if os.path.isdir(folder):
            for entry in os.walk(folder):
                yield entry

def install_windows_drivers_from_folder():
    """Install every driver the instructor put in drivers/ (one admin prompt)."""
    infs, exes = [], []
    for root, _, files in walk_drivers():
        for f in files:
            path = os.path.join(root, f)
            if f.lower().endswith(".inf"):
                infs.append(path)
            elif f.lower().endswith(".exe"):
                exes.append(path)
    if not infs and not exes:
        return False
    say("  Installing drivers from drivers/ (Windows will ask for permission) ...")
    # Write the commands to a script and run it once as administrator.
    lines = ["pnputil /add-driver '%s' /install" % p for p in infs]
    lines += ["Start-Process -Wait '%s'" % p for p in exes]   # vendor installers (CH341SER.EXE)
    ps1 = os.path.join(tempfile.mkdtemp(), "install_drivers.ps1")
    with open(ps1, "w") as f:
        f.write("\n".join(lines) + "\n")
    run(["powershell", "-NoProfile", "-Command",
         "Start-Process powershell -Verb RunAs -Wait -ArgumentList "
         "'-NoProfile','-ExecutionPolicy','Bypass','-File','%s'" % ps1])
    return True

def step_drivers():
    step("4/8  USB drivers")
    if SYSTEM == "Darwin":
        dexts = "/System/Library/DriverExtensions"
        have = os.path.isdir(dexts) and set(os.listdir(dexts))
        if have and {"com.apple.DriverKit-AppleUSBSLCOM.dext",
                     "com.apple.DriverKit-AppleUSBCHCOM.dext"} <= have:
            ok("macOS has built-in drivers for both CP210x and CH340 chips")
        else:
            problem("This macOS is too old for built-in drivers. Install the driver for "
                    "your board's chip: " + USB_CHIPS["10c4"][1] + " or " + USB_CHIPS["1a86"][1])
    elif SYSTEM == "Linux":
        ok("Linux includes drivers for CP210x and CH340 chips")
        groups = output(["id", "-nG"]).split()
        if "dialout" in groups or "uucp" in groups:
            ok("You have permission to use serial ports")
        elif ask("Add yourself to the 'dialout' group so the IDE can use USB ports?"):
            run(["sudo", "usermod", "-aG", "dialout", os.environ.get("USER", "")])
            problems.append("Log out and back in so the 'dialout' group takes effect.")
        else:
            problem("Not in the 'dialout' group: sudo usermod -aG dialout $USER, then log out/in")
    else:
        have = windows_drivers_installed()
        if not all(have.values()):
            if install_windows_drivers_from_folder():
                have = windows_drivers_installed()
        for vid, (name, url) in USB_CHIPS.items():
            if have[vid]:
                ok(name + " driver installed")
            else:
                warn(name + " driver not installed")
        missing = [vid for vid in USB_CHIPS if not have[vid]]
        needed = [vid for vid in plugged_in_chips() if vid in missing]
        if needed:
            name, url = USB_CHIPS[needed[0]]
            problem("Your board uses a %s chip. Install its driver from %s" % (name, url))
            open_page(url)
        elif missing:
            say("  You only need the driver for your board's chip. Windows Update often")
            say("  installs it the first time you plug in the board. If no COM port appears")
            say("  in the IDE, run this script again with the board plugged in.")


# ---- arduino-cli: the build engine inside the IDE --------------------------------
def downloaded_cli_path():
    return os.path.join(TOOLS_DIR, "arduino-cli")

def download_cli():
    """Linux only: the IDE's own copy is sealed inside its AppImage."""
    name = "Linux_ARM64.tar.gz" if MACHINE in ("aarch64", "arm64") else "Linux_64bit.tar.gz"
    url = "https://downloads.arduino.cc/arduino-cli/arduino-cli_%s_%s" % (
        ARDUINO_CLI_VERSION, name)
    os.makedirs(TOOLS_DIR, exist_ok=True)
    archive = os.path.join(TOOLS_DIR, name)
    download(url, archive)
    with tarfile.open(archive) as t:
        t.extract(t.getmember("arduino-cli"), TOOLS_DIR)
    os.remove(archive)
    os.chmod(downloaded_cli_path(), 0o755)
    return downloaded_cli_path()

def find_cli():
    for c in (ide_bundled_cli(), shutil.which("arduino-cli"), downloaded_cli_path()):
        if c and os.path.isfile(c):
            return c
    return None


class Cli:
    """Runs arduino-cli with the Arduino IDE's own settings file, so anything
    installed here shows up in the IDE."""

    def __init__(self, path):
        self.base = [path, "--config-file", IDE_CONFIG, "--no-color"]

    def run(self, *args, check=True, tries=1):
        """Run arduino-cli. Downloads get tries > 1: if the connection drops,
        running the same command again carries on where it stopped."""
        for attempt in range(1, tries + 1):
            r = run(self.base + list(args))
            if r.returncode == 0 or attempt == tries:
                break
            say("  that didn't finish (exit code %d); trying again ..." % r.returncode)
            time.sleep(5)
        if check and r.returncode != 0:
            fail("arduino-cli %s %s failed (exit code %d). The messages just above say why;\n"
                 "         it's usually the internet connection. Run this again to retry."
                 % (args[0], args[1] if len(args) > 1 else "", r.returncode))
        return r

    def json(self, *args):
        r = run(self.base + list(args) + ["--format", "json"],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        try:
            return json.loads(r.stdout)
        except ValueError:
            return {}

def get_cli(may_install):
    path = find_cli()
    if not path and may_install and SYSTEM == "Linux":
        path = download_cli()
    if not path:
        return None
    cli = Cli(path)
    if may_install:
        # The IDE writes this file on its first launch; if it hasn't been opened
        # yet we create it with the same defaults.
        if not os.path.isfile(IDE_CONFIG):
            os.makedirs(IDE_DIR, exist_ok=True)
            run([path, "config", "init", "--dest-file", IDE_CONFIG],
                stdout=subprocess.DEVNULL)
        # Same as pasting the URL into the IDE's "Additional boards manager URLs".
        urls = cli.json("config", "get", "board_manager.additional_urls") or []
        if ESP32_INDEX_URL not in urls:
            cli.run("config", "add", "board_manager.additional_urls", ESP32_INDEX_URL)
    return cli

def installed_core_version(cli):
    data = cli.json("core", "list")
    for p in data.get("platforms", []):
        if p.get("id") == "esp32:esp32":
            return p.get("installed_version")
    return None

def installed_libraries(cli):
    found = {}
    for item in cli.json("lib", "list").get("installed_libraries", []):
        lib = item.get("library", {})
        found[lib.get("name")] = lib.get("version")
    return found

def library_ok(libs, spec):
    name, _, want = spec.partition("@")
    return libs.get(name) is not None and (not want or libs[name] == want)


# ---- 5-7. Board package, libraries, LittleFS plugin -----------------------------------
def step_core(cli):
    step("5/8  ESP32 board package " + ESP32_CORE_VERSION)
    have = installed_core_version(cli)
    if have == ESP32_CORE_VERSION:
        ok("Already installed")
        return
    if have:
        say("  Version %s is installed; switching to %s." % (have, ESP32_CORE_VERSION))
    say("  Installing only the parts for the original ESP32 (about 500 MB to download,")
    say("  1.5 GB on disk). It can take 5-15 minutes and may sit on one line for a")
    say("  while. Don't close this window.")
    index = trimmed_esp32_index()
    cli.run("core", "update-index", "--additional-urls", index, tries=3)
    cli.run("core", "install", "esp32:esp32@" + ESP32_CORE_VERSION,
            "--additional-urls", index, tries=3)
    cli.run("cache", "clean")      # the downloaded archives aren't needed any more
    # Swap our trimmed list back for Espressif's real one, so the IDE's Boards
    # Manager shows the normal list of versions.
    cli.run("core", "update-index", tries=3)
    ok("Installed")

def trimmed_esp32_index():
    """Download Espressif's package list and keep only our version, needing only
    the tools for the original ESP32. Returns it as a file:// URL for arduino-cli.
    (It keeps the same file name so it replaces, not joins, the real list.)"""
    os.makedirs(TOOLS_DIR, exist_ok=True)
    path = os.path.join(TOOLS_DIR, "package_esp32_index.json")
    download(ESP32_INDEX_URL, path)
    with open(path) as f:
        index = json.load(f)
    for package in index["packages"]:
        if package["name"] != "esp32":
            continue
        platforms = [p for p in package["platforms"] if p["version"] == ESP32_CORE_VERSION]
        if not platforms:
            fail("Espressif's package list has no version %s." % ESP32_CORE_VERSION)
        for p in platforms:
            p["toolsDependencies"] = [t for t in p["toolsDependencies"]
                                      if t["name"] in ESP32_TOOLS_NEEDED]
        package["platforms"] = platforms
    with open(path, "w") as f:
        json.dump(index, f)
    return Path(path).as_uri()

def step_libraries(cli):
    step("6/8  Libraries")
    libs = installed_libraries(cli)
    missing = [s for s in LIBRARIES if not library_ok(libs, s)]
    for spec in LIBRARIES:
        if spec not in missing:
            ok("%s %s already installed" % (spec.partition("@")[0], libs[spec.partition("@")[0]]))
    if missing:
        cli.run("lib", "update-index", tries=3)
        for spec in missing:
            cli.run("lib", "install", spec, tries=3)   # also installs its dependencies
            ok("Installed " + spec)

def littlefs_plugin_installed():
    folder = os.path.join(IDE_DIR, "plugins")
    return os.path.isdir(folder) and any(
        f.startswith("arduino-littlefs-upload") for f in os.listdir(folder))

def step_plugin():
    step("7/8  Upload LittleFS plugin")
    if littlefs_plugin_installed():
        ok("Already installed")
        return
    folder = os.path.join(IDE_DIR, "plugins")
    os.makedirs(folder, exist_ok=True)
    download(LITTLEFS_PLUGIN_URL, os.path.join(folder, LITTLEFS_PLUGIN_URL.rsplit("/", 1)[-1]))
    ok("Installed (restart the IDE if it's open)")


# ---- 8. Test compile -------------------------------------------------------------------
def ctags_shim_dir():
    """Without Rosetta the x86-only ctags fails. This sketch declares its
    functions up front, so for our test compile we can skip ctags entirely."""
    d = os.path.join(TOOLS_DIR, "ctags_shim")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "ctags"), "w") as f:
        f.write("#!/bin/sh\nexit 0\n")
    os.chmod(os.path.join(d, "ctags"), 0o755)
    return d

TEST_SKETCH = """\
// Written by setup_panel.py. If this compiles, the ESP32 board package and the
// LED matrix libraries are installed correctly. It's never uploaded.
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <WiFi.h>
#include <LittleFS.h>

MatrixPanel_I2S_DMA *display = nullptr;

void setup() {
  HUB75_I2S_CFG config(32, 32, 1);
  display = new MatrixPanel_I2S_DMA(config);
  display->begin();
  display->print("OK");
  WiFi.softAP("test");
  LittleFS.begin();
}

void loop() {}
"""

def test_sketch_dir():
    d = os.path.join(TOOLS_DIR, "setup_test")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "setup_test.ino"), "w") as f:
        f.write(TEST_SKETCH)
    return d

def sketch_fqbn():
    """The board this sketch builds for: sketch.yaml's default_fqbn, else FQBN."""
    if not PROJECT_DIR:
        return FQBN
    path = os.path.join(PROJECT_DIR, "sketch.yaml")
    if os.path.isfile(path):
        with open(path) as f:
            for line in f:
                key, _, value = line.partition(":")
                if key.strip() == "default_fqbn" and value.strip():
                    return value.strip().strip("\"'")
    return FQBN

def partition_name():
    scheme = sketch_fqbn().partition("PartitionScheme=")[2].split(",")[0]
    return PARTITION_NAMES.get(scheme, scheme or "(leave as it is)")

def step_verify(cli):
    step("8/8  Test compile (like clicking Verify in the IDE)")
    if PROJECT_DIR:
        sketch, name = PROJECT_DIR, "LED_Art_Panel"
        if os.path.basename(PROJECT_DIR) != "LED_Art_Panel":
            problem("The project folder must be named exactly 'LED_Art_Panel' (it's '%s'),"
                    " or the IDE won't open it. Rename it." % os.path.basename(PROJECT_DIR))
    else:
        sketch, name = test_sketch_dir(), "A test sketch using the LED library"
    say("  Compiling: " + name + ". The first compile takes a few minutes.")
    say("  Nothing is uploaded.")
    cmd = cli.base + ["compile", "--fqbn", sketch_fqbn(),
                      "--build-path", os.path.join(tempfile.gettempdir(), "led_art_panel_build")]
    if needs_rosetta() and not rosetta_installed():
        cmd += ["--build-property", "runtime.tools.ctags.path=" + ctags_shim_dir()]
    if run_logged(cmd + [sketch]) == 0:
        ok(name + " compiles")
    else:
        problem("The test compile failed; see the errors above.")


def connected_boards(cli):
    boards = []
    for entry in cli.json("board", "list").get("detected_ports", []):
        port = entry.get("port", {})
        vid = port.get("properties", {}).get("vid", "").lower().replace("0x", "")
        if port.get("protocol") == "serial" and vid:
            boards.append((port["address"], USB_CHIPS.get(vid, ("USB serial",))[0]))
    return boards


# ---- Before we start --------------------------------------------------------------------
def ide_running():
    if SYSTEM == "Darwin":
        return run(["pgrep", "-x", "Arduino IDE"], stdout=subprocess.DEVNULL).returncode == 0
    if SYSTEM == "Windows":
        return "arduino ide.exe" in output(["tasklist"]).lower()
    return run(["pgrep", "-f", "arduino-ide"], stdout=subprocess.DEVNULL).returncode == 0

def step_preflight():
    step("Before we start")
    free_gb = shutil.disk_usage(HOME).free / 1e9
    if free_gb < DISK_NEEDED_GB:
        fail("Only %.1f GB of disk space is free; this needs about %d GB. Free some up"
             " (empty the Trash/Recycle Bin, delete old downloads) and run this again."
             % (free_gb, DISK_NEEDED_GB))
    ok("%.0f GB of disk space free" % free_gb)

    for url, what in SERVERS.items():
        try:
            urllib.request.urlopen(urllib.request.Request(url, method="HEAD"), timeout=20)
        except urllib.error.HTTPError:
            pass                    # the server answered, which is all we need
        except Exception as e:
            log("  %s: %r" % (url, e))
            if "CERTIFICATE" in str(e).upper():
                fail("Couldn't check the security certificate of %s. This is a problem\n"
                     "         with the setup program, not your computer: please email the\n"
                     "         log file below to your instructor." % url.split("//")[1])
            fail("Can't reach %s, which has %s.\n"
                 "         Check that you're online. School and work networks sometimes\n"
                 "         block it: if so, run this again on a home network."
                 % (url.split("//")[1], what))
    ok("The download servers can be reached")

    while ide_running():
        warn("The Arduino IDE is open. Please quit it (File -> Quit) so it doesn't")
        say("         get in the way of the installation.")
        if ASSUME_YES or not ask("Have you closed it? (n = carry on anyway)"):
            break
    if not ide_running():
        ok("The Arduino IDE isn't running")


# ---- check: report only ------------------------------------------------------------------
def do_check():
    step("Checking this computer")
    say("  %s %s, Python %s" % (SYSTEM, MACHINE, platform.python_version()))
    ide = find_ide()
    if ide:
        ok("Arduino IDE: " + ide)
    else:
        warn("Arduino IDE not installed")
    if needs_rosetta():
        if rosetta_installed():
            ok("Rosetta installed")
        else:
            warn("Rosetta not installed")
    if SYSTEM == "Darwin":
        if apple_tools_installed():
            ok("Apple Command Line Developer Tools installed")
        else:
            warn("Apple Command Line Developer Tools not installed (Verify and Upload need them)")
    cli = get_cli(may_install=False)
    if not cli:
        warn("Nothing else to check until the IDE is installed")
        return
    core = installed_core_version(cli)
    if core == ESP32_CORE_VERSION:
        ok("ESP32 board package " + core)
    else:
        warn("ESP32 board package: %s (want %s)" % (core or "not installed", ESP32_CORE_VERSION))
    libs = installed_libraries(cli)
    for spec in LIBRARIES:
        name = spec.partition("@")[0]
        if library_ok(libs, spec):
            ok("Library %s %s" % (name, libs[name]))
        else:
            warn("Library %s: %s (want %s)" % (name, libs.get(name) or "not installed",
                                                spec.partition("@")[2] or "any"))
    if littlefs_plugin_installed():
        ok("Upload LittleFS plugin")
    else:
        warn("Upload LittleFS plugin missing")
    report_boards(cli)

def report_boards(cli):
    boards = connected_boards(cli)
    for addr, chip in boards:
        ok("Panel connected on %s (%s); pick this in Tools -> Port" % (addr, chip))
    if not boards:
        say("  [..]   No panel plugged in. That's fine: you'll plug it in in class.")


# ---- Main ----------------------------------------------------------------------------
def main():
    global ASSUME_YES
    use_system_certificates()
    parser = argparse.ArgumentParser(
        description="Set up this computer to program the LED Art Panel.")
    parser.add_argument("command", nargs="?", default="setup", choices=["setup", "check"])
    parser.add_argument("--yes", "-y", action="store_true", help="don't ask questions")
    parser.add_argument("--no-verify", action="store_true", help="skip the test compile")
    args = parser.parse_args()
    ASSUME_YES = args.yes

    if args.command == "check":
        do_check()
        finish()

    open_log()
    if SYSTEM == "Darwin" and not apple_tools_installed():
        size, minutes = "1.7 GB", "30-45"     # plus Apple's developer tools
    else:
        size, minutes = "700 MB", "15-30"
    say("Setting up this computer for the LED Art Panel. This downloads about")
    say("%s and usually takes %s minutes. Keep this window open until it" % (size, minutes))
    say("says it's finished.")
    step_preflight()
    step_ide()
    step_rosetta()
    step_apple_tools()
    step_drivers()
    cli = get_cli(may_install=True)
    if not cli:
        fail("Couldn't find the IDE's build tools (arduino-cli).")
    step_core(cli)
    step_libraries(cli)
    step_plugin()
    if args.no_verify:
        say("\n  (Skipped the test compile.)")
    else:
        step_verify(cli)

    step("Summary")
    report_boards(cli)
    if problems:
        say("\n  Still to do:")
        for p in problems:
            say("    - " + p)
        finish(1)
    partition = partition_name() if PROJECT_DIR else "the one in the project's README"
    say("""
  All done: this computer is ready for the LED Art Panel.

  In class, to program your panel:
    1. Open LED_Art_Panel.ino (from the project folder) in the Arduino IDE.
    2. Tools -> Board -> esp32 -> ESP32 Dev Module
       (exactly that one: not "ESP32S2", "ESP32S3" or "ESP32C3 Dev Module")
    3. Tools -> Partition Scheme -> %s
    4. Tools -> Port -> your panel
    5. Make a change, then click Upload (->).
  After editing the web page in data/: Ctrl+Shift+P (Cmd+Shift+P on a Mac)
  -> "Upload LittleFS to Pico/ESP8266/ESP32".

  (A record of this setup is in: %s)""" % (partition, LOG_PATH))
    question = "Open the project in the Arduino IDE now?" if PROJECT_DIR else \
               "Open the Arduino IDE now, to check it starts?"
    if not ASSUME_YES and ask(question):
        target = [os.path.join(PROJECT_DIR, "LED_Art_Panel.ino")] if PROJECT_DIR else []
        if SYSTEM == "Darwin":
            run(["open", "-a", find_ide()] + target)
        else:
            subprocess.Popen([find_ide()] + target)
    finish()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        say("\n  Stopped.")
        sys.exit(1)
    except Exception as e:      # show the error instead of a window that vanishes
        log(traceback.format_exc())
        say("\n  [FAIL] Something unexpected went wrong: %r" % e)
        finish(1)
