# SaltyNX display-sync IPC endpoint

The NetherSX2-to-FPSLocker bridge needs to read the current display rate and map SaltyNX's existing FPSLocker shared block. SaltyNX 2.1.3 exposes these commands only through the legacy `SaltySD` port, which is occupied by SaltyNX Core in the supplied device log (`0x00010801`, Kernel `OutOfResource`).

`saltysd-display-endpoint.patch` adds a separate, single-client `NethFPS` port. It permits only `GetDisplayRefreshRate` and `GetSharedMemoryHandle`; it does not change the existing SaltySD endpoint or NX-FPS behavior.

Apply the patch at the root of the SaltyNX source checkout, rebuild SaltyNX, and install the rebuilt sysmodule on the Switch. NetherSX2 tries `SaltySDDisplay` first and retains a fallback attempt to the legacy `SaltySD` port.

This patch targets the upstream SaltyNX source layout containing `saltysd_proc/source/main.c` and `saltysd_proc/source/service.c`. It must be built and device-tested with the exact SaltyNX release installed on the console before relying on the new endpoint.
