// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string.h>
#include <yaml.h>

// The text of a scalar node, or NULL. Scalars with embedded NUL bytes are rejected: C would truncate them.
static inline const char *yaml_text(const yaml_node_t *n)
{
	if (!n || n->type != YAML_SCALAR_NODE || strlen((const char *)n->data.scalar.value) != n->data.scalar.length)
		return NULL;
	return (const char *)n->data.scalar.value;
}
