/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2024, NFS-Ganesha Project
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

/**
 * @file config.c
 * @brief SeaweedFS FSAL configuration parsing
 */

#include "config.h"
#include "fsal.h" 
#include "fsal_seaweed.h"
#include "FSAL/fsal_commonlib.h"

/* Configuration parameter definitions */
static struct config_item seaweed_params[] = {
	CONF_ITEM_STR("filer_endpoints", 1, SEAWEED_MAX_ENDPOINT_LEN,
		      "localhost:18888", seaweed_fsal_module, filer_endpoints[0]),
	CONF_ITEM_UI32("connection_timeout", 1, 300, 30,
		       seaweed_fsal_module, connection_timeout),
	CONF_ITEM_UI32("connection_pool_size", 1, SEAWEED_MAX_CONNECTIONS, 
		       SEAWEED_DEFAULT_CONNECTIONS,
		       seaweed_fsal_module, connection_pool_size),
	CONF_ITEM_UI32("max_concurrent_requests", 1, 1000, 100,
		       seaweed_fsal_module, max_concurrent_requests),
	CONF_ITEM_UI32("path_cache_size", 1000, SEAWEED_MAX_CACHE_SIZE,
		       SEAWEED_DEFAULT_CACHE_SIZE,
		       seaweed_fsal_module, path_cache_size),
	CONF_ITEM_UI32("path_cache_ttl", 10, 3600, SEAWEED_DEFAULT_CACHE_TTL,
		       seaweed_fsal_module, path_cache_ttl),
	CONF_ITEM_BOOL("enable_file_locks", true,
		       seaweed_fsal_module, enable_file_locks),
	CONF_ITEM_UI32("default_lock_timeout", 10, 3600, 300,
		       seaweed_fsal_module, default_lock_timeout),
	CONFIG_EOL
};

static struct config_block seaweed_block = {
	.dbus_interface_name = "org.ganesha.nfsd.config.fsal.seaweedfs",
	.blk_desc.name = "SEAWEEDFS",
	.blk_desc.type = CONFIG_BLOCK,
	.blk_desc.u.blk.init = noop_conf_init,
	.blk_desc.u.blk.params = seaweed_params,
	.blk_desc.u.blk.commit = noop_conf_commit
};

/**
 * @brief Parse filer endpoints string
 * 
 * Parses a comma-separated list of filer endpoints.
 */
