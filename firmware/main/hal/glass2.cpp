#include "glass2.h"

#include "board/config.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr char kTag[] = "GLASS2";
constexpr std::array<std::uint8_t, 2> kAddresses = {0x3c, 0x3d};
constexpr int kProbeAttempts = 5;
constexpr int kProbeTimeoutMs = 30;
constexpr int kPowerSettleMs = 250;
constexpr int kRetryDelayMs = 100;
constexpr int kWidth = 128;
constexpr int kHeight = 64;
constexpr int kBufferSize = kWidth * kHeight / 8;

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_device = nullptr;
std::uint8_t s_address = 0;
std::array<std::uint8_t, kBufferSize> s_framebuffer{};
std::mutex s_mutex;

struct Usage {
    int percent = 0;
    std::int64_t updated_at_unix_seconds = 0;
};

Usage s_usage;

esp_err_t write_commands(const std::uint8_t* commands, std::size_t length)
{
    std::array<std::uint8_t, 32> packet{};
    if (length + 1 > packet.size()) {
        return ESP_ERR_INVALID_SIZE;
    }

    packet[0] = 0x00;
    std::memcpy(packet.data() + 1, commands, length);
    return i2c_master_transmit(s_device, packet.data(), length + 1, 100);
}

esp_err_t flush_framebuffer()
{
    const std::uint8_t window_commands[] = {
        0x21, 0x00, 0x7f,  // Column range
        0x22, 0x00, 0x07,  // Page range
    };
    esp_err_t error = write_commands(window_commands, sizeof(window_commands));
    if (error != ESP_OK) {
        return error;
    }

    static std::array<std::uint8_t, kBufferSize + 1> packet;
    packet[0] = 0x40;
    std::memcpy(packet.data() + 1, s_framebuffer.data(), s_framebuffer.size());
    return i2c_master_transmit(s_device, packet.data(), packet.size(), 100);
}

const std::array<std::uint8_t, 5>& glyph(char character)
{
    static constexpr std::array<std::uint8_t, 5> blank = {0, 0, 0, 0, 0};
    static constexpr std::array<std::uint8_t, 5> digits[] = {
        {{0x3e, 0x51, 0x49, 0x45, 0x3e}},
        {{0x00, 0x42, 0x7f, 0x40, 0x00}},
        {{0x42, 0x61, 0x51, 0x49, 0x46}},
        {{0x21, 0x41, 0x45, 0x4b, 0x31}},
        {{0x18, 0x14, 0x12, 0x7f, 0x10}},
        {{0x27, 0x45, 0x45, 0x45, 0x39}},
        {{0x3c, 0x4a, 0x49, 0x49, 0x30}},
        {{0x01, 0x71, 0x09, 0x05, 0x03}},
        {{0x36, 0x49, 0x49, 0x49, 0x36}},
        {{0x06, 0x49, 0x49, 0x29, 0x1e}},
    };
    static constexpr std::array<std::uint8_t, 5> capital_g = {0x3e, 0x41, 0x49, 0x49, 0x7a};
    static constexpr std::array<std::uint8_t, 5> lower_r = {0x7c, 0x08, 0x04, 0x04, 0x08};
    static constexpr std::array<std::uint8_t, 5> lower_o = {0x38, 0x44, 0x44, 0x44, 0x38};
    static constexpr std::array<std::uint8_t, 5> lower_k = {0x7f, 0x10, 0x28, 0x44, 0x00};
    static constexpr std::array<std::uint8_t, 5> percent = {0x63, 0x13, 0x08, 0x64, 0x63};

    if (character >= '0' && character <= '9') {
        return digits[character - '0'];
    }
    switch (character) {
        case 'G':
            return capital_g;
        case 'r':
            return lower_r;
        case 'o':
            return lower_o;
        case 'k':
            return lower_k;
        case '%':
            return percent;
        default:
            return blank;
    }
}

void set_pixel(int x, int y)
{
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) {
        return;
    }
    s_framebuffer[x + (y / 8) * kWidth] |= static_cast<std::uint8_t>(1U << (y & 7));
}

void draw_text(const char* text)
{
    constexpr int scale = 2;
    constexpr int character_width = 6 * scale;
    const int text_width = static_cast<int>(std::strlen(text)) * character_width - scale;
    int cursor_x = (kWidth - text_width) / 2;
    constexpr int cursor_y = (kHeight - 7 * scale) / 2;

    for (; *text != '\0'; ++text, cursor_x += character_width) {
        const auto& columns = glyph(*text);
        for (int column = 0; column < static_cast<int>(columns.size()); ++column) {
            for (int row = 0; row < 7; ++row) {
                if ((columns[column] & (1U << row)) == 0) {
                    continue;
                }
                for (int dx = 0; dx < scale; ++dx) {
                    for (int dy = 0; dy < scale; ++dy) {
                        set_pixel(cursor_x + column * scale + dx, cursor_y + row * scale + dy);
                    }
                }
            }
        }
    }
}

void release_bus()
{
    if (s_device != nullptr) {
        i2c_master_bus_rm_device(s_device);
        s_device = nullptr;
    }
    if (s_bus != nullptr) {
        i2c_del_master_bus(s_bus);
        s_bus = nullptr;
    }
    s_address = 0;
}

