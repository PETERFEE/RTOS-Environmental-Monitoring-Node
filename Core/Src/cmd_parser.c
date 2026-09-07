/**
 * @file    cmd_parser.c
 * @brief   Line protocol parser. Pure C, host-testable.
 */
#include "cmd_parser.h"

#include <stddef.h>

#include "app_config.h"

static bool is_space(char c)
{
    return (c == ' ') || (c == '\t') || (c == '\r') || (c == '\n');
}

static char to_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/** Case-insensitive compare of @p line against @p verb, stopping at the end of
 *  the verb. Returns the number of characters consumed, or 0 on mismatch. */
static size_t match_verb(const char *line, const char *verb)
{
    size_t i = 0U;
    while (verb[i] != '\0')
    {
        if (to_upper(line[i]) != verb[i])
        {
            return 0U;
        }
        i++;
    }
    return i;
}

/** Parse an unsigned decimal. Rejects empty strings, non-digits and overflow. */
static bool parse_u32(const char *s, uint32_t *out)
{
    if (s == NULL || out == NULL || *s == '\0')
    {
        return false;
    }

    uint32_t value = 0U;
    size_t   digits = 0U;

    while (*s != '\0' && !is_space(*s))
    {
        if (*s < '0' || *s > '9')
        {
            return false;
        }
        const uint32_t digit = (uint32_t)(*s - '0');

        /* Overflow guard before the multiply-add. */
        if (value > ((0xFFFFFFFFU - digit) / 10U))
        {
            return false;
        }
        value = (value * 10U) + digit;
        digits++;
        s++;
    }

    if (digits == 0U)
    {
        return false;
    }

    *out = value;
    return true;
}

const char *cmd_name(cmd_type_t type)
{
    switch (type)
    {
        case CMD_NONE:         return "NONE";
        case CMD_STATUS:       return "STATUS";
        case CMD_SAMPLE_NOW:   return "SAMPLE_NOW";
        case CMD_SET_PERIOD:   return "SET_PERIOD";
        case CMD_GET_PERIOD:   return "GET_PERIOD";
        case CMD_RESET_STATS:  return "RESET_STATS";
        case CMD_PING:         return "PING";
        case CMD_HELP:         return "HELP";
        case CMD_BAD_ARGUMENT: return "BAD_ARGUMENT";
        case CMD_UNKNOWN:      /* fall through */
        default:               return "UNKNOWN";
    }
}

bool cmd_parse(const char *line, cmd_t *out)
{
    if (out == NULL)
    {
        return false;
    }

    out->type = CMD_UNKNOWN;
    out->arg  = 0U;

    if (line == NULL)
    {
        out->type = CMD_NONE;
        return false;
    }

    /* Skip leading whitespace. */
    while (*line != '\0' && is_space(*line))
    {
        line++;
    }

    if (*line == '\0')
    {
        out->type = CMD_NONE;
        return false;
    }

    size_t n;

    /* SET_PERIOD=<ms> is checked first: it is the only verb that is a prefix
     * relationship risk, and it must consume its argument. */
    if ((n = match_verb(line, "SET_PERIOD")) != 0U)
    {
        const char *p = line + n;
        while (*p != '\0' && is_space(*p)) { p++; }

        if (*p != '=')
        {
            out->type = CMD_BAD_ARGUMENT;
            return false;
        }
        p++;
        while (*p != '\0' && is_space(*p)) { p++; }

        uint32_t period;
        if (!parse_u32(p, &period))
        {
            out->type = CMD_BAD_ARGUMENT;
            return false;
        }
        if (period < SENSOR_PERIOD_MS_MIN || period > SENSOR_PERIOD_MS_MAX)
        {
            out->type = CMD_BAD_ARGUMENT;
            out->arg  = period;     /* echoed back so the error can name it */
            return false;
        }

        out->type = CMD_SET_PERIOD;
        out->arg  = period;
        return true;
    }

    static const struct { const char *verb; cmd_type_t type; } k_verbs[] = {
        { "STATUS",      CMD_STATUS      },
        { "SAMPLE_NOW",  CMD_SAMPLE_NOW  },
        { "GET_PERIOD",  CMD_GET_PERIOD  },
        { "RESET_STATS", CMD_RESET_STATS },
        { "PING",        CMD_PING        },
        { "HELP",        CMD_HELP        },
    };

    for (size_t i = 0U; i < (sizeof(k_verbs) / sizeof(k_verbs[0])); i++)
    {
        n = match_verb(line, k_verbs[i].verb);
        if (n == 0U)
        {
            continue;
        }

        /* The verb must be the whole token -- "STATUSX" is not "STATUS". */
        const char *rest = line + n;
        while (*rest != '\0' && is_space(*rest)) { rest++; }
        if (*rest != '\0')
        {
            continue;
        }

        out->type = k_verbs[i].type;
        return true;
    }

    out->type = CMD_UNKNOWN;
    return false;
}
