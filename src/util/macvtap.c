/*
 * Copyright (C) 2010-2011 Red Hat, Inc.
 * Copyright (C) 2010 IBM Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Authors:
 *     Stefan Berger <stefanb@us.ibm.com>
 *
 * Notes:
 * netlink: http://lovezutto.googlepages.com/netlink.pdf
 *          iproute2 package
 *
 */

#include <config.h>

#include <stdint.h>

#if WITH_MACVTAP || WITH_VIRTUALPORT

# include <stdio.h>
# include <errno.h>
# include <fcntl.h>
# include <c-ctype.h>
# include <sys/socket.h>
# include <sys/ioctl.h>

# include <linux/if.h>
# include <linux/if_tun.h>

/* Older kernels lacked this enum value.  */
# if !HAVE_DECL_MACVLAN_MODE_PASSTHRU
#  define MACVLAN_MODE_PASSTHRU 8
# endif

#endif /* WITH_MACVTAP || WITH_VIRTUALPORT */

#include "util.h"
#include "macvtap.h"
#include "network.h"

VIR_ENUM_IMPL(virMacvtapMode, VIR_MACVTAP_MODE_LAST,
              "vepa",
              "private",
              "bridge",
              "passthrough")

#if WITH_MACVTAP || WITH_VIRTUALPORT

# include "memory.h"
# include "logging.h"
# include "interface.h"
# include "virterror_internal.h"
# include "uuid.h"
# include "virfile.h"
# include "netlink.h"

# define VIR_FROM_THIS VIR_FROM_NET

# define macvtapError(code, ...)                                           \
        virReportErrorHelper(VIR_FROM_NET, code, __FILE__,                 \
                             __FUNCTION__, __LINE__, __VA_ARGS__)

# define MACVTAP_NAME_PREFIX	"macvtap"
# define MACVTAP_NAME_PATTERN	"macvtap%d"

# define MICROSEC_PER_SEC       (1000 * 1000)

# define NLMSGBUF_SIZE  256
# define RATTBUF_SIZE   64

# define STATUS_POLL_TIMEOUT_USEC (10 * MICROSEC_PER_SEC)
# define STATUS_POLL_INTERVL_USEC (MICROSEC_PER_SEC / 8)


# define LLDPAD_PID_FILE  "/var/run/lldpad.pid"


enum virVirtualPortOp {
    ASSOCIATE = 0x1,
    DISASSOCIATE = 0x2,
    PREASSOCIATE = 0x3,
    PREASSOCIATE_RR = 0x4,
};




# if WITH_MACVTAP

/* Open the macvtap's tap device.
 * @ifname: Name of the macvtap interface
 * @retries : Number of retries in case udev for example may need to be
 *            waited for to create the tap chardev
 * Returns negative value in case of error, the file descriptor otherwise.
 */
static
int openTap(const char *ifname,
            int retries)
{
    FILE *file;
    char path[64];
    int ifindex;
    char tapname[50];
    int tapfd;

    if (snprintf(path, sizeof(path),
                 "/sys/class/net/%s/ifindex", ifname) >= sizeof(path)) {
        virReportSystemError(errno,
                             "%s",
                             _("buffer for ifindex path is too small"));
        return -1;
    }

    file = fopen(path, "r");

    if (!file) {
        virReportSystemError(errno,
                             _("cannot open macvtap file %s to determine "
                               "interface index"), path);
        return -1;
    }

    if (fscanf(file, "%d", &ifindex) != 1) {
        virReportSystemError(errno,
                             "%s",_("cannot determine macvtap's tap device "
                             "interface index"));
        VIR_FORCE_FCLOSE(file);
        return -1;
    }

    VIR_FORCE_FCLOSE(file);

    if (snprintf(tapname, sizeof(tapname),
                 "/dev/tap%d", ifindex) >= sizeof(tapname)) {
        virReportSystemError(errno,
                             "%s",
                             _("internal buffer for tap device is too small"));
        return -1;
    }

    while (1) {
        /* may need to wait for udev to be done */
        tapfd = open(tapname, O_RDWR);
        if (tapfd < 0 && retries > 0) {
            retries--;
            usleep(20000);
            continue;
        }
        break;
    }

    if (tapfd < 0)
        virReportSystemError(errno,
                             _("cannot open macvtap tap device %s"),
                             tapname);

    return tapfd;
}


/**
 * configMacvtapTap:
 * @tapfd: file descriptor of the macvtap tap
 * @vnet_hdr: 1 to enable IFF_VNET_HDR, 0 to disable it
 *
 * Returns 0 on success, -1 in case of fatal error, error code otherwise.
 *
 * Turn the IFF_VNET_HDR flag, if requested and available, make sure
 * it's off in the other cases.
 * A fatal error is defined as the VNET_HDR flag being set but it cannot
 * be turned off for some reason. This is reported with -1. Other fatal
 * error is not being able to read the interface flags. In that case the
 * macvtap device should not be used.
 */
