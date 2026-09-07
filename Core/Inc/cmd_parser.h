/**
 * @file    cmd_parser.h
 * @brief   Line-oriented command protocol spoken over the UART links.
 *
 * Pure parsing logic, host-testable. The RTOS side lives in comm_task.c.
 *
 *   STATUS            -> current readings, period, heap, uptime
 *   SAMPLE_NOW        -> force an immediate acquisition
 *   SET_PERIOD=<ms>   -> change the sensor task period (100..60000)
 *   GET_PERIOD        -> report the current period
 *   RESET_STATS       -> zero the sample/error counters
 *   PING              -> "PONG"
 *   HELP              -> list of supported commands
 */
#ifndef CMD_PARSER_H
#define CMD_PARSER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    CMD_NONE = 0,      /**< empty line                                     */
    CMD_STATUS,
    CMD_SAMPLE_NOW,
    CMD_SET_PERIOD,    /**< argument in .arg                               */
    CMD_GET_PERIOD,
    CMD_RESET_STATS,
    CMD_PING,
    CMD_HELP,
    CMD_UNKNOWN,       /**< recognised as a line, but not a known verb     */
    CMD_BAD_ARGUMENT   /**< known verb, unusable argument                  */
} cmd_type_t;

typedef struct
{
    cmd_type_t type;
    uint32_t   arg;
} cmd_t;

/**
 * Parse one received line. Leading/trailing whitespace and CR/LF are ignored
 * and the verb is matched case-insensitively.
 *
 * @return true when @p out->type is an actionable command (not NONE/UNKNOWN/
 *         BAD_ARGUMENT); @p out is always written.
 */
bool cmd_parse(const char *line, cmd_t *out);

/** Canonical name of a command verb, for logging and HELP output. */
const char *cmd_name(cmd_type_t type);

#endif /* CMD_PARSER_H */
