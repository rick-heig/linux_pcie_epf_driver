// SPDX-License-Identifier: GPL-2.0
/*
 * Simple driver to test endpoint functionality
 * Copyright (C) 2023 Rick Wertenbroek <rick.wertenbroek@gmail.com>
 * 
 * Inspired by ;
 * drivers/pci/endpoint/functions/pci-epf-test-c
 * Test driver to test endpoint functionality
 * Copyright (C) 2017 Texas Instruments
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 */

#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pci_ids.h>
#include <linux/random.h>

#include <linux/pci-epc.h>
#include <linux/pci-epf.h>
#include <linux/pci_regs.h>

#define COMMAND_MOO			0xdeadbeef
const static char moo[] = "\n"
	"         (__)  \n"
	"         (oo)  \n"
	"   /------\\/  \n"
	"  / |    ||    \n"
	" *  /\\---/\\  \n"
	"    ~~   ~~    \n"
	"....\"Have you mooed today?\"...\n";

#define TIMER_RESOLUTION		1
static struct workqueue_struct *kpcie_epfl_simple_workqueue;

struct pci_epf_simple {
	void				*reg[PCI_STD_NUM_BARS];
	struct pci_epf			*epf;
	enum pci_barno			simple_reg_bar;
	struct delayed_work		cmd_handler;
	const struct pci_epc_features	*epc_features;
};

struct pci_epf_simple_reg {
	u32	magic;
	u32	command;
	u32	status;
} __packed;

static struct pci_epf_header simple_header = {
	.vendorid	= PCI_ANY_ID,
	.deviceid	= PCI_ANY_ID,
	.baseclass_code = PCI_CLASS_OTHERS,
	.interrupt_pin	= PCI_INTERRUPT_INTA,
};

static size_t bar_size[] = { 512, 512, 1024, 16384, 131072, 1048576 };

static void pci_epf_simple_cmd_handler(struct work_struct *work)
{
	//int ret;
	u32 command;
	struct pci_epf_simple *epf_simple = container_of(work, struct pci_epf_simple,
							 cmd_handler.work);
	struct pci_epf *epf = epf_simple->epf;
	struct device *dev = &epf->dev;
	//struct pci_epc *epc = epf->epc;
	enum pci_barno simple_reg_bar = epf_simple->simple_reg_bar;
	struct pci_epf_simple_reg *reg = epf_simple->reg[simple_reg_bar];

	command = READ_ONCE(reg->command);
	if (!command)
		goto reset_handler;

	WRITE_ONCE(reg->command, 0);
	WRITE_ONCE(reg->status, 0);

	if (command == COMMAND_MOO) {
		WRITE_ONCE(reg->status, COMMAND_MOO);
		//dev_info(dev, "%s", moo);
		dev_info(dev, "will this work ?\n");
		goto reset_handler;
	} else {
		WRITE_ONCE(reg->status, 0xdeadc0de);
		goto reset_handler;
	}

reset_handler:
	queue_delayed_work(kpcie_epfl_simple_workqueue, &epf_simple->cmd_handler,
			   msecs_to_jiffies(1));
}

static void pci_epf_simple_unbind(struct pci_epf *epf)
{
	struct pci_epf_simple *epf_simple = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;
	struct pci_epf_bar *epf_bar;
	int bar;

	dev_warn(&epf->dev, "unbind called, BARs will be cleared\n");

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		epf_bar = &epf->bar[bar];

		if (epf_simple->reg[bar]) {
			pci_epc_clear_bar(epc, epf->func_no, epf->vfunc_no,
					  epf_bar);
			pci_epf_free_space(epf, epf_simple->reg[bar], bar,
					   PRIMARY_INTERFACE);
		}
	}
}

static int pci_epf_simple_set_bar(struct pci_epf *epf)
{
	int bar, add;
	int ret;
	struct pci_epf_bar *epf_bar;
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;
	struct pci_epf_simple *epf_simple = epf_get_drvdata(epf);
	enum pci_barno simple_reg_bar = epf_simple->simple_reg_bar;
	const struct pci_epc_features *epc_features;

	dev_warn(dev, "set_bar called, BARs will be (re)set(?)\n");

	epc_features = epf_simple->epc_features;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar += add) {
		epf_bar = &epf->bar[bar];
		/*
		 * pci_epc_set_bar() sets PCI_BASE_ADDRESS_MEM_TYPE_64
		 * if the specific implementation required a 64-bit BAR,
		 * even if we only requested a 32-bit BAR.
		 */
		add = (epf_bar->flags & PCI_BASE_ADDRESS_MEM_TYPE_64) ? 2 : 1;

		if (!!(epc_features->reserved_bar & (1 << bar)))
			continue;

		ret = pci_epc_set_bar(epc, epf->func_no, epf->vfunc_no,
				      epf_bar);
		if (ret) {
			pci_epf_free_space(epf, epf_simple->reg[bar], bar,
					   PRIMARY_INTERFACE);
			dev_err(dev, "Failed to set BAR%d\n", bar);
			if (bar == simple_reg_bar)
				return ret;
		}
	}

	return 0;
}