static int
configMacvtapTap(int tapfd, int vnet_hdr)
{
    unsigned int features;
    struct ifreq ifreq;
    short new_flags = 0;
    int rc_on_fail = 0;
    const char *errmsg = NULL;

    memset(&ifreq, 0, sizeof(ifreq));

    if (ioctl(tapfd, TUNGETIFF, &ifreq) < 0) {
        virReportSystemError(errno, "%s",
                             _("cannot get interface flags on macvtap tap"));
        return -1;
    }

    new_flags = ifreq.ifr_flags;

    if ((ifreq.ifr_flags & IFF_VNET_HDR) && !vnet_hdr) {
        new_flags = ifreq.ifr_flags & ~IFF_VNET_HDR;
        rc_on_fail = -1;
        errmsg = _("cannot clean IFF_VNET_HDR flag on macvtap tap");
    } else if ((ifreq.ifr_flags & IFF_VNET_HDR) == 0 && vnet_hdr) {
        if (ioctl(tapfd, TUNGETFEATURES, &features) != 0)
            return errno;
        if ((features & IFF_VNET_HDR)) {
            new_flags = ifreq.ifr_flags | IFF_VNET_HDR;
            errmsg = _("cannot set IFF_VNET_HDR flag on macvtap tap");
        }
    }

    if (new_flags != ifreq.ifr_flags) {
        ifreq.ifr_flags = new_flags;
        if (ioctl(tapfd, TUNSETIFF, &ifreq) < 0) {
            virReportSystemError(errno, "%s", errmsg);
            return rc_on_fail;
        }
    }

    return 0;
}


static const uint32_t modeMap[VIR_MACVTAP_MODE_LAST] = {
    [VIR_MACVTAP_MODE_VEPA] = MACVLAN_MODE_VEPA,
    [VIR_MACVTAP_MODE_PRIVATE] = MACVLAN_MODE_PRIVATE,
    [VIR_MACVTAP_MODE_BRIDGE] = MACVLAN_MODE_BRIDGE,
    [VIR_MACVTAP_MODE_PASSTHRU] = MACVLAN_MODE_PASSTHRU,
};

/**
 * openMacvtapTap:
 * Create an instance of a macvtap device and open its tap character
 * device.
 * @tgifname: Interface name that the macvtap is supposed to have. May
 *    be NULL if this function is supposed to choose a name
 * @macaddress: The MAC address for the macvtap device
 * @linkdev: The interface name of the NIC to connect to the external bridge
 * @mode: int describing the mode for 'bridge', 'vepa', 'private' or 'passthru'.
 * @vnet_hdr: 1 to enable IFF_VNET_HDR, 0 to disable it
 * @vmuuid: The UUID of the VM the macvtap belongs to
 * @virtPortProfile: pointer to object holding the virtual port profile data
 * @res_ifname: Pointer to a string pointer where the actual name of the
 *     interface will be stored into if everything succeeded. It is up
 *     to the caller to free the string.
 *
 * Returns file descriptor of the tap device in case of success,
 * negative value otherwise with error reported.
 *
 */
