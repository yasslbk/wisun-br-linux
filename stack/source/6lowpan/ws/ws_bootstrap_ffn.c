/*
 * Copyright (c) 2021, Pelion and affiliates.
 * Copyright (c) 2021-2023 Silicon Laboratories Inc. (www.silabs.com)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include "common/log.h"
#include "common/rand.h"
#include "common/trickle.h"
#include "common/log_legacy.h"
#include "common/endian.h"
#include "common/events_scheduler.h"
#include "common/serial_number_arithmetic.h"
#include "service_libs/etx/etx.h"
#include "service_libs/mac_neighbor_table/mac_neighbor_table.h"
#include "service_libs/blacklist/blacklist.h"
#include "service_libs/random_early_detection/random_early_detection_api.h"
#include "core/net_interface.h"
#include "stack/mac/platform/topo_trace.h"
#include "stack/mac/mac_common_defines.h"
#include "stack/mac/mac_api.h"
#include "stack/mac/fhss_config.h"
#include "stack/mac/sw_mac.h"

#include "app_wsbrd/rcp_api.h"
#include "nwk_interface/protocol.h"
#include "ipv6_stack/ipv6_routing_table.h"
#include "mpl/mpl.h"
#include "rpl/rpl_protocol.h"
#include "rpl/rpl_control.h"
#include "rpl/rpl_data.h"
#include "rpl/rpl_policy.h"
#include "core/timers.h"
#include "common_protocols/icmpv6.h"
#include "common_protocols/ipv6_constants.h"
#include "common_protocols/ip.h"
#include "legacy/dhcpv6_utils.h"
#include "legacy/dhcpv6_service.h"
#include "legacy/dhcpv6_client.h"
#include "6lowpan/lowpan_adaptation_interface.h"
#include "6lowpan/bootstraps/protocol_6lowpan.h"
#include "6lowpan/bootstraps/protocol_6lowpan_interface.h"
#include "6lowpan/mac/mac_helper.h"
#include "6lowpan/mac/mpx_api.h"

#include "6lowpan/ws/ws_bbr_api_internal.h"
#include "6lowpan/ws/ws_bootstrap.h"
#include "6lowpan/ws/ws_bootstrap_ffn.h"
#include "6lowpan/ws/ws_cfg_settings.h"
#include "6lowpan/ws/ws_common_defines.h"
#include "6lowpan/ws/ws_common.h"
#include "6lowpan/ws/ws_config.h"
#include "6lowpan/ws/ws_eapol_auth_relay.h"
#include "6lowpan/ws/ws_eapol_pdu.h"
#include "6lowpan/ws/ws_eapol_relay.h"
#include "6lowpan/ws/ws_ie_lib.h"
#include "6lowpan/ws/ws_ie_validation.h"
#include "6lowpan/ws/ws_llc.h"
#include "6lowpan/ws/ws_management_api.h"
#include "6lowpan/ws/ws_neighbor_class.h"
#include "6lowpan/ws/ws_pae_controller.h"
#include "6lowpan/ws/ws_stats.h"

#define TRACE_GROUP "wsbs"

static parent_info_t *ws_bootstrap_ffn_candidate_parent_get_best(struct net_if *cur)
{
    ns_list_foreach_safe(parent_info_t, entry, &cur->ws_info.parent_list_reserved) {
        tr_info("candidate list a:%s panid:%x cost:%d size:%d rssi:%d txFailure:%u age:%"PRIu32, tr_eui64(entry->addr), entry->pan_id, entry->pan_information.routing_cost, entry->pan_information.pan_size, entry->signal_dbm, entry->tx_fail, g_monotonic_time_100ms - entry->age);
    }

    return ns_list_get_first(&cur->ws_info.parent_list_reserved);
}

static parent_info_t *ws_bootstrap_ffn_candidate_parent_allocate(struct net_if *cur, const uint8_t *addr)
{
    parent_info_t *entry = ns_list_get_first(&cur->ws_info.parent_list_free);
    if (entry) {
        memcpy(entry->addr, addr, 8);
        ns_list_remove(&cur->ws_info.parent_list_free, entry);
        ns_list_add_to_end(&cur->ws_info.parent_list_reserved, entry);
    } else {
        // If there is no free entries always allocate the last one of reserved as it is the worst
        entry = ns_list_get_last(&cur->ws_info.parent_list_reserved);

    }
    if (entry) {
        entry->tx_fail = 0;
        entry->link_acceptable = false;
    }
    return entry;
}

parent_info_t *ws_bootstrap_ffn_candidate_parent_get(struct net_if *cur, const uint8_t *addr, bool create)
{
    ns_list_foreach_safe(parent_info_t, entry, &cur->ws_info.parent_list_reserved) {
        if (memcmp(entry->addr, addr, 8) == 0) {
            return entry;
        }
    }
    if (create) {
        return ws_bootstrap_ffn_candidate_parent_allocate(cur, addr);
    }
    return NULL;
}

static bool ws_bootstrap_ffn_candidate_parent_compare(parent_info_t *p1, parent_info_t *p2)
{
    // Return true if P2 is better
    // signal lower than threshold for both
    // pan_cost
    // signal quality

    if (p2->tx_fail > p1->tx_fail) {
        return false;
    }

    if (p2->tx_fail < p1->tx_fail) {
        return true;
    }

    if (p1->link_acceptable && !p2->link_acceptable) {
        // Link acceptable is always better than not
        return true;
    }
    if (!p1->link_acceptable && p2->link_acceptable) {
        // Link acceptable is always better than not
        return false;
    }

    // Select the lowest PAN cost
    uint16_t p1_pan_cost = (p1->pan_information.routing_cost / PRC_WEIGHT_FACTOR) + (p1->pan_information.pan_size / PS_WEIGHT_FACTOR);
    uint16_t p2_pan_cost = (p2->pan_information.routing_cost / PRC_WEIGHT_FACTOR) + (p2->pan_information.pan_size / PS_WEIGHT_FACTOR);
    if (p1_pan_cost > p2_pan_cost) {
        return true;
    } else if (p1_pan_cost < p2_pan_cost) {
        return false;
    }

    // If pan cost is the same then we select the one we hear highest
    if (p1->signal_dbm < p2->signal_dbm) {
        return true;
    }
    return false;
}

static void ws_bootstrap_ffn_candidate_parent_sort(struct net_if *cur, parent_info_t *new_entry)
{
    //Remove from the list

    ns_list_foreach_safe(parent_info_t, entry, &cur->ws_info.parent_list_reserved) {

        if (entry == new_entry) {
            // own entry skip it
            continue;
        }

        if (ws_bootstrap_ffn_candidate_parent_compare(entry, new_entry)) {
            // New entry is better
            //tr_debug("candidate list new is better");
            ns_list_remove(&cur->ws_info.parent_list_reserved, new_entry);
            ns_list_add_before(&cur->ws_info.parent_list_reserved, entry, new_entry);
            return;
        }
    }
    // This is the last entry
    ns_list_remove(&cur->ws_info.parent_list_reserved, new_entry);
    ns_list_add_to_end(&cur->ws_info.parent_list_reserved, new_entry);
}

static void ws_bootstrap_ffn_candidate_parent_mark_failure(struct net_if *cur, const uint8_t *addr)
{
    parent_info_t *entry = ws_bootstrap_ffn_candidate_parent_get(cur, addr, false);
    if (entry) {
        if (entry->tx_fail >= 2) {
            ns_list_remove(&cur->ws_info.parent_list_reserved, entry);
            ns_list_add_to_end(&cur->ws_info.parent_list_free, entry);
        } else {
            entry->tx_fail++;
            ws_bootstrap_ffn_candidate_parent_sort(cur, entry);
        }

    }
}

static int8_t ws_bootstrap_ffn_neighbor_set(struct net_if *cur, parent_info_t *parent_ptr, bool clear_list)
{
    uint16_t pan_id = cur->ws_info.network_pan_id;

    // Add EAPOL neighbor
    cur->ws_info.network_pan_id = parent_ptr->pan_id;
    cur->ws_info.pan_information.pan_size = parent_ptr->pan_information.pan_size;
    cur->ws_info.pan_information.routing_cost = parent_ptr->pan_information.routing_cost;
    cur->ws_info.pan_information.use_parent_bs = parent_ptr->pan_information.use_parent_bs;
    cur->ws_info.pan_information.pan_version = 0; // This is learned from actual configuration
    cur->ws_info.pan_information.lpan_version = 0; // This is learned from actual configuration

    // If PAN ID changes, clear learned neighbors and activate FHSS
    if (pan_id != cur->ws_info.network_pan_id) {
        if (clear_list) {
            ws_bootstrap_neighbor_list_clean(cur);
        }
        ws_bootstrap_fhss_activate(cur);
    }

    llc_neighbour_req_t neighbor_info;
    if (!ws_bootstrap_neighbor_get(cur, parent_ptr->addr, &neighbor_info) &&
        !ws_bootstrap_neighbor_add(cur, parent_ptr->addr, &neighbor_info, WS_NR_ROLE_ROUTER)) {
        //Remove Neighbour and set Link setup back
        ns_list_remove(&cur->ws_info.parent_list_reserved, parent_ptr);
        ns_list_add_to_end(&cur->ws_info.parent_list_free, parent_ptr);
        return -1;
    }
    ws_bootstrap_neighbor_set_stable(cur, parent_ptr->addr);
    ws_neighbor_class_ut_update(neighbor_info.ws_neighbor, parent_ptr->ws_utt.ufsi, parent_ptr->timestamp, parent_ptr->addr);
    ws_neighbor_class_us_update(cur, neighbor_info.ws_neighbor, &parent_ptr->ws_us.chan_plan,
                                parent_ptr->ws_us.dwell_interval, parent_ptr->addr);
    return 0;
}

void ws_bootstrap_ffn_seconds_timer(struct net_if *cur, uint32_t seconds)
{
    /* Border router keep alive check
     */
    if (cur->ws_info.pan_timeout_timer) {
        // PAN version timer running
        if (cur->ws_info.pan_timeout_timer > seconds) {
            cur->ws_info.pan_timeout_timer -= seconds;
            if (cur->ws_info.pan_timeout_timer < cur->ws_info.cfg->timing.pan_timeout / 10) {
                /* pan timeout is closing need to verify that DAO is tested before the pan times out.
                   This will give some extra time for RPL to find better parents.
                   Border router liveliness can be checked from version number change or from successful DAO registrations
                   in this case there has not been any version number changes during this PAN lifetime.
                */
                rpl_control_dao_timeout(cur->rpl_domain, 20);
            }
        } else {
            // Border router has timed out
            //Clear Timeout timer
            cur->ws_info.pan_timeout_timer = 0;
            tr_warn("Border router has timed out");
            ws_bootstrap_event_disconnect(cur, WS_FAST_DISCONNECT);
        }
    }
    if (cur->ws_info.aro_registration_timer) {
        if (cur->ws_info.aro_registration_timer > seconds) {
            cur->ws_info.aro_registration_timer -= seconds;
        } else {
            // Update all addressess. This function will update the timer value if needed
            cur->ws_info.aro_registration_timer = 0;
            ws_address_registration_update(cur, NULL);
        }
    }

    if (cur->ws_info.ws_bsi_block.block_time) {
        if (cur->ws_info.ws_bsi_block.block_time > seconds) {
            cur->ws_info.ws_bsi_block.block_time -= seconds;
        } else {
            //Clear A BSI blokker
            cur->ws_info.ws_bsi_block.block_time = 0;
            cur->ws_info.ws_bsi_block.old_bsi = 0;
        }
    }
}

