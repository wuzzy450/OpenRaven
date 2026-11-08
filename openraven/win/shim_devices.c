// libudev and libiw (wireless tools) stubs: no devices, no wireless.
// Real input comes later through the rio2pad/XInput path (milestone 7).
#include "shim.h"

static char g_obj[4][16];  // non-NULL dummy handles: udev, enumerate, monitor, device
static SV void *s_udev(void) { return g_obj[0]; }
static SV void *s_enum(void) { return g_obj[1]; }
static SV void *s_mon(void) { return g_obj[2]; }
static SV void *s_null(void) { return NULL; }
static SV int s_zero(void) { return 0; }
static SV int s_neg(void) { return -1; }
// Never readable in our select(); a real fd number so FD_SET stays in range.
static SV int s_mon_fd(void) { return 901; }

const shim_entry shims_devices[] = {
    {"udev_new", s_udev}, {"udev_unref", s_null},
    {"udev_enumerate_new", s_enum}, {"udev_enumerate_unref", s_null},
    {"udev_enumerate_add_match_subsystem", s_zero}, {"udev_enumerate_scan_devices", s_zero},
    {"udev_enumerate_get_list_entry", s_null}, {"udev_list_entry_get_next", s_null},
    {"udev_list_entry_get_name", s_null},
    {"udev_device_new_from_syspath", s_null}, {"udev_device_unref", s_null},
    {"udev_device_get_action", s_null}, {"udev_device_get_devnode", s_null},
    {"udev_device_get_sysname", s_null},
    {"udev_monitor_new_from_netlink", s_mon}, {"udev_monitor_unref", s_null},
    {"udev_monitor_filter_add_match_subsystem_devtype", s_zero},
    {"udev_monitor_enable_receiving", s_zero}, {"udev_monitor_get_fd", s_mon_fd},
    {"udev_monitor_receive_device", s_null},
    {"iw_sockets_open", s_neg}, {"iw_get_basic_config", s_neg}, {"iw_get_range_info", s_neg},
    {"iw_get_stats", s_neg}, {"iw_init_event_stream", s_neg}, {"iw_extract_event_stream", s_zero},
    {"iw_freq2float", s_zero}, {"iw_freq_to_channel", s_neg}, {"iw_channel_to_freq", s_neg},
    {"iw_check_mac_addr_type", s_neg}, {"iw_ether_ntop", s_null}, {"iw_sawap_ntop", s_null},
    {"iw_print_bitrate", s_null},
    {NULL, NULL},
};
