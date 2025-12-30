#include "sayo_screen_capture.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <fstream>

#if _DEBUG
#include <iostream>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "hidapi.h"

namespace sayo {
    namespace {
        std::wstring to_lower_copy(std::wstring s) {
            for (wchar_t& ch : s) {
                ch = static_cast<wchar_t>(std::towlower(ch));
            }
            return s;
        }

        bool wcontains_ci(const wchar_t* str, const std::wstring& search) {
            if (search.empty()) {
                return true;
            }
            if (!str) {
                return false;
            }
            std::wstring wstr(str);
            const std::wstring hLower = to_lower_copy(std::move(wstr));
            const std::wstring nLower = to_lower_copy(search);
            return hLower.find(nLower) != std::wstring::npos;
        }

        bool matches_device_filters(const hid_device_info* it, const DeviceIds& ids) {
            if (!it) {
                return false;
            }
            if (wcontains_ci(it->manufacturer_string, ids.manufacturerContains)) {
                return true;
            }
            if (wcontains_ci(it->product_string, ids.productContains)) {
                return true;
            }
            return false;
        }

        uint16_t crc16_sum_words_le(const uint8_t* data, const size_t len) {
            uint16_t crc = 0;
            for (size_t i = 0; i < len; i++) {
                uint16_t contribution = data[i];
                if ((i & 1u) != 0u) {
                    contribution = static_cast<uint16_t>(contribution << 8);
                }
                crc = static_cast<uint16_t>(crc + contribution);
            }
            return crc;
        }

        HidHeader parse_header(const uint8_t* report, const size_t reportLen) {
            (void)reportLen;
            HidHeader h{};
            h.reportId = report[0];
            h.echo = report[1];
            h.crc = static_cast<uint16_t>(report[2] | (static_cast<uint16_t>(report[3]) << 8));
            const uint16_t staLen = static_cast<uint16_t>(report[4] | (static_cast<uint16_t>(report[5]) << 8));
            h.status = static_cast<uint8_t>(staLen >> 10);
            h.len = static_cast<uint16_t>(staLen & 0x03FF);
            h.cmd = report[6];
            h.index = report[7];
            return h;
        }

        bool verify_crc(const uint8_t* report, const size_t reportLen, const size_t headerSize) {
            if (reportLen < headerSize) {
                return false;
            }
            const uint16_t packetCrc = static_cast<uint16_t>(report[2] | (static_cast<uint16_t>(report[3]) << 8));
            uint16_t crc = 0;
            for (size_t i = 0; i < reportLen; i++) {
                uint8_t byte = report[i];
                if (i == 2 || i == 3) {
                    byte = 0;
                }

                uint16_t contribution = byte;
                if ((i & 1u) != 0u) {
                    contribution = static_cast<uint16_t>(contribution << 8);
                }
                crc = static_cast<uint16_t>(crc + contribution);
            }
            return packetCrc == crc;
        }

        std::vector<uint8_t> build_report_22(
            const uint8_t echo,
            const uint8_t cmd,
            const uint8_t index,
            const std::vector<uint8_t>& body,
            const size_t headerSize,
            const size_t reportLen22) {
            std::vector<uint8_t> out(reportLen22, 0);
            out[0] = 0x22;
            out[1] = echo;
            out[2] = 0;
            out[3] = 0;

            // sayo_api_rs sets header.len to (body_len + 0x04)
            const uint16_t lenField = static_cast<uint16_t>(body.size() + 0x04);
            const uint16_t staLen = static_cast<uint16_t>((0x00u << 10) | (lenField & 0x03FFu));
            out[4] = static_cast<uint8_t>(staLen & 0xFF);
            out[5] = static_cast<uint8_t>((staLen >> 8) & 0xFF);
            out[6] = cmd;
            out[7] = index;

            for (size_t i = 0; i < body.size() && (headerSize + i) < out.size(); i++) {
                out[headerSize + i] = body[i];
            }

            // Compute CRC with crc field set to 0.
            const uint16_t crc = crc16_sum_words_le(out.data(), out.size());
            out[2] = static_cast<uint8_t>(crc & 0xFF);
            out[3] = static_cast<uint8_t>((crc >> 8) & 0xFF);
            return out;
        }

