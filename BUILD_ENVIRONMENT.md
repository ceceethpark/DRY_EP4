# Reproducible ESP-IDF build environment

This project is built with the following fixed environment:

- ESP-IDF: `v5.5.3`
- Target: `esp32p4`
- Project configuration: tracked in `sdkconfig`
- Managed component versions: tracked in `dependencies.lock`
- Partition table: tracked in `partitions.csv`

Do not copy or synchronize `build/` or `managed_components/` between PCs. They
contain generated files and paths that are specific to one ESP-IDF installation.

## Set up another Windows PC

1. Install ESP-IDF `v5.5.3` and the Espressif IDF VS Code extension.
2. Clone this repository and check out `master`:

   ```powershell
   git clone https://github.com/ceceethpark/DRY_EP4.git
   cd DRY_EP4
   git switch master
   git pull --ff-only
   ```

3. In VS Code, run **ESP-IDF: Select Current ESP-IDF Version** and select
   `v5.5.3`. Select the serial port connected to the board separately; the port
   name is PC-specific and is intentionally not stored in Git.
4. Open an ESP-IDF terminal in the project directory and verify the version:

   ```powershell
   idf.py --version
   ```

   The output must report ESP-IDF `v5.5.3`.

5. Recreate all generated files from the tracked configuration:

   ```powershell
   idf.py fullclean
   idf.py reconfigure
   idf.py build
   ```

   The ESP-IDF Component Manager restores `managed_components/` using
   `dependencies.lock`.

6. Flash and monitor the board (replace `COM4` with that PC's port):

   ```powershell
   idf.py -p COM4 flash monitor
   ```

If firmware behavior still differs because the board contains old NVS or other
persistent data, erase the entire flash once and flash again:

```powershell
idf.py -p COM4 erase-flash
idf.py -p COM4 flash monitor
```

`erase-flash` permanently removes all settings and data stored on the board.

## Settings intentionally not tracked

- `.vscode/settings.json`: contains a user-specific ESP-IDF installation path,
  temporary build path, and serial port.
- `build/`: generated build output and CMake cache.
- `managed_components/`: restored from `dependencies.lock`.
