/*
 *
 * Conky, a system monitor, based on torsmo
 *
 * Any original torsmo code is licensed under the BSD license
 *
 * All code written since the fork of torsmo is licensed under the GPL
 *
 * Please see COPYING for details
 *
 * Copyright (c) 2007 Toni Spets
 * Copyright (c) 2005-2024 Brenden Matthews, Philip Kovacs, et. al.
 *	(see AUTHORS)
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <kvm.h>
#include <sys/ioctl.h>
#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/proc.h>
#include <sys/sensors.h>
#include <sys/sched.h>
#include <sys/socket.h>
#include <sys/swap.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/vmmeter.h>

#include <net/if.h>
#include <net/if_media.h>
#include <netinet/in.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <machine/apmvar.h>
#include <unistd.h>

#include <net80211/ieee80211.h>
#include <net80211/ieee80211_ioctl.h>

#include "../../conky.h"
#include "../hardware/diskio.h"
#include "../../logging.h"
#include "../network/net_stat.h"
#include "openbsd.h"
#include "../../content/temphelper.h"
#include "../top.h"

#define MAXSHOWDEVS 16

#define LOG1024 10
#define pagetok(size) ((size) << pageshift)

inline void proc_find_top(struct process **cpu, struct process **mem);

static kvm_t *kd = 0;

struct ifmibdata *data = nullptr;
size_t len = 0;

static int init_cpu = 0;
static int init_kvm = 0;
static int init_sensors = 0;

struct cpu_load {
  u_int64_t old_used;
  u_int64_t old_total;
};

static struct cpu_load *cpu_loads = nullptr;

static int kvm_init() {
  if (init_kvm) { return 1; }

  kd = kvm_open(nullptr, NULL, NULL, KVM_NO_FILES, NULL);
  if (kd == nullptr) {
    NORM_ERR("error opening kvm");
  } else {
    init_kvm = 1;
  }

  return 1;
}

/* note: swapmode taken from 'top' source */
/* swapmode is rewritten by Tobias Weingartner <weingart@openbsd.org>
 * to be based on the new swapctl(2) system call. */
static int swapmode(int *used, int *total) {
  struct swapent *swdev;
  int nswap, rnswap, i;

  nswap = swapctl(SWAP_NSWAP, 0, 0);
  if (nswap == 0) { return 0; }

  swdev = (struct swapent *)malloc(nswap * sizeof(*swdev));
  if (swdev == nullptr) { return 0; }

  rnswap = swapctl(SWAP_STATS, swdev, nswap);
  if (rnswap == -1) {
    free(swdev);
    return 0;
  }

  /* if rnswap != nswap, then what? */

  /* Total things up */
  *total = *used = 0;
  for (i = 0; i < nswap; i++) {
    if (swdev[i].se_flags & SWF_ENABLE) {
      *used += (swdev[i].se_inuse / (1024 / DEV_BSIZE));
      *total += (swdev[i].se_nblks / (1024 / DEV_BSIZE));
    }
  }
  free(swdev);
  return 1;
}

int check_mount(struct text_object *obj) {
  /* stub */
  (void)obj;
  return 0;
}

int update_uptime() {
  int mib[2] = {CTL_KERN, KERN_BOOTTIME};
  struct timeval boottime;
  time_t now;
  size_t size = sizeof(boottime);

  if ((sysctl(mib, 2, &boottime, &size, nullptr, 0) != -1) &&
      (boottime.tv_sec != 0)) {
    time(&now);
    info.uptime = now - boottime.tv_sec;
  } else {
    NORM_ERR("Could not get uptime");
    info.uptime = 0;
  }

  return 0;
}