        void write_u16_le(std::ofstream& f, const uint16_t v) {
            f.put(static_cast<char>(v & 0xFF));
            f.put(static_cast<char>((v >> 8) & 0xFF));
        }

        void write_u32_le(std::ofstream& f, const uint32_t v) {
            f.put(static_cast<char>(v & 0xFF));
            f.put(static_cast<char>((v >> 8) & 0xFF));
            f.put(static_cast<char>((v >> 16) & 0xFF));
            f.put(static_cast<char>((v >> 24) & 0xFF));
        }

        void write_i32_le(std::ofstream& f, const int32_t v) {
            write_u32_le(f, static_cast<uint32_t>(v));
        }
    }

#if _DEBUG 
    void DumpDevices(const DeviceIds& ids, const OutputStream output) {
        std::ostream& out = (output == OutputStream::StdErr) ? std::cerr : std::cout;
        std::wostream& wout = (output == OutputStream::StdErr) ? std::wcerr : std::wcout;
        hid_device_info* devs = hid_enumerate(ids.vid, ids.pid);
        const hid_device_info* cur = devs;
        out << "Found devices for VID=" << std::hex << ids.vid << " PID=" << ids.pid << std::dec << "\n";
        while (cur) {
            if (!matches_device_filters(cur, ids)) {
                cur = cur->next;
                continue;
            }
            out << "- path=" << (cur->path ? cur->path : "")
                << " interface=" << cur->interface_number
                << " usage_page=0x" << std::hex << cur->usage_page
                << " usage=0x" << cur->usage
                << std::dec << "\n";
            if (cur->manufacturer_string) {
                wout << L"  manufacturer: " << cur->manufacturer_string << L"\n";
            }
            if (cur->product_string) {
                wout << L"  product: " << cur->product_string << L"\n";
            }
            if (cur->serial_number) {
                wout << L"  serial: " << cur->serial_number << L"\n";
            }
            cur = cur->next;
        }
        hid_free_enumeration(devs);
    }
#endif

    OpenResult OpenVendorInterface(const DeviceIds& ids, const OutputStream output) {
#if !_DEBUG
        (void)output;
#endif
        OpenResult result{};

        auto pick_best = [&](hid_device_info* devsList) -> const hid_device_info* {
            const hid_device_info* best = nullptr;

            // IMPORTANT: MI_01 exposes multiple top-level collections.
            // Some of them are keyboard/consumer/etc and will deny WriteFile.
            // The report-id 0x22 channel is typically exposed as usage_page=0xFF12 usage=0x02.
            auto find_first = [&](const unsigned short usagePage, const unsigned short usage) -> const hid_device_info* {
                for (const hid_device_info* it = devsList; it; it = it->next) {
                    if (it->interface_number != 1) {
                        continue;
                    }
                    if (!matches_device_filters(it, ids)) {
                        continue;
                    }
                    if (it->usage_page == usagePage && it->usage == usage) {
                        return it;
                    }
                }
                return nullptr;
            };

            best = find_first(0xFF12, 0x0002);
            if (!best) {
                best = find_first(0xFF11, 0x0002);
            }
            if (!best) {
                best = find_first(0xFF00, 0x0001);
            }
            if (!best) {
                // last resort: pick the first interface-1 collection that is not a standard desktop/consumer page.
                for (const hid_device_info* it = devsList; it; it = it->next) {
                    if (it->interface_number != 1) {
                        continue;
                    }
                    if (!matches_device_filters(it, ids)) {
                        continue;
                    }
                    if (it->usage_page >= 0xFF00) {
                        best = it;
                        break;
                    }
                }
            }

            return best;
        };

        hid_device_info* devs = hid_enumerate(ids.vid, ids.pid);
        const hid_device_info* best = pick_best(devs);
        if (!best && (ids.vid != 0 || ids.pid != 0)) {
#if _DEBUG
            std::ostream& out = (output == OutputStream::StdErr) ? std::cerr : std::cout;
            out << "No matching devices found for VID/PID, falling back to enumerate all HID devices.\n";
#endif
            hid_free_enumeration(devs);
            devs = hid_enumerate(0, 0);
            best = pick_best(devs);
        }

        if (best && best->path) {
#if _DEBUG
            std::ostream& out = (output == OutputStream::StdErr) ? std::cerr : std::cout;
            out << "Opening path: " << best->path
                << " (interface=" << best->interface_number
                << " usage_page=0x" << std::hex << best->usage_page
                << " usage=0x" << best->usage << std::dec << ")\n";
#endif
            result.handle = hid_open_path(best->path);
            result.openedPath = best->path;
            result.usagePage = best->usage_page;
            result.usage = best->usage;
            result.interfaceNumber = best->interface_number;
        }

        hid_free_enumeration(devs);
        return result;
    }