int
openMacvtapTap(const char *tgifname,
               const unsigned char *macaddress,
               const char *linkdev,
               enum virMacvtapMode mode,
               int vnet_hdr,
               const unsigned char *vmuuid,
               virVirtualPortProfileParamsPtr virtPortProfile,
               char **res_ifname,
               enum virVMOperationType vmOp,
               char *stateDir,
               virBandwidthPtr bandwidth)
{
    const char *type = "macvtap";
    int c, rc;
    char ifname[IFNAMSIZ];
    int retries, do_retry = 0;
    uint32_t macvtapMode;
    const char *cr_ifname;
    int ifindex;

    macvtapMode = modeMap[mode];

    *res_ifname = NULL;

    VIR_DEBUG("%s: VM OPERATION: %s", __FUNCTION__, virVMOperationTypeToString(vmOp));

    /** Note: When using PASSTHROUGH mode with MACVTAP devices the link
     * device's MAC address must be set to the VMs MAC address. In
     * order to not confuse the first switch or bridge in line this MAC
     * address must be reset when the VM is shut down.
     * This is especially important when using SRIOV capable cards that
     * emulate their switch in firmware.
     */
    if (mode == VIR_MACVTAP_MODE_PASSTHRU) {
        if (ifaceReplaceMacAddress(macaddress, linkdev, stateDir) != 0) {
            return -1;
        }
    }

    if (tgifname) {
        if(ifaceGetIndex(false, tgifname, &ifindex) == 0) {
            if (STRPREFIX(tgifname,
                          MACVTAP_NAME_PREFIX)) {
                goto create_name;
            }
            virReportSystemError(errno,
                                 _("Interface %s already exists"), tgifname);
            return -1;
        }
        cr_ifname = tgifname;
        rc = ifaceMacvtapLinkAdd(type, macaddress, 6, tgifname, linkdev,
                                 macvtapMode, &do_retry);
        if (rc < 0)
            return -1;
    } else {
create_name:
        retries = 5;
        for (c = 0; c < 8192; c++) {
            snprintf(ifname, sizeof(ifname), MACVTAP_NAME_PATTERN, c);
            if (ifaceGetIndex(false, ifname, &ifindex) == -ENODEV) {
                rc = ifaceMacvtapLinkAdd(type, macaddress, 6, ifname, linkdev,
                                         macvtapMode, &do_retry);
                if (rc == 0)
                    break;

                if (do_retry && --retries)
                    continue;
                return -1;
            }
        }
        cr_ifname = ifname;
    }

    if (vpAssociatePortProfileId(cr_ifname,
                                 macaddress,
                                 linkdev,
                                 virtPortProfile,
                                 vmuuid, vmOp) != 0) {
        rc = -1;
        goto link_del_exit;
    }

    rc = ifaceUp(cr_ifname);
    if (rc < 0) {
        virReportSystemError(errno,
                             _("cannot 'up' interface %s -- another "
                             "macvtap device may be 'up' and have the same "
                             "MAC address"),
                             cr_ifname);
        rc = -1;
        goto disassociate_exit;
    }

    rc = openTap(cr_ifname, 10);

    if (rc >= 0) {
        if (configMacvtapTap(rc, vnet_hdr) < 0) {
            VIR_FORCE_CLOSE(rc); /* sets rc to -1 */
            goto disassociate_exit;
        }
        *res_ifname = strdup(cr_ifname);
    } else
        goto disassociate_exit;

    if (virBandwidthEnable(bandwidth, cr_ifname) < 0) {
        macvtapError(VIR_ERR_INTERNAL_ERROR,
                     _("cannot set bandwidth limits on %s"),
                     cr_ifname);
        rc = -1;
        goto disassociate_exit;
    }


    return rc;

disassociate_exit:
    vpDisassociatePortProfileId(cr_ifname,
                                macaddress,
                                linkdev,
                                virtPortProfile,
                                vmOp);

link_del_exit:
    ifaceLinkDel(cr_ifname);

    return rc;
}


/**
 * delMacvtap:
 * @ifname : The name of the macvtap interface
 * @linkdev: The interface name of the NIC to connect to the external bridge
 * @virtPortProfile: pointer to object holding the virtual port profile data
 *
 * Delete an interface given its name. Disassociate
 * it with the switch if port profile parameters
 * were provided.
 */
void
delMacvtap(const char *ifname,
           const unsigned char *macaddr,
           const char *linkdev,
           int mode,
           virVirtualPortProfileParamsPtr virtPortProfile,
           char *stateDir)
{
    if (mode == VIR_MACVTAP_MODE_PASSTHRU) {
        ifaceRestoreMacAddress(linkdev, stateDir);
    }

    if (ifname) {
        vpDisassociatePortProfileId(ifname, macaddr,
                                    linkdev,
                                    virtPortProfile,
                                    VIR_VM_OP_DESTROY);
        ifaceLinkDel(ifname);
    }
}

# endif /* WITH_MACVTAP */

# ifdef IFLA_PORT_MAX

static struct nla_policy ifla_port_policy[IFLA_PORT_MAX + 1] =
{
  [IFLA_PORT_RESPONSE]      = { .type = NLA_U16 },
};


static uint32_t
getLldpadPid(void) {
    int fd;
    uint32_t pid = 0;

    fd = open(LLDPAD_PID_FILE, O_RDONLY);
    if (fd >= 0) {
        char buffer[10];

        if (saferead(fd, buffer, sizeof(buffer)) <= sizeof(buffer)) {
            unsigned int res;
            char *endptr;

            if (virStrToLong_ui(buffer, &endptr, 10, &res) == 0
                && (*endptr == '\0' || c_isspace(*endptr))
                && res != 0) {
                pid = res;
            } else {
                macvtapError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("error parsing pid of lldpad"));
            }
        }
    } else {
        virReportSystemError(errno,
                             _("Error opening file %s"), LLDPAD_PID_FILE);
    }

    VIR_FORCE_CLOSE(fd);

    return pid;
}