int update_meminfo() {
  static int mib[2] = {CTL_VM, VM_METER};
  struct vmtotal vmtotal;
  size_t size;
  int pagesize, pageshift, swap_avail, swap_used;

  pagesize = getpagesize();
  pageshift = 0;
  while (pagesize > 1) {
    pageshift++;
    pagesize >>= 1;
  }

  /* we only need the amount of log(2)1024 for our conversion */
  pageshift -= LOG1024;

  /* get total -- systemwide main memory usage structure */
  size = sizeof(vmtotal);
  if (sysctl(mib, 2, &vmtotal, &size, nullptr, 0) < 0) {
    warn("sysctl failed");
    bzero(&vmtotal, sizeof(vmtotal));
  }

  info.memmax = pagetok(vmtotal.t_rm) + pagetok(vmtotal.t_free);
  info.mem = info.memwithbuffers = pagetok(vmtotal.t_rm);
  info.memeasyfree = info.memfree = info.memmax - info.mem;
  info.legacymem = info.mem;

  if ((swapmode(&swap_used, &swap_avail)) >= 0) {
    info.swapmax = swap_avail;
    info.swap = swap_used;
    info.swapfree = swap_avail - swap_used;
  } else {
    info.swapmax = 0;
    info.swap = 0;
    info.swapfree = 0;
  }

  return 0;
}

int update_net_stats() {
  struct net_stat *ns;
  double delta;
  long long r, t, last_recv, last_trans;
  struct ifaddrs *ifap, *ifa;
  struct if_data *ifd;

  /* get delta */
  delta = current_update_time - last_update_time;
  if (delta <= 0.0001) { return 0; }

  if (getifaddrs(&ifap) < 0) { return 0; }

  for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
    ns = get_net_stat((const char *)ifa->ifa_name, nullptr, NULL);

    if (ifa->ifa_flags & IFF_UP) {
      struct ifaddrs *iftmp;

      ns->up = 1;
      last_recv = ns->recv;
      last_trans = ns->trans;

      if (ifa->ifa_addr->sa_family != AF_LINK) { continue; }

      for (iftmp = ifa->ifa_next;
           iftmp != nullptr && strcmp(ifa->ifa_name, iftmp->ifa_name) == 0;
           iftmp = iftmp->ifa_next) {
        if (iftmp->ifa_addr->sa_family == AF_INET) {
          memcpy(&(ns->addr), iftmp->ifa_addr, iftmp->ifa_addr->sa_len);
        }
      }

      ifd = (struct if_data *)ifa->ifa_data;
      r = ifd->ifi_ibytes;
      t = ifd->ifi_obytes;

      if (r < ns->last_read_recv) {
        ns->recv += ((long long)4294967295U - ns->last_read_recv) + r;
      } else {
        ns->recv += (r - ns->last_read_recv);
      }

      ns->last_read_recv = r;

      if (t < ns->last_read_trans) {
        ns->trans += (long long)4294967295U - ns->last_read_trans + t;
      } else {
        ns->trans += (t - ns->last_read_trans);
      }

      ns->last_read_trans = t;

      /* calculate speeds */
      ns->recv_speed = (ns->recv - last_recv) / delta;
      ns->trans_speed = (ns->trans - last_trans) / delta;
    } else {
      ns->up = 0;
    }
  }

  freeifaddrs(ifap);

  return 0;
}

int update_total_processes() {
  int n_processes;

  kvm_init();
  kvm_getprocs(kd, KERN_PROC_ALL, 0, sizeof(struct kinfo_proc),  &n_processes);

  info.procs = n_processes;

  return 0;
}

int update_running_processes() {
  struct kinfo_proc *p;
  int n_processes;
  int i, cnt = 0;

  kvm_init();
  int max_size = sizeof(struct kinfo_proc);

  p = kvm_getprocs(kd, KERN_PROC_ALL, 0, max_size, &n_processes);
  for (i = 0; i < n_processes; i++) {
    if (p[i].p_stat == SRUN) { cnt++; }
  }

  info.run_procs = cnt;
  return 0;
}

void get_cpu_count() {
  int cpu_count = 0;
  int mib[2] = {CTL_HW, HW_NCPU};
  size_t size = sizeof(cpu_count);

  if (sysctl(mib, 2, &cpu_count, &size, nullptr, 0) != 0) {
    NORM_ERR("unable to get hw.ncpu, defaulting to 1");
    info.cpu_count = 1;
  } else {
    info.cpu_count = cpu_count;
  }

  // [1, 2, ..., N] - CPU0, CPU1, ..., CPUN-1
  info.cpu_usage = (float *)calloc(info.cpu_count + 1, sizeof(float));
  if (info.cpu_usage == nullptr) {
    CRIT_ERR("calloc");
  }

  cpu_loads = (struct cpu_load*)calloc(info.cpu_count + 1, sizeof(struct cpu_load));
  if (cpu_loads == nullptr) {
    CRIT_ERR("calloc");
  }
}