    std::optional<std::pair<uint16_t, uint16_t>> TryGetLcdSize(hid_device* dev, const ProtocolConstants& proto) {
        // Request SystemInfo (CMD 0x02), index 0, empty body.
        const std::vector<uint8_t> out = build_report_22(proto.echo, proto.cmdSystemInfo, 0x00, {}, proto.headerSize,
                                         proto.reportLen22);
        const int response = hid_write(dev, out.data(), static_cast<int>(out.size()));
        if (response < 0) {
            return std::nullopt;
        }

        std::vector<uint8_t> in(proto.reportLen22, 0);
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(proto.commandTimeoutMs)) {
            const int r = hid_read_timeout(dev, in.data(), static_cast<int>(in.size()),
                                           static_cast<int>(proto.readTimeoutMs));
            if (r <= 0) {
                continue;
            }
            if (static_cast<size_t>(r) < proto.headerSize) {
                continue;
            }

            const HidHeader h = parse_header(in.data(), static_cast<size_t>(r));
            if (h.reportId != proto.reportId22) {
                continue;
            }
            if (h.echo != proto.echo && h.echo != 0x00) {
                continue;
            }
            if (!verify_crc(in.data(), static_cast<size_t>(r), proto.headerSize)) {
                continue;
            }
            if (h.cmd != proto.cmdSystemInfo) {
                continue;
            }

            const size_t dataEnd = static_cast<size_t>(h.len) + 4;
            if (dataEnd <= proto.headerSize || dataEnd > in.size()) {
                continue;
            }
            const size_t payloadLen = dataEnd - proto.headerSize;
            if (payloadLen < 5) {
                continue;
            }
            const uint8_t* payload = in.data() + proto.headerSize;
            const uint16_t w = static_cast<uint16_t>(payload[0] | (static_cast<uint16_t>(payload[1]) << 8));
            const uint16_t hgt = static_cast<uint16_t>(payload[2] | (static_cast<uint16_t>(payload[3]) << 8));
            return std::make_pair(w, hgt);
        }

