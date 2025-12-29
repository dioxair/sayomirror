#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct hid_device;
struct hid_device_info;

namespace sayo {
    struct HidHeader {
        uint8_t reportId{};
        uint8_t echo{};
        uint16_t crc{};
        uint8_t status{};
        uint16_t len{}; // 10-bit length (sayo_api_rs stores body_len + 4)
        uint8_t cmd{};
        uint8_t index{};
    };

    struct CaptureStats {
        uint32_t packets = 0;
        uint32_t bytesCovered = 0;
        uint32_t durationMs = 0;
    };

    // TODO: I doubt these are the same for all models and all revisions of the O3C/O3C++, figure out a way to account for that.
    // anyway shoutout to this sketchy ass german website for saving this project: https://www.uwe-sieber.de/usbtreeview_e.html#download
    struct DeviceIds {
        unsigned short vid = 0x8089;
        unsigned short pid = 0x0009;
    };

    struct ProtocolConstants {
        uint8_t reportId22 = 0x22; // 1023-byte report payload + report id
        uint8_t echo = 0x13; // native echo used by sayo_api_rs
        uint8_t cmdSystemInfo = 0x02;
        uint8_t cmdScreenBuffer = 0x25;
        size_t headerSize = 8;
        size_t reportLen22 = 1024;
        uint32_t readTimeoutMs = 50;
        uint32_t commandTimeoutMs = 1500;
        uint32_t idleBreakMs = 10;
    };

    struct OpenResult {
        hid_device* handle = nullptr;
        std::string openedPath;
        unsigned short usagePage = 0;
        unsigned short usage = 0;
        int interfaceNumber = -1;
    };

    enum class OutputStream {
        StdErr,
        StdOut
    };

    // Enumerate available HID collections matching the device IDs. For debug
    void DumpDevices(const DeviceIds& ids, OutputStream output);

    // Opens the vendor HID collection that can accept report-id 0x22 writes.
    OpenResult OpenVendorInterface(const DeviceIds& ids, OutputStream output);

    // Queries LCD size via SystemInfo (CMD 0x02). Returns nullopt on timeout.
    std::optional<std::pair<uint16_t, uint16_t>> TryGetLcdSize(
        hid_device* dev,
        const ProtocolConstants& proto = {});

    // Captures the screen buffer into RGB565 (little-endian, 2 bytes/pixel).
    // outRgb565 will be resized to width * height * 2.
    bool CaptureScreenFrame(
        hid_device* handle,
        uint16_t lcdW,
        uint16_t lcdH,
        std::vector<uint8_t>& scratchIn,
        std::vector<uint8_t>& outRgb565,
        CaptureStats* stats = nullptr,
        const ProtocolConstants& proto = {});

    // Writes raw RGB565 bytes to a file, exactly width * height * 2 bytes.
    bool WriteRgb565BinFile(
        const std::string& path,
        const std::vector<uint8_t>& rgb565,
        uint16_t width,
        uint16_t height);

    // Interprets input as little-endian RGB565 (2 bytes/pixel) and writes a 24bpp BMP.
    bool WriteBmpFromRgb565(
        const std::string& path,
        const std::vector<uint8_t>& rgb565,
        uint16_t width,
        uint16_t height);

#if defined(_WIN32)
    // Convenience helper for Win32 rendering: blit RGB565 into an HDC.
    // dstW/dstH can be scaled size; use COLORONCOLOR stretch for nearest-ish.
    bool BlitRgb565ToHdc(
        void* hdc,
        const std::vector<uint8_t>& rgb565,
        uint16_t srcW,
        uint16_t srcH,
        int dstX,
        int dstY,
        int dstW,
        int dstH);
#endif
}
