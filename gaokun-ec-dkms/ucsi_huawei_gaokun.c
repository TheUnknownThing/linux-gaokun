// SPDX-License-Identifier: GPL-2.0-only
/*
 * ucsi-huawei-gaokun - A UCSI driver for HUAWEI Matebook E Go
 *
 * Copyright (C) 2024-2025 Pengyu Luo <mitltlatltl@gmail.com>
 */

#include <drm/bridge/aux-bridge.h>
#include <linux/auxiliary_bus.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/container_of.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include "huawei-gaokun-ec.h"
#include <linux/string.h>
#include <linux/usb/pd_vdo.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/workqueue_types.h>

#include "ucsi.h"

#define EC_EVENT_UCSI	0x21
#define EC_EVENT_USB	0x22

#define GAOKUN_UCSI_REGISTER_DELAY	(3 * HZ)
#define GAOKUN_UCSI_RETRY_DELAY		(10 * HZ)
#define GAOKUN_UCSI_MAX_RETRIES		3

#define GAOKUN_CCX_MASK		GENMASK(1, 0)
#define GAOKUN_MUX_MASK		GENMASK(3, 2)

#define GAOKUN_DPAM_MASK	GENMASK(3, 0)
#define GAOKUN_HPD_STATE_MASK	BIT(4)
#define GAOKUN_HPD_IRQ_MASK	BIT(5)

#define GAOKUN_UCSI_BYTES_PER_PORT	2
#define GAOKUN_UCSI_MAX_PORTS \
	(sizeof(((struct gaokun_ucsi_reg *)0)->port_data) / \
	 GAOKUN_UCSI_BYTES_PER_PORT)