        return std::nullopt;
    }

    bool CaptureScreenFrame(
        hid_device* handle,
        const uint16_t lcdW,
        const uint16_t lcdH,
        std::vector<uint8_t>& scratchIn,
        std::vector<uint8_t>& outRgb565,
        CaptureStats* stats,
        const ProtocolConstants& proto) {
        const size_t expectedFrameBytes = static_cast<size_t>(lcdW) * static_cast<size_t>(lcdH) * 2;
        if (lcdW == 0 || lcdH == 0) {
            return false;
        }
        if (scratchIn.size() != proto.reportLen22) {
            scratchIn.assign(proto.reportLen22, 0);
        }
        if (outRgb565.size() != expectedFrameBytes) {
            outRgb565.assign(expectedFrameBytes, 0);
        }

        if (stats) {
            stats->packets = 0;
            stats->bytesCovered = 0;
            stats->durationMs = 0;
        }

        const std::vector<uint8_t> req = build_report_22(proto.echo, proto.cmdScreenBuffer, 0x00, {}, proto.headerSize,
                                         proto.reportLen22);
        const auto t0 = std::chrono::steady_clock::now();
        const int response = hid_write(handle, req.data(), static_cast<int>(req.size()));
        if (response < 0) {
            return false;
        }

        size_t maxEnd = 0;
        auto lastChunk = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(proto.commandTimeoutMs)) {
            const int r = hid_read_timeout(handle, scratchIn.data(), static_cast<int>(scratchIn.size()),
                                           static_cast<int>(proto.readTimeoutMs));
            if (r <= 0) {
                if (maxEnd >= expectedFrameBytes && (std::chrono::steady_clock::now() - lastChunk) >
                    std::chrono::milliseconds(proto.idleBreakMs)) {
                    break;
                }
                continue;
            }
            if (static_cast<size_t>(r) < proto.headerSize) {
                continue;
            }
            if (scratchIn[0] != proto.reportId22) {
                continue;
            }
            if (scratchIn[1] != proto.echo && scratchIn[1] != 0x00) {
                continue;
            }
            if (scratchIn[6] != proto.cmdScreenBuffer) {
                continue;
            }
            if (!verify_crc(scratchIn.data(), static_cast<size_t>(r), proto.headerSize)) {
                continue;
            }

            if (stats) {
                stats->packets++;
            }

            const HidHeader h = parse_header(scratchIn.data(), static_cast<size_t>(r));
            const size_t dataEnd = static_cast<size_t>(h.len) + 4;
            if (dataEnd <= proto.headerSize || dataEnd > scratchIn.size()) {
                continue;
            }
            const size_t payloadLen = dataEnd - proto.headerSize;
            if (payloadLen < 4) {
                continue;
            }
            const uint8_t* payload = scratchIn.data() + proto.headerSize;
            const uint32_t addr = payload[0] | static_cast<uint32_t>(payload[1]) << 8 |
                static_cast<uint32_t>(payload[2]) << 16 | static_cast<uint32_t>(payload[3]) << 24;
            const size_t bytesLen = payloadLen - 4;
            if (bytesLen == 0) {
                continue;
            }
            const size_t end = static_cast<size_t>(addr) + bytesLen;
            if (end <= outRgb565.size()) {
                std::memcpy(outRgb565.data() + addr, payload + 4, bytesLen);
            }
            maxEnd = (std::max)(maxEnd, end);
            lastChunk = std::chrono::steady_clock::now();
            if (maxEnd >= expectedFrameBytes) {
                break;
            }
        }

        if (stats) {
            stats->bytesCovered = static_cast<uint32_t>((std::min)(maxEnd, expectedFrameBytes));
            stats->durationMs = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        }

        return maxEnd > 0;
    }

