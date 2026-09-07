# Vendored third-party sources

Everything under `Drivers/` (except the port layers in `Core/`) and
`Middleware/` is unmodified upstream code, vendored into the repository so the
project builds from a fresh clone with nothing but CMake and
`arm-none-eabi-gcc`. No submodules, no CubeMX code generation step.

| Component | Upstream | Version / commit | License |
|---|---|---|---|
| CMSIS Core (Cortex-M) | `STMicroelectronics/cmsis_core` | `afc5ca6` | Apache-2.0 |
| CMSIS Device F1 | `STMicroelectronics/cmsis_device_f1` | `c8e9a4a` | Apache-2.0 |
| STM32F1xx HAL Driver | `STMicroelectronics/stm32f1xx_hal_driver` | `baeff0a` | BSD-3-Clause |
| FreeRTOS Kernel | `FreeRTOS/FreeRTOS-Kernel` | `V11.1.0` (`dbf7055`) | MIT |
| BME68x Sensor API | `boschsensortec/BME68x_SensorAPI` | `v4.4.8` (`80ea120`) | BSD-3-Clause |

Trimmed to keep the tree small:

* CMSIS Core keeps only the Cortex-M3 headers (`core_cm3.h`, `m-profile/`);
  the A-profile, R-profile and other M-profile cores were removed.
* CMSIS Device F1 keeps only `stm32f103xb.h` and the F1 startup/system files
  for this part.
* FreeRTOS keeps the kernel sources, the `GCC/ARM_CM3` port and `heap_4`.

The linker script in `linker/` is **not** vendored -- it was written for this
project so the repository carries no redistribution-restricted script.