int update_cpu_usage() {
  long cp_time[CPUSTATES];
  int mib_cp_time[2] = {CTL_KERN, KERN_CPTIME};
  u_int64_t cp_time2[CPUSTATES];
  int mib_cp_time2[3] = {CTL_KERN, KERN_CPTIME2, 0};
  size_t size;
  u_int64_t used = 0, total = 0;

  if (init_cpu == 0){
    get_cpu_count();
    init_cpu = 1;
  }

  size = sizeof(cp_time);
  if (sysctl(mib_cp_time, 2, &cp_time, &size, nullptr, 0) != 0) {
      NORM_ERR("unable to get kern.cp_time");
      return 1;
  }

  for (int j = 0; j < CPUSTATES; ++j) {
    total += cp_time[j];
  }
  used = total - cp_time[CP_IDLE];

  if ((total - cpu_loads[0].old_total) != 0) {
    const float diff_used = (float)(used - cpu_loads[0].old_used);
    const float diff_total = (float)(total - cpu_loads[0].old_total);
    info.cpu_usage[0] = diff_used / diff_total;
  } else {
    info.cpu_usage[0] = 0;
  }
  cpu_loads[0].old_used = used;
  cpu_loads[0].old_total = total;

  for (int i = 0; i < info.cpu_count; ++i) {
    mib_cp_time2[2] = i;
    size = sizeof(cp_time2);
    if (sysctl(mib_cp_time2, 3, &cp_time2, &size, nullptr, 0) != 0) {
      NORM_ERR("unable to get kern.cp_time2 for cpu%d", i);
      return 1;
    }

    total = 0;
    used = 0;
    for (int j = 0; j < CPUSTATES; ++j) {
      total += cp_time2[j];
    }
    used = total - cp_time2[CP_IDLE];

    const int n = i + 1; // [0] is the total CPU, must shift by 1
    if ((total - cpu_loads[n].old_total) != 0) {
      const float diff_used = (float)(used - cpu_loads[n].old_used);
      const float diff_total = (float)(total - cpu_loads[n].old_total);
      info.cpu_usage[n] = diff_used / diff_total;
    } else {
      info.cpu_usage[n] = 0;
    }

    cpu_loads[n].old_used = used;
    cpu_loads[n].old_total = total;
  }

  return 0;
}

void free_cpu(struct text_object *) { /* no-op */
}

int update_load_average() {
  double v[3];

  getloadavg(v, 3);

  info.loadavg[0] = (float)v[0];
  info.loadavg[1] = (float)v[1];
  info.loadavg[2] = (float)v[2];

  return 0;
}

#define MAXSENSORDEVICES 128
#define OBSD_MAX_SENSORS 256
static struct obsd_sensors_struct {
  int device;
  float temp[MAXSENSORDEVICES][OBSD_MAX_SENSORS];
  unsigned int fan[MAXSENSORDEVICES][OBSD_MAX_SENSORS];
  float volt[MAXSENSORDEVICES][OBSD_MAX_SENSORS];
} obsd_sensors;

static conky::simple_config_setting<int> sensor_device("sensor_device", 0,
                                                       false);

