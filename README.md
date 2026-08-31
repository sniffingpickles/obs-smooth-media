# Smooth Media Source

An OBS source for remote RTMP, SRT, and RIST feeds.

Use it when a normal Media Source starts sounding choppy, falls out of sync,
or needs to be restarted after a rough network patch. Smooth Media Source
keeps a small audio buffer, rebuilds that buffer cleanly after a dropout, and
reconnects when the feed drops. Audio stays at its original speed by default.

This plugin was made for remote cameras, guest feeds, and IRL streams. It is
currently in beta.

## Install

You need 64-bit Windows and OBS Studio 32.0 through 32.2. The installer
detects the FFmpeg version included with OBS and installs the matching build.

1. Download the latest Windows installer from
   [Releases](https://github.com/sniffingpickles/obs-smooth-media/releases).
2. Run the installer. It finds the normal OBS folder automatically.
3. Open OBS and add a **Smooth Media Source**.

The installer also handles updates and clean uninstall. Use **Check for
updates** in the Start menu, or run a newer installer over the old one.
Version-specific portable zips are still available for manual installs.

The installer is not code-signed yet, so Windows may show a SmartScreen
warning. Release downloads include a SHA-256 digest for verification.

## Use it

Add a **Smooth Media Source**, paste the playback URL, and press **OK**.

```text
rtmp://server:1935/app/stream
srt://server:9000?mode=caller
rist://server:9001
```

The default settings are a good starting point.

| Setting | What it does |
|---|---|
| Stream URL | The playback URL for the feed |
| Input Format | Forces a format such as `mpegts`; leave blank unless needed |
| Reconnect Delay | How long to wait before reconnecting |
| Hardware Decoding | Uses the GPU when a supported decoder is available |
| Strict PTS A/V Sync | Advanced fallback for feeds that drift with normal sync; leave off unless needed |
| Adaptive Audio Speed | Slightly corrects a feed whose clock always runs fast or slow; leave off for original-speed audio |
| Close When Inactive | Disconnects when the source is not in use |
| Disable Video Preview | Keeps audio running without sending video to OBS |
| FFmpeg Options | Extra input options for unusual feeds |
| Verbose Debug Logging | Adds timing details to the OBS log for troubleshooting |

RTMP, SRT, and RIST support depends on the FFmpeg build loaded by OBS. The
plugin lists the available protocols in the OBS log when it starts.
Enhanced RTMP feeds automatically advertise AV1, HEVC, and VP9 support to
compatible servers such as MediaMTX.

## If something goes wrong

Turn on **Verbose Debug Logging**, reproduce the problem for a minute, then use
**Help → Log Files → Upload Current Log File** in OBS.

Please include:

- the uploaded log link;
- the kind of feed you used;
- whether hardware decoding was enabled; and
- what you saw or heard.

Bug reports belong in the
[issue tracker](https://github.com/sniffingpickles/obs-smooth-media/issues).
Remember that stream URLs can contain private keys or tokens.

<details>
<summary>Building and testing</summary>

### Windows build

Install Visual Studio 2022 with the C++ desktop workload and CMake 3.16 or
newer. The setup script downloads the OBS 32.2.2 runtime, source headers, and
dependency SDK used by the current Windows build. Release builds also compile
and test the FFmpeg 61 variant used by OBS 32.0 and 32.1.

```powershell
git clone https://github.com/sniffingpickles/obs-smooth-media.git
cd obs-smooth-media
.\setup-windows-deps.ps1
.\build-windows.bat "deps\obs-studio" "deps\obs-deps"
```

The DLL is written to `build_win\Release\obs-smooth-media.dll`.

### Standalone tests

```bash
cmake -S . -B build-test -G Ninja \
  -DBUILD_PLUGIN=OFF \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-test
ctest --test-dir build-test --output-on-failure
```

The test suite covers buffering, timing, decoding, damaged streams, and
connection recovery.

</details>

## Community

- [IRLtools Discord](https://discord.gg/IRLtools)
- [IRLhosting.com](https://irlhosting.com)

## License

GNU General Public License v2.0. See [LICENSE](LICENSE).
