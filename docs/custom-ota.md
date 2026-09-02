# Custom X4 Pro OTA releases

This fork publishes X4 Pro firmware from GitHub Actions and checks
`AashJ/crosspoint-reader` for updates. Each workflow run uses a monotonically
increasing version in the `1000.0.N` range so custom releases remain newer than
upstream versions.

## Bootstrap once

Build and install the fork firmware once by USB or SD card:

```sh
source .venv/bin/activate
pio run -e x4pro
```

The resulting image is `.pio/build/x4pro/firmware.bin`. After this image is
installed, **Settings → System → Check for updates** uses this fork's releases.

## Publish an update

1. Push the desired commit or branch to `AashJ/crosspoint-reader`.
2. Open **Actions → Publish Custom X4 Pro OTA → Run workflow**.
3. Select the branch containing the desired firmware and run it.
4. Wait for the workflow to publish `firmware-x4pro.bin` as a GitHub Release.
5. On the reader, open **Settings → System → Check for updates** and confirm the update.

The release workflow embeds `1000.0.<GitHub run number>` in the firmware and
uses the same value for the release tag. It also publishes a SHA-256 checksum.
GitHub Releases and their firmware assets are public because this fork is public.

The reader streams the download into the inactive OTA application partition,
validates the ESP32 chip and CrossPoint board tag, and changes the boot slot only
after the complete image has been written successfully. It does not erase the SD
card or settings storage, although keeping a backup of irreplaceable books and
notes is still prudent before testing custom firmware.
