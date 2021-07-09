/* purple
 *
 * Purple is the legal property of its developers, whose names are too numerous
 * to list here.  Please refer to the COPYRIGHT file distributed with this
 * source distribution.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "../xmlnode.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	gchar *malicious_xml = g_new0(gchar, size + 1);
	gchar *str;
	xmlnode *xml;

	memcpy(malicious_xml, data, size);
	malicious_xml[size] = '\0';

	xml = xmlnode_from_str(malicious_xml, -1);
	if(xml == NULL) {
		g_free(malicious_xml);

		return 0;
	}

	str = xmlnode_to_str(xml, NULL);
	if(str == NULL) {
		xmlnode_free(xml);
		free(malicious_xml);

		return 0;
	}

	if(strcmp(malicious_xml, str) != 0) {
		g_free(str);
		xmlnode_free(xml);
		free(malicious_xml);
		__builtin_trap();
	}

	g_free(str);

	xmlnode_free(xml);

	g_free(malicious_xml);

	return 0;
}
