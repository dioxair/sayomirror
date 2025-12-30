# sayomirror
<img width="1762" height="932" alt="sayomirror_KDQStWV0hV" src="https://github.com/user-attachments/assets/3434afd0-0ba2-4836-b1f1-fca85e7e913e" />

A small Win32 C++ app displays the internal screen of your SayoDevice O3C/O3C++ in a desktop window 

## Usage

- [Download the .zip from the GitHub Releases page](https://github.com/dioxair/sayomirror/releases/latest/sayomirror.zip)
- Extract the .zip file to a folder anywhere and run the .exe
- Bonus: Double-click inside the window to re-fit the window to the device aspect/scale

## License / third-party
This project is under the GPL-3.0 license

This repository includes third-party code/binaries:
- HIDAPI headers in [lib/hidapi-win/include/](lib/hidapi-win/include/)
- hidapi binaries in [lib/hidapi-win/x64/](lib/hidapi-win/x64/)

See the upstream HIDAPI project for its licensing details:
https://github.com/libusb/hidapi

The `sayo_screen_capture` protocol logic is a C++ reimplementation based on [svr2kos2](https://github.com/svr2kos2)'s [sayo_api_rs](https://github.com/svr2kos2/sayo_api_rs) and [sayo_rs (sayodevice_protocol.xlsx)](https://github.com/svr2kos2/sayo_rs)