/**
 * getPortProfileStatus
 *
 * tb: top level netlink response attributes + values
 * vf: The virtual function used in the request
 * instanceId: instanceId of the interface (vm uuid in case of 802.1Qbh)
 * is8021Qbg: whether this function is call for 8021Qbg
 * status: pointer to a uint16 where the status will be written into
 *
 * Get the status from the IFLA_PORT_RESPONSE field; Returns 0 in
 * case of success, != 0 otherwise with error having been reported
 */
static int
getPortProfileStatus(struct nlattr **tb, int32_t vf,
                     const unsigned char *instanceId,
                     bool nltarget_kernel,
                     bool is8021Qbg,
                     uint16_t *status)
{
    int rc = 1;
    const char *msg = NULL;
    struct nlattr *tb_port[IFLA_PORT_MAX + 1] = { NULL, };

    if (vf == PORT_SELF_VF && nltarget_kernel) {
        if (tb[IFLA_PORT_SELF]) {
            if (nla_parse_nested(tb_port, IFLA_PORT_MAX, tb[IFLA_PORT_SELF],
                                 ifla_port_policy)) {
                msg = _("error parsing IFLA_PORT_SELF part");
                goto err_exit;
            }
        } else {
            msg = _("IFLA_PORT_SELF is missing");
            goto err_exit;
        }
    } else {
        if (tb[IFLA_VF_PORTS]) {
            int rem;
            bool found = false;
            struct nlattr *tb_vf_ports = { NULL, };

            nla_for_each_nested(tb_vf_ports, tb[IFLA_VF_PORTS], rem) {

                if (nla_type(tb_vf_ports) != IFLA_VF_PORT) {
                    msg = _("error while iterating over IFLA_VF_PORTS part");
                    goto err_exit;
                }

                if (nla_parse_nested(tb_port, IFLA_PORT_MAX, tb_vf_ports,
                                     ifla_port_policy)) {
                    msg = _("error parsing IFLA_VF_PORT part");
                    goto err_exit;
                }

                if (instanceId &&
                    tb_port[IFLA_PORT_INSTANCE_UUID] &&
                    !memcmp(instanceId,
                            (unsigned char *)
                                   RTA_DATA(tb_port[IFLA_PORT_INSTANCE_UUID]),
                            VIR_UUID_BUFLEN) &&
                    tb_port[IFLA_PORT_VF] &&
                    vf == *(uint32_t *)RTA_DATA(tb_port[IFLA_PORT_VF])) {
                        found = true;
                        break;
                }
            }

            if (!found) {
                msg = _("Could not find netlink response with "
                        "expected parameters");
                goto err_exit;
            }
        } else {
            msg = _("IFLA_VF_PORTS is missing");
            goto err_exit;
        }
    }

    if (tb_port[IFLA_PORT_RESPONSE]) {
        *status = *(uint16_t *)RTA_DATA(tb_port[IFLA_PORT_RESPONSE]);
        rc = 0;
    } else {
        if (is8021Qbg) {
            /* no in-progress here; may be missing */
            *status = PORT_PROFILE_RESPONSE_INPROGRESS;
            rc = 0;
        } else {
            msg = _("no IFLA_PORT_RESPONSE found in netlink message");
            goto err_exit;
        }
    }

err_exit:
    if (msg)
        macvtapError(VIR_ERR_INTERNAL_ERROR, "%s", msg);

    return rc;
}


static int
doPortProfileOpSetLink(bool nltarget_kernel,
                       const char *ifname, int ifindex,
                       const unsigned char *macaddr,
                       int vlanid,
                       const char *profileId,
                       struct ifla_port_vsi *portVsi,
                       const unsigned char *instanceId,
                       const unsigned char *hostUUID,
                       int32_t vf,
                       uint8_t op)
{
    int rc = 0;
    struct nlmsghdr *resp;
    struct nlmsgerr *err;
    struct ifinfomsg ifinfo = {
        .ifi_family = AF_UNSPEC,
        .ifi_index  = ifindex,
    };
    unsigned char *recvbuf = NULL;
    unsigned int recvbuflen = 0;
    uint32_t pid = 0;
    struct nl_msg *nl_msg;
    struct nlattr *vfports = NULL, *vfport;

    nl_msg = nlmsg_alloc_simple(RTM_SETLINK, NLM_F_REQUEST);
    if (!nl_msg) {
        virReportOOMError();
        return -1;
    }

    if (nlmsg_append(nl_msg,  &ifinfo, sizeof(ifinfo), NLMSG_ALIGNTO) < 0)
        goto buffer_too_small;

    if (ifname &&
        nla_put(nl_msg, IFLA_IFNAME, strlen(ifname)+1, ifname) < 0)
        goto buffer_too_small;

    if (macaddr || vlanid >= 0) {
        struct nlattr *vfinfolist, *vfinfo;

        if (!(vfinfolist = nla_nest_start(nl_msg, IFLA_VFINFO_LIST)))
            goto buffer_too_small;

        if (!(vfinfo = nla_nest_start(nl_msg, IFLA_VF_INFO)))
            goto buffer_too_small;

        if (macaddr) {
            struct ifla_vf_mac ifla_vf_mac = {
                .vf = vf,
                .mac = { 0, },
            };

            memcpy(ifla_vf_mac.mac, macaddr, 6);

            if (nla_put(nl_msg, IFLA_VF_MAC, sizeof(ifla_vf_mac),
                        &ifla_vf_mac) < 0)
                goto buffer_too_small;
        }

        if (vlanid >= 0) {
            struct ifla_vf_vlan ifla_vf_vlan = {
                .vf = vf,
                .vlan = vlanid,
                .qos = 0,
            };

            if (nla_put(nl_msg, IFLA_VF_VLAN, sizeof(ifla_vf_vlan),
                        &ifla_vf_vlan) < 0)
                goto buffer_too_small;
        }

        nla_nest_end(nl_msg, vfinfo);
        nla_nest_end(nl_msg, vfinfolist);
    }

    if (vf == PORT_SELF_VF && nltarget_kernel) {
        if (!(vfport = nla_nest_start(nl_msg, IFLA_PORT_SELF)))
            goto buffer_too_small;
    } else {
        if (!(vfports = nla_nest_start(nl_msg, IFLA_VF_PORTS)))
            goto buffer_too_small;

        /* begin nesting vfports */
        if (!(vfport = nla_nest_start(nl_msg, IFLA_VF_PORT)))
            goto buffer_too_small;
    }

    if (profileId) {
        if (nla_put(nl_msg, IFLA_PORT_PROFILE, strlen(profileId) + 1,
                    profileId) < 0)
            goto buffer_too_small;
    }

    if (portVsi) {
        if (nla_put(nl_msg, IFLA_PORT_VSI_TYPE, sizeof(*portVsi),
                    portVsi) < 0)
            goto buffer_too_small;
    }

    if (instanceId) {
        if (nla_put(nl_msg, IFLA_PORT_INSTANCE_UUID, VIR_UUID_BUFLEN,
                    instanceId) < 0)
            goto buffer_too_small;
    }

    if (hostUUID) {
        if (nla_put(nl_msg, IFLA_PORT_HOST_UUID, VIR_UUID_BUFLEN,
                    hostUUID) < 0)
            goto buffer_too_small;
    }

    if (vf != PORT_SELF_VF) {
        if (nla_put(nl_msg, IFLA_PORT_VF, sizeof(vf), &vf) < 0)
            goto buffer_too_small;
    }

    if (nla_put(nl_msg, IFLA_PORT_REQUEST, sizeof(op), &op) < 0)
        goto buffer_too_small;

    /* end nesting of vport */
    nla_nest_end(nl_msg, vfport);

    if (vfports) {
        /* end nesting of vfports */
        nla_nest_end(nl_msg, vfports);
    }

    if (!nltarget_kernel) {
        pid = getLldpadPid();
        if (pid == 0) {
            rc = -1;
            goto err_exit;
        }
    }

    if (nlComm(nl_msg, &recvbuf, &recvbuflen, pid) < 0) {
        rc = -1;
        goto err_exit;
    }

    if (recvbuflen < NLMSG_LENGTH(0) || recvbuf == NULL)
        goto malformed_resp;

    resp = (struct nlmsghdr *)recvbuf;

    switch (resp->nlmsg_type) {
    case NLMSG_ERROR:
        err = (struct nlmsgerr *)NLMSG_DATA(resp);
        if (resp->nlmsg_len < NLMSG_LENGTH(sizeof(*err)))
            goto malformed_resp;

        if (err->error) {
            virReportSystemError(-err->error,
                _("error during virtual port configuration of ifindex %d"),
                ifindex);
            rc = -1;
        }
        break;

    case NLMSG_DONE:
        break;

    default:
        goto malformed_resp;
    }

err_exit:
    nlmsg_free(nl_msg);

    VIR_FREE(recvbuf);

    return rc;

malformed_resp:
    nlmsg_free(nl_msg);

    macvtapError(VIR_ERR_INTERNAL_ERROR, "%s",
                 _("malformed netlink response message"));
    VIR_FREE(recvbuf);
    return -1;

buffer_too_small:
    nlmsg_free(nl_msg);

    macvtapError(VIR_ERR_INTERNAL_ERROR, "%s",
                 _("allocated netlink buffer is too small"));
    return -1;
}