const uint8_t *ws_bootstrap_authentication_next_target(struct net_if *cur, const uint8_t *previous_eui_64, uint16_t *pan_id)
{
    ws_bootstrap_ffn_candidate_parent_mark_failure(cur, previous_eui_64);

    // Gets best target
    parent_info_t *parent_info = ws_bootstrap_ffn_candidate_parent_get_best(cur);
    if (parent_info) {
        /* On failure still continues with the new parent, and on next call,
           will try to set the neighbor again */
        ws_bootstrap_ffn_neighbor_set(cur, parent_info, true);
        *pan_id = parent_info->pan_id;
        return parent_info->addr;
    }

    // If no targets found, retries the last one
    return previous_eui_64;
}

void ws_ffn_trickle_stop(struct ws_mngt *mngt)
{
    mngt->trickle_pas_running = false;
    mngt->trickle_pcs_running = false;
}

static void ws_bootstrap_pan_advert_solicit(struct net_if *cur)
{
    struct ws_llc_mngt_req req = {
        .frame_type = WS_FT_PAS,
        .wh_ies.utt     = true,
        .wp_ies.us      = true,
        .wp_ies.netname = true,
        .wp_ies.pom     = ws_version_1_1(cur),
    };

    ws_stats_update(cur, STATS_WS_ASYNCH_TX_PAS, 1);
    ws_llc_asynch_request(cur, &req);
}

