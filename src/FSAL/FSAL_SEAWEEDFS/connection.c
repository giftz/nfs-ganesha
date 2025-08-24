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
 * @file connection.c
 * @brief SeaweedFS Filer connection pool management
 */

#include "config.h"
#include "fsal.h"
#include "fsal_seaweed.h"
#include <pthread.h>
#include <time.h>

/* Connection pool management */

/**
 * @brief Initialize connection pool
 */
fsal_status_t seaweed_init_connection_pool(struct seaweed_fsal_module *module)
{
	struct seaweed_connection_pool *pool = &module->conn_pool;
	struct seaweed_filer_connection *conn;
	int i;

	LogDebug(COMPONENT_FSAL, "Initializing SeaweedFS connection pool");

	pool->connections = NULL;
	pool->pool_size = module->connection_pool_size;
	pool->active_count = 0;

	/* Create initial connections */
	for (i = 0; i < pool->pool_size && i < module->num_endpoints; i++) {
		conn = gsh_calloc(1, sizeof(struct seaweed_filer_connection));
		if (!conn) {
			LogMajor(COMPONENT_FSAL,
				"Failed to allocate connection structure");
			seaweed_destroy_connection_pool(module);
			return fsalstat(ERR_FSAL_NOMEM, ENOMEM);
		}

		/* Copy endpoint information */
		strncpy(conn->endpoint, module->filer_endpoints[i % module->num_endpoints],
			sizeof(conn->endpoint) - 1);
		conn->endpoint[sizeof(conn->endpoint) - 1] = '\0';
		
		/* Initialize connection mutex */
		if (pthread_mutex_init(&conn->lock, NULL) != 0) {
			LogMajor(COMPONENT_FSAL,
				"Failed to initialize connection mutex");
			gsh_free(conn);
			seaweed_destroy_connection_pool(module);
			return fsalstat(ERR_FSAL_SERVERFAULT, errno);
		}

		/* TODO: Initialize gRPC connection - for MVP we'll create stub connections */
		conn->grpc_channel = NULL;
		conn->filer_stub = NULL;
		conn->last_used = 0;
		conn->is_healthy = false; /* Will be set to true when first used successfully */

		/* Add to pool */
		conn->next = pool->connections;
		pool->connections = conn;
		pool->active_count++;

		LogDebug(COMPONENT_FSAL,
			"Created connection to %s", conn->endpoint);
	}

	/* Fill remaining pool slots by round-robin endpoints */
	while (pool->active_count < pool->pool_size) {
		int endpoint_idx = pool->active_count % module->num_endpoints;
		
		conn = gsh_calloc(1, sizeof(struct seaweed_filer_connection));
		if (!conn) {
			LogMajor(COMPONENT_FSAL,
				"Failed to allocate connection structure");
			break; /* Continue with fewer connections */
		}

		strncpy(conn->endpoint, module->filer_endpoints[endpoint_idx],
			sizeof(conn->endpoint) - 1);
		conn->endpoint[sizeof(conn->endpoint) - 1] = '\0';
		
		if (pthread_mutex_init(&conn->lock, NULL) != 0) {
			LogMajor(COMPONENT_FSAL,
				"Failed to initialize connection mutex");
			gsh_free(conn);
			break;
		}

		conn->grpc_channel = NULL;
		conn->filer_stub = NULL;
		conn->last_used = 0;
		conn->is_healthy = false;

		conn->next = pool->connections;
		pool->connections = conn;
		pool->active_count++;
	}

	LogInfo(COMPONENT_FSAL,
		"Connection pool initialized with %d connections across %d endpoints",
		pool->active_count, module->num_endpoints);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Destroy connection pool
 */
void seaweed_destroy_connection_pool(struct seaweed_fsal_module *module)
{
	struct seaweed_connection_pool *pool = &module->conn_pool;
	struct seaweed_filer_connection *conn, *next_conn;

	LogDebug(COMPONENT_FSAL, "Destroying SeaweedFS connection pool");

	pthread_mutex_lock(&pool->pool_lock);

	conn = pool->connections;
	while (conn) {
		next_conn = conn->next;

		/* Close gRPC connection */
		if (conn->grpc_channel) {
			/* TODO: Close gRPC channel properly */
			/* grpc_channel_destroy(conn->grpc_channel); */
			conn->grpc_channel = NULL;
		}

		if (conn->filer_stub) {
			/* TODO: Clean up gRPC stub */
			conn->filer_stub = NULL;
		}

		pthread_mutex_destroy(&conn->lock);
		gsh_free(conn);
		
		conn = next_conn;
	}

	pool->connections = NULL;
	pool->active_count = 0;

	pthread_mutex_unlock(&pool->pool_lock);

	LogInfo(COMPONENT_FSAL, "Connection pool destroyed");
}

/**
 * @brief Acquire a connection from the pool
 */
struct seaweed_filer_connection *seaweed_acquire_connection(
	struct seaweed_fsal_module *module)
{
	struct seaweed_connection_pool *pool = &module->conn_pool;
	struct seaweed_filer_connection *conn;
	struct seaweed_filer_connection *best_conn = NULL;
	time_t now = time(NULL);
	time_t oldest_time = now;

	pthread_mutex_lock(&pool->pool_lock);

	/* Find an available connection */
	conn = pool->connections;
	while (conn) {
		if (pthread_mutex_trylock(&conn->lock) == 0) {
			/* Connection is available */
			
			/* Prefer healthy connections */
			if (conn->is_healthy) {
				best_conn = conn;
				break;
			}
			
			/* Among unhealthy connections, prefer the least recently used */
			if (!best_conn || conn->last_used < oldest_time) {
				if (best_conn) {
					pthread_mutex_unlock(&best_conn->lock);
				}
				best_conn = conn;
				oldest_time = conn->last_used;
			} else {
				pthread_mutex_unlock(&conn->lock);
			}
		}
		conn = conn->next;
	}

	pthread_mutex_unlock(&pool->pool_lock);

	if (best_conn) {
		best_conn->last_used = now;
		LogFullDebug(COMPONENT_FSAL,
			     "Acquired connection to %s", best_conn->endpoint);
		
		/* Update statistics */
		pthread_mutex_lock(&module->stats_lock);
		module->stats.connections_active++;
		pthread_mutex_unlock(&module->stats_lock);
		
		return best_conn;
	}

	LogDebug(COMPONENT_FSAL, "No available connections in pool");
	
	/* Update failure statistics */
	pthread_mutex_lock(&module->stats_lock);
	module->stats.connections_failed++;
	pthread_mutex_unlock(&module->stats_lock);
	
	return NULL;
}

/**
 * @brief Release a connection back to the pool
 */
void seaweed_release_connection(struct seaweed_filer_connection *conn)
{
	if (!conn) {
		return;
	}

	LogFullDebug(COMPONENT_FSAL,
		     "Releasing connection to %s", conn->endpoint);

	pthread_mutex_unlock(&conn->lock);
}

/**
 * @brief Mark a connection as healthy
 */
void seaweed_mark_connection_healthy(struct seaweed_filer_connection *conn, 
				     bool healthy)
{
	if (!conn) {
		return;
	}

	if (conn->is_healthy != healthy) {
		conn->is_healthy = healthy;
		LogDebug(COMPONENT_FSAL,
			 "Connection to %s marked as %s",
			 conn->endpoint, healthy ? "healthy" : "unhealthy");
	}
}

/**
 * @brief Test if a connection is working
 * 
 * This is a stub implementation for MVP. In the full version,
 * this would actually test the gRPC connection.
 */
bool seaweed_test_connection(struct seaweed_filer_connection *conn)
{
	if (!conn) {
		return false;
	}

	/* For MVP, assume connections are healthy if they have been used recently */
	time_t now = time(NULL);
	if (conn->last_used > 0 && (now - conn->last_used) < 300) {
		return true;
	}

	/* TODO: Implement actual gRPC health check */
	/* For now, simulate a successful connection test */
	LogFullDebug(COMPONENT_FSAL,
		     "Testing connection to %s (stub implementation)", 
		     conn->endpoint);
	
	return true;
}

/**
 * @brief Initialize gRPC connection for a connection object
 * 
 * This is a stub for MVP. The actual implementation would
 * create gRPC channels and stubs.
 */
fsal_status_t seaweed_init_grpc_connection(struct seaweed_filer_connection *conn,
					   uint32_t timeout_sec)
{
	if (!conn) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	LogDebug(COMPONENT_FSAL,
		 "Initializing gRPC connection to %s (timeout: %ds)",
		 conn->endpoint, timeout_sec);

	/* TODO: Implement actual gRPC connection initialization */
	/*
	 * Example of what would be done:
	 * 
	 * grpc_channel_args args = GRPC_CHANNEL_ARGS_INIT;
	 * grpc_channel_args_set_integer(&args, GRPC_ARG_KEEPALIVE_TIME_MS, 30000);
	 * grpc_channel_args_set_integer(&args, GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 5000);
	 * 
	 * conn->grpc_channel = grpc_insecure_channel_create(conn->endpoint, &args);
	 * if (!conn->grpc_channel) {
	 *     return fsalstat(ERR_FSAL_SERVERFAULT, ECONNREFUSED);
	 * }
	 * 
	 * conn->filer_stub = seaweed_filer_stub_create(conn->grpc_channel);
	 * if (!conn->filer_stub) {
	 *     grpc_channel_destroy(conn->grpc_channel);
	 *     conn->grpc_channel = NULL;
	 *     return fsalstat(ERR_FSAL_SERVERFAULT, ENOMEM);
	 * }
	 */

	/* For MVP stub implementation */
	conn->grpc_channel = (void*)0x1; /* Non-null placeholder */
	conn->filer_stub = (void*)0x1;   /* Non-null placeholder */
	conn->is_healthy = true;
	conn->last_used = time(NULL);

	LogInfo(COMPONENT_FSAL,
		"gRPC connection to %s initialized successfully (stub)", 
		conn->endpoint);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Close gRPC connection
 */
void seaweed_close_grpc_connection(struct seaweed_filer_connection *conn)
{
	if (!conn) {
		return;
	}

	LogDebug(COMPONENT_FSAL,
		 "Closing gRPC connection to %s", conn->endpoint);

	/* TODO: Implement actual gRPC connection cleanup */
	/*
	 * if (conn->filer_stub) {
	 *     seaweed_filer_stub_destroy(conn->filer_stub);
	 *     conn->filer_stub = NULL;
	 * }
	 * 
	 * if (conn->grpc_channel) {
	 *     grpc_channel_destroy(conn->grpc_channel);
	 *     conn->grpc_channel = NULL;
	 * }
	 */

	/* For MVP stub implementation */
	conn->grpc_channel = NULL;
	conn->filer_stub = NULL;
	conn->is_healthy = false;

	LogDebug(COMPONENT_FSAL,
		 "gRPC connection to %s closed (stub)", conn->endpoint);
}