static int
doPortProfileOpCommon(bool nltarget_kernel,
                      const char *ifname, int ifindex,
                      const unsigned char *macaddr,
                      int vlanid,
                      const char *profileId,
                      struct ifla_port_vsi *portVsi,
                      const unsigned char *instanceId,
                      const unsigned char *hostUUID,
                      int32_t vf,
                      uint8_t op)
{
    int rc;
    unsigned char *recvbuf = NULL;
    struct nlattr *tb[IFLA_MAX + 1] = { NULL , };
    int repeats = STATUS_POLL_TIMEOUT_USEC / STATUS_POLL_INTERVL_USEC;
    uint16_t status = 0;
    bool is8021Qbg = (profileId == NULL);

    rc = doPortProfileOpSetLink(nltarget_kernel,
                                ifname, ifindex,
                                macaddr,
                                vlanid,
                                profileId,
                                portVsi,
                                instanceId,
                                hostUUID,
                                vf,
                                op);

    if (rc) {
        macvtapError(VIR_ERR_INTERNAL_ERROR, "%s",
                     _("sending of PortProfileRequest failed."));
        return rc;
    }

    while (--repeats >= 0) {
        rc = ifaceMacvtapLinkDump(nltarget_kernel, NULL, ifindex, tb,
                                  &recvbuf, getLldpadPid);
        if (rc)
            goto err_exit;
        rc = getPortProfileStatus(tb, vf, instanceId, nltarget_kernel,
                                  is8021Qbg, &status);
        if (rc)
            goto err_exit;
        if (status == PORT_PROFILE_RESPONSE_SUCCESS ||
            status == PORT_VDP_RESPONSE_SUCCESS) {
            break;
        } else if (status == PORT_PROFILE_RESPONSE_INPROGRESS) {
            /* keep trying... */
        } else {
            virReportSystemError(EINVAL,
                    _("error %d during port-profile setlink on "
                      "interface %s (%d)"),
                    status, ifname, ifindex);
            rc = 1;
            break;
        }

        usleep(STATUS_POLL_INTERVL_USEC);

        VIR_FREE(recvbuf);
    }

    if (status == PORT_PROFILE_RESPONSE_INPROGRESS) {
        macvtapError(VIR_ERR_INTERNAL_ERROR, "%s",
                     _("port-profile setlink timed out"));
        rc = -ETIMEDOUT;
    }

err_exit:
    VIR_FREE(recvbuf);

    return rc;
}

# endif /* IFLA_PORT_MAX */


# ifdef IFLA_VF_PORT_MAX

static int
getPhysdevAndVlan(const char *ifname, int *root_ifindex, char *root_ifname,
                  int *vlanid)
{
    int ret;
    unsigned int nth;
    int ifindex = -1;

    *vlanid = -1;
    while (1) {
        if ((ret = ifaceGetNthParent(ifindex, ifname, 1,
                                     root_ifindex, root_ifname, &nth)))
            return ret;
        if (nth == 0)
            break;
        if (*vlanid == -1) {
            if (ifaceGetVlanID(root_ifname, vlanid) < 0)
                *vlanid = -1;
        }

        ifindex = *root_ifindex;
        ifname = NULL;
    }

    return 0;
}

# endif

static int
doPortProfileOp8021Qbg(const char *ifname,
                       const unsigned char *macaddr,
                       const virVirtualPortProfileParamsPtr virtPort,
                       enum virVirtualPortOp virtPortOp)
{
    int rc;

# ifndef IFLA_VF_PORT_MAX

    (void)ifname;
    (void)macaddr;
    (void)virtPort;
    (void)virtPortOp;
    macvtapError(VIR_ERR_INTERNAL_ERROR, "%s",
                 _("Kernel VF Port support was missing at compile time."));
    rc = 1;

# else /* IFLA_VF_PORT_MAX */

    int op = PORT_REQUEST_ASSOCIATE;
    struct ifla_port_vsi portVsi = {
        .vsi_mgr_id       = virtPort->u.virtPort8021Qbg.managerID,
        .vsi_type_version = virtPort->u.virtPort8021Qbg.typeIDVersion,
    };
    bool nltarget_kernel = false;
    int vlanid;
    int physdev_ifindex = 0;
    char physdev_ifname[IFNAMSIZ] = { 0, };
    int vf = PORT_SELF_VF;

    if (getPhysdevAndVlan(ifname, &physdev_ifindex, physdev_ifname,
                          &vlanid) != 0) {
        rc = 1;
        goto err_exit;
    }

