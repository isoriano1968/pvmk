#ifndef PVMK_CONF_H
#define PVMK_CONF_H

#include "pvmk/bootcap.h"

void conf_defaults(struct conf *c);

/*
 * Parse pvmk.conf text (key=value lines, '#' comments). Unknown keys and
 * bad values are reported through kprintf and otherwise ignored.
 */
void conf_parse(struct conf *c, const char *text, u32 len);
void conf_show(const struct conf *c);

#endif /* PVMK_CONF_H */