#define CCX_TO_ORI(ccx) (++ccx % 3) /* convert ccx to enum typec_orientation */
#define GAOKUN_TRACE_TS_NS ((unsigned long long)ktime_get_mono_fast_ns())
#define gaokun_ucsi_trace(uec, fmt, ...) \
	dev_dbg((uec)->dev, "trace: " fmt, ##__VA_ARGS__)
#define gaokun_ucsi_trace_rl(uec, fmt, ...) \
	dev_dbg_ratelimited((uec)->dev, "trace: " fmt, ##__VA_ARGS__)

/* Configuration Channel Extension */
enum gaokun_ucsi_ccx {
	USBC_CCX_NORMAL,
	USBC_CCX_REVERSE,
	USBC_CCX_NONE,
};

enum gaokun_ucsi_mux {
	USBC_MUX_NONE,
	USBC_MUX_USB_2L,
	USBC_MUX_DP_4L,
	USBC_MUX_USB_DP,
};

/* based on pmic_glink_altmode_pin_assignment */
enum gaokun_ucsi_dpam_pan {	/* DP Alt Mode Pin Assignments */
	USBC_DPAM_PAN_NONE,
	USBC_DPAM_PAN_A,	/* Not supported after USB Type-C Standard v1.0b */
	USBC_DPAM_PAN_B,	/* Not supported after USB Type-C Standard v1.0b */
	USBC_DPAM_PAN_C,	/* USBC_DPAM_PAN_C_REVERSE - 6 */
	USBC_DPAM_PAN_D,
	USBC_DPAM_PAN_E,
	USBC_DPAM_PAN_F,	/* Not supported after USB Type-C Standard v1.0b */
	USBC_DPAM_PAN_A_REVERSE,/* Not supported after USB Type-C Standard v1.0b */
	USBC_DPAM_PAN_B_REVERSE,/* Not supported after USB Type-C Standard v1.0b */
	USBC_DPAM_PAN_C_REVERSE,
	USBC_DPAM_PAN_D_REVERSE,
	USBC_DPAM_PAN_E_REVERSE,
	USBC_DPAM_PAN_F_REVERSE,/* Not supported after USB Type-C Standard v1.0b */
};

struct gaokun_ucsi_reg {
	u8 num_ports;
	u8 port_updt;
	u8 port_data[4];
	u8 checksum;
	u8 reserved;
} __packed;

struct gaokun_ucsi_port {
	struct completion usb_ack;
	struct delayed_work usb_work;
	spinlock_t lock; /* serializing port resource access */

	struct gaokun_ucsi *ucsi;
	struct auxiliary_device *bridge;

	int idx;
	enum gaokun_ucsi_ccx ccx;
	enum gaokun_ucsi_mux mux;
	u8 mode;
	u16 svid;
	u8 hpd_state;
	u8 hpd_irq;
};

struct gaokun_ucsi {
	struct gaokun_ec *ec;
	struct ucsi *ucsi;
	struct gaokun_ucsi_port *ports;
	struct device *dev;
	struct delayed_work work;
	struct notifier_block nb;
	u16 version;
	u8 num_ports;
	u8 register_retries;
	bool ports_initialized;
	bool notifier_registered;
	bool ucsi_registered;
};

/* -------------------------------------------------------------------------- */
/* For UCSI */

static int gaokun_ucsi_read_version(struct ucsi *ucsi, u16 *version)
{
	struct gaokun_ucsi *uec = ucsi_get_drvdata(ucsi);

	*version = uec->version;

	return 0;
}

static int gaokun_ucsi_read_cci(struct ucsi *ucsi, u32 *cci)
{
	struct gaokun_ucsi *uec = ucsi_get_drvdata(ucsi);
	u8 buf[GAOKUN_UCSI_READ_SIZE];
	int ret;

	ret = gaokun_ec_ucsi_read(uec->ec, buf);
	if (ret)
		return ret;

	memcpy(cci, buf, sizeof(*cci));

	return 0;
}

static int gaokun_ucsi_read_message_in(struct ucsi *ucsi,
				       void *val, size_t val_len)
{
	struct gaokun_ucsi *uec = ucsi_get_drvdata(ucsi);
	u8 buf[GAOKUN_UCSI_READ_SIZE];
	int ret;

	ret = gaokun_ec_ucsi_read(uec->ec, buf);
	if (ret)
		return ret;

	memcpy(val, buf + GAOKUN_UCSI_CCI_SIZE,
	       min(val_len, GAOKUN_UCSI_MSGI_SIZE));

	return 0;
}

static int gaokun_ucsi_async_control(struct ucsi *ucsi, u64 command)
{
	struct gaokun_ucsi *uec = ucsi_get_drvdata(ucsi);
	u8 buf[GAOKUN_UCSI_WRITE_SIZE] = {};

	memcpy(buf, &command, sizeof(command));

	return gaokun_ec_ucsi_write(uec->ec, buf);
}

static void gaokun_ucsi_update_connector(struct ucsi_connector *con)
{
	struct gaokun_ucsi *uec = ucsi_get_drvdata(con->ucsi);

	if (con->num > uec->num_ports)
		return;

	con->typec_cap.orientation_aware = true;
}

static void gaokun_set_orientation(struct ucsi_connector *con,
				   struct gaokun_ucsi_port *port)
{
	struct gaokun_ucsi *uec = port->ucsi;
	enum gaokun_ucsi_ccx ccx;
	enum gaokun_ucsi_ccx raw_ccx;
	enum typec_orientation orientation;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&port->lock, flags);
	ccx = port->ccx;
	spin_unlock_irqrestore(&port->lock, flags);

	raw_ccx = ccx;
	orientation = CCX_TO_ORI(ccx);
	ret = typec_set_orientation(con->port, orientation);
	gaokun_ucsi_trace_rl(uec,
			     "set orientation: con=%d port=%d ccx=%u orientation=%u ret=%d\n",
			     con->num, port->idx, raw_ccx, orientation, ret);
}

static void gaokun_ucsi_connector_status(struct ucsi_connector *con)
{
	struct gaokun_ucsi *uec = ucsi_get_drvdata(con->ucsi);
	int idx;

	idx = con->num - 1;
	if (con->num > uec->num_ports) {
		dev_warn(uec->dev, "set orientation out of range: con%d\n", idx);
		return;
	}

	gaokun_set_orientation(con, &uec->ports[idx]);
}

const struct ucsi_operations gaokun_ucsi_ops = {
	.read_version = gaokun_ucsi_read_version,
	.read_cci = gaokun_ucsi_read_cci,
	.poll_cci = gaokun_ucsi_read_cci,
	.read_message_in = gaokun_ucsi_read_message_in,
	.sync_control = ucsi_sync_control_common,
	.async_control = gaokun_ucsi_async_control,
	.update_connector = gaokun_ucsi_update_connector,
	.connector_status = gaokun_ucsi_connector_status,
};

/* -------------------------------------------------------------------------- */
/* For Altmode */

static void gaokun_ucsi_port_update(struct gaokun_ucsi_port *port,
				    const u8 *port_data)
{
	struct gaokun_ucsi *uec = port->ucsi;
	int offset = port->idx * GAOKUN_UCSI_BYTES_PER_PORT;
	unsigned long flags;
	enum gaokun_ucsi_ccx ccx;
	enum gaokun_ucsi_mux mux;
	u8 mode;
	u16 svid;
	u8 hpd_state;
	u8 hpd_irq;
	u8 dcc, ddi;

	dcc = port_data[offset];
	ddi = port_data[offset + 1];

	spin_lock_irqsave(&port->lock, flags);

	port->ccx = FIELD_GET(GAOKUN_CCX_MASK, dcc);
	port->mux = FIELD_GET(GAOKUN_MUX_MASK, dcc);
	port->mode = FIELD_GET(GAOKUN_DPAM_MASK, ddi);
	port->hpd_state = FIELD_GET(GAOKUN_HPD_STATE_MASK, ddi);
	port->hpd_irq = FIELD_GET(GAOKUN_HPD_IRQ_MASK, ddi);

	/* Mode and SVID are unused; keeping them to make things clearer */
	switch (port->mode) {
	case USBC_DPAM_PAN_C:
	case USBC_DPAM_PAN_C_REVERSE:
		port->mode = DP_PIN_ASSIGN_C; /* correct it for usb later */
		break;
	case USBC_DPAM_PAN_D:
	case USBC_DPAM_PAN_D_REVERSE:
		port->mode = DP_PIN_ASSIGN_D;
		break;
	case USBC_DPAM_PAN_E:
	case USBC_DPAM_PAN_E_REVERSE:
		port->mode = DP_PIN_ASSIGN_E;
		break;
	case USBC_DPAM_PAN_NONE:
		port->mode = TYPEC_STATE_SAFE;
		break;
	default:
		dev_warn(uec->dev, "unknown mode %d\n", port->mode);
		break;
	}

	switch (port->mux) {
	case USBC_MUX_NONE:
		port->svid = 0;
		break;
	case USBC_MUX_USB_2L:
		port->svid = USB_SID_PD;
		port->mode = TYPEC_STATE_USB; /* same as PAN_C, correct it */
		break;
	case USBC_MUX_DP_4L:
	case USBC_MUX_USB_DP:
		port->svid = USB_SID_DISPLAYPORT;
		break;
	default:
		dev_warn(uec->dev, "unknown mux state %d\n", port->mux);
		break;
	}

	ccx = port->ccx;
	mux = port->mux;
	mode = port->mode;
	svid = port->svid;
	hpd_state = port->hpd_state;
	hpd_irq = port->hpd_irq;

	spin_unlock_irqrestore(&port->lock, flags);

	gaokun_ucsi_trace(uec,
			  "port snapshot: port=%d raw_dcc=%#x raw_ddi=%#x ccx=%u mux=%u mode=%u svid=%#x hpd_state=%u hpd_irq=%u\n",
			  port->idx, dcc, ddi, ccx, mux, mode, svid,
			  hpd_state, hpd_irq);
}

static u8 gaokun_ucsi_valid_port_mask(const struct gaokun_ucsi *uec)
{
	if (!uec->num_ports)
		return 0;

	return GENMASK(uec->num_ports - 1, 0);
}

static int gaokun_ucsi_refresh(struct gaokun_ucsi *uec, u8 *port_mask)
{
	struct gaokun_ucsi_reg ureg;
	u8 valid_mask, updates;
	int ret, idx;

	ret = gaokun_ec_ucsi_get_reg(uec->ec, &ureg);
	if (ret)
		return ret;

	gaokun_ucsi_trace(uec,
			  "refresh reg: ts_ns=%llu ec_num_ports=%u ec_port_updt=%#x checksum=%#x\n",
			  GAOKUN_TRACE_TS_NS, ureg.num_ports, ureg.port_updt,
			  ureg.checksum);

	if (ureg.num_ports != uec->num_ports)
		dev_warn_ratelimited(uec->dev, "EC reported %u ports, expected %u\n",
				     ureg.num_ports, uec->num_ports);

	valid_mask = gaokun_ucsi_valid_port_mask(uec);
	if (ureg.port_updt & ~valid_mask)
		dev_warn_ratelimited(uec->dev,
				     "ignoring invalid EC port update mask %#x\n",
				     ureg.port_updt);

	updates = ureg.port_updt & valid_mask;
	for (idx = 0; idx < uec->num_ports; idx++) {
		if (!(updates & BIT(idx)))
			continue;

		gaokun_ucsi_port_update(&uec->ports[idx], ureg.port_data);
	}

	*port_mask = updates;
	gaokun_ucsi_trace(uec, "refresh updates mask=%#x valid_mask=%#x\n",
			  updates, valid_mask);

	return 0;
}

static void gaokun_ucsi_handle_altmode(struct gaokun_ucsi_port *port)
{
	struct gaokun_ucsi *uec = port->ucsi;
	int idx = port->idx;

	if (idx >= uec->ucsi->cap.num_connectors) {
		dev_warn(uec->dev, "altmode port out of range: %d\n", idx);
		return;
	}

	/* UCSI callback .connector_status() have set orientation */
	if (port->bridge)
		drm_aux_hpd_bridge_notify(&port->bridge->dev,
					  port->hpd_state ?
					  connector_status_connected :
					  connector_status_disconnected);
}

static void gaokun_ucsi_complete_usb_ack(struct gaokun_ucsi *uec, u8 port_mask)
{
	struct gaokun_ucsi_port *port;
	int idx = 0;

	while (idx < uec->num_ports) {
		port = &uec->ports[idx++];
		if (!(port_mask & BIT(port->idx)))
			continue;
		if (!completion_done(&port->usb_ack))
			complete_all(&port->usb_ack);
		gaokun_ucsi_trace(uec,
				  "usb follow-up complete: port=%d mask=%#x now_done=%d\n",
				  port->idx, port_mask,
				  completion_done(&port->usb_ack));
	}
}

static void gaokun_ucsi_ack_updates(struct gaokun_ucsi *uec, u8 port_mask)
{
	int first_ret = 0;
	int idx;

	if (!port_mask) {
		first_ret = gaokun_ec_ucsi_pan_ack(uec->ec,
						   GAOKUN_UCSI_NO_PORT_UPDATE);
		goto out;
	}

	for (idx = 0; idx < uec->num_ports; idx++) {
		int ret;

		if (!(port_mask & BIT(idx)))
			continue;

		ret = gaokun_ec_ucsi_pan_ack(uec->ec, idx);
		if (ret && !first_ret)
			first_ret = ret;
	}

out:
	if (first_ret)
		dev_warn_ratelimited(uec->dev,
				     "failed to ack EC port updates %#x: %d\n",
				     port_mask, first_ret);
}

static int gaokun_ucsi_refresh_ppm_locked(struct gaokun_ucsi *uec, u8 *port_mask)
{
	int ret;

	/*
	 * These EC register accesses are outside the normal UCSI core command
	 * flow. Serialize them with the core's PPM lock so delayed altmode
	 * recovery cannot collide with GET_CONNECTOR_STATUS and leave the
	 * connector state stale after repeated plug/unplug cycles.
	 */
	mutex_lock(&uec->ucsi->ppm_lock);
	ret = gaokun_ucsi_refresh(uec, port_mask);
	mutex_unlock(&uec->ucsi->ppm_lock);

	return ret;
}

static void gaokun_ucsi_ack_updates_ppm_locked(struct gaokun_ucsi *uec, u8 port_mask)
{
	mutex_lock(&uec->ucsi->ppm_lock);
	gaokun_ucsi_ack_updates(uec, port_mask);
	mutex_unlock(&uec->ucsi->ppm_lock);
}

static void gaokun_ucsi_altmode_notify_ind(struct gaokun_ucsi *uec,
					   bool got_usb_event)
{
	u8 port_mask;
	int idx;
	int ret;

	gaokun_ucsi_trace_rl(uec, "altmode notify: ts_ns=%llu got_usb_event=%d\n",
			     GAOKUN_TRACE_TS_NS, got_usb_event);

	if (!uec->ucsi_registered || !uec->ucsi->connector) {
		/*
		 * On MateBook E Go, early EC/UCSI activity indirectly shares
		 * the same boot-time path as internal display bring-up. Touching
		 * UCSI state too early can leave the panel dark, so ack and drop
		 * pending events until UCSI core registration has completed.
		 */
		dev_warn_ratelimited(uec->dev,
				     "ucsi connector is not initialized yet, acking pending event\n");
		gaokun_ucsi_ack_updates_ppm_locked(uec, 0);
		return;
	}

	ret = gaokun_ucsi_refresh_ppm_locked(uec, &port_mask);
	if (ret)
		return;
	gaokun_ucsi_trace(uec, "altmode notify refresh done: port_mask=%#x\n",
			  port_mask);

	if (got_usb_event)
		gaokun_ucsi_complete_usb_ack(uec, port_mask);

	if (!port_mask) {
		gaokun_ucsi_ack_updates_ppm_locked(uec, 0);
		return;
	}

	for (idx = 0; idx < uec->num_ports; idx++) {
		if (!(port_mask & BIT(idx)))
			continue;

		gaokun_ucsi_handle_altmode(&uec->ports[idx]);
	}

	gaokun_ucsi_ack_updates_ppm_locked(uec, port_mask);
}

/*
 * USB event is necessary for enabling altmode, the event should follow
 * UCSI event, if not after timeout(this notify may be disabled somehow),
 * then force to enable altmode.
 */
static void gaokun_ucsi_handle_no_usb_event(struct work_struct *work)
{
	struct gaokun_ucsi_port *port;
	struct gaokun_ucsi *uec;

	port = container_of(to_delayed_work(work), struct gaokun_ucsi_port,
			    usb_work);
	uec = port->ucsi;
	gaokun_ucsi_trace_rl(uec,
			     "usb follow-up worker fired: ts_ns=%llu port=%d done=%d\n",
			     GAOKUN_TRACE_TS_NS, port->idx,
			     completion_done(&port->usb_ack));
	if (completion_done(&port->usb_ack))
		return;

	dev_warn(uec->dev, "missing USB event for port %d after UCSI event\n",
		 port->idx);
	gaokun_ucsi_altmode_notify_ind(uec, false);
}

static int gaokun_ucsi_notify(struct notifier_block *nb,
			      unsigned long action, void *data)
{
	u32 cci;
	struct gaokun_ucsi *uec = container_of(nb, struct gaokun_ucsi, nb);

	gaokun_ucsi_trace_rl(uec, "notifier action=%#lx ts_ns=%llu\n",
			     action, GAOKUN_TRACE_TS_NS);

	switch (action) {
	case EC_EVENT_USB:
		gaokun_ucsi_trace_rl(uec, "notifier EC_EVENT_USB\n");
		gaokun_ucsi_altmode_notify_ind(uec, true);
		return NOTIFY_OK;

	case EC_EVENT_UCSI:
		if (gaokun_ucsi_read_cci(uec->ucsi, &cci))
			return NOTIFY_BAD;
		gaokun_ucsi_trace_rl(uec, "notifier EC_EVENT_UCSI cci=%#x connector=%lu\n",
				     cci, UCSI_CCI_CONNECTOR(cci));
		ucsi_notify_common(uec->ucsi, cci);
		if (UCSI_CCI_CONNECTOR(cci)) {
			struct gaokun_ucsi_port *port;
			u8 idx = UCSI_CCI_CONNECTOR(cci) - 1;

			if (idx >= uec->num_ports) {
				dev_warn(uec->dev, "connector out of range: %lu\n",
					 UCSI_CCI_CONNECTOR(cci));
				return NOTIFY_BAD;
			}

			port = &uec->ports[idx];
			reinit_completion(&port->usb_ack);
			mod_delayed_work(system_wq, &port->usb_work, 2 * HZ);
			gaokun_ucsi_trace(uec,
					  "usb follow-up armed: port=%u delay_jiffies=%u\n",
					  idx, 2 * HZ);
		}

		return NOTIFY_OK;

	default:
		return NOTIFY_DONE;
	}
}

static int gaokun_ucsi_ports_init(struct gaokun_ucsi *uec)
{
	struct gaokun_ucsi_port *ucsi_port;
	struct device *dev = uec->dev;
	struct fwnode_handle *fwnode;
	int i, ret, num_ports;
	u32 port;

	if (uec->ports_initialized)
		return 0;

	num_ports = 0;
	device_for_each_child_node(dev, fwnode)
		num_ports++;

	if (num_ports <= 0)
		return dev_err_probe(dev, -ENODEV,
				     "no connector child nodes found for UCSI bridge setup\n");
	if (num_ports > GAOKUN_UCSI_MAX_PORTS)
		return dev_err_probe(dev, -EINVAL,
				     "DT reported %d UCSI ports, max %zu\n",
				     num_ports, (size_t)GAOKUN_UCSI_MAX_PORTS);

	uec->num_ports = num_ports;
	uec->ports = devm_kcalloc(dev, num_ports, sizeof(*(uec->ports)),
				  GFP_KERNEL);
	if (!uec->ports)
		return -ENOMEM;

	for (i = 0; i < num_ports; ++i) {
		ucsi_port = &uec->ports[i];
		ucsi_port->ccx = USBC_CCX_NONE;
		ucsi_port->idx = i;
		ucsi_port->ucsi = uec;
		init_completion(&ucsi_port->usb_ack);
		INIT_DELAYED_WORK(&ucsi_port->usb_work,
				  gaokun_ucsi_handle_no_usb_event);
		spin_lock_init(&ucsi_port->lock);
	}

	device_for_each_child_node(dev, fwnode) {
		ret = fwnode_property_read_u32(fwnode, "reg", &port);
		if (ret < 0) {
			dev_err(dev, "missing reg property of %pOFn\n", fwnode);
			fwnode_handle_put(fwnode);
			return ret;
		}

		if (port >= num_ports) {
			dev_warn(dev, "invalid connector number %d, ignoring\n", port);
			continue;
		}

		ucsi_port = &uec->ports[port];
		ucsi_port->bridge = devm_drm_dp_hpd_bridge_alloc(dev, to_of_node(fwnode));
		if (IS_ERR(ucsi_port->bridge)) {
			fwnode_handle_put(fwnode);
			return PTR_ERR(ucsi_port->bridge);
		}
	}

	for (i = 0; i < num_ports; i++) {
		if (!uec->ports[i].bridge)
			continue;

		ret = devm_drm_dp_hpd_bridge_add(dev, uec->ports[i].bridge);
		if (ret)
			return ret;
	}

	uec->ports_initialized = true;

	return 0;
}

static int gaokun_ucsi_ec_init(struct gaokun_ucsi *uec)
{
	struct gaokun_ucsi_reg ureg = {};
	int ret;

	ret = gaokun_ec_ucsi_get_reg(uec->ec, &ureg);
	if (ret)
		return ret;

	if (ureg.num_ports <= 0)
		return -ENODEV;

	if (ureg.num_ports > uec->num_ports)
		return -EINVAL;

	uec->num_ports = ureg.num_ports;

	return 0;
}

static void gaokun_ucsi_register_worker(struct work_struct *work)
{
	struct gaokun_ucsi *uec;
	struct ucsi *ucsi;
	int ret;

	uec = container_of(work, struct gaokun_ucsi, work.work);
	ucsi = uec->ucsi;

	ret = gaokun_ucsi_ports_init(uec);
	if (ret)
		goto retry;

	ret = gaokun_ucsi_ec_init(uec);
	if (ret)
		goto retry;

	ret = ucsi_register(ucsi);
	if (ret) {
		dev_err_probe(ucsi->dev, ret, "ucsi register failed\n");
		goto retry;
	}
	uec->ucsi_registered = true;

	ret = gaokun_ec_register_notify(uec->ec, &uec->nb);
	if (ret) {
		dev_err_probe(ucsi->dev, ret, "notifier register failed\n");
		ucsi_unregister(ucsi);
		uec->ucsi_registered = false;
		goto retry;
	}
	uec->notifier_registered = true;

	return;

retry:
	if (++uec->register_retries > GAOKUN_UCSI_MAX_RETRIES) {
		dev_err(uec->dev, "giving up on UCSI registration after %u attempts\n",
			uec->register_retries);
		return;
	}

	dev_warn(uec->dev, "retrying UCSI registration in %u seconds (attempt %u/%u)\n",
		 GAOKUN_UCSI_RETRY_DELAY / HZ,
		 uec->register_retries, GAOKUN_UCSI_MAX_RETRIES);
	schedule_delayed_work(&uec->work, GAOKUN_UCSI_RETRY_DELAY);
}

static int gaokun_ucsi_probe(struct auxiliary_device *adev,
			     const struct auxiliary_device_id *id)
{
	struct gaokun_ec *ec = adev->dev.platform_data;
	struct device *dev = &adev->dev;
	struct gaokun_ucsi *uec;
	int ret;

	uec = devm_kzalloc(dev, sizeof(*uec), GFP_KERNEL);
	if (!uec)
		return -ENOMEM;

	uec->ec = ec;
	uec->dev = dev;
	uec->version = UCSI_VERSION_1_0;
	uec->nb.notifier_call = gaokun_ucsi_notify;

	INIT_DELAYED_WORK(&uec->work, gaokun_ucsi_register_worker);

	ret = gaokun_ucsi_ports_init(uec);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize UCSI ports\n");

	uec->ucsi = ucsi_create(dev, &gaokun_ucsi_ops);
	if (IS_ERR(uec->ucsi))
		return dev_err_probe(dev, PTR_ERR(uec->ucsi),
				     "failed to create UCSI instance\n");

	ucsi_set_drvdata(uec->ucsi, uec);
	auxiliary_set_drvdata(adev, uec);

	/*
	 * On MateBook E Go, early EC/UCSI traffic indirectly participates in
	 * the internal display bring-up chain. Defer registration until the
	 * platform settles to avoid booting with a dark panel.
	 */
	schedule_delayed_work(&uec->work, GAOKUN_UCSI_REGISTER_DELAY);

	return 0;
}

static void gaokun_ucsi_remove(struct auxiliary_device *adev)
{
	struct gaokun_ucsi *uec = auxiliary_get_drvdata(adev);
	int i;

	cancel_delayed_work_sync(&uec->work);
	if (uec->ports_initialized) {
		for (i = 0; i < uec->num_ports; i++)
			cancel_delayed_work_sync(&uec->ports[i].usb_work);
	}

	if (uec->notifier_registered)
		gaokun_ec_unregister_notify(uec->ec, &uec->nb);
	if (uec->ucsi_registered)
		ucsi_unregister(uec->ucsi);
	ucsi_destroy(uec->ucsi);
}

static const struct auxiliary_device_id gaokun_ucsi_id_table[] = {
	{ .name = GAOKUN_MOD_NAME "." GAOKUN_DEV_UCSI, },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, gaokun_ucsi_id_table);

static struct auxiliary_driver gaokun_ucsi_driver = {
	.name = GAOKUN_DEV_UCSI,
	.id_table = gaokun_ucsi_id_table,
	.probe = gaokun_ucsi_probe,
	.remove = gaokun_ucsi_remove,
};

module_auxiliary_driver(gaokun_ucsi_driver);

MODULE_DESCRIPTION("HUAWEI Matebook E Go UCSI driver");
MODULE_LICENSE("GPL");