    if (vlanid < 0)
        vlanid = 0;

    portVsi.vsi_type_id[2] = virtPort->u.virtPort8021Qbg.typeID >> 16;
    portVsi.vsi_type_id[1] = virtPort->u.virtPort8021Qbg.typeID >> 8;
    portVsi.vsi_type_id[0] = virtPort->u.virtPort8021Qbg.typeID;

    switch (virtPortOp) {
    case PREASSOCIATE:
        op = PORT_REQUEST_PREASSOCIATE;
        break;
    case ASSOCIATE:
        op = PORT_REQUEST_ASSOCIATE;
        break;
    case DISASSOCIATE:
        op = PORT_REQUEST_DISASSOCIATE;
        break;
    default:
        macvtapError(VIR_ERR_INTERNAL_ERROR,
                     _("operation type %d not supported"), virtPortOp);
        rc = 1;
        goto err_exit;
    }

    rc = doPortProfileOpCommon(nltarget_kernel,
                               physdev_ifname, physdev_ifindex,
                               macaddr,
                               vlanid,
                               NULL,
                               &portVsi,
                               virtPort->u.virtPort8021Qbg.instanceID,
                               NULL,
                               vf,
                               op);

err_exit:

# endif /* IFLA_VF_PORT_MAX */

    return rc;
}


# ifdef IFLA_VF_PORT_MAX
static int
getPhysfnDev(const char *linkdev,
             int32_t *vf,
             char **physfndev)
{
    int rc = 0;

    if (ifaceIsVirtualFunction(linkdev)) {

        /* if linkdev is SR-IOV VF, then set vf = VF index */
        /* and set linkdev = PF device */

        rc = ifaceGetPhysicalFunction(linkdev, physfndev);
        if (!rc)
            rc = ifaceGetVirtualFunctionIndex(*physfndev, linkdev, vf);
    } else {

        /* Not SR-IOV VF: physfndev is linkdev and VF index
         * refers to linkdev self
         */

        *vf = PORT_SELF_VF;
        *physfndev = strdup(linkdev);
        if (!*physfndev) {
            virReportOOMError();
            rc = -1;
        }
    }

    return rc;
}
# endif /* IFLA_VF_PORT_MAX */

static int
doPortProfileOp8021Qbh(const char *ifname,
                       const unsigned char *macaddr,
                       const virVirtualPortProfileParamsPtr virtPort,
                       const unsigned char *vm_uuid,
                       enum virVirtualPortOp virtPortOp)
{
    int rc;

# ifndef IFLA_VF_PORT_MAX

    (void)ifname;
    (void)macaddr;
    (void)virtPort;
    (void)vm_uuid;
    (void)virtPortOp;
    macvtapError(VIR_ERR_INTERNAL_ERROR, "%s",
                 _("Kernel VF Port support was missing at compile time."));
    rc = 1;

# else /* IFLA_VF_PORT_MAX */

    char *physfndev = NULL;
    unsigned char hostuuid[VIR_UUID_BUFLEN];
    int32_t vf;
    bool nltarget_kernel = true;
    int ifindex;
    int vlanid = -1;

    rc = getPhysfnDev(ifname, &vf, &physfndev);
    if (rc)
        goto err_exit;

    if (ifaceGetIndex(true, physfndev, &ifindex) < 0) {
        rc = 1;
        goto err_exit;
    }

    switch (virtPortOp) {
    case PREASSOCIATE_RR:
    case ASSOCIATE:
        rc = virGetHostUUID(hostuuid);
        if (rc)
            goto err_exit;

        rc = doPortProfileOpCommon(nltarget_kernel, NULL, ifindex,
                                   macaddr,
                                   vlanid,
                                   virtPort->u.virtPort8021Qbh.profileID,
                                   NULL,
                                   vm_uuid,
                                   hostuuid,
                                   vf,
                                   (virtPortOp == PREASSOCIATE_RR) ?
                                    PORT_REQUEST_PREASSOCIATE_RR
                                    : PORT_REQUEST_ASSOCIATE);
        if (rc == -ETIMEDOUT)
            /* Association timed out, disassociate */
            doPortProfileOpCommon(nltarget_kernel, NULL, ifindex,
                                  NULL,
                                  vlanid,
                                  NULL,
                                  NULL,
                                  NULL,
                                  NULL,
                                  vf,
                                  PORT_REQUEST_DISASSOCIATE);
        break;

    case DISASSOCIATE:
        rc = doPortProfileOpCommon(nltarget_kernel, NULL, ifindex,
                                   NULL,
                                   vlanid,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   vf,
                                   PORT_REQUEST_DISASSOCIATE);
        break;

    default:
        macvtapError(VIR_ERR_INTERNAL_ERROR,
                     _("operation type %d not supported"), virtPortOp);
        rc = 1;
    }

err_exit:
    VIR_FREE(physfndev);

# endif /* IFLA_VF_PORT_MAX */

