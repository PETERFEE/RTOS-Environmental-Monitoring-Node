/**
 * @file    telemetry.c
 * @brief   Wire-format helpers. Host-testable: no HAL, no FreeRTOS, no printf.
 *
 * The firmware links newlib-nano, whose printf deliberately omits %f. Rather
 * than paying ~7 KB of flash to get it back on a 128 KB part, floats are
 * rendered here with fixed-point integer maths.
 */
#include "telemetry.h"

#include <string.h>

#include "app_config.h"

/* -------------------------------------------------------------------------
 *  Small internal string builder
 * ---------------------------------------------------------------------- */
typedef struct
{
    char  *buf;
    size_t cap;   /* usable characters, excluding the NUL */
    size_t len;
    bool   ok;
} strbuf_t;

static void sb_init(strbuf_t *sb, char *buf, size_t size)
{
    sb->buf = buf;
    sb->cap = (size > 0U) ? (size - 1U) : 0U;
    sb->len = 0U;
    sb->ok  = (buf != NULL) && (size > 0U);
    if (sb->ok)
    {
        sb->buf[0] = '\0';
    }
}

static void sb_putc(strbuf_t *sb, char c)
{
    if (!sb->ok) { return; }
    if (sb->len >= sb->cap) { sb->ok = false; return; }
    sb->buf[sb->len++] = c;
    sb->buf[sb->len]   = '\0';
}

static void sb_puts(strbuf_t *sb, const char *s)
{
    if (s == NULL) { return; }
    while (*s != '\0' && sb->ok)
    {
        sb_putc(sb, *s++);
    }
}

/** Unsigned decimal, no printf. */
static void sb_putu32(strbuf_t *sb, uint32_t value)
{
    char tmp[11];
    uint8_t n = 0U;

    do {
        tmp[n++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && n < sizeof(tmp));

    while (n > 0U)
    {
        sb_putc(sb, tmp[--n]);
    }
}

static void sb_putfloat(strbuf_t *sb, float value, uint8_t decimals)
{
    char tmp[24];
    (void)telemetry_ftoa(value, decimals, tmp, sizeof(tmp));
    sb_puts(sb, tmp);
}

/* -------------------------------------------------------------------------
 *  Public helpers
 * ---------------------------------------------------------------------- */
static const char *const k_status_names[ENV_STATUS_COUNT] = {
    "NORMAL",
    "WARM",
    "COLD",
    "HUMID",
    "DRY",
    "POOR_AIR",
    "SENSOR_FAULT"
};

const char *telemetry_status_name(env_status_t status)
{
    /* Compared as unsigned: the enum has no negative enumerators, so a signed
     * lower-bound check would be dead code (and a -Wtype-limits warning). */
    if ((unsigned)status >= (unsigned)ENV_STATUS_COUNT)
    {
        return "UNKNOWN";
    }
    return k_status_names[status];
}

size_t telemetry_ftoa(float value, uint8_t decimals, char *out, size_t out_size)
{
    static const uint32_t k_pow10[5] = { 1U, 10U, 100U, 1000U, 10000U };

    if (out == NULL || out_size == 0U)
    {
        return 0U;
    }

    strbuf_t sb;
    sb_init(&sb, out, out_size);

    /* NaN is the only value that compares unequal to itself. */
    if (value != value)
    {
        sb_puts(&sb, "nan");
        return sb.len;
    }

    if (decimals > 4U)
    {
        decimals = 4U;
    }
    const uint32_t scale = k_pow10[decimals];

    bool negative = (value < 0.0f);
    if (negative)
    {
        value = -value;
    }

    /* Guard the float->uint32 conversion below. */
    const float limit = 2000000000.0f / (float)scale;
    if (value >= limit)
    {
        sb_puts(&sb, negative ? "-inf" : "inf");
        return sb.len;
    }

    /* Round half away from zero, then split integer / fractional parts. */
    const uint32_t scaled   = (uint32_t)((value * (float)scale) + 0.5f);
    const uint32_t integral = scaled / scale;
    const uint32_t fraction = scaled % scale;

    if (negative && (scaled != 0U))
    {
        sb_putc(&sb, '-');
    }
    sb_putu32(&sb, integral);

    if (decimals > 0U)
    {
        sb_putc(&sb, '.');
        /* Zero-pad the fractional part to the requested width. */
        for (uint32_t divisor = scale / 10U; divisor > 0U; divisor /= 10U)
        {
            sb_putc(&sb, (char)('0' + ((fraction / divisor) % 10U)));
        }
    }

    return sb.len;
}

size_t telemetry_format_line(const processed_data_t *data, char *out, size_t out_size)
{
    if (data == NULL || out == NULL || out_size == 0U)
    {
        if (out != NULL && out_size > 0U) { out[0] = '\0'; }
        return 0U;
    }

    strbuf_t sb;
    sb_init(&sb, out, out_size);

    sb_puts(&sb, "T=");   sb_putfloat(&sb, data->raw.temperature,    2U);
    sb_puts(&sb, ",H=");  sb_putfloat(&sb, data->raw.humidity,       2U);
    sb_puts(&sb, ",P=");  sb_putfloat(&sb, data->raw.pressure,       2U);
    sb_puts(&sb, ",G=");  sb_putfloat(&sb, data->raw.gas_resistance, 0U);
    sb_puts(&sb, ",A=");  sb_putu32(&sb, (uint32_t)data->air_quality);
    sb_puts(&sb, ",S=");  sb_puts(&sb, telemetry_status_name(data->status));
    sb_puts(&sb, ",TS="); sb_putu32(&sb, data->raw.timestamp);
    sb_puts(&sb, ",N=");  sb_putu32(&sb, data->raw.sequence);
    sb_putc(&sb, '\n');

    return sb.ok ? sb.len : 0U;
}

size_t telemetry_format_status(const processed_data_t *data,
                               uint32_t period_ms,
                               uint32_t free_heap,
                               uint32_t uptime_ms,
                               uint32_t samples,
                               uint32_t errors,
                               char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U)
    {
        return 0U;
    }

    strbuf_t sb;
    sb_init(&sb, out, out_size);

    sb_puts(&sb, "TEMP=");
    sb_putfloat(&sb, (data != NULL) ? data->raw.temperature : 0.0f, 2U);
    sb_puts(&sb, ",HUM=");
    sb_putfloat(&sb, (data != NULL) ? data->raw.humidity : 0.0f, 2U);
    sb_puts(&sb, ",PRESS=");
    sb_putfloat(&sb, (data != NULL) ? data->raw.pressure : 0.0f, 2U);
    sb_puts(&sb, ",GAS=");
    sb_putfloat(&sb, (data != NULL) ? data->raw.gas_resistance : 0.0f, 0U);
    sb_puts(&sb, ",AQ=");
    sb_putu32(&sb, (data != NULL) ? (uint32_t)data->air_quality : 0U);
    sb_puts(&sb, ",PERIOD=");  sb_putu32(&sb, period_ms);
    sb_puts(&sb, ",HEAP=");    sb_putu32(&sb, free_heap);
    sb_puts(&sb, ",UPTIME=");  sb_putu32(&sb, uptime_ms);
    sb_puts(&sb, ",SAMPLES="); sb_putu32(&sb, samples);
    sb_puts(&sb, ",ERRORS=");  sb_putu32(&sb, errors);
    sb_putc(&sb, '\n');

    return sb.ok ? sb.len : 0U;
}
