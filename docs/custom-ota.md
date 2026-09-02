# Custom X4 Pro OTA releases

This fork publishes X4 Pro firmware from GitHub Actions and checks
`AashJ/crosspoint-reader` for updates. Each workflow run uses a monotonically
increasing version in the `1000.1.N` range so custom releases remain newer than
upstream versions.

## Bootstrap once

Build and install the fork firmware once by USB or SD card:

```sh
source .venv/bin/activate
pio run -e x4pro
```

The resulting image is `.pio/build/x4pro/firmware.bin`. After this image is
installed, the Settings header displays the build version with the `-aj` suffix,
and **Settings → System → Check for updates** uses this fork's releases.

## Publish an update

The release branch is `release/aj-rc`. Merge a tested feature branch into it,
then wait for **Publish Custom X4 Pro OTA** to finish in GitHub Actions. Every
commit that lands on the release branch automatically builds and publishes
`firmware-x4pro.bin` as the latest GitHub Release. The workflow can also be run
manually from GitHub Actions if a rebuild is needed.

On the reader, open **Settings → System → Check for updates** and confirm the
update. Publishing never initiates an update on the device by itself.

The release workflow embeds `1000.1.<GitHub run number>` in the firmware, shows
`1000.1.<GitHub run number>-aj` in the Settings header, and uses the numeric
value for the release tag. It also publishes a SHA-256 checksum. GitHub Releases
and their firmware assets are public because this fork is public.

The reader streams the download into the inactive OTA application partition,
validates the ESP32 chip and CrossPoint board tag, and changes the boot slot only
after the complete image has been written successfully. It does not erase the SD
card or settings storage, although keeping a backup of irreplaceable books and
notes is still prudent before testing custom firmware.
