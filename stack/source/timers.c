#include <assert.h>
#include "stack/source/6lowpan/lowpan_adaptation_interface.h"
#include "stack/source/6lowpan/fragmentation/cipv6_fragmenter.h"
#include "stack/source/6lowpan/nd/nd_router_object.h"
#include "stack/source/6lowpan/ws/ws_common.h"
#include "stack/source/6lowpan/ws/ws_pae_controller.h"
#include "stack/source/core/ns_address_internal.h"
#include "stack/source/ipv6_stack/ipv6_routing_table.h"
#include "stack/source/common_protocols/mld.h"
#include "stack/source/common_protocols/ipv6_fragmentation_rx.h"
#include "stack/source/nwk_interface/protocol_core.h"
#include "stack/source/mpl/mpl.h"
#include "stack/source/rpl/rpl_control.h"
#include "stack/source/service_libs/etx/etx.h"
#include "stack/source/legacy/dhcpv6_service.h"
#include "stack/timers.h"
#include "common/utils.h"
#include "common/log.h"

int g_monotonic_time_100ms = 0;

static void timer_update_monotonic_time(int ticks)
{
    g_monotonic_time_100ms += ticks;
}

#define timer_entry(name, callback, period_ms, is_periodic) \
    [TIMER_##name] { #name, callback, period_ms, is_periodic, 0 }
static struct {
    const char *trace_name;
    void (*callback)(int);
    int period_ms;
    bool periodic;
    int timeout;
} s_timers[] = {
    timer_entry(MONOTONIC_TIME,         timer_update_monotonic_time,                100,                                          true),
    timer_entry(MPL_FAST,               mpl_fast_timer,                             MPL_TICK_MS,                                  false),
    timer_entry(MPL_SLOW,               mpl_slow_timer,                             1000,                                         true),
    timer_entry(RPL_FAST,               rpl_control_fast_timer,                     100,                                          true),
    timer_entry(RPL_SLOW,               rpl_control_slow_timer,                     1000,                                         true),
    timer_entry(IPV6_DESTINATION,       ipv6_destination_cache_timer,               DCACHE_GC_PERIOD * 1000,                      true),
    timer_entry(IPV6_ROUTE,             ipv6_route_table_ttl_update,                1000,                                         true),
    timer_entry(IPV6_FRAG,              ipv6_frag_timer,                            1000,                                         true),
    timer_entry(CIPV6_FRAG,             cipv6_frag_timer,                           1000,                                         true),
    timer_entry(ICMP_FAST,              icmp_fast_timer,                            100,                                          true),
    timer_entry(ICMP_SLOW,              icmp_slow_timer,                            1000,                                         true),
    timer_entry(PAE_FAST,               ws_pae_controller_fast_timer,               100,                                          true),
    timer_entry(PAE_SLOW,               ws_pae_controller_slow_timer,               1000,                                         true),
    timer_entry(DHCPV6_SOCKET,          dhcp_service_timer_cb,                      100,                                          false),
    timer_entry(6LOWPAN_MLD_FAST,       mld_fast_timer,                             100,                                          true),
    timer_entry(6LOWPAN_MLD_SLOW,       mld_slow_timer,                             125000,                                       true),
    timer_entry(6LOWPAN_ADDR_FAST,      addr_fast_timer,                            100,                                          true),
    timer_entry(6LOWPAN_ADDR_SLOW,      addr_slow_timer,                            1000,                                         true),
    timer_entry(WS_COMMON_FAST,         ws_common_fast_timer,                       100,                                          true),
    timer_entry(WS_COMMON_SLOW,         ws_common_seconds_timer,                    1000,                                         true),
    timer_entry(6LOWPAN_ND,             nd_object_timer,                            100,                                          true),
    timer_entry(6LOWPAN_ETX,            etx_cache_timer,                            1000,                                         true),
    timer_entry(6LOWPAN_ADAPTATION,     lowpan_adaptation_interface_slow_timer,     1000,                                         true),
    timer_entry(6LOWPAN_NEIGHBOR,       mac_neighbor_table_neighbor_timeout_update, 1000,                                         true),
    timer_entry(6LOWPAN_NEIGHBOR_SLOW,  ipv6_neighbour_cache_slow_timer,            1000,                                         true),
    timer_entry(6LOWPAN_NEIGHBOR_FAST,  ipv6_neighbour_cache_fast_timer,            100,                                          true),
    timer_entry(6LOWPAN_CONTEXT,        lowpan_context_timer,                       100,                                          true),
    timer_entry(6LOWPAN_BOOTSTRAP,      nwk_bootstrap_timer,                        100,                                          true),
    timer_entry(6LOWPAN_REACHABLE_TIME, update_reachable_time,                      1000,                                         true),
};
static_assert(ARRAY_SIZE(s_timers) == TIMER_COUNT, "missing timer declarations");

void timer_start(enum timer_id id)
{
    BUG_ON(s_timers[id].period_ms % TIMER_GLOBAL_PERIOD_MS);
    s_timers[id].timeout = s_timers[id].period_ms / TIMER_GLOBAL_PERIOD_MS;
}

void timer_stop(enum timer_id id)
{
    s_timers[id].timeout = 0;
}

void timer_global_tick()
{
    for (int i = 0; i < ARRAY_SIZE(s_timers); i++) {
        if (!s_timers[i].timeout)
            continue;

        s_timers[i].timeout--; // Always advance one tick at a time
        if (s_timers[i].timeout)
            continue;

        s_timers[i].callback(1);
        TRACE(TR_TIMERS, "timer: %s", s_timers[i].trace_name);
        if (s_timers[i].periodic)
            timer_start(i);
    }
}
