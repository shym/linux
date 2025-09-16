// SPDX-License-Identifier: GPL-2.0
#include <string.h>
#include <stdlib.h>
#include "util/string2.h"

#include "demangle-ocaml.h"

#include <linux/ctype.h>

/* Legacy mangling scheme */

static const char *caml_prefix = "caml";
static const size_t caml_prefix_len = 4;

/* mangled OCaml symbols start with "caml" followed by an upper-case letter */
static bool ocaml_is_legacy_mangled(const char *sym)
{
	return 0 == strncmp(sym, caml_prefix, caml_prefix_len)
	    && isupper(sym[caml_prefix_len]);
}

/*
 * input:
 *     sym: a symbol which may have been mangled by the OCaml compiler
 * return:
 *     if the input doesn't look like a mangled OCaml symbol, NULL is returned
 *     otherwise, a newly allocated string containing the demangled symbol is returned
 */
static char *ocaml_legacy_demangle_sym(const char *sym)
{
	char *result;
	int j = 0;
	int i;
	int len;

	if (!ocaml_is_legacy_mangled(sym)) {
		return NULL;
	}

	len = strlen(sym);

	/* the demangled symbol is always smaller than the mangled symbol */
	result = malloc(len + 1);
	if (!result)
		return NULL;

	/* skip "caml" prefix */
	i = caml_prefix_len;

	while (i < len) {
		if (sym[i] == '_' && sym[i + 1] == '_') {
			/* "__" -> "." */
			result[j++] = '.';
			i += 2;
		} else if (sym[i] == '$' && isxdigit(sym[i + 1])
			   && isxdigit(sym[i + 2])) {
			/* "$xx" is a hex-encoded character */
			result[j++] = (hex(sym[i + 1]) << 4) | hex(sym[i + 2]);
			i += 3;
		} else {
			result[j++] = sym[i++];
		}
	}
	result[j] = '\0';

	return result;
}

/* New mangling scheme */

/* Maximal length of a symbol */
#define SYMBOL_MAX (1024*1024)
#define ERROR (~((unsigned)0))

/* Decode the decimal integer at *pos in sym
   Require a non-empty integer to appear
   Leave *pos to the first byte after the integer */
static unsigned decode_decimal(const char *sym, size_t *pos)
{
	unsigned res = 0;
	size_t p = *pos;
	while (sym[p] >= '0' && sym[p] <= '9') {
		if (res > SYMBOL_MAX)
			return ERROR;
		res = res * 10 + (sym[p] - '0');
		p++;
	}
	if (*pos == p)
		// No digit was found
		return ERROR;
	*pos = p;
	return res;
}

static unsigned decode_26(const char *sym, size_t *pos)
{
	unsigned res = 0;
	size_t p = *pos;
	while (sym[p] >= 'A' && sym[p] <= 'Z') {
		if (res > SYMBOL_MAX)
			return ERROR;
		res = res * 26 + (sym[p] - 'A');
		p++;
	}
	if (*pos == p)
		// No digit was found
		return ERROR;
	*pos = p;
	return res;
}

static int is_hex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

static char *ocaml_demangle_sym_v1(const char *sym)
{
	char *outbuf, *tmp;
	size_t sympos, outpos, codedpos, endpos, len, l;
	unsigned raw;

	if (sym[0] != '_' || sym[1] != 'O')
		return NULL;

	sympos = 2;
	outpos = 0;
	len = strlen(sym);

	switch (sym[sympos++]) {
	case 'N':
		/* The result is always shorter than the encoded symbol */
		/* Is it worth to compute the precise length instead? */
		outbuf = malloc(len + 1);
		if (outbuf == NULL)
			return NULL;

#define ENDONERROR() do { \
  free(outbuf);           \
  return NULL;            \
} while(0)

		while (sympos < len) {
			if (sym[sympos] == 'u') {
				sympos++;
				if (outpos)
					outbuf[outpos++] = '.';
				l = decode_decimal(sym, &sympos);
				if (l == ERROR || l == 0 || sympos + l > len)
					ENDONERROR();
				codedpos = sympos;
				endpos = sympos + l;
				tmp = strchr(sym + sympos, '_');
				if (!tmp)
					ENDONERROR();
				sympos = (size_t)(tmp - sym + 1);
				if (sympos > endpos)
					ENDONERROR();
				while (sym[codedpos] != '_') {
					raw = decode_26(sym, &codedpos);
					if (raw == ERROR
					    || sympos + raw > endpos)
						ENDONERROR();
					tmp = stpncpy(outbuf +
						      outpos,
						      sym + sympos, raw);
					sympos += raw;
					outpos += raw;
					if ((size_t)(tmp - outbuf) != outpos)
						ENDONERROR();
					while (is_hex(sym[codedpos])) {
						if (!is_hex(sym[codedpos + 1]))
							ENDONERROR();
						outbuf[outpos++] =
						    hex(sym[codedpos])
						    << 4 |
						    hex(sym[codedpos + 1]);
						codedpos += 2;
					}
				}
				if (sympos < endpos) {
					tmp = stpncpy(outbuf +
						      outpos,
						      sym + sympos,
						      endpos - sympos);
					outpos += endpos - sympos;
					sympos = endpos;
					if ((size_t)(tmp - outbuf) != outpos)
						ENDONERROR();
				}
			} else if (sym[sympos] != '_') {
				if (outpos)
					outbuf[outpos++] = '.';
				l = decode_decimal(sym, &sympos);
				if (l == ERROR || l == 0 || sympos + l > len)
					ENDONERROR();
				tmp = stpncpy(outbuf + outpos, sym + sympos, l);
				sympos += l;
				outpos += l;
				if ((size_t)(tmp - outbuf) != outpos)
					ENDONERROR();
			} else {
				// we are on the _ that separates the symbol per se from its unique
				// id, so we have nothing left to do in that loop
				break;
			}
		}
		outbuf[outpos] = '\0';
		break;
	case 'A':
		outbuf = strdup("anonymous");
		break;
	default:
		return NULL;
	}

	return outbuf;
}

/* Main entry point */

/*
 * input:
 *     sym: a symbol which may have been mangled by the OCaml compiler
 * return:
 *     if the input doesn't look like a mangled OCaml symbol, NULL is returned
 *     otherwise, a newly allocated string containing the demangled symbol is returned
 */
char *ocaml_demangle_sym(const char *sym)
{
	if (sym[0] == '_' || sym[1] == 'O')
		return ocaml_demangle_sym_v1(sym);
	return ocaml_legacy_demangle_sym(sym);
}
