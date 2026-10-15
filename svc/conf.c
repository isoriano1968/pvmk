/*
 * pvmk.conf parser.
 */
#include "pvmk/conf.h"
#include "pvmk/keyboard.h"
#include "pvmk/kprintf.h"

void
conf_defaults(struct conf *c)
{
	memset(c, 0, sizeof(*c));
	memcpy(c->serial, "debug", 6);
	c->baud = 115200;
	c->timeout = 3;
	c->loglevel = 2;
	c->display = 1;
	c->menu = 1;
	c->fallback = 60;
}

static int
streq(const char *a, const char *b, u32 blen)
{
	u32 i;

	for (i = 0; i < blen; i++)
		if (a[i] != b[i])
			return 0;
	return a[blen] == 0;
}

static int
parse_u32(const char *s, u32 len, u32 *out)
{
	u32 v = 0, i;

	if (!len)
		return 0;
	for (i = 0; i < len; i++) {
		if (s[i] < '0' || s[i] > '9')
			return 0;
		v = v * 10 + (u32)(s[i] - '0');
	}
	*out = v;
	return 1;
}

static void
set_str(char *dst, u32 cap, const char *s, u32 len)
{
	if (len >= cap)
		len = cap - 1;
	memcpy(dst, s, len);
	dst[len] = 0;
}

static void
conf_line(struct conf *c, const char *k, u32 klen, const char *v, u32 vlen,
    u32 lineno)
{
	u32 n;

	if (streq("serial", k, klen)) {
		if (streq("debug", v, vlen) || streq("guest", v, vlen) ||
		    streq("shared", v, vlen))
			set_str(c->serial, sizeof(c->serial), v, vlen);
		else
			goto bad;
	} else if (streq("baud", k, klen)) {
		if (!parse_u32(v, vlen, &n) || n < 300 || n > 230400)
			goto bad;
		c->baud = n;
	} else if (streq("default", k, klen)) {
		set_str(c->def, sizeof(c->def), v, vlen);
	} else if (streq("timeout", k, klen)) {
		if (!parse_u32(v, vlen, &n) || n > 3600)
			goto bad;
		c->timeout = n;
	} else if (streq("display", k, klen)) {
		if (streq("on", v, vlen))
			c->display = 1;
		else if (streq("off", v, vlen))
			c->display = 0;
		else
			goto bad;
	} else if (streq("keymap", k, klen)) {
		if (streq("us", v, vlen))
			c->keymap = KBD_US;
		else if (streq("de", v, vlen))
			c->keymap = KBD_DE;
		else if (streq("de-pc", v, vlen))
			c->keymap = KBD_DE_PC;
		else
			goto bad;
	} else if (streq("log", k, klen)) {
		if (!parse_u32(v, vlen, &n) || n > 4)
			goto bad;
		c->loglevel = n;
	} else if (streq("menu", k, klen)) {
		if (streq("on", v, vlen))
			c->menu = 1;
		else if (streq("off", v, vlen))
			c->menu = 0;
		else
			goto bad;
	} else if (streq("fallback", k, klen)) {
		if (!parse_u32(v, vlen, &n) || n > 3600)
			goto bad;
		c->fallback = n;
	} else if (streq("entry", k, klen)) {
		/* entry=label,dev:path[,pv|native] */
		struct conf_entry *e;
		u32 c1, c2;

		for (c1 = 0; c1 < vlen && v[c1] != ','; c1++)
			;
		for (c2 = c1 + 1; c2 < vlen && v[c2] != ','; c2++)
			;
		if (c1 == 0 || c1 + 1 >= vlen || c2 == c1 + 1 ||
		    c->nentries >= CONF_MAXENT)
			goto bad;
		e = &c->entry[c->nentries];
		set_str(e->label, sizeof(e->label), v, c1);
		set_str(e->target, sizeof(e->target), v + c1 + 1, c2 - c1 - 1);
		e->mode = ENTRY_AUTO;
		if (c2 < vlen) {
			if (streq("pv", v + c2 + 1, vlen - c2 - 1))
				e->mode = ENTRY_PV;
			else if (streq("native", v + c2 + 1, vlen - c2 - 1))
				e->mode = ENTRY_NATIVE;
			else
				goto bad;
		}
		c->nentries++;
	} else {
		kprintf("conf: line %u: unknown key ignored\n", lineno);
	}
	return;
bad:
	kprintf("conf: line %u: bad value ignored\n", lineno);
}

void
conf_parse(struct conf *c, const char *text, u32 len)
{
	u32 pos = 0, lineno = 0;

	while (pos < len) {
		u32 start = pos, end, eq, ks, ke, vs, ve;

		lineno++;
		while (pos < len && text[pos] != '\n')
			pos++;
		end = pos++;
		ks = start;
		while (ks < end && (text[ks] == ' ' || text[ks] == '\t'))
			ks++;
		if (ks == end || text[ks] == '#' || text[ks] == '\r')
			continue;
		for (eq = ks; eq < end && text[eq] != '='; eq++)
			;
		if (eq == end) {
			kprintf("conf: line %u: expected key=value\n", lineno);
			continue;
		}
		ke = eq;
		while (ke > ks && (text[ke - 1] == ' ' || text[ke - 1] == '\t'))
			ke--;
		vs = eq + 1;
		while (vs < end && (text[vs] == ' ' || text[vs] == '\t'))
			vs++;
		ve = end;
		while (ve > vs && (text[ve - 1] == ' ' || text[ve - 1] == '\t' ||
		    text[ve - 1] == '\r'))
			ve--;
		conf_line(c, text + ks, ke - ks, text + vs, ve - vs, lineno);
	}
}

void
conf_show(const struct conf *c)
{
	kprintf("serial=%s\nbaud=%u\ndisplay=%s\nkeymap=%s\ndefault=%s\n"
	    "timeout=%u\nlog=%u\n", c->serial, c->baud,
	    c->display ? "on" : "off", c->keymap == KBD_DE ? "de" :
	    c->keymap == KBD_DE_PC ? "de-pc" : "us", c->def,
	    c->timeout, c->loglevel);
	kprintf("menu=%s\nfallback=%u\n", c->menu ? "on" : "off", c->fallback);
	for (u32 i = 0; i < c->nentries; i++)
		kprintf("entry=%s,%s%s\n", c->entry[i].label, c->entry[i].target,
		    c->entry[i].mode == ENTRY_PV ? ",pv" :
		    c->entry[i].mode == ENTRY_NATIVE ? ",native" : "");
}
