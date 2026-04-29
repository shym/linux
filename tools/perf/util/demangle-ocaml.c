// SPDX-License-Identifier: GPL-2.0
#include <string.h>
#include <stdlib.h>
#include "util/string2.h"

#include "demangle-ocaml.h"

#include <linux/ctype.h>

/*
 * Flat (legacy) OCaml demangling
 *
 * Mangled symbols start with "caml" followed by an uppercase letter.
 * "__" encodes "." and "$xx" encodes character with hex value xx.
 */

static const char *caml_prefix = "caml";
static const size_t caml_prefix_len = 4;

/* mangled flat OCaml symbols start with "caml" followed by an upper-case letter */
static bool
ocaml_is_flat_mangled(const char *sym)
{
	return 0 == strncmp(sym, caml_prefix, caml_prefix_len)
		&& isupper(sym[caml_prefix_len]);
}

static char *
ocaml_demangle_flat(const char *sym)
{
	char *result;
	int j = 0;
	int i;
	int len;

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

/*
 * Structured OCaml demangling
 *
 * Mangled symbols have the form "_Caml<path>" where <path> is a sequence of
 * tagged, length-prefixed identifiers:
 *
 *   Tags:
 *     U - compilation Unit
 *     I - Inline marker
 *     M - Module
 *     S - anonymous Struct (module)
 *     O - class (Object)
 *     F - Function
 *     L - anonymous function (Lambda)
 *     P - Partial application
 *
 *   Each identifier is encoded as either:
 *     <decimal_length><payload>          (simple: payload is literal chars)
 *     u<decimal_length><escaped>_<raw>   (universal: contains non-alnum chars)
 *
 *   In universal encoding:
 *     - <escaped> is a sequence of (base26_position, hex_bytes) pairs
 *     - base26 uses A=0, B=1, ..., Z=25, BA=26, BB=27, ...
 *     - hex bytes are lowercase [0-9a-f], two chars per byte
 *     - <raw> is the subsequence of output characters
 *     - positions are relative insertion points into the raw string
 *
 *   Example: _CamlU3FooM3BarF3baz -> Foo.Bar.baz
 */

#define STRUCTURED_MAX_LEN (1024 * 1024)

static const char *structured_prefix = "_Caml";
static const size_t structured_prefix_len = 5;

static bool
ocaml_is_structured(const char *sym)
{
	return 0 == strncmp(sym, structured_prefix, structured_prefix_len);
}

static int
parse_decimal(const char *sym, int len, int *pos)
{
	int val = 0;
	int start = *pos;

	while (*pos < len && sym[*pos] >= '0' && sym[*pos] <= '9') {
		val = val * 10 + (sym[*pos] - '0');
		/* Overflow check: the bound is small enough compared the max
		 * int that we don't need to check before the multiplication */
		if (val > STRUCTURED_MAX_LEN)
			return -1;
		(*pos)++;
	}
	if (*pos == start)
		return -1;
	return val;
}

static int
parse_base26(const char *sym, int len, int *pos)
{
	int val = 0;
	int start = *pos;

	while (*pos < len && sym[*pos] >= 'A' && sym[*pos] <= 'Z') {
		val = val * 26 + (sym[*pos] - 'A');
		/* Overflow check: the bound is small enough compared the max
		 * int that we don't need to check before the multiplication */
		if (val > STRUCTURED_MAX_LEN)
			return -1;
		(*pos)++;
	}
	if (*pos == start)
		return -1;
	return val;
}

static int
parse_hex_byte(const char *sym, int len, int *pos)
{
	int val;

	if (*pos + 1 >= len)
		return -1;
	if (!isxdigit(sym[*pos]) || !isxdigit(sym[*pos + 1]))
		return -1;
	val = (hex(sym[*pos]) << 4) | hex(sym[*pos + 1]);
	*pos += 2;
	return val;
}

/*
 * Decode universal-encoded payload: <escaped>_<raw>
 *
 * Streams in a single pass: for each insertion in the escaped section,
 * copy raw characters up to the insertion point, then decode the hex
 * bytes directly to the output. The decoded string is always shorter
 * than the encoded payload, so payload_len + 1 suffices for output.
 */
static char *
decode_universal(const char *payload, int payload_len)
{
	int sep = -1;
	int i;
	int escaped_len;
	const char *raw;
	int raw_len;
	int epos = 0;
	int raw_pos = 0;
	int out = 0;
	char *result;

	for (i = 0; i < payload_len; i++) {
		if (payload[i] == '_') {
			sep = i;
			break;
		}
	}
	if (sep < 0)
		return NULL;

	escaped_len = sep;
	raw = payload + sep + 1;
	raw_len = payload_len - sep - 1;

	result = malloc(payload_len + 1);
	if (!result)
		return NULL;

	while (epos < escaped_len) {
		int skip = parse_base26(payload, escaped_len, &epos);

		if (skip < 0)
			goto fail;

		/* Copy 'skip' raw characters to output */
		for (i = 0; i < skip && raw_pos < raw_len; i++)
			result[out++] = raw[raw_pos++];

		/* Decode hex bytes until next base26 position or end */
		while (epos < escaped_len &&
		       !(payload[epos] >= 'A' && payload[epos] <= 'Z')) {
			int byte_val = parse_hex_byte(payload, escaped_len, &epos);

			if (byte_val < 0)
				goto fail;
			result[out++] = (char)byte_val;
		}
	}

	/* Copy remaining raw characters */
	while (raw_pos < raw_len)
		result[out++] = raw[raw_pos++];

	result[out] = '\0';
	return result;

fail:
	free(result);
	return NULL;
}

static char *
decode_ident(const char *sym, int len, int *pos)
{
	bool universal = false;
	int ident_len;
	char *result;

	if (*pos < len && sym[*pos] == 'u') {
		universal = true;
		(*pos)++;
	}

	ident_len = parse_decimal(sym, len, pos);

	if (ident_len < 0 || *pos + ident_len > len)
		return NULL;

	if (!universal) {
		result = malloc(ident_len + 1);

		if (!result)
			return NULL;
		memcpy(result, sym + *pos, ident_len);
		result[ident_len] = '\0';
		*pos += ident_len;
		return result;
	}

	result = decode_universal(sym + *pos, ident_len);
	*pos += ident_len;
	return result;
}

/* Human-readable names for structured path tags */
static const char *
tag_label(char tag)
{
	switch (tag) {
	case 'S': return "{struct}";
	case 'L': return "{lambda}";
	case 'P': return "{partial}";
	default:  return NULL;
	}
}

static bool
is_structured_tag(char c)
{
	return c == 'U' || c == 'I' || c == 'M' || c == 'S' ||
	       c == 'O' || c == 'F' || c == 'L' || c == 'P';
}

/*
 * Demangle a structured OCaml symbol (_Caml prefix).
 *
 * Output format: components separated by "." with:
 *   U (unit):     just the name
 *   M (module):   just the name
 *   O (class):    just the name
 *   F (function): just the name
 *   S (struct):   {struct}<decoded_location>
 *   L (lambda):   {lambda}<decoded_location>
 *   P (partial):  {partial}<decoded_location>
 *   I (inline):   {inline}
 */
static char *
ocaml_demangle_structured(const char *sym)
{
	int len = strlen(sym);
	int pos = structured_prefix_len;
	/*
	 * Each tag can expand to at most 9 chars ("{partial}" is longest label)
	 * plus decoded identifier plus separator. Use 4x input length as a safe
	 * upper bound for the output buffer.
	 */
	int buf_size = len * 4 + 64;
	char *result;
	int out = 0;
	bool first = true;

	if (len > STRUCTURED_MAX_LEN)
		return NULL;

	result = malloc(buf_size);
	if (!result)
		return NULL;

	while (pos < len) {
		char tag = sym[pos];
		const char *label;
		char *ident;
		int ident_len;
		int label_len;

		if (!is_structured_tag(tag))
			goto fail;

		pos++;

		/* Add separator between components */
		if (!first) {
			if (out + 1 >= buf_size)
				goto fail;
			result[out++] = '.';
		}
		first = false;

		if (tag == 'I') {
			/* Inline marker has no payload */
			if (out + 8 >= buf_size)
				goto fail;
			memcpy(result + out, "{inline}", 8);
			out += 8;
			continue;
		}

		/* All other tags have an encoded identifier payload */
		ident = decode_ident(sym, len, &pos);
		if (!ident)
			goto fail;

		label = tag_label(tag);
		ident_len = strlen(ident);

		if (label) {
			label_len = strlen(label);

			if (out + label_len + ident_len >= buf_size) {
				free(ident);
				goto fail;
			}
			memcpy(result + out, label, label_len);
			out += label_len;
		} else {
			if (out + ident_len >= buf_size) {
				free(ident);
				goto fail;
			}
		}

		memcpy(result + out, ident, ident_len);
		out += ident_len;
		free(ident);
	}

	/* Require at least one successfully decoded component */
	if (out == 0)
		goto fail;

	result[out] = '\0';
	return result;

fail:
	free(result);
	return NULL;
}

/*
 * input:
 *     sym: a symbol which may have been mangled by the OCaml compiler
 * return:
 *     if the input doesn't look like a mangled OCaml symbol, NULL is returned
 *     otherwise, a newly allocated string containing the demangled symbol is returned
 *
 * Supports both:
 *   - Flat mangling:       "caml<Name>..." (legacy OCaml compiler)
 *   - Structured mangling: "_Caml<path>"   (OxCaml compiler)
 */
char *
ocaml_demangle_sym(const char *sym)
{
	if (ocaml_is_structured(sym))
		return ocaml_demangle_structured(sym);

	if (ocaml_is_flat_mangled(sym))
		return ocaml_demangle_flat(sym);

	return NULL;
}
