/*
 *
 *  Connection Manager
 *
 *  Copyright (C) 2007-2012  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "src/shared/sha1.h"

struct prf_data {
	const char *key;
	unsigned int key_len;
	const char *prefix;
	unsigned int prefix_len;
	const char *data;
	unsigned int data_len;
	const char *prf;
};

static void prf_test(gconstpointer data)
{
	const struct prf_data *test = data;
	unsigned int prf_len;
	unsigned char output[512];
	char prf[128];
	unsigned int i;
	int result;

	prf_len = strlen(test->prf) / 2;

	if (g_test_verbose()) {
		g_print("PRF    = %s (%d octects)\n", test->prf, prf_len);
	}

	result = prf_sha1(test->key, test->key_len, test->prefix,
				test->prefix_len, test->data, test->data_len,
						output, prf_len);

	g_assert(result == 0);

	for (i = 0; i < prf_len; i++)
		sprintf(prf + (i * 2), "%02x", output[i]);

	if (g_test_verbose()) {
		g_print("Result = %s\n", prf);
	}

	g_assert(strcmp(test->prf, prf) == 0);
}

static const struct prf_data test_case_1 = {
	.key		= "\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b"
			  "\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b",
	.key_len	= 20,
	.prefix		= "prefix",
	.prefix_len	= 6,
	.data		= "Hi There",
	.data_len	= 8,
	.prf		= "bcd4c650b30b9684951829e0d75f9d54"
			  "b862175ed9f00606e17d8da35402ffee"
			  "75df78c3d31e0f889f012120c0862beb"
			  "67753e7439ae242edb8373698356cf5a",
};

static const struct prf_data test_case_2 = {
	.key		= "Jefe",
	.key_len	= 4,
	.prefix		= "prefix",
	.prefix_len	= 6,
	.data		= "what do ya want for nothing?",
	.data_len	= 28,
	.prf		= "51f4de5b33f249adf81aeb713a3c20f4"
			  "fe631446fabdfa58244759ae58ef9009"
			  "a99abf4eac2ca5fa87e692c440eb4002"
			  "3e7babb206d61de7b92f41529092b8fc",
};

static const struct prf_data test_case_3 = {
	.key		= "\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa"
			  "\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa",
	.key_len	= 20,
	.prefix		= "prefix",
	.prefix_len	= 6,
	.data		= "\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd"
			  "\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd"
			  "\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd"
			  "\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd"
			  "\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd",
	.data_len	= 50,
	.prf		= "e1ac546ec4cb636f9976487be5c86be1"
			  "7a0252ca5d8d8df12cfb0473525249ce"
			  "9dd8d177ead710bc9b590547239107ae"
			  "f7b4abd43d87f0a68f1cbd9e2b6f7607",
};

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_data_func("/prf-sha1/Test case 1",
					&test_case_1, prf_test);
	g_test_add_data_func("/prf-sha1/Test case 2",
					&test_case_2, prf_test);
	g_test_add_data_func("/prf-sha1/Test case 3",
					&test_case_3, prf_test);

	return g_test_run();
}
