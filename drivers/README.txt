USB drivers for Windows
=======================

INCLUDED: CP210x_Universal_Windows_Driver/ (Silicon Labs, version 11.6.0.420,
downloaded 2026-10-22 from the page below, unmodified). The class's ELEGOO
ESP-WROOM-32 USB-C boards use a CP2102 chip (USB ID 10C4:EA60), so this is the
only driver they need. See SLAB_License_Agreement_VCP_Windows.txt inside it.


macOS and Linux already include the drivers the ESP32 needs. On Windows,
setup_panel.py checks for them, but it can't download them itself: the vendors'
sites only allow downloads from a browser.

If you put the drivers in this folder before handing out the project,
setup_panel.py installs them on Windows automatically (Windows asks the
student for permission once). It installs every .inf file in this folder and
its subfolders, then runs every .exe.

You only need the driver for the USB chip on your boards. It's the small chip
next to the USB socket:

  CP2102 / CP2104 (Silicon Labs)
    Download "CP210x Universal Windows Driver" (a .zip) from
    https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers
    and unzip it into this folder. The script uses silabser.inf.

  CH340 / CH341 (WCH)
    Download CH341SER.EXE from
    https://www.wch-ic.com/downloads/CH341SER_EXE.html
    and put it in this folder.

  CH9102 / CH343 (WCH)
    Download CH343SER.EXE from
    https://www.wch-ic.com/downloads/CH343SER_EXE.html
    and put it in this folder.

The .exe installers open a small window; the student clicks INSTALL.