    return rc;
}

/**
 * vpAssociatePortProfile
 *
 * @macvtap_ifname: The name of the macvtap device
 * @virtPort: pointer to the object holding port profile parameters
 * @vmuuid : the UUID of the virtual machine
 * @vmOp : The VM operation (i.e., create, no-op)
 *
 * Associate a port on a swtich with a profile. This function
 * may notify a kernel driver or an external daemon to run
 * the setup protocol. If profile parameters were not supplied
 * by the user, then this function returns without doing
 * anything.
 *
 * Returns 0 in case of success, != 0 otherwise with error
 * having been reported.
 */
int
vpAssociatePortProfileId(const char *macvtap_ifname,
                         const unsigned char *macvtap_macaddr,
                         const char *linkdev,
                         const virVirtualPortProfileParamsPtr virtPort,
                         const unsigned char *vmuuid,
                         enum virVMOperationType vmOp)
{
    int rc = 0;

    VIR_DEBUG("Associating port profile '%p' on link device '%s'",
              virtPort, macvtap_ifname);

    VIR_DEBUG("%s: VM OPERATION: %s", __FUNCTION__, virVMOperationTypeToString(vmOp));

    if (!virtPort || vmOp == VIR_VM_OP_NO_OP)
        return 0;

    switch (virtPort->virtPortType) {
    case VIR_VIRTUALPORT_NONE:
    case VIR_VIRTUALPORT_TYPE_LAST:
        break;

    case VIR_VIRTUALPORT_8021QBG:
        rc = doPortProfileOp8021Qbg(macvtap_ifname, macvtap_macaddr,
                                    virtPort,
                                    (vmOp == VIR_VM_OP_MIGRATE_IN_START)
                                      ? PREASSOCIATE
                                      : ASSOCIATE);
        break;

    case VIR_VIRTUALPORT_8021QBH:
        rc = doPortProfileOp8021Qbh(linkdev, macvtap_macaddr,
                                    virtPort, vmuuid,
                                    (vmOp == VIR_VM_OP_MIGRATE_IN_START)
                                      ? PREASSOCIATE_RR
                                      : ASSOCIATE);
        if (vmOp != VIR_VM_OP_MIGRATE_IN_START && !rc)
            ifaceUp(linkdev);
        break;
    }

    return rc;
}


/**
 * vpDisassociatePortProfile
 *
 * @macvtap_ifname: The name of the macvtap device
 * @macvtap_macaddr : The MAC address of the macvtap
 * @linkdev: The link device in case of macvtap
 * @virtPort: point to object holding port profile parameters
 *
 * Returns 0 in case of success, != 0 otherwise with error
 * having been reported.
 */
int
vpDisassociatePortProfileId(const char *macvtap_ifname,
                            const unsigned char *macvtap_macaddr,
                            const char *linkdev,
                            const virVirtualPortProfileParamsPtr virtPort,
                            enum virVMOperationType vmOp)
{
    int rc = 0;

    VIR_DEBUG("Disassociating port profile id '%p' on link device '%s' ",
              virtPort, macvtap_ifname);

    VIR_DEBUG("%s: VM OPERATION: %s", __FUNCTION__, virVMOperationTypeToString(vmOp));

    if (!virtPort)
       return 0;

    switch (virtPort->virtPortType) {
    case VIR_VIRTUALPORT_NONE:
    case VIR_VIRTUALPORT_TYPE_LAST:
        break;

    case VIR_VIRTUALPORT_8021QBG:
        rc = doPortProfileOp8021Qbg(macvtap_ifname, macvtap_macaddr,
                                    virtPort, DISASSOCIATE);
        break;

    case VIR_VIRTUALPORT_8021QBH:
        /* avoid disassociating twice */
        if (vmOp == VIR_VM_OP_MIGRATE_IN_FINISH)
            break;
        ifaceDown(linkdev);
        rc = doPortProfileOp8021Qbh(linkdev, macvtap_macaddr,
                                    virtPort, NULL, DISASSOCIATE);
        break;
    }

    return rc;
}

#endif /* WITH_MACVTAP || WITH_VIRTUALPORT */

VIR_ENUM_IMPL(virVMOperation, VIR_VM_OP_LAST,
    "create",
    "save",
    "restore",
    "destroy",
    "migrate out",
    "migrate in start",
    "migrate in finish",
    "no-op")