static int pci_epf_simple_core_init(struct pci_epf *epf)
{
	//struct pci_epf_simple *epf_simple = epf_get_drvdata(epf);
	struct pci_epf_header *header = epf->header;
	const struct pci_epc_features *epc_features;
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;
	int ret;

	dev_warn(dev, "core init called\n");

	epc_features = pci_epc_get_features(epc, epf->func_no, epf->vfunc_no);
	if (epc_features) {
		/* Nothing, query features here */
	}

	/** @todo check this */
	if (epf->vfunc_no <= 1) {
		ret = pci_epc_write_header(epc, epf->func_no, epf->vfunc_no, header);
		if (ret) {
			dev_err(dev, "Configuration header write failed\n");
			return ret;
		}
	}

	ret = pci_epf_simple_set_bar(epf);
	if (ret)
		return ret;

	return 0;
}

static int pci_epf_simple_notifier(struct notifier_block *nb, unsigned long val,
				 void *data)
{
	struct pci_epf *epf = container_of(nb, struct pci_epf, nb);
	//struct pci_epf_simple *epf_simple = epf_get_drvdata(epf);
	int ret;

	switch (val) {
	case CORE_INIT:
		ret = pci_epf_simple_core_init(epf);
		if (ret)
			return NOTIFY_BAD;
		break;

	case LINK_UP:
		break;

	default:
		dev_err(&epf->dev, "Invalid EPF simple notifier event\n");
		return NOTIFY_BAD;
	}

	return NOTIFY_OK;
}

static int pci_epf_simple_alloc_space(struct pci_epf *epf)
{
	struct pci_epf_simple *epf_simple = epf_get_drvdata(epf);
	struct device *dev = &epf->dev;
	struct pci_epf_bar *epf_bar;
	size_t simple_reg_bar_size;
	void *base;
	int bar, add;
	enum pci_barno simple_reg_bar = epf_simple->simple_reg_bar;
	const struct pci_epc_features *epc_features;
	size_t simple_reg_size;

	dev_warn(dev, "alloc space called\n");

	epc_features = epf_simple->epc_features;

	simple_reg_bar_size = ALIGN(sizeof(struct pci_epf_simple_reg), 128);

	simple_reg_size = simple_reg_bar_size;

	if (epc_features->bar_fixed_size[simple_reg_bar]) {
		if (simple_reg_size > bar_size[simple_reg_bar])
			return -ENOMEM;
		simple_reg_size = bar_size[simple_reg_bar];
	}

	base = pci_epf_alloc_space(epf, simple_reg_size, simple_reg_bar,
				   epc_features->align, PRIMARY_INTERFACE);
	if (!base) {
		dev_err(dev, "Failed to allocated register space\n");
		return -ENOMEM;
	}
	epf_simple->reg[simple_reg_bar] = base;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar += add) {
		epf_bar = &epf->bar[bar];
		add = (epf_bar->flags & PCI_BASE_ADDRESS_MEM_TYPE_64) ? 2 : 1;

		if (bar == simple_reg_bar)
			continue;

		if (!!(epc_features->reserved_bar & (1 << bar)))
			continue;

		base = pci_epf_alloc_space(epf, bar_size[bar], bar,
					   epc_features->align,
					   PRIMARY_INTERFACE);
		if (!base)
			dev_err(dev, "Failed to allocate space for BAR%d\n",
				bar);
		epf_simple->reg[bar] = base;
	}

	return 0;
}

