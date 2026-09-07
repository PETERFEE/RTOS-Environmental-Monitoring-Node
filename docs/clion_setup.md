# CLion setup

The project is plain CMake, so CLion needs no plugin to build it. The embedded
debugging integration is optional but worth ten minutes.

---

## 1. Toolchain

Install the **GNU Arm Embedded Toolchain** (`arm-none-eabi-gcc`):

* Windows — [Arm developer downloads](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads), or `winget install Arm.GnuArmEmbeddedToolchain`
* macOS — `brew install --cask gcc-arm-embedded`
* Linux — `apt install gcc-arm-none-eabi` (or the Arm tarball for a newer version)

Check it is on `PATH`:

```bash
arm-none-eabi-gcc --version
```

If you would rather not put it on `PATH`, pass its location instead:
`-DTOOLCHAIN_DIR=/path/to/bin`.

---

## 2. CMake profile

**Settings → Build, Execution, Deployment → CMake**

Edit the default profile (or add one):

| Field | Value |
|---|---|
| Name | `Debug` |
| Build type | `Debug` |
| CMake options | `-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake` |
| Build directory | `cmake-build-debug` |

Add a second profile with build type `Release` and the same CMake options when
you want the `-Os` image.

CLion reloads the project and the target `rtos-iot-sensor-node.elf` appears in
the run-configuration dropdown, along with `flash`, `erase` and `gdbserver`.

> If configuration fails with *"This project must be configured with the ARM
> cross toolchain"*, the CMake options line has not been applied to the profile
> you are actually building.

---

## 3. Flashing

Install OpenOCD (`brew install open-ocd`, `apt install openocd`, or the
[xPack build](https://github.com/xpack-dev-tools/openocd-xpack/releases) on
Windows) and put it on `PATH`.

The simplest route needs no further setup — build the `flash` target:

**Run → Run… → flash**

It runs `openocd -f openocd/stm32f103rb.cfg -c "program ... verify reset exit"`.
`erase` mass-erases the part, and `gdbserver` starts OpenOCD on port 3333 for a
manual GDB session.

---

## 4. Debugging with breakpoints

For source-level debugging in the IDE:

**Run → Edit Configurations… → + → OpenOCD Download & Run**

| Field | Value |
|---|---|
| Target | `rtos-iot-sensor-node.elf` |
| Executable | `rtos-iot-sensor-node.elf` |
| Board config file | `openocd/stm32f103rb.cfg` |
| Download | Always |
| Reset | Init |

The OpenOCD config sets `-rtos FreeRTOS`, so once the scheduler is running,
**CLion's Frames/Threads pane lists the FreeRTOS tasks by name** and you can
switch between their stacks. That is by far the most useful debugging feature on
this project — a fault in `Process` is obvious when you can see what `Sensor`
was doing at the time.

`vQueueAddToRegistry()` is called for all three queues, so `sensorQ`,
`telemetryQ` and `logQ` also show up in RTOS-aware views.

---

## 5. Serial console

CLion has no built-in serial terminal. Either use the project's own tool:

```bash
python scripts/uart_test.py -p /dev/ttyACM0 monitor
```

or install the **Serial Port Monitor** plugin (Settings → Plugins), or use
`picocom` / PuTTY / `screen` at **115200 8N1**.

The port is the ST-LINK virtual COM port:

* Windows — `COMx` (Device Manager → Ports)
* Linux — `/dev/ttyACM0`
* macOS — `/dev/tty.usbmodem*`

---

## 6. Running the tests from CLion

The Python suite is independent of the firmware build:

```bash
pip install -r tests/requirements.txt
pytest tests -q                                 # host tests only
pytest tests -q --port /dev/ttyACM0             # plus hardware round-trip
```

To run them from inside the IDE, add a **Python tests → pytest**
configuration with target `tests` and the working directory set to the repo
root. CLion will build the host shared library automatically the first time,
via `tests/conftest.py`.