/* read sensors from sysctl */
int update_obsd_sensors() {
  int sensor_cnt, dev, numt, mib[5] = {CTL_HW, HW_SENSORS, 0, 0, 0};
  struct sensor sensor;
  struct sensordev sensordev;
  size_t slen, sdlen;
  enum sensor_type type;

  slen = sizeof(sensor);
  sdlen = sizeof(sensordev);

  sensor_cnt = 0;

  dev = obsd_sensors.device;  // FIXME: read more than one device

  /* for (dev = 0; dev < MAXSENSORDEVICES; dev++) { */
  mib[2] = dev;
  if (sysctl(mib, 3, &sensordev, &sdlen, nullptr, 0) == -1) {
    if (errno != ENOENT) { warn("sysctl"); }
    return 0;
    // continue;
  }
  for (int t = 0; t < SENSOR_MAX_TYPES; t++) {
    type = (enum sensor_type) t;
    mib[3] = type;
    for (numt = 0; numt < sensordev.maxnumt[type]; numt++) {
      mib[4] = numt;
      if (sysctl(mib, 5, &sensor, &slen, nullptr, 0) == -1) {
        if (errno != ENOENT) { warn("sysctl"); }
        continue;
      }
      if (sensor.flags & SENSOR_FINVALID) { continue; }

      switch (type) {
        case SENSOR_TEMP:
          obsd_sensors.temp[dev][sensor.numt] =
              (sensor.value - 273150000) / 1000000.0;
          break;
        case SENSOR_FANRPM:
          obsd_sensors.fan[dev][sensor.numt] = sensor.value;
          break;
        case SENSOR_VOLTS_DC:
          obsd_sensors.volt[dev][sensor.numt] = sensor.value / 1000000.0;
          break;
        default:
          break;
      }

      sensor_cnt++;
    }
  }
  /* } */

  init_sensors = 1;

  return 0;
}

void parse_obsd_sensor(struct text_object *obj, const char *arg) {
  if (!isdigit((unsigned char)arg[0]) || atoi(&arg[0]) < 0 ||
      atoi(&arg[0]) > OBSD_MAX_SENSORS - 1) {
    obj->data.l = 0;
    NORM_ERR("Invalid sensor number!");
  } else
    obj->data.l = atoi(&arg[0]);
}

void print_obsd_sensors_temp(struct text_object *obj, char *p,
                             unsigned int p_max_size) {
  obsd_sensors.device = sensor_device.get(*state);
  update_obsd_sensors();
  temp_print(p, p_max_size, obsd_sensors.temp[obsd_sensors.device][obj->data.l],
             TEMP_CELSIUS, 1);
}

void print_obsd_sensors_fan(struct text_object *obj, char *p,
                            unsigned int p_max_size) {
  obsd_sensors.device = sensor_device.get(*state);
  update_obsd_sensors();
  snprintf(p, p_max_size, "%d",
           obsd_sensors.fan[obsd_sensors.device][obj->data.l]);
}

void print_obsd_sensors_volt(struct text_object *obj, char *p,
                             unsigned int p_max_size) {
  obsd_sensors.device = sensor_device.get(*state);
  update_obsd_sensors();
  snprintf(p, p_max_size, "%.2f",
           obsd_sensors.volt[obsd_sensors.device][obj->data.l]);
}

/* chipset vendor */
void get_obsd_vendor(struct text_object *obj, char *buf,
                     unsigned int client_buffer_size) {
  int mib[2];
  char vendor[64];
  size_t size = sizeof(vendor);

  (void)obj;

  mib[0] = CTL_HW;
  mib[1] = HW_VENDOR;

  if (sysctl(mib, 2, vendor, &size, nullptr, 0) == -1) {
    NORM_ERR("error reading vendor");
    snprintf(buf, client_buffer_size, "%s", "unknown");
  } else {
    snprintf(buf, client_buffer_size, "%s", vendor);
  }
}

/* chipset name */
void get_obsd_product(struct text_object *obj, char *buf,
                      unsigned int client_buffer_size) {
  int mib[2];
  char product[64];
  size_t size = sizeof(product);

  (void)obj;

  mib[0] = CTL_HW;
  mib[1] = HW_PRODUCT;

  if (sysctl(mib, 2, product, &size, nullptr, 0) == -1) {
    NORM_ERR("error reading product");
    snprintf(buf, client_buffer_size, "%s", "unknown");
  } else {
    snprintf(buf, client_buffer_size, "%s", product);
  }
}

/* void */
char get_freq(char *p_client_buffer, size_t client_buffer_size,
              const char *p_format, int divisor, unsigned int cpu) {
  int freq = cpu;
  int mib[2] = {CTL_HW, HW_CPUSPEED};

  if (!p_client_buffer || client_buffer_size <= 0 || !p_format ||
      divisor <= 0) {
    return 0;
  }

  size_t size = sizeof(freq);

  if (sysctl(mib, 2, &freq, &size, nullptr, 0) == 0) {
    snprintf(p_client_buffer, client_buffer_size, p_format,
             (float)freq / divisor);
  } else {
    snprintf(p_client_buffer, client_buffer_size, p_format, 0.0f);
  }

  return 1;
}