void ws_ffn_pas_trickle(struct net_if *cur, int ticks)
{
    if (cur->ws_info.mngt.trickle_pas_running &&
            trickle_timer(&cur->ws_info.mngt.trickle_pas, &cur->ws_info.mngt.trickle_params, ticks)) {
        // send PAN advertisement solicit
        ws_bootstrap_pan_advert_solicit(cur);
    }
}

void ws_ffn_pas_test_exec(struct net_if *cur, int procedure)
{
    tr_info("trigger PAN advertisement Solicit");
    if (procedure != PROCEDURE_PAS_TRICKLE_INCON) {
        tr_info("send PAN advertisement Solicit");
        ws_bootstrap_pan_advert_solicit(cur);
    }
    if (cur->ws_info.mngt.trickle_pas_running) {
        trickle_inconsistent_heard(&cur->ws_info.mngt.trickle_pas, &cur->ws_info.mngt.trickle_params);
    }
}

void ws_ffn_pas_test_trigger(struct net_if *cur, int seconds)
{
    if (cur->ws_info.mngt.trickle_pas_running) {
        if (cur->ws_info.test_proc_trg.pas_trigger_timer > seconds) {
            cur->ws_info.test_proc_trg.pas_trigger_timer -= seconds;
        } else  {
            if (cur->ws_info.test_proc_trg.pas_trigger_count > 2) {
                ws_bootstrap_test_procedure_trigger_exec(cur, PROCEDURE_PAS_TRICKLE_INCON);
            } else {
                cur->ws_info.test_proc_trg.pas_trigger_count++;
                ws_bootstrap_test_procedure_trigger_exec(cur, PROCEDURE_PAS);
            }
            cur->ws_info.test_proc_trg.pas_trigger_timer = (cur->ws_info.mngt.trickle_params.Imin / 10);
        }
        if (cur->ws_info.test_proc_trg.eapol_trigger_timer > seconds) {
            cur->ws_info.test_proc_trg.eapol_trigger_timer -= seconds;
        } else {
            ws_bootstrap_test_procedure_trigger_exec(cur, PROCEDURE_EAPOL);
            cur->ws_info.test_proc_trg.eapol_trigger_timer = (cur->ws_info.mngt.trickle_params.Imin / 10) / 2;
        }
    }
}

