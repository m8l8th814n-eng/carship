#include "module.h"

#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Up/down modules share one shape: a symbol and a style per state, plus
 * $state for formats that want the word. */
static void add_state(module_state *st, bool up, const char *up_symbol, const char *down_symbol,
                      const char *up_style, const char *down_style)
{
	if (up) {
		var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "up_symbol", up_symbol));
		var_addz(&st->vars, "style", toml_str_at(st->cfg, "up_style", up_style));
	} else {
		var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "down_symbol", down_symbol));
		var_addz(&st->vars, "style", toml_str_at(st->cfg, "down_style", down_style));
	}
	var_addz(&st->vars, "state", up ? "up" : "down");
}

/* Kept to two units so the segment stays short: 42m, 5h12m, 3d4h. */
static bool probe_uptime(module_state *st)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
		return false;

	long long m = (long long)ts.tv_sec / 60;
	long long h = m / 60;
	long long d = h / 24;

	if (d)
		var_addf(&st->vars, "uptime", "%lldd%lldh", d, h % 24);
	else if (h)
		var_addf(&st->vars, "uptime", "%lldh%lldm", h, m % 60);
	else
		var_addf(&st->vars, "uptime", "%lldm", m);

	var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "symbol", "\U000f051b"));
	return true;
}

/* tailscaled keeps its interface around after `tailscale down` but drops the
 * addresses, so "up" means the interface is up and has one. Asking the
 * tailscale CLI instead would cost a process on every prompt. */
static bool probe_tailscale(module_state *st)
{
	const char *iface = toml_str_at(st->cfg, "interface", "tailscale0");

	bool present = false, up = false;
	struct ifaddrs *addrs = NULL;
	if (getifaddrs(&addrs) == 0) {
		for (const struct ifaddrs *ifa = addrs; ifa; ifa = ifa->ifa_next) {
			if (!str_eq(ifa->ifa_name, iface))
				continue;
			present = true;
			if ((ifa->ifa_flags & IFF_UP) && ifa->ifa_addr &&
			    (ifa->ifa_addr->sa_family == AF_INET || ifa->ifa_addr->sa_family == AF_INET6))
				up = true;
		}
		freeifaddrs(addrs);
	}

	/* No interface and no daemon installed: nothing to report. */
	if (!present) {
		static const char *const daemons[] = {"/usr/sbin/tailscaled", "/usr/bin/tailscaled",
		                                      "/usr/local/bin/tailscaled", "/sbin/tailscaled"};
		bool installed = false;
		for (size_t i = 0; i < sizeof daemons / sizeof *daemons && !installed; i++)
			installed = access(daemons[i], X_OK) == 0;
		if (!installed)
			return false;
	}

	add_state(st, up, "\U000f0582", "\U000f0582", "bold green", "dimmed red");
	return true;
}

/* From the kernel's <net/bluetooth/hci.h>, which libc does not ship. */
#define CARSHIP_AF_BLUETOOTH 31
#define CARSHIP_BTPROTO_HCI 1
#define CARSHIP_HCIGETDEVINFO _IOR('H', 211, int)
#define CARSHIP_HCI_UP 0x1

/* A controller is up when the kernel has it open, which is what bluez's
 * "Powered: yes" and an rfkill unblock both come down to. One ioctl per
 * adapter; no D-Bus round trip. */
static bool probe_bluetooth(module_state *st)
{
	DIR *dir = opendir("/sys/class/bluetooth");
	if (!dir)
		return false;

	int sock = socket(CARSHIP_AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, CARSHIP_BTPROTO_HCI);
	bool any = false, up = false;

	for (const struct dirent *e; (e = readdir(dir));) {
		if (!has_prefix(e->d_name, "hci"))
			continue;
		any = true;
		if (sock < 0)
			continue;

		/* struct hci_dev_info: u16 dev_id, char name[8], bdaddr[6], then
		 * u32 flags at offset 16. The buffer is larger than the struct so
		 * the kernel's copy of the remainder always fits. */
		union {
			uint32_t align;
			unsigned char bytes[256];
		} info = {0};
		uint16_t id = (uint16_t)atoi(e->d_name + 3);
		memcpy(info.bytes, &id, sizeof id);

		if (ioctl(sock, CARSHIP_HCIGETDEVINFO, info.bytes) == 0) {
			uint32_t flags;
			memcpy(&flags, info.bytes + 16, sizeof flags);
			if (flags & CARSHIP_HCI_UP)
				up = true;
		}
	}

	if (sock >= 0)
		close(sock);
	closedir(dir);

	if (!any)
		return false;

	add_state(st, up, "\U000f00af", "\U000f00b2", "bold blue", "dimmed white");
	return true;
}

const module_def system_modules[] = {
	{.name = "uptime", .format = "[$symbol $uptime]($style) ", .style = "dimmed white",
	 .probe = probe_uptime},
	{.name = "tailscale", .format = "[$symbol]($style) ", .style = "", .probe = probe_tailscale},
	{.name = "bluetooth", .format = "[$symbol]($style) ", .style = "", .probe = probe_bluetooth},
};

const size_t system_modules_count = sizeof system_modules / sizeof *system_modules;