#if 0
/* deprecated, will rewrite this soon in update_net_stats() -hifi */
void update_wifi_stats()
{
	struct net_stat *ns;
	struct ifaddrs *ifap, *ifa;
	struct ifmediareq ifmr;
	struct ieee80211_nodereq nr;
	struct ieee80211_bssid bssid;
	int s, ibssid;

	/* Get iface table */
	if (getifaddrs(&ifap) < 0) {
		return;
	}

	for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
		ns = get_net_stat((const char *) ifa->ifa_name);

		s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

		/* Get media type */
		bzero(&ifmr, sizeof(ifmr));
		strlcpy(ifmr.ifm_name, ifa->ifa_name, IFNAMSIZ);
		if (ioctl(s, SIOCGIFMEDIA, (caddr_t) &ifmr) < 0) {
			close(s);
			return;
		}

		/* We can monitor only wireless interfaces
		 * which are not in hostap mode */
		if ((ifmr.ifm_active & IFM_IEEE80211)
				&& !(ifmr.ifm_active & IFM_IEEE80211_HOSTAP)) {
			/* Get wi status */

			memset(&bssid, 0, sizeof(bssid));
			strlcpy(bssid.i_name, ifa->ifa_name, sizeof(bssid.i_name));
			ibssid = ioctl(s, SIOCG80211BSSID, &bssid);

			bzero(&nr, sizeof(nr));
			bcopy(bssid.i_bssid, &nr.nr_macaddr, sizeof(nr.nr_macaddr));
			strlcpy(nr.nr_ifname, ifa->ifa_name, sizeof(nr.nr_ifname));

			if (ioctl(s, SIOCG80211NODE, &nr) == 0 && nr.nr_rssi) {
				ns->linkstatus = nr.nr_rssi;
			}
		}
cleanup:
		close(s);
	}
}
#endif

int update_diskio() { return 0; /* XXX: implement? hifi: not sure how */ }

// conky uses time in hundredths of seconds (centiseconds)
static unsigned long to_conky_time(u_int32_t sec, u_int32_t usec) {
  return sec * 100 + (unsigned long)(usec * 0.0001);
}

void get_top_info(void) {
  struct kinfo_proc *p;
  struct process *proc;
  int n_processes;
  int i;

  kvm_init();

  p = kvm_getprocs(kd, KERN_PROC_ALL, 0, sizeof(struct kinfo_proc),
                   &n_processes);

  // NOTE(gmb): https://github.com/openbsd/src/blob/master/sys/sys/sysctl.h
  for (i = 0; i < n_processes; i++) {
    if (!((p[i].p_flag & P_SYSTEM)) && p[i].p_comm[0] != 0) {
      proc = get_process(p[i].p_pid);
      if (!proc) continue;

      proc->time_stamp = g_time;
      proc->user_time = to_conky_time(p[i].p_uutime_sec, p[i].p_uutime_usec);
      proc->kernel_time = to_conky_time(p[i].p_ustime_sec, p[i].p_ustime_usec);
      proc->total = proc->user_time + proc->kernel_time;
      proc->uid = p[i].p_uid;
      proc->name = strndup(p[i].p_comm, text_buffer_size.get(*state));
      proc->basename = strndup(p[i].p_comm, text_buffer_size.get(*state));
      proc->amount = 100.0 * p[i].p_pctcpu / FSCALE;
      proc->vsize = p[i].p_vm_map_size;
      proc->rss = (p[i].p_vm_rssize * getpagesize());
      proc->total_cpu_time = to_conky_time(p[i].p_rtime_sec, p[i].p_rtime_usec);
    }
  }
}

void get_battery_short_status(char *buffer, unsigned int n, const char *bat) {
  /* Not implemented */
  (void)bat;
  if (buffer && n > 0) memset(buffer, 0, n);
}

/* empty stubs so conky links */
void prepare_update() {}

int get_entropy_avail(unsigned int *val) { return 1; }

int get_entropy_poolsize(unsigned int *val) { return 1; }