#if _DEBUG
    bool WriteRgb565BinFile(const std::string& path, const std::vector<uint8_t>& rgb565, const uint16_t width,
                            const uint16_t height) {
        const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 2;
        if (rgb565.size() < expected || width == 0 || height == 0) {
            return false;
        }

        std::ofstream f(path, std::ios::binary);
        if (!f) {
            return false;
        }
        f.write(reinterpret_cast<const char*>(rgb565.data()), static_cast<std::streamsize>(expected));
        return static_cast<bool>(f);
    }

    bool WriteBmpFromRgb565(const std::string& path, const std::vector<uint8_t>& rgb565, const uint16_t width,
                            const uint16_t height) {
        const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 2;
        if (rgb565.size() < expected || width == 0 || height == 0) {
            return false;
        }

        const uint32_t rowSize = (static_cast<uint32_t>(width) * 3u + 3u) & ~3u; // 4-byte aligned
        const uint32_t imageSize = rowSize * static_cast<uint32_t>(height);
        const uint32_t fileSize = 14u + 40u + imageSize;

        std::ofstream f(path, std::ios::binary);
        if (!f) {
            return false;
        }

        // BITMAPFILEHEADER (14 bytes)
        f.put('B');
        f.put('M');
        write_u32_le(f, fileSize);
        write_u16_le(f, 0);
        write_u16_le(f, 0);
        write_u32_le(f, 14u + 40u);

        // BITMAPINFOHEADER (40 bytes)
        write_u32_le(f, 40u);
        write_i32_le(f, width);
        // Negative height = top-down DIB, so we can write rows in natural order.
        write_i32_le(f, -static_cast<int32_t>(height));
        write_u16_le(f, 1); // planes
        write_u16_le(f, 24); // bpp
        write_u32_le(f, 0); // BI_RGB
        write_u32_le(f, imageSize);
        write_i32_le(f, 2835); // 72 DPI
        write_i32_le(f, 2835);
        write_u32_le(f, 0);
        write_u32_le(f, 0);

        const uint32_t padding = rowSize - static_cast<uint32_t>(width) * 3u;
        constexpr char pad[3] = {0, 0, 0};

        for (uint16_t y = 0; y < height; y++) {
            for (uint16_t x = 0; x < width; x++) {
                const size_t i = (static_cast<size_t>(y) * width + x) * 2;
                const uint16_t v = static_cast<uint16_t>(rgb565[i] | (static_cast<uint16_t>(rgb565[i + 1]) << 8));
                const uint8_t r = static_cast<uint8_t>(((v >> 11) & 0x1F) * 255 / 31);
                const uint8_t g = static_cast<uint8_t>(((v >> 5) & 0x3F) * 255 / 63);
                const uint8_t b = static_cast<uint8_t>((v & 0x1F) * 255 / 31);
                // BMP pixel order is B, G, R
                f.put(static_cast<char>(b));
                f.put(static_cast<char>(g));
                f.put(static_cast<char>(r));
            }
            if (padding) {
                f.write(pad, padding);
            }
        }

        return static_cast<bool>(f);
    }
#endif

#if defined(_WIN32)
    bool BlitRgb565ToHdc(
        void* hdcVoid,
        const std::vector<uint8_t>& rgb565,
        const uint16_t srcW,
        const uint16_t srcH,
        const int dstX,
        const int dstY,
        const int dstW,
        const int dstH) {
        if (!hdcVoid || srcW == 0 || srcH == 0) {
            return false;
        }
        const size_t expected = static_cast<size_t>(srcW) * static_cast<size_t>(srcH) * 2;
        if (rgb565.size() < expected) {
            return false;
        }

        const HDC hdc = reinterpret_cast<HDC>(hdcVoid);
        struct Bmi565 {
            BITMAPINFOHEADER hdr;
            DWORD masks[3];
        } bmi{};
        bmi.hdr.biSize = sizeof(BITMAPINFOHEADER);
        bmi.hdr.biWidth = static_cast<LONG>(srcW);
        bmi.hdr.biHeight = -static_cast<LONG>(srcH); // top-down
        bmi.hdr.biPlanes = 1;
        bmi.hdr.biBitCount = 16;
        bmi.hdr.biCompression = BI_BITFIELDS;
        bmi.masks[0] = 0xF800; // R
        bmi.masks[1] = 0x07E0; // G
        bmi.masks[2] = 0x001F; // B

        SetStretchBltMode(hdc, COLORONCOLOR);
        const int ok = StretchDIBits(
            hdc,
            dstX,
            dstY,
            dstW,
            dstH,
            0,
            0,
            srcW,
            srcH,
            rgb565.data(),
            reinterpret_cast<BITMAPINFO*>(&bmi),
            DIB_RGB_COLORS,
            SRCCOPY);

        return ok != GDI_ERROR;
    }
#endif
}
