/*
 * Designware HDMI CEC driver
 *
 * Copyright (C) 2015-2017 Russell King.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_data/dw_hdmi-cec.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include <drm/drm_edid.h>

#include <media/cec.h>
#include <media/cec-edid.h>
#include <media/cec-notifier.h>

#define DEV_NAME "mxc_hdmi_cec"

enum {
	HDMI_IH_CEC_STAT0	= 0x0106,
	HDMI_IH_MUTE_CEC_STAT0	= 0x0186,

	HDMI_CEC_CTRL		= 0x7d00,
	CEC_CTRL_START		= BIT(0),
	CEC_CTRL_NORMAL		= 1 << 1,

	HDMI_CEC_STAT		= 0x7d01,
	CEC_STAT_DONE		= BIT(0),
	CEC_STAT_EOM		= BIT(1),
	CEC_STAT_NACK		= BIT(2),
	CEC_STAT_ARBLOST	= BIT(3),
	CEC_STAT_ERROR_INIT	= BIT(4),
	CEC_STAT_ERROR_FOLL	= BIT(5),
	CEC_STAT_WAKEUP		= BIT(6),

	HDMI_CEC_MASK		= 0x7d02,
	HDMI_CEC_POLARITY	= 0x7d03,
	HDMI_CEC_INT		= 0x7d04,
	HDMI_CEC_ADDR_L		= 0x7d05,
	HDMI_CEC_ADDR_H		= 0x7d06,
	HDMI_CEC_TX_CNT		= 0x7d07,
	HDMI_CEC_RX_CNT		= 0x7d08,
	HDMI_CEC_TX_DATA0	= 0x7d10,
	HDMI_CEC_RX_DATA0	= 0x7d20,
	HDMI_CEC_LOCK		= 0x7d30,
	HDMI_CEC_WKUPCTRL	= 0x7d31,
};

struct dw_hdmi_cec {
	void __iomem *base;
	u32 addresses;
	struct cec_adapter *adap;
	struct cec_msg rx_msg;
	unsigned int tx_status;
	bool tx_done;
	bool rx_done;
	const struct dw_hdmi_cec_ops *ops;
	struct cec_notifier *notify;
	void *ops_data;
	int retries;
	int irq;
};

static int dw_hdmi_cec_log_addr(struct cec_adapter *adap, u8 logical_addr)
{
	struct dw_hdmi_cec *cec = adap->priv;
	u32 addresses;

	if (logical_addr == CEC_LOG_ADDR_INVALID)
		addresses = cec->addresses = 0;
	else
		addresses = cec->addresses |= BIT(logical_addr) | BIT(15);

	writeb_relaxed(addresses & 255, cec->base + HDMI_CEC_ADDR_L);
	writeb_relaxed(addresses >> 8, cec->base + HDMI_CEC_ADDR_H);

	return 0;
}

static int dw_hdmi_cec_transmit(struct cec_adapter *adap, u8 attempts,
				u32 signal_free_time, struct cec_msg *msg)
{
	struct dw_hdmi_cec *cec = adap->priv;
	unsigned i;

	cec->retries = attempts;

	for (i = 0; i < msg->len; i++)
		writeb_relaxed(msg->msg[i], cec->base + HDMI_CEC_TX_DATA0 + i);

	writeb_relaxed(msg->len, cec->base + HDMI_CEC_TX_CNT);
	writeb_relaxed(CEC_CTRL_NORMAL | CEC_CTRL_START, cec->base + HDMI_CEC_CTRL);

	return 0;
}

static irqreturn_t dw_hdmi_cec_hardirq(int irq, void *data)
{
	struct cec_adapter *adap = data;
	struct dw_hdmi_cec *cec = adap->priv;
	unsigned stat = readb_relaxed(cec->base + HDMI_IH_CEC_STAT0);
	irqreturn_t ret = IRQ_HANDLED;

	if (stat == 0)
		return IRQ_NONE;

	writeb_relaxed(stat, cec->base + HDMI_IH_CEC_STAT0);

	if (stat & CEC_STAT_ERROR_INIT) {
		if (cec->retries) {
			unsigned v = readb_relaxed(cec->base + HDMI_CEC_CTRL);
			writeb_relaxed(v | CEC_CTRL_START, cec->base + HDMI_CEC_CTRL);
			cec->retries -= 1;
		} else {
			cec->tx_status = CEC_TX_STATUS_MAX_RETRIES;
			cec->tx_done = true;
			ret = IRQ_WAKE_THREAD;
		}
	} else if (stat & CEC_STAT_DONE) {
		cec->tx_status = CEC_TX_STATUS_OK;
		cec->tx_done = true;
		ret = IRQ_WAKE_THREAD;
	} else if (stat & CEC_STAT_NACK) {
		cec->tx_status = CEC_TX_STATUS_NACK;
		cec->tx_done = true;
		ret = IRQ_WAKE_THREAD;
	}

	if (stat & CEC_STAT_EOM) {
		unsigned len, i;
		void *base = cec->base;

		len = readb_relaxed(base + HDMI_CEC_RX_CNT);
		if (len > sizeof(cec->rx_msg.msg))
			len = sizeof(cec->rx_msg.msg);

		for (i = 0; i < len; i++)
			cec->rx_msg.msg[i] =
				readb_relaxed(base + HDMI_CEC_RX_DATA0 + i);

		writeb_relaxed(0, base + HDMI_CEC_LOCK);

		cec->rx_msg.len = len;
		smp_wmb();
		cec->rx_done = true;

		ret = IRQ_WAKE_THREAD;
	}

	return ret;
}

static irqreturn_t dw_hdmi_cec_thread(int irq, void *data)
{
	struct cec_adapter *adap = data;
	struct dw_hdmi_cec *cec = adap->priv;

	if (cec->tx_done) {
		cec->tx_done = false;
		cec_transmit_done(adap, cec->tx_status, 0, 0, 0, 0);
	}
	if (cec->rx_done) {
		cec->rx_done = false;
		smp_rmb();
		cec_received_msg(adap, &cec->rx_msg);
	}
	return IRQ_HANDLED;
}

static int dw_hdmi_cec_enable(struct cec_adapter *adap, bool enable)
{
	struct dw_hdmi_cec *cec = adap->priv;

	if (!enable) {
		writeb_relaxed(~0, cec->base + HDMI_CEC_MASK);
		writeb_relaxed(~0, cec->base + HDMI_IH_MUTE_CEC_STAT0);
		writeb_relaxed(0, cec->base + HDMI_CEC_POLARITY);

		cec->ops->disable(cec->ops_data);
	} else {
		unsigned irqs;

		writeb_relaxed(0, cec->base + HDMI_CEC_CTRL);
		writeb_relaxed(~0, cec->base + HDMI_IH_CEC_STAT0);
		writeb_relaxed(0, cec->base + HDMI_CEC_LOCK);

		dw_hdmi_cec_log_addr(cec->adap, CEC_LOG_ADDR_INVALID);

		cec->ops->enable(cec->ops_data);

		irqs = CEC_STAT_ERROR_INIT | CEC_STAT_NACK | CEC_STAT_EOM |
		       CEC_STAT_DONE;
		writeb_relaxed(irqs, cec->base + HDMI_CEC_POLARITY);
		writeb_relaxed(~irqs, cec->base + HDMI_CEC_MASK);
		writeb_relaxed(~irqs, cec->base + HDMI_IH_MUTE_CEC_STAT0);
	}
	return 0;
}

static const struct cec_adap_ops dw_hdmi_cec_ops = {
	.adap_enable = dw_hdmi_cec_enable,
	.adap_log_addr = dw_hdmi_cec_log_addr,
	.adap_transmit = dw_hdmi_cec_transmit,
};

static void dw_hdmi_cec_del(void *data)
{
	struct dw_hdmi_cec *cec = data;

	cec_delete_adapter(cec->adap);
}

static int dw_hdmi_cec_probe(struct platform_device *pdev)
{
	struct dw_hdmi_cec_data *data = dev_get_platdata(&pdev->dev);
	struct dw_hdmi_cec *cec;
	int ret;

	if (!data)
		return -ENXIO;

	/*
	 * Our device is just a convenience - we want to link to the real
	 * hardware device here, so that userspace can see the association
	 * between the HDMI hardware and its associated CEC chardev.
	 */
	cec = devm_kzalloc(&pdev->dev, sizeof(*cec), GFP_KERNEL);
	if (!cec)
		return -ENOMEM;

	cec->base = data->base;
	cec->irq = data->irq;
	cec->ops = data->ops;
	cec->ops_data = data->ops_data;

	platform_set_drvdata(pdev, cec);

	writeb_relaxed(0, cec->base + HDMI_CEC_TX_CNT);
	writeb_relaxed(~0, cec->base + HDMI_CEC_MASK);
	writeb_relaxed(~0, cec->base + HDMI_IH_MUTE_CEC_STAT0);
	writeb_relaxed(0, cec->base + HDMI_CEC_POLARITY);

	cec->adap = cec_allocate_adapter(&dw_hdmi_cec_ops, cec, "dw_hdmi",
					 CEC_CAP_LOG_ADDRS | CEC_CAP_TRANSMIT |
					 CEC_CAP_RC, CEC_MAX_LOG_ADDRS);
	if (IS_ERR(cec->adap))
		return PTR_ERR(cec->adap);

	/* override the module pointer */
	cec->adap->owner = THIS_MODULE;

	ret = devm_add_action(&pdev->dev, dw_hdmi_cec_del, cec);
	if (ret) {
		cec_delete_adapter(cec->adap);
		return ret;
	}

	ret = devm_request_threaded_irq(&pdev->dev, cec->irq,
					dw_hdmi_cec_hardirq,
					dw_hdmi_cec_thread, IRQF_SHARED,
					DEV_NAME, cec->adap);
	if (ret < 0)
		return ret;

	cec->notify = cec_notifier_get(pdev->dev.parent);
	if (!cec->notify)
		return -ENOMEM;

	ret = cec_register_adapter(cec->adap, pdev->dev.parent);
	if (ret < 0) {
		cec_notifier_put(cec->notify);
		return ret;
	}

	/*
	 * CEC documentation says we must not call cec_delete_adapter
	 * after a successful call to cec_register_adapter().
	 */
	devm_remove_action(&pdev->dev, dw_hdmi_cec_del, cec);

	cec_register_cec_notifier(cec->adap, cec->notify);

	return 0;
}

static int dw_hdmi_cec_remove(struct platform_device *pdev)
{
	struct dw_hdmi_cec *cec = platform_get_drvdata(pdev);

	cec_unregister_adapter(cec->adap);
	cec_notifier_put(cec->notify);

	return 0;
}

static struct platform_driver dw_hdmi_cec_driver = {
	.probe	= dw_hdmi_cec_probe,
	.remove	= dw_hdmi_cec_remove,
	.driver = {
		.name = "dw-hdmi-cec",
		.owner = THIS_MODULE,
	},
};
module_platform_driver(dw_hdmi_cec_driver);

MODULE_AUTHOR("Russell King <rmk+kernel@arm.linux.org.uk>");
MODULE_DESCRIPTION("Synopsis Designware HDMI CEC driver for i.MX");
MODULE_LICENSE("GPL");
MODULE_ALIAS(PLATFORM_MODULE_PREFIX "dw-hdmi-cec");
