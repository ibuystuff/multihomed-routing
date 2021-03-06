/*
 * Copyright 2017 Kristian Evensen <kristian.evensen@gmail.com>
 *
 * This file is part of Usb Monitor. Usb Monitor is free software: you can
 * redistribute it and/or modify it under the terms of the Lesser GNU General
 * Public License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 *
 * Usb Montior is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Usb Monitor. If not, see http://www.gnu.org/licenses/.
 */

#include <stdint.h>
#include <strings.h>
#include <time.h>

#include <table_allocator_shared_log.h>

#include "table_allocator_server_clients.h"
#include "table_allocator_server.h"
#include "table_allocator_server_sqlite.h"

static uint32_t allocate_table(struct tas_ctx *ctx, uint8_t addr_family)
{
    uint32_t rt_table = 0;
    uint32_t *rt_tables = NULL;

    if (addr_family == AF_INET) {
        rt_tables = ctx->tables_inet; 
    } else if (addr_family == AF_INET6) {
        rt_tables = ctx->tables_inet6;
    } else {
        rt_tables = ctx->tables_unspec;
    }

    for (int i = 0; i < ctx->num_table_elements; i++) {
        //Zero means all indexes represented by this element is taken
        if(!rt_tables[i])
            continue;

        //Lowest value returned by ffs is 1, so must fix when setting
        rt_table = ffs(rt_tables[i]);
        rt_tables[i] ^= (1 << (rt_table - 1));
        rt_table += (i*(sizeof(rt_tables[i]))*8);
        break;
    }

    //Prevent reurning file descriptors larger than the limit. Larger than
    //because lowest bit has index 1, not 0 (so we have 1-MAX and not 0-MAX -1)
    if (rt_table > ctx->num_tables)
        return 0;
    else
        return rt_table;
}

static void release_table(struct tas_ctx *ctx, uint8_t addr_family,
        uint32_t rt_table)
{
    uint32_t element_index, element_bit;

    //we do not add +1, since when using the normal bitwise operators we index
    //at 0
    rt_table = rt_table - ctx->table_offset;

    element_index = rt_table >> 5;
    //What we do here is to mask out the lowest five bits. They contain the
    //index of the bit to be set (remember that 32 is 0x20);
     element_bit = rt_table & 0x1F;

    if (addr_family == AF_INET) {
        ctx->tables_inet[element_index] ^= (1 << element_bit);
    } else if (addr_family == AF_INET6) {
        ctx->tables_inet6[element_index] ^= (1 << element_bit);
    } else {
        ctx->tables_unspec[element_index] ^= (1 << element_bit);
    }
}

static void set_table(struct tas_ctx *ctx, uint8_t addr_family,
        uint32_t rt_table)
{
    uint32_t element_index, element_bit;

    //we do not add +1, since when using the normal bitwise operators we index
    //at 0
    rt_table = rt_table - ctx->table_offset;

    element_index = rt_table >> 5;
    //What we do here is to mask out the lowest five bits. They contain the
    //index of the bit to be set (remember that 32 is 0x20);
    element_bit = rt_table & 0x1F;

    if (element_index >= ctx->num_table_elements)
        return;

    if (addr_family == AF_INET) {
        ctx->tables_inet[element_index] ^= (1 << element_bit);
    } else if (addr_family == AF_INET6) {
        ctx->tables_inet6[element_index] ^= (1 << element_bit);
    } else {
        ctx->tables_unspec[element_index] ^= (1 << element_bit);
    }
}

static uint8_t is_table_free(struct tas_ctx *ctx, uint8_t addr_family,
        uint32_t rt_table)
{
    uint32_t element_index, element_bit, element_mask, element_masked;

    //we do not add +1, since when using the normal bitwise operators we index
    //at 0
    rt_table = rt_table - ctx->table_offset;

    element_index = rt_table >> 5;
    //What we do here is to mask out the lowest five bits. They contain the
    //index of the bit to be set (remember that 32 is 0x20);
    element_bit = rt_table & 0x1F;

    if (element_index >= ctx->num_table_elements)
        return 0;

    element_mask = 1 << element_bit;

    if (addr_family == AF_INET) {
        element_masked = ctx->tables_inet[element_index] & element_mask;
    } else if (addr_family == AF_INET6) {
        element_masked = ctx->tables_inet6[element_index] & element_mask;
    } else {
        element_masked = ctx->tables_unspec[element_index] & element_mask;
    }

    return !!element_masked;
}

