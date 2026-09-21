#include "glass2.h"

#include "board/config.h"

#include <array>
#include <cctype>
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
constexpr std::size_t kMaxServiceIdLength = 15;

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_device = nullptr;
std::uint8_t s_address = 0;
std::array<std::uint8_t, kBufferSize> s_framebuffer{};
std::mutex s_mutex;

struct UsageSlot {
    std::array<char, kMaxServiceIdLength + 1> id{};
    int percent = 0;
    bool infinite = false;
    bool occupied = false;
};

std::array<UsageSlot, GLASS2_USAGE_SLOT_COUNT> s_usage_slots{};
std::int64_t s_updated_at_unix_seconds = 0;

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
    static constexpr std::array<std::uint8_t, 5> uppercase[] = {
        {{0x7e, 0x11, 0x11, 0x11, 0x7e}}, {{0x7f, 0x49, 0x49, 0x49, 0x36}},
        {{0x3e, 0x41, 0x41, 0x41, 0x22}}, {{0x7f, 0x41, 0x41, 0x22, 0x1c}},
        {{0x7f, 0x49, 0x49, 0x49, 0x41}}, {{0x7f, 0x09, 0x09, 0x09, 0x01}},
        {{0x3e, 0x41, 0x49, 0x49, 0x7a}}, {{0x7f, 0x08, 0x08, 0x08, 0x7f}},
        {{0x00, 0x41, 0x7f, 0x41, 0x00}}, {{0x20, 0x40, 0x41, 0x3f, 0x01}},
        {{0x7f, 0x08, 0x14, 0x22, 0x41}}, {{0x7f, 0x40, 0x40, 0x40, 0x40}},
        {{0x7f, 0x02, 0x0c, 0x02, 0x7f}}, {{0x7f, 0x04, 0x08, 0x10, 0x7f}},
        {{0x3e, 0x41, 0x41, 0x41, 0x3e}}, {{0x7f, 0x09, 0x09, 0x09, 0x06}},
        {{0x3e, 0x41, 0x51, 0x21, 0x5e}}, {{0x7f, 0x09, 0x19, 0x29, 0x46}},
        {{0x46, 0x49, 0x49, 0x49, 0x31}}, {{0x01, 0x01, 0x7f, 0x01, 0x01}},
        {{0x3f, 0x40, 0x40, 0x40, 0x3f}}, {{0x1f, 0x20, 0x40, 0x20, 0x1f}},
        {{0x3f, 0x40, 0x38, 0x40, 0x3f}}, {{0x63, 0x14, 0x08, 0x14, 0x63}},
        {{0x07, 0x08, 0x70, 0x08, 0x07}}, {{0x61, 0x51, 0x49, 0x45, 0x43}},
    };
    static constexpr std::array<std::uint8_t, 5> lowercase[] = {
        {{0x20, 0x54, 0x54, 0x54, 0x78}}, {{0x7f, 0x48, 0x44, 0x44, 0x38}},
        {{0x38, 0x44, 0x44, 0x44, 0x20}}, {{0x38, 0x44, 0x44, 0x48, 0x7f}},
        {{0x38, 0x54, 0x54, 0x54, 0x18}}, {{0x08, 0x7e, 0x09, 0x01, 0x02}},
        {{0x0c, 0x52, 0x52, 0x52, 0x3e}}, {{0x7f, 0x08, 0x04, 0x04, 0x78}},
        {{0x00, 0x44, 0x7d, 0x40, 0x00}}, {{0x20, 0x40, 0x44, 0x3d, 0x00}},
        {{0x7f, 0x10, 0x28, 0x44, 0x00}}, {{0x00, 0x41, 0x7f, 0x40, 0x00}},
        {{0x7c, 0x04, 0x18, 0x04, 0x78}}, {{0x7c, 0x08, 0x04, 0x04, 0x78}},
        {{0x38, 0x44, 0x44, 0x44, 0x38}}, {{0x7c, 0x14, 0x14, 0x14, 0x08}},
        {{0x08, 0x14, 0x14, 0x18, 0x7c}}, {{0x7c, 0x08, 0x04, 0x04, 0x08}},
        {{0x48, 0x54, 0x54, 0x54, 0x20}}, {{0x04, 0x3f, 0x44, 0x40, 0x20}},
        {{0x3c, 0x40, 0x40, 0x20, 0x7c}}, {{0x1c, 0x20, 0x40, 0x20, 0x1c}},
        {{0x3c, 0x40, 0x30, 0x40, 0x3c}}, {{0x44, 0x28, 0x10, 0x28, 0x44}},
        {{0x0c, 0x50, 0x50, 0x50, 0x3c}}, {{0x44, 0x64, 0x54, 0x4c, 0x44}},
    };
    static constexpr std::array<std::uint8_t, 5> percent = {0x63, 0x13, 0x08, 0x64, 0x63};
    static constexpr std::array<std::uint8_t, 5> question = {0x02, 0x01, 0x51, 0x09, 0x06};

    if (character >= '0' && character <= '9') {
        return digits[character - '0'];
    }
    if (character >= 'A' && character <= 'Z') {
        return uppercase[character - 'A'];
    }
    if (character >= 'a' && character <= 'z') {
        return lowercase[character - 'a'];
    }
    switch (character) {
        case '%':
            return percent;
        case '?':
            return question;
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

int text_width(const char* text, int scale)
{
    const int character_width = 5 * scale + 1;
    return static_cast<int>(std::strlen(text)) * character_width - 1;
}

void draw_text_at(const char* text, int cursor_x, int cursor_y, int scale)
{
    const int character_width = 5 * scale + 1;
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

void draw_text(const char* text, int cursor_y)
{
    const int scale = std::strlen(text) <= 11 ? 2 : 1;
    const int width = text_width(text, scale);
    draw_text_at(text, (kWidth - width) / 2, cursor_y, scale);
}

void draw_infinite_dots(const char* label, int cursor_y)
{
    constexpr int kScale = 2;
    constexpr int kDotCount = 4;
    constexpr int kDotSize = 3;
    constexpr int kDotGap = 3;
    constexpr int kLabelGap = 4;

    char truncated_label[9];
    std::snprintf(truncated_label, sizeof(truncated_label), "%.8s", label);
    const int label_width = text_width(truncated_label, kScale);
    const int dots_width = kDotCount * kDotSize + (kDotCount - 1) * kDotGap;
    const int total_width = label_width + kLabelGap + dots_width;
    const int start_x = (kWidth - total_width) / 2;

    draw_text_at(truncated_label, start_x, cursor_y, kScale);
    const int dots_x = start_x + label_width + kLabelGap;
    const int dots_y = cursor_y + 6;
    for (int dot = 0; dot < kDotCount; ++dot) {
        const int dot_x = dots_x + dot * (kDotSize + kDotGap);
        for (int dx = 0; dx < kDotSize; ++dx) {
            for (int dy = 0; dy < kDotSize; ++dy) {
                set_pixel(dot_x + dx, dots_y + dy);
            }
        }
    }
}

const char* service_label(const char* id)
{
    if (std::strcmp(id, "grok") == 0) {
        return "Grok";
    }
    if (std::strcmp(id, "claude") == 0) {
        return "Claude";
    }
    if (std::strcmp(id, "codex") == 0) {
        return "Codex";
    }
    if (std::strcmp(id, "opencode") == 0) {
        return "OpenCode";
    }
    return id;
}

esp_err_t redraw_usage()
{
    constexpr int kLineHeight = 14;

    int occupied_count = 0;
    for (const auto& slot : s_usage_slots) {
        occupied_count += slot.occupied ? 1 : 0;
    }

    s_framebuffer.fill(0);
    if (occupied_count == 0) {
        return flush_framebuffer();
    }

    const int line_step = occupied_count == 4 ? 16 : 21;
    const int content_height = kLineHeight + (occupied_count - 1) * line_step;
    int cursor_y = (kHeight - content_height) / 2;
    for (const auto& slot : s_usage_slots) {
        if (!slot.occupied) {
            continue;
        }

        char line[20];
        if (slot.infinite) {
            draw_infinite_dots(service_label(slot.id.data()), cursor_y);
        } else {
            std::snprintf(line, sizeof(line), "%.8s %d%%", service_label(slot.id.data()), slot.percent);
            draw_text(line, cursor_y);
        }
        cursor_y += line_step;
    }
    return flush_framebuffer();
}

bool normalize_service_id(const char* id, std::array<char, kMaxServiceIdLength + 1>& normalized)
{
    if (id == nullptr) {
        return false;
    }

    const std::size_t length = std::strlen(id);
    if (length == 0 || length > kMaxServiceIdLength) {
        return false;
    }

    normalized.fill('\0');
    for (std::size_t i = 0; i < length; ++i) {
        const unsigned char character = static_cast<unsigned char>(id[i]);
        normalized[i] = static_cast<char>(std::tolower(character));
    }
    return true;
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
    const Glass2UsageItem item = {
        .id = "grok",
        .percent = percent,
        .infinite = false,
    };
    return glass2_update_usage_items(&item, 1, updated_at_unix_seconds);
}

bool glass2_update_usage_items(const Glass2UsageItem* items, std::size_t count,
                               std::int64_t updated_at_unix_seconds)
{
    if (items == nullptr || count == 0 || count > GLASS2_USAGE_SLOT_COUNT) {
        ESP_LOGW(kTag, "glass2: rejected usage item count %u", static_cast<unsigned>(count));
        return false;
    }

    std::array<std::array<char, kMaxServiceIdLength + 1>, GLASS2_USAGE_SLOT_COUNT> normalized_ids{};
    for (std::size_t i = 0; i < count; ++i) {
        if ((!items[i].infinite && (items[i].percent < 0 || items[i].percent > 100)) ||
            !normalize_service_id(items[i].id, normalized_ids[i])) {
            ESP_LOGW(kTag, "glass2: rejected usage item %u", static_cast<unsigned>(i));
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_device == nullptr) {
        return false;
    }

    for (std::size_t i = 0; i < count; ++i) {
        UsageSlot* target = nullptr;
        for (auto& slot : s_usage_slots) {
            if (slot.occupied && std::strcmp(slot.id.data(), normalized_ids[i].data()) == 0) {
                target = &slot;
                break;
            }
        }
        if (target == nullptr) {
            for (auto& slot : s_usage_slots) {
                if (!slot.occupied) {
                    target = &slot;
                    target->id = normalized_ids[i];
                    target->occupied = true;
                    break;
                }
            }
        }
        if (target == nullptr) {
            ESP_LOGW(kTag, "glass2: no free slot for service '%s'", normalized_ids[i].data());
            continue;
        }

        target->percent = items[i].percent;
        target->infinite = items[i].infinite;
        if (target->infinite) {
            ESP_LOGI(kTag, "glass2: service=%s usage=infinite updated_at=%lld", target->id.data(),
                     static_cast<long long>(updated_at_unix_seconds));
        } else {
            ESP_LOGI(kTag, "glass2: service=%s usage=%d updated_at=%lld", target->id.data(), target->percent,
                     static_cast<long long>(updated_at_unix_seconds));
        }
    }
    s_updated_at_unix_seconds = updated_at_unix_seconds;

    const esp_err_t error = redraw_usage();
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "glass2: update failed: %s", esp_err_to_name(error));
        return false;
    }

    ESP_LOGI(kTag, "glass2: usage display refreshed updated_at=%lld",
             static_cast<long long>(s_updated_at_unix_seconds));
    return true;
}