std::uint8_t probe_glass2_addresses()
{
    // ESP-IDF's i2c_master_probe() always probes at 100 kHz, independent of
    // the eventual device transaction speed.
    for (int attempt = 1; attempt <= kProbeAttempts; ++attempt) {
        for (const std::uint8_t address : kAddresses) {
            if (i2c_master_probe(s_bus, address, kProbeTimeoutMs) == ESP_OK) {
                ESP_LOGI(kTag, "glass2: probe found 0x%02x on attempt %d/%d", address, attempt, kProbeAttempts);
                return address;
            }
        }

        ESP_LOGI(kTag, "glass2: probe attempt %d/%d: no response at 0x3c or 0x3d", attempt, kProbeAttempts);
        if (attempt < kProbeAttempts) {
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
        }
    }
    return 0;
}

std::uint8_t scan_port_a()
{
    ESP_LOGI(kTag, "glass2: scanning Port A I2C0 SDA=%d SCL=%d", PORT_A_I2C_SDA_PIN, PORT_A_I2C_SCL_PIN);

    int found_count = 0;
    std::uint8_t glass2_address = 0;
    for (std::uint16_t address = 0x01; address < 0x7f; ++address) {
        if (i2c_master_probe(s_bus, address, kProbeTimeoutMs) != ESP_OK) {
            continue;
        }

        ++found_count;
        ESP_LOGI(kTag, "glass2: scan found address 0x%02x", address);
        if (address == kAddresses[0] || address == kAddresses[1]) {
            glass2_address = static_cast<std::uint8_t>(address);
        }
    }

    ESP_LOGI(kTag, "glass2: scan complete, %d responding address(es)", found_count);
    return glass2_address;
}

}  // namespace

bool glass2_init()
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = PORT_A_I2C_PORT,
        .sda_io_num = PORT_A_I2C_SDA_PIN,
        .scl_io_num = PORT_A_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags =
            {
                .enable_internal_pullup = 1,
            },
    };

    esp_err_t error = i2c_new_master_bus(&bus_config, &s_bus);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "glass2: not found (Port A bus: %s)", esp_err_to_name(error));
        release_bus();
        return false;
    }

    // Hal::init calls us only after xiaozhi_board_init() returns. That board
    // constructor initializes AXP2101 and AW9523 before returning; allow Port A
    // power and the Glass2 controller additional time to settle.
    ESP_LOGI(kTag, "glass2: CoreS3 AXP2101/AW9523 init complete; waiting %d ms for Port A power", kPowerSettleMs);
    vTaskDelay(pdMS_TO_TICKS(kPowerSettleMs));

    s_address = probe_glass2_addresses();
    if (s_address == 0) {
        s_address = scan_port_a();
    }
    if (s_address == 0) {
        ESP_LOGW(kTag, "glass2: not found");
        release_bus();
        return false;
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_address,
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
        .flags =
            {
                .disable_ack_check = 0,
            },
    };
    error = i2c_master_bus_add_device(s_bus, &device_config, &s_device);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "glass2: not found (device setup: %s)", esp_err_to_name(error));
        release_bus();
        return false;
    }

    // Matches M5GFX's Panel_SSD1306 sequence used for the SSD1309 Glass2,
    // with an explicit visible contrast value.
    const std::uint8_t init_commands[] = {
        0xae,       // Display off
        0xd5, 0x80, // Clock divisor
        0xa8, 0x3f, // 64-row multiplex
        0xd3, 0x00, // Display offset
        0x40,       // Start line
        0x20, 0x00, // Horizontal addressing
        0xa1,       // Reverse segment mapping for the mounted orientation
        0xc8,       // Reverse COM scan direction (180 degrees with 0xa1)
        0xdb, 0x10, // VCOM deselect
        0xa4,       // Resume RAM display
        0x2e,       // Disable scrolling
        0x8d, 0x14, // Charge pump
        0x81, 0xcf, // Contrast
        0xd9, 0x11, // Precharge
        0xaf,       // Display on
    };
    error = write_commands(init_commands, sizeof(init_commands));
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "glass2: not found (init: %s)", esp_err_to_name(error));
        release_bus();
        return false;
    }

    s_framebuffer.fill(0);
    error = flush_framebuffer();
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "glass2: not found (clear: %s)", esp_err_to_name(error));
        release_bus();
        return false;
    }

    ESP_LOGI(kTag, "glass2: OK addr=0x%02x Port A SDA=%d SCL=%d", s_address, PORT_A_I2C_SDA_PIN,
             PORT_A_I2C_SCL_PIN);
    return true;
}

bool glass2_update_usage(int percent, std::int64_t updated_at_unix_seconds)
{
    if (percent < 0 || percent > 100) {
        ESP_LOGW(kTag, "glass2: rejected usage percent %d", percent);
        return false;
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_device == nullptr) {
        return false;
    }

    s_usage = {
        .percent = percent,
        .updated_at_unix_seconds = updated_at_unix_seconds,
    };

    char line[16];
    std::snprintf(line, sizeof(line), "Grok %d%%", s_usage.percent);
    s_framebuffer.fill(0);
    draw_text(line);

    const esp_err_t error = flush_framebuffer();
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "glass2: update failed: %s", esp_err_to_name(error));
        return false;
    }

    ESP_LOGI(kTag, "glass2: usage=%d updated_at=%lld", s_usage.percent,
             static_cast<long long>(s_usage.updated_at_unix_seconds));
    return true;
}
