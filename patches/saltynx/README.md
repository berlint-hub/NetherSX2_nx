# SaltyNX display-sync IPC endpoint

The NetherSX2-to-FPSLocker bridge needs to read the current display rate and map SaltyNX's existing FPSLocker shared block. SaltyNX 2.1.3 exposes these commands only through the legacy `SaltySD` port, which is occupied by SaltyNX Core in the supplied device log (`0x00010801`, Kernel `OutOfResource`).

The preferred integration is the SaltyNX branch [`arena/e0ad6872-saltynx`](https://github.com/berlint-hub/SaltyNX/tree/arena/e0ad6872-saltynx), which adds a separate, single-client `NethFPS` port for `GetDisplayRefreshRate` and `GetSharedMemoryHandle`. It leaves the legacy SaltySD endpoint and NX-FPS behavior unchanged. Build and install SaltyNX from that branch alongside the NetherSX2 build; NetherSX2 tries `NethFPS` first and falls back to the legacy `SaltySD` port.

`saltysd-display-endpoint.patch` is a standalone equivalent for the SaltyNX 2.1.3 source layout. It is not needed when building from the linked branch. The endpoint still needs a real-device test after installing the rebuilt SaltyNX sysmodule.
