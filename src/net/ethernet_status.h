#pragma once

/* Wired-link detection via sysfs. Reading /sys/class/net/<if>/carrier is a
 * few microseconds (no nmcli shell-out), so callers may poll it frequently. */

namespace jetson {

// True when any wired NIC (eth* / en*) has link, i.e. a LAN cable is plugged
// in and the switch answers. False with the cable out or the interface down.
bool IsEthernetConnected();

} // namespace jetson
