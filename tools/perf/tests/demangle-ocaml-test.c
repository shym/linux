// SPDX-License-Identifier: GPL-2.0
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "debug.h"
#include "symbol.h"
#include "tests.h"

static int test__demangle_ocaml(struct test_suite *test __maybe_unused, int subtest __maybe_unused)
{
	int ret = TEST_OK;
	char *buf = NULL;
	size_t i;

	struct {
		const char *mangled, *demangled;
	} test_cases[] = {
		/* non-OCaml symbols should not be demangled */
		{ "main",
		  NULL },
		/* "caml" followed by lowercase is not a mangled symbol */
		{ "camlfoo",
		  NULL },
		/* runtime symbol "caml_" prefix is not a mangled user symbol */
		{ "caml_exn_Match_failure",
		  NULL },

		/* === Flat (legacy) mangling === */

		/* compilation unit name only, no member */
		{ "camlFoo",
		  "Foo" },
		/* unit with a single member separated by __ */
		{ "camlFoo__bar_0",
		  "Foo.bar_0" },
		/* deeply nested module path */
		{ "camlA__B__C__D__func_0",
		  "A.B.C.D.func_0" },
		/* single underscores are preserved (not separators) */
		{ "camlFoo__bar_baz_42",
		  "Foo.bar_baz_42" },
		{ "camlStdlib__array__map_154",
		  "Stdlib.array.map_154" },
		/* hex-encoded special chars in anonymous function name */
		{ "camlStdlib__anon_fn$5bstdlib$2eml$3a334$2c0$2d$2d54$5d_1453",
		  "Stdlib.anon_fn[stdlib.ml:334,0--54]_1453" },
		/* hex-encoded operator (++) */
		{ "camlStdlib__bytes__$2b$2b_2205",
		  "Stdlib.bytes.++_2205" },
		/* multiple consecutive hex-encoded chars (+++) */
		{ "camlFoo__$2b$2b$2b_1",
		  "Foo.+++_1" },
		/* incomplete hex escape at end: $ not followed by two hex digits */
		{ "camlFoo__bar$2",
		  "Foo.bar$2" },
		/* $ followed by non-hex chars is kept literal */
		{ "camlFoo__bar$gz",
		  "Foo.bar$gz" },
                /* CR with which compiler you get this for-pack prefix? I see
                 * something different on the various compilers I tried */
		/* for-pack prefix: unit Baz inside pack Foo.Bar */
		{ "camlBaz__Foo__Bar__init_0",
		  "Baz.Foo.Bar.init_0" },
		/* instance arguments (4 underscores = two __ separators) */
		{ "camlFoo____Bar__baz_1",
		  "Foo..Bar.baz_1" },
		/* trailing double underscore */
		{ "camlFoo__",
		  "Foo." },
		/* === Structured mangling === */
		/* bare prefix with no path items is malformed */
		{ "_Caml",
		  NULL },
		/* basic: _CamlU3FooM3BarF3baz -> Foo.Bar.baz */
		{ "_CamlU3FooM3BarF3baz",
		  "Foo.Bar.baz" },
		/* unit + function only */
		{ "_CamlU6StdlibF3map",
		  "Stdlib.map" },
		/* class tag */
		{ "_CamlU3FooO5MyObj",
		  "Foo.MyObj" },
		/* inline marker between two compilation units */
		{ "_CamlU3FooIU3BarF3qux",
		  "Foo.{inline}.Bar.qux" },
		/* universal encoding: operator >>= (all non-output chars)
		 * >>=: hex 3e,3e,3d at position 0 (A), empty raw
		 * payload: A3e3e3d_ (8 chars) */
		{ "_CamlU3FooFu8A3e3e3d_",
		  "Foo.>>=" },
		/* universal encoding: let* (* at position 3 = D, hex 2a)
		 * payload: D2a_let (7 chars) */
		{ "_CamlU3FooFu7D2a_let",
		  "Foo.let*" },
		/* universal encoding: func'sub' (two insertions)
		 * ' at position 4 = E, hex 27; ' at relative position 3 = D, hex 27
		 * payload: E27D27_funcsub (14 chars) */
		{ "_CamlU3FooFu14E27D27_funcsub",
		  "Foo.func'sub'" },
		/* anonymous function with file location
		 * Anonymous_function(334, 0, "stdlib.ml")
		 * location string: stdlib.ml_334_0
		 * . at position 6 = G, hex 2e; raw: stdlibml_334_0
		 * payload: G2e_stdlibml_334_0 (18 chars) */
		{ "_CamlU6StdlibLu18G2e_stdlibml_334_0",
		  "Stdlib.{lambda}stdlib.ml_334_0" },
		/* anonymous module (struct) without file
		 * Anonymous_module(42, 7, None) -> location string: _42_7
		 * all output chars, no escaping needed (5 chars) */
		{ "_CamlU3FooS5_42_7",
		  "Foo.{struct}_42_7" },
		/* partial application without file
		 * Partial_function(10, 5, None) -> location string: _10_5
		 * all output chars, no escaping needed (5 chars) */
		{ "_CamlU3FooP5_10_5",
		  "Foo.{partial}_10_5" },
		/* nested modules and function */
		{ "_CamlU3FooM3BarM3BazF6my_fun",
		  "Foo.Bar.Baz.my_fun" },
		/* universal encoding: identifier starting with digit
		 * string "0foo" starts with digit -> requires universal encoding
		 * no non-output chars, so escaped is empty, raw = 0foo
		 * payload: _0foo (5 chars) */
		{ "_CamlU3FooFu5_0foo",
		  "Foo.0foo" },
	};

	for (i = 0; i < ARRAY_SIZE(test_cases); i++) {
		buf = dso__demangle_sym(/*dso=*/NULL, /*kmodule=*/0, test_cases[i].mangled);
		if ((buf == NULL && test_cases[i].demangled != NULL)
				|| (buf != NULL && test_cases[i].demangled == NULL)
				|| (buf != NULL && strcmp(buf, test_cases[i].demangled))) {
			pr_debug("FAILED: %s: %s != %s\n", test_cases[i].mangled,
				 buf == NULL ? "(null)" : buf,
				 test_cases[i].demangled == NULL ? "(null)" : test_cases[i].demangled);
			ret = TEST_FAIL;
		}
		free(buf);
	}

	return ret;
}

DEFINE_SUITE("Demangle OCaml", demangle_ocaml);