static void ws_bootstrap_pan_config_solicit(struct net_if *cur)
{
    struct ws_llc_mngt_req req = {
        .frame_type = WS_FT_PCS,
        .wh_ies.utt     = true,
        .wp_ies.us      = true,
        .wp_ies.netname = true,
    };

    ws_stats_update(cur, STATS_WS_ASYNCH_TX_PCS, 1);
    ws_llc_asynch_request(cur, &req);
}

void ws_ffn_pcs_trickle(struct net_if *cur, int ticks)
{
    if (cur->ws_info.mngt.trickle_pcs_running) {

        //Update MAX config sol timeout timer
        if (cur->ws_info.mngt.pcs_max_timeout > ticks) {
            cur->ws_info.mngt.pcs_max_timeout -= ticks;
        } else {
            //Config sol state timeout
            cur->ws_info.mngt.pcs_max_timeout = 0;
        }

        if (trickle_timer(&cur->ws_info.mngt.trickle_pcs, &cur->ws_info.mngt.trickle_params, ticks)) {
            if (cur->ws_info.mngt.pcs_count < PCS_MAX) {
                // send PAN Configuration solicit
                ws_bootstrap_pan_config_solicit(cur);
            }
            //Update counter every time reason that we detect PCS_MAX higher state
            cur->ws_info.mngt.pcs_count++;
        }

        if (cur->ws_info.mngt.pcs_count > PCS_MAX || cur->ws_info.mngt.pcs_max_timeout == 0) {
            // if MAX PCS sent or max waited timeout restart discovery
            // Trickle is reseted when entering to discovery from state 3
            tr_info("PAN configuration Solicit timeout");
            trickle_inconsistent_heard(&cur->ws_info.mngt.trickle_pas, &cur->ws_info.mngt.trickle_params);
            ws_bootstrap_event_discovery_start(cur);
            return;
        }
    }
}

void ws_ffn_pcs_test_exec(struct net_if *cur, int procedure)
{
    if (cur->ws_info.mngt.trickle_pcs_running || ws_bootstrap_state_active(cur)) {
        tr_info("trigger PAN configuration Solicit");
        if (procedure != PROCEDURE_PCS_TRICKLE_INCON) {
            tr_info("send PAN configuration Solicit");
            ws_bootstrap_pan_config_solicit(cur);
        }
        if (cur->ws_info.mngt.trickle_pcs_running) {
            trickle_inconsistent_heard(&cur->ws_info.mngt.trickle_pcs, &cur->ws_info.mngt.trickle_params);
        }
    } else {
        tr_info("wrong state: PAN configuration Solicit not triggered");
    }
}

void ws_ffn_pcs_test_trigger(struct net_if *cur, int seconds)
{
    if (cur->ws_info.mngt.trickle_pcs_running) {
        if (cur->ws_info.test_proc_trg.pcs_trigger_timer > seconds) {
            cur->ws_info.test_proc_trg.pcs_trigger_timer -= seconds;
        } else  {
            if (cur->ws_info.test_proc_trg.pcs_trigger_count > 2) {
                ws_bootstrap_test_procedure_trigger_exec(cur, PROCEDURE_PCS_TRICKLE_INCON);
            } else {
                cur->ws_info.test_proc_trg.pcs_trigger_count++;
                ws_bootstrap_test_procedure_trigger_exec(cur, PROCEDURE_PCS);
            }
            cur->ws_info.test_proc_trg.pcs_trigger_timer = (cur->ws_info.mngt.trickle_params.Imin / 10);
        }
    }
}