static fsal_status_t parse_filer_endpoints(struct seaweed_fsal_module *module,
					   const char *endpoints_str)
{
	char *endpoints_copy = NULL;
	char *token = NULL;
	char *saveptr = NULL;
	int endpoint_count = 0;

	if (!endpoints_str || strlen(endpoints_str) == 0) {
		LogMajor(COMPONENT_FSAL, "No filer endpoints specified");
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	endpoints_copy = gsh_strdup(endpoints_str);
	if (!endpoints_copy) {
		LogMajor(COMPONENT_FSAL, "Failed to duplicate endpoints string");
		return fsalstat(ERR_FSAL_NOMEM, ENOMEM);
	}

	/* Parse comma-separated endpoints */
	token = strtok_r(endpoints_copy, ",", &saveptr);
	while (token != NULL && endpoint_count < SEAWEED_MAX_ENDPOINTS) {
		/* Trim whitespace */
		while (*token == ' ' || *token == '\t')
			token++;
		
		char *end = token + strlen(token) - 1;
		while (end > token && (*end == ' ' || *end == '\t'))
			*end-- = '\0';

		/* Validate endpoint format */
		if (strlen(token) == 0) {
			token = strtok_r(NULL, ",", &saveptr);
			continue;
		}

		/* Add default port if not specified */
		if (strchr(token, ':') == NULL) {
			snprintf(module->filer_endpoints[endpoint_count],
				 SEAWEED_MAX_ENDPOINT_LEN,
				 "%s:%d", token, SEAWEED_DEFAULT_FILER_PORT);
		} else {
			strncpy(module->filer_endpoints[endpoint_count], token,
				SEAWEED_MAX_ENDPOINT_LEN - 1);
			module->filer_endpoints[endpoint_count][SEAWEED_MAX_ENDPOINT_LEN - 1] = '\0';
		}

		LogDebug(COMPONENT_FSAL, "Added filer endpoint: %s",
			 module->filer_endpoints[endpoint_count]);
		
		endpoint_count++;
		token = strtok_r(NULL, ",", &saveptr);
	}

	gsh_free(endpoints_copy);

	if (endpoint_count == 0) {
		LogMajor(COMPONENT_FSAL, "No valid filer endpoints found");
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	module->num_endpoints = endpoint_count;
	
	LogInfo(COMPONENT_FSAL, "Parsed %d filer endpoints", endpoint_count);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Load SeaweedFS FSAL configuration
 */
fsal_status_t seaweed_load_config(struct seaweed_fsal_module *module,
				  config_file_t config_struct,
				  struct config_error_type *err_type)
{
	config_file_t seaweed_config;
	fsal_status_t status;

	LogDebug(COMPONENT_FSAL, "Loading SeaweedFS FSAL configuration");

	/* Find SEAWEEDFS block in configuration */
	seaweed_config = config_FindItemByName(config_struct, "SEAWEEDFS");
	if (seaweed_config == NULL) {
		LogInfo(COMPONENT_FSAL, 
			"SEAWEEDFS block not found in config, using defaults");
		
		/* Set default values */
		strncpy(module->filer_endpoints[0], "localhost:18888", 
			SEAWEED_MAX_ENDPOINT_LEN - 1);
		module->filer_endpoints[0][SEAWEED_MAX_ENDPOINT_LEN - 1] = '\0';
		module->num_endpoints = 1;
		module->connection_timeout = 30;
		module->connection_pool_size = SEAWEED_DEFAULT_CONNECTIONS;
		module->max_concurrent_requests = 100;
		module->path_cache_size = SEAWEED_DEFAULT_CACHE_SIZE;
		module->path_cache_ttl = SEAWEED_DEFAULT_CACHE_TTL;
		module->enable_file_locks = true;
		module->default_lock_timeout = 300;
		
		return fsalstat(ERR_FSAL_NO_ERROR, 0);
	}

	/* Load configuration using config parser */
	int ret = load_config_from_parse(seaweed_config,
					 &seaweed_block,
					 module,
					 true,  /* Unknown parameters cause errors */
					 err_type);
	if (ret != 0) {
		LogMajor(COMPONENT_FSAL,
			"Failed to parse SeaweedFS configuration: %s",
			err_type->type_name);
		return fsalstat(ERR_FSAL_INVAL, ret);
	}

	/* Parse filer endpoints (they come as a single string) */
	status = parse_filer_endpoints(module, module->filer_endpoints[0]);
	if (FSAL_IS_ERROR(status)) {
		return status;
	}

	/* Validate configuration values */
	if (module->connection_pool_size < 1) {
		LogMajor(COMPONENT_FSAL,
			"Invalid connection pool size: %d",
			module->connection_pool_size);
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	if (module->path_cache_size < 1000) {
		LogMajor(COMPONENT_FSAL,
			"Path cache size too small: %d (minimum 1000)",
			module->path_cache_size);
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	if (module->path_cache_ttl < 10) {
		LogMajor(COMPONENT_FSAL,
			"Path cache TTL too small: %d (minimum 10 seconds)",
			module->path_cache_ttl);
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	LogInfo(COMPONENT_FSAL,
		"SeaweedFS FSAL configuration loaded successfully");
	
	LogDebug(COMPONENT_FSAL,
		 "Config: endpoints=%d, pool_size=%d, cache_size=%d, "
		 "cache_ttl=%d, locks=%s, lock_timeout=%d",
		 module->num_endpoints,
		 module->connection_pool_size,
		 module->path_cache_size,
		 module->path_cache_ttl,
		 module->enable_file_locks ? "enabled" : "disabled",
		 module->default_lock_timeout);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Register configuration parameters with Ganesha
 * 
 * This function should be called during FSAL initialization.
 */
void seaweed_register_config(void)
{
	int ret;

	ret = register_config_block(&seaweed_block);
	if (ret != 0) {
		LogCrit(COMPONENT_FSAL,
			"Failed to register SeaweedFS configuration block");
	}
}