static void pci_epf_configure_bar(struct pci_epf *epf,
				  const struct pci_epc_features *epc_features)
{
	struct pci_epf_bar *epf_bar;
	bool bar_fixed_64bit;
	int i;

	dev_warn(&epf->dev, "configure bar called\n");

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		epf_bar = &epf->bar[i];
		bar_fixed_64bit = !!(epc_features->bar_fixed_64bit & (1 << i));
		if (bar_fixed_64bit)
			epf_bar->flags |= PCI_BASE_ADDRESS_MEM_TYPE_64;
		if (epc_features->bar_fixed_size[i])
			bar_size[i] = epc_features->bar_fixed_size[i];
	}
}

static int pci_epf_simple_bind(struct pci_epf *epf)
{
	int ret;
	struct pci_epf_simple *epf_simple = epf_get_drvdata(epf);
	const struct pci_epc_features *epc_features;
	enum pci_barno simple_reg_bar = BAR_0;
	struct pci_epc *epc = epf->epc;
	bool linkup_notifier = false;
	bool core_init_notifier = false;

	dev_warn(&epf->dev, "bind called\n");

	if (WARN_ON_ONCE(!epc))
		return -EINVAL;

	epc_features = pci_epc_get_features(epc, epf->func_no, epf->vfunc_no);
	if (!epc_features) {
		dev_err(&epf->dev, "epc_features not implemented\n");
		return -EOPNOTSUPP;
	}

	linkup_notifier = epc_features->linkup_notifier;
	core_init_notifier = epc_features->core_init_notifier;
	simple_reg_bar = pci_epc_get_first_free_bar(epc_features);
	if (simple_reg_bar < 0)
		return -EINVAL;
	pci_epf_configure_bar(epf, epc_features);

	epf_simple->simple_reg_bar = simple_reg_bar;
	epf_simple->epc_features = epc_features;

	ret = pci_epf_simple_alloc_space(epf);
	if (ret)
		return ret;

	if (!core_init_notifier) {
		ret = pci_epf_simple_core_init(epf);
		if (ret)
			return ret;
	}

	if (linkup_notifier || core_init_notifier) {
		epf->nb.notifier_call = pci_epf_simple_notifier;
		pci_epc_register_notifier(epc, &epf->nb);
	} else {
		queue_work(kpcie_epfl_simple_workqueue, &epf_simple->cmd_handler.work);
	}

	return 0;
}

static int pci_epf_simple_probe(struct pci_epf *epf)
{
	struct pci_epf_simple *epf_simple;
	struct device *dev = &epf->dev;

	dev_warn(&epf->dev, "probe called\n");

	epf_simple = devm_kzalloc(dev, sizeof(*epf_simple), GFP_KERNEL);
	if (!epf_simple)
		return -ENOMEM;

	epf->header = &simple_header;
	epf_simple->epf = epf;

	INIT_DELAYED_WORK(&epf_simple->cmd_handler, pci_epf_simple_cmd_handler);

	epf_set_drvdata(epf, epf_simple);
	return 0;
}

static struct pci_epf_ops ops = {
	.unbind	= pci_epf_simple_unbind,
	.bind	= pci_epf_simple_bind,
};

static const struct pci_epf_device_id pci_epf_simple_ids[] = {
	{
		.name = "pcie_epf_simple",
	},
	{},
};

static struct pci_epf_driver simple_driver = {
	.driver.name	= "pcie_epf_simple",
	.probe		= pci_epf_simple_probe,
	.id_table	= pci_epf_simple_ids,
	.ops		= &ops,
	.owner		= THIS_MODULE,
};

static int __init pci_epf_simple_init(void)
{
	int ret;

	kpcie_epfl_simple_workqueue = alloc_workqueue("kpcie_epf_simple",
						      WQ_MEM_RECLAIM | WQ_HIGHPRI, 0);
	if (!kpcie_epfl_simple_workqueue) {
		pr_err("Failed to allocate the kpcie_epf_simple work queue\n");
		return -ENOMEM;
	}

	ret = pci_epf_register_driver(&simple_driver);
	if (ret) {
		pr_err("Failed to register pci epf simple driver --> %d\n", ret);
		return ret;
	}

	return 0;
}
module_init(pci_epf_simple_init);

static void __exit pci_epf_simple_exit(void)
{
	if (kpcie_epfl_simple_workqueue)
		destroy_workqueue(kpcie_epfl_simple_workqueue);
	pci_epf_unregister_driver(&simple_driver);
}
module_exit(pci_epf_simple_exit);

MODULE_DESCRIPTION("PCI EPF simple DRIVER");
MODULE_AUTHOR("Rick Wertenbroek <rick.wertenbroek@gmail.com>");
MODULE_LICENSE("GPL v2");
