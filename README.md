# sayomirror
<img width="1762" height="912" alt="Image of a SayoDevice O3C++'s screen being mirrored through sayomirror" src="https://github.com/user-attachments/assets/bc7d89cf-2d1a-43d5-8877-cb304595d7b9" />

A small Win32 C++ app that displays the internal screen of your SayoDevice O3C/O3C++ in a desktop window 

**If you run into any issues, please make a [GitHub Issue](https://github.com/dioxair/sayomirror/issues) since I've only tested this on one device!**

## Usage

- [Download the latest sayomirror.zip file from the GitHub releases page](https://github.com/dioxair/sayomirror/releases/latest/download/sayomirror.zip)
- Extract the sayomirror.zip file to a folder anywhere and run the .exe
- Bonus: Double-click inside the window to re-fit the window to scale according to your monitor resolution
- Bonus 2: Shift + double-click inside the window to re-fit the window to your SayoDevice screen's native resolution

## License / third-party
This project is under the GPL-3.0 license

This repository includes third-party code/binaries:
- HIDAPI headers in [lib/hidapi-win/include/](lib/hidapi-win/include/)

See the upstream HIDAPI project for its licensing details:
https://github.com/libusb/hidapi

---

**[Big McThankies From McSpankies](https://www.youtube.com/watch?v=VEAW9PHAZ7s) to [svr2kos2](https://github.com/svr2kos2) and [khang06](https://github.com/khang06)**

The `sayo_screen_capture` protocol logic is a C++ reimplementation based on [svr2kos2](https://github.com/svr2kos2)'s [sayo_api_rs](https://github.com/svr2kos2/sayo_api_rs) and [sayo_rs (sayodevice_protocol.xlsx)](https://github.com/svr2kos2/sayo_rs)

[khang06](https://github.com/khang06)'s [O3C Internals doc](https://gist.github.com/khang06/6186543b560548370ce7cc08cad7f710) also helped me out a lot at later stages of the project