static void release_dead_lease(void *ptr, uint8_t addr_family,
        uint32_t rt_table)
{
    struct tas_ctx *ctx = ptr;

    if (!is_table_free(ctx, addr_family, rt_table)) {
        TA_PRINT_SYSLOG(ctx, LOG_INFO, "Will release dead lease on table "
                "%u-%u\n", addr_family, rt_table);
        release_table(ctx, addr_family, rt_table);
    }
}

void table_allocator_server_clients_delete_dead_leases(struct tas_ctx *ctx)
{
    struct timespec t_now;
    
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &t_now)) {
        return;
    }

    table_allocator_sqlite_delete_dead_leases(ctx, t_now.tv_sec,
            release_dead_lease);
}

//return 0/1 on success, on successs, table is stored in table. Reason for not
//just returning table, is that we might want to expand with more error codes
//later
uint8_t table_allocator_server_clients_handle_req(struct tas_ctx *ctx,
        struct tas_client_req *req, uint32_t *rt_table, uint32_t *lease_sec_ptr)
{
    uint32_t rt_table_returned = 0;
    struct timespec t_now;
    time_t lease_sec = 0;

    switch(req->addr_family) {
    case AF_INET:
        if (!ctx->tables_inet) {
            return 0;
        }
        break;
    case AF_INET6:
        if (!ctx->tables_inet6) {
            return 0;
        }
        break;
    case AF_UNSPEC:
        if (!ctx->tables_unspec) {
            return 0;
        }
        break;
    default:
        return 0;
    }

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &t_now)) {
        return 0;
    }

    lease_sec = t_now.tv_sec + ctx->table_timeout;

    //check database for existing table allocation
    rt_table_returned = table_allocator_sqlite_get_table(ctx, req);

    if (rt_table_returned) {
        //if for some reason the initial database read should fail, we need to
        //update map when we find leases in the databases. There is no race with
        //new leases. If we get the lease request for a new tuple, then the
        //value stored in the db will (potentially) be overwritten
        if (is_table_free(ctx, req->addr_family, rt_table_returned)) {
            set_table(ctx, req->addr_family, rt_table_returned);
        }

        //update lease, silently fail and trust client logic (for now)
        if (!table_allocator_sqlite_update_lease(ctx, rt_table_returned,
                    req->addr_family, lease_sec)) {
            return 0;
        }

        TA_PRINT_SYSLOG(ctx, LOG_INFO, "Reallocated table %u to %s (%s)\n",
                rt_table_returned, req->address, req->ifname);

        *rt_table = rt_table_returned;
        *lease_sec_ptr = lease_sec;
        return 1;
    }
    
    //allocate table if not found
    if (!(rt_table_returned = allocate_table(ctx, req->addr_family))) {
        return 0;
    }

    //subtract 1 from table returned so that offset works correctly (ffs returns
    //1 as index for the first bit)
    rt_table_returned = ctx->table_offset + (rt_table_returned - 1);
    TA_PRINT(ctx->logfile, "Allocated table %u for %s (%s)\n",
            rt_table_returned, req->address, req->ifname);

    //insert into database
    if (!table_allocator_sqlite_insert_table(ctx, req, rt_table_returned,
                lease_sec)) {
        release_table(ctx, req->addr_family, rt_table_returned);
        return 0;
    }

    *rt_table = rt_table_returned;
    *lease_sec_ptr = lease_sec;
    return 1;
}

uint8_t table_allocator_server_clients_handle_release(struct tas_ctx *ctx,
        struct tas_client_req *req)
{
    uint32_t rt_table = table_allocator_sqlite_get_table(ctx, req);

    //if no table is find, just return succesful to prevent for example clients
    //hanging on releasing non-existent leases
    if (!rt_table) {
        return 1;
    }

    if (table_allocator_sqlite_remove_table(ctx, req)) {
        TA_PRINT_SYSLOG(ctx, LOG_INFO, "Release table %u for %s (%s)\n",
                rt_table, req->address, req->ifname);
        release_table(ctx, req->addr_family, rt_table);
        return 1;
    } else {
        return 0;
    }
}

void table_allocator_server_clients_set_table(struct tas_ctx *ctx,
        uint8_t addr_family, uint32_t rt_table)
{
    TA_PRINT_SYSLOG(ctx, LOG_INFO, "Will set active table %u-%u\n", addr_family,
            rt_table);
    set_table(ctx, addr_family, rt_table);
}
