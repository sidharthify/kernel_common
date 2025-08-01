// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for USB Mass Storage compliant devices
 *
 * Current development and maintenance by:
 *   (c) 1999-2003 Matthew Dharm (mdharm-usb@one-eyed-alien.net)
 *
 * Developed with the assistance of:
 *   (c) 2000 David L. Brown, Jr. (usb-storage@davidb.org)
 *   (c) 2003-2009 Alan Stern (stern@rowland.harvard.edu)
 *
 * Initial work by:
 *   (c) 1999 Michael Gee (michael@linuxspecific.com)
 *
 * usb_device_id support by Adam J. Richter (adam@yggdrasil.com):
 *   (c) 2000 Yggdrasil Computing, Inc.
 *
 * This driver is based on the 'USB Mass Storage Class' document. This
 * describes in detail the protocol used to communicate with such
 * devices.  Clearly, the designers had SCSI and ATAPI commands in
 * mind when they created this document.  The commands are all very
 * similar to commands in the SCSI-II and ATAPI specifications.
 *
 * It is important to note that in a number of cases this class
 * exhibits class-specific exemptions from the USB specification.
 * Notably the usage of NAK, STALL and ACK differs from the norm, in
 * that they are used to communicate wait, failed and OK on commands.
 *
 * Also, for certain devices, the interrupt endpoint is used to convey
 * status of a command.
 */

#ifdef CONFIG_USB_STORAGE_DEBUG
#define DEBUG
#endif

#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/utsname.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>

#include "usb.h"
#include "scsiglue.h"
#include "transport.h"
#include "protocol.h"
#include "debug.h"
#include "initializers.h"

#include "sierra_ms.h"
#include "option_ms.h"

#if IS_ENABLED(CONFIG_USB_UAS)
#include "uas-detect.h"
#endif

#define DRV_NAME "usb-storage"

/* Some informational data */
MODULE_AUTHOR("Matthew Dharm <mdharm-usb@one-eyed-alien.net>");
MODULE_DESCRIPTION("USB Mass Storage driver for Linux");
MODULE_LICENSE("GPL");

static unsigned int delay_use = 1 * MSEC_PER_SEC;

/**
 * parse_delay_str - parse an unsigned decimal integer delay
 * @str: String to parse.
 * @ndecimals: Number of decimal to scale up.
 * @suffix: Suffix string to parse.
 * @val: Where to store the parsed value.
 *
 * Parse an unsigned decimal value in @str, optionally end with @suffix.
 * Stores the parsed value in @val just as it is if @str ends with @suffix.
 * Otherwise store the value scale up by 10^(@ndecimal).
 *
 * Returns 0 on success, a negative error code otherwise.
 */
static int parse_delay_str(const char *str, int ndecimals, const char *suffix,
			unsigned int *val)
{
	int n, n2, l;
	char buf[16];

	l = strlen(suffix);
	n = strlen(str);
	if (n > 0 && str[n - 1] == '\n')
		--n;
	if (n >= l && !strncmp(&str[n - l], suffix, l)) {
		n -= l;
		n2 = 0;
	} else
		n2 = ndecimals;

	if (n + n2 > sizeof(buf) - 1)
		return -EINVAL;

	memcpy(buf, str, n);
	while (n2-- > 0)
		buf[n++] = '0';
	buf[n] = 0;

	return kstrtouint(buf, 10, val);
}

/**
 * format_delay_ms - format an integer value into a delay string
 * @val: The integer value to format, scaled by 10^(@ndecimals).
 * @ndecimals: Number of decimal to scale down.
 * @suffix: Suffix string to format.
 * @str: Where to store the formatted string.
 * @size: The size of buffer for @str.
 *
 * Format an integer value in @val scale down by 10^(@ndecimals) without @suffix
 * if @val is divisible by 10^(@ndecimals).
 * Otherwise format a value in @val just as it is with @suffix
 *
 * Returns the number of characters written into @str.
 */
static int format_delay_ms(unsigned int val, int ndecimals, const char *suffix,
			char *str, int size)
{
	u64 delay_ms = val;
	unsigned int rem = do_div(delay_ms, int_pow(10, ndecimals));
	int ret;

	if (rem)
		ret = scnprintf(str, size, "%u%s\n", val, suffix);
	else
		ret = scnprintf(str, size, "%u\n", (unsigned int)delay_ms);
	return ret;
}

static int delay_use_set(const char *s, const struct kernel_param *kp)
{
	unsigned int delay_ms;
	int ret;

	ret = parse_delay_str(skip_spaces(s), 3, "ms", &delay_ms);
	if (ret < 0)
		return ret;

	*((unsigned int *)kp->arg) = delay_ms;
	return 0;
}

static int delay_use_get(char *s, const struct kernel_param *kp)
{
	unsigned int delay_ms = *((unsigned int *)kp->arg);

	return format_delay_ms(delay_ms, 3, "ms", s, PAGE_SIZE);
}

static const struct kernel_param_ops delay_use_ops = {
	.set = delay_use_set,
	.get = delay_use_get,
};
module_param_cb(delay_use, &delay_use_ops, &delay_use, 0644);
MODULE_PARM_DESC(delay_use, "time to delay before using a new device");

static char quirks[128];
module_param_string(quirks, quirks, sizeof(quirks), S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(quirks, "supplemental list of device IDs and their quirks");


/*
 * The entries in this table correspond, line for line,
 * with the entries in usb_storage_usb_ids[], defined in usual-tables.c.
 */

/*
 *The vendor name should be kept at eight characters or less, and
 * the product name should be kept at 16 characters or less. If a device
 * has the US_FL_FIX_INQUIRY flag, then the vendor and product names
 * normally generated by a device through the INQUIRY response will be
 * taken from this list, and this is the reason for the above size
 * restriction. However, if the flag is not present, then you
 * are free to use as many characters as you like.
 */

#define UNUSUAL_DEV(idVendor, idProduct, bcdDeviceMin, bcdDeviceMax, \
		    vendor_name, product_name, use_protocol, use_transport, \
		    init_function, Flags) \
{ \
	.vendorName = vendor_name,	\
	.productName = product_name,	\
	.useProtocol = use_protocol,	\
	.useTransport = use_transport,	\
	.initFunction = init_function,	\
}

#define COMPLIANT_DEV	UNUSUAL_DEV

#define USUAL_DEV(use_protocol, use_transport) \
{ \
	.useProtocol = use_protocol,	\
	.useTransport = use_transport,	\
}

#define UNUSUAL_VENDOR_INTF(idVendor, cl, sc, pr, \
		vendor_name, product_name, use_protocol, use_transport, \
		init_function, Flags) \
{ \
	.vendorName = vendor_name,	\
	.productName = product_name,	\
	.useProtocol = use_protocol,	\
	.useTransport = use_transport,	\
	.initFunction = init_function,	\
}

static const struct us_unusual_dev us_unusual_dev_list[] = {
#	include "unusual_devs.h"
	{ }		/* Terminating entry */
};

static const struct us_unusual_dev for_dynamic_ids =
		USUAL_DEV(USB_SC_SCSI, USB_PR_BULK);

#undef UNUSUAL_DEV
#undef COMPLIANT_DEV
#undef USUAL_DEV
#undef UNUSUAL_VENDOR_INTF

#ifdef CONFIG_LOCKDEP

static struct lock_class_key us_interface_key[USB_MAXINTERFACES];

static void us_set_lock_class(struct mutex *mutex,
		struct usb_interface *intf)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_host_config *config = udev->actconfig;
	int i;

	for (i = 0; i < config->desc.bNumInterfaces; i++) {
		if (config->interface[i] == intf)
			break;
	}

	BUG_ON(i == config->desc.bNumInterfaces);

	lockdep_set_class(mutex, &us_interface_key[i]);
}

#else

static void us_set_lock_class(struct mutex *mutex,
		struct usb_interface *intf)
{
}

#endif

#ifdef CONFIG_PM	/* Minimal support for suspend and resume */

int usb_stor_suspend(struct usb_interface *iface, pm_message_t message)
{
	struct us_data *us = usb_get_intfdata(iface);

	/* Wait until no command is running */
	mutex_lock(&us->dev_mutex);

	if (us->suspend_resume_hook)
		(us->suspend_resume_hook)(us, US_SUSPEND);

	/*
	 * When runtime PM is working, we'll set a flag to indicate
	 * whether we should autoresume when a SCSI request arrives.
	 */

	mutex_unlock(&us->dev_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(usb_stor_suspend);

int usb_stor_resume(struct usb_interface *iface)
{
	struct us_data *us = usb_get_intfdata(iface);

	mutex_lock(&us->dev_mutex);

	if (us->suspend_resume_hook)
		(us->suspend_resume_hook)(us, US_RESUME);

	mutex_unlock(&us->dev_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(usb_stor_resume);

int usb_stor_reset_resume(struct usb_interface *iface)
{
	struct us_data *us = usb_get_intfdata(iface);

	/* Report the reset to the SCSI core */
	usb_stor_report_bus_reset(us);

	/*
	 * If any of the subdrivers implemented a reinitialization scheme,
	 * this is where the callback would be invoked.
	 */
	return 0;
}
EXPORT_SYMBOL_GPL(usb_stor_reset_resume);

#endif /* CONFIG_PM */

/*
 * The next two routines get called just before and just after
 * a USB port reset, whether from this driver or a different one.
 */

int usb_stor_pre_reset(struct usb_interface *iface)
{
	struct us_data *us = usb_get_intfdata(iface);

	/* Make sure no command runs during the reset */
	mutex_lock(&us->dev_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(usb_stor_pre_reset);

int usb_stor_post_reset(struct usb_interface *iface)
{
	struct us_data *us = usb_get_intfdata(iface);

	/* Report the reset to the SCSI core */
	usb_stor_report_bus_reset(us);

	/*
	 * If any of the subdrivers implemented a reinitialization scheme,
	 * this is where the callback would be invoked.
	 */

	mutex_unlock(&us->dev_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(usb_stor_post_reset);

/*
 * fill_inquiry_response takes an unsigned char array (which must
 * be at least 36 characters) and populates the vendor name,
 * product name, and revision fields. Then the array is copied
 * into the SCSI command's response buffer (oddly enough
 * called request_buffer). data_len contains the length of the
 * data array, which again must be at least 36.
 */

void fill_inquiry_response(struct us_data *us, unsigned char *data,
		unsigned int data_len)
{
	if (data_len < 36) /* You lose. */
		return;

	memset(data+8, ' ', 28);
	if (data[0]&0x20) { /*
			     * USB device currently not connected. Return
			     * peripheral qualifier 001b ("...however, the
			     * physical device is not currently connected
			     * to this logical unit") and leave vendor and
			     * product identification empty. ("If the target
			     * does store some of the INQUIRY data on the
			     * device, it may return zeros or ASCII spaces
			     * (20h) in those fields until the data is
			     * available from the device.").
			     */
	} else {
		u16 bcdDevice = le16_to_cpu(us->pusb_dev->descriptor.bcdDevice);
		int n;

		n = strlen(us->unusual_dev->vendorName);
		memcpy(data+8, us->unusual_dev->vendorName, min(8, n));
		n = strlen(us->unusual_dev->productName);
		memcpy(data+16, us->unusual_dev->productName, min(16, n));

		data[32] = 0x30 + ((bcdDevice>>12) & 0x0F);
		data[33] = 0x30 + ((bcdDevice>>8) & 0x0F);
		data[34] = 0x30 + ((bcdDevice>>4) & 0x0F);
		data[35] = 0x30 + ((bcdDevice) & 0x0F);
	}

	usb_stor_set_xfer_buf(data, data_len, us->srb);
}
EXPORT_SYMBOL_GPL(fill_inquiry_response);

static int usb_stor_control_thread(void * __us)
{
	struct us_data *us = (struct us_data *)__us;
	struct Scsi_Host *host = us_to_host(us);
	struct scsi_cmnd *srb;

	for (;;) {
		usb_stor_dbg(us, "*** thread sleeping\n");
		if (wait_for_completion_interruptible(&us->cmnd_ready))
			break;

		usb_stor_dbg(us, "*** thread awakened\n");

		/* lock the device pointers */
		mutex_lock(&(us->dev_mutex));

		/* lock access to the state */
		scsi_lock(host);

		/* When we are called with no command pending, we're done */
		srb = us->srb;
		if (srb == NULL) {
			scsi_unlock(host);
			mutex_unlock(&us->dev_mutex);
			usb_stor_dbg(us, "-- exiting\n");
			break;
		}

		/* has the command timed out *already* ? */
		if (test_bit(US_FLIDX_TIMED_OUT, &us->dflags)) {
			srb->result = DID_ABORT << 16;
			goto SkipForAbort;
		}

		scsi_unlock(host);

		/*
		 * reject the command if the direction indicator
		 * is UNKNOWN
		 */
		if (srb->sc_data_direction == DMA_BIDIRECTIONAL) {
			usb_stor_dbg(us, "UNKNOWN data direction\n");
			srb->result = DID_ERROR << 16;
		}

		/*
		 * reject if target != 0 or if LUN is higher than
		 * the maximum known LUN
		 */
		else if (srb->device->id &&
				!(us->fflags & US_FL_SCM_MULT_TARG)) {
			usb_stor_dbg(us, "Bad target number (%d:%llu)\n",
				     srb->device->id,
				     srb->device->lun);
			srb->result = DID_BAD_TARGET << 16;
		}

		else if (srb->device->lun > us->max_lun) {
			usb_stor_dbg(us, "Bad LUN (%d:%llu)\n",
				     srb->device->id,
				     srb->device->lun);
			srb->result = DID_BAD_TARGET << 16;
		}

		/*
		 * Handle those devices which need us to fake
		 * their inquiry data
		 */
		else if ((srb->cmnd[0] == INQUIRY) &&
			    (us->fflags & US_FL_FIX_INQUIRY)) {
			unsigned char data_ptr[36] = {
			    0x00, 0x80, 0x02, 0x02,
			    0x1F, 0x00, 0x00, 0x00};

			usb_stor_dbg(us, "Faking INQUIRY command\n");
			fill_inquiry_response(us, data_ptr, 36);
			srb->result = SAM_STAT_GOOD;
		}

		/* we've got a command, let's do it! */
		else {
			US_DEBUG(usb_stor_show_command(us, srb));
			us->proto_handler(srb, us);
			usb_mark_last_busy(us->pusb_dev);
		}

		/* lock access to the state */
		scsi_lock(host);

		/* was the command aborted? */
		if (srb->result == DID_ABORT << 16) {
SkipForAbort:
			usb_stor_dbg(us, "scsi command aborted\n");
			srb = NULL;	/* Don't call scsi_done() */
		}

		/*
		 * If an abort request was received we need to signal that
		 * the abort has finished.  The proper test for this is
		 * the TIMED_OUT flag, not srb->result == DID_ABORT, because
		 * the timeout might have occurred after the command had
		 * already completed with a different result code.
		 */
		if (test_bit(US_FLIDX_TIMED_OUT, &us->dflags)) {
			complete(&(us->notify));

			/* Allow USB transfers to resume */
			clear_bit(US_FLIDX_ABORTING, &us->dflags);
			clear_bit(US_FLIDX_TIMED_OUT, &us->dflags);
		}

		/* finished working on this command */
		us->srb = NULL;
		scsi_unlock(host);

		/* unlock the device pointers */
		mutex_unlock(&us->dev_mutex);

		/* now that the locks are released, notify the SCSI core */
		if (srb) {
			usb_stor_dbg(us, "scsi cmd done, result=0x%x\n",
					srb->result);
			scsi_done_direct(srb);
		}
	} /* for (;;) */

	/* Wait until we are told to stop */
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

/***********************************************************************
 * Device probing and disconnecting
 ***********************************************************************/

/* Associate our private data with the USB device */
static int associate_dev(struct us_data *us, struct usb_interface *intf)
{
	/* Fill in the device-related fields */
	us->pusb_dev = interface_to_usbdev(intf);
	us->pusb_intf = intf;
	us->ifnum = intf->cur_altsetting->desc.bInterfaceNumber;
	usb_stor_dbg(us, "Vendor: 0x%04x, Product: 0x%04x, Revision: 0x%04x\n",
		     le16_to_cpu(us->pusb_dev->descriptor.idVendor),
		     le16_to_cpu(us->pusb_dev->descriptor.idProduct),
		     le16_to_cpu(us->pusb_dev->descriptor.bcdDevice));
	usb_stor_dbg(us, "Interface Subclass: 0x%02x, Protocol: 0x%02x\n",
		     intf->cur_altsetting->desc.bInterfaceSubClass,
		     intf->cur_altsetting->desc.bInterfaceProtocol);

	/* Store our private data in the interface */
	usb_set_intfdata(intf, us);

	/* Allocate the control/setup and DMA-mapped buffers */
	us->cr = kmalloc(sizeof(*us->cr), GFP_KERNEL);
	if (!us->cr)
		return -ENOMEM;

	us->iobuf = usb_alloc_coherent(us->pusb_dev, US_IOBUF_SIZE,
			GFP_KERNEL, &us->iobuf_dma);
	if (!us->iobuf) {
		usb_stor_dbg(us, "I/O buffer allocation failed\n");
		return -ENOMEM;
	}
	return 0;
}

/* Works only for digits and letters, but small and fast */
#define TOLOWER(x) ((x) | 0x20)

/* Adjust device flags based on the "quirks=" module parameter */
void usb_stor_adjust_quirks(struct usb_device *udev, unsigned long *fflags)
{
	char *p;
	u16 vid = le16_to_cpu(udev->descriptor.idVendor);
	u16 pid = le16_to_cpu(udev->descriptor.idProduct);
	unsigned f = 0;
	unsigned int mask = (US_FL_SANE_SENSE | US_FL_BAD_SENSE |
			US_FL_FIX_CAPACITY | US_FL_IGNORE_UAS |
			US_FL_CAPACITY_HEURISTICS | US_FL_IGNORE_DEVICE |
			US_FL_NOT_LOCKABLE | US_FL_MAX_SECTORS_64 |
			US_FL_CAPACITY_OK | US_FL_IGNORE_RESIDUE |
			US_FL_SINGLE_LUN | US_FL_NO_WP_DETECT |
			US_FL_NO_READ_DISC_INFO | US_FL_NO_READ_CAPACITY_16 |
			US_FL_INITIAL_READ10 | US_FL_WRITE_CACHE |
			US_FL_NO_ATA_1X | US_FL_NO_REPORT_OPCODES |
			US_FL_MAX_SECTORS_240 | US_FL_NO_REPORT_LUNS |
			US_FL_ALWAYS_SYNC);

	p = quirks;
	while (*p) {
		/* Each entry consists of VID:PID:flags */
		if (vid == simple_strtoul(p, &p, 16) &&
				*p == ':' &&
				pid == simple_strtoul(p+1, &p, 16) &&
				*p == ':')
			break;

		/* Move forward to the next entry */
		while (*p) {
			if (*p++ == ',')
				break;
		}
	}
	if (!*p)	/* No match */
		return;

	/* Collect the flags */
	while (*++p && *p != ',') {
		switch (TOLOWER(*p)) {
		case 'a':
			f |= US_FL_SANE_SENSE;
			break;
		case 'b':
			f |= US_FL_BAD_SENSE;
			break;
		case 'c':
			f |= US_FL_FIX_CAPACITY;
			break;
		case 'd':
			f |= US_FL_NO_READ_DISC_INFO;
			break;
		case 'e':
			f |= US_FL_NO_READ_CAPACITY_16;
			break;
		case 'f':
			f |= US_FL_NO_REPORT_OPCODES;
			break;
		case 'g':
			f |= US_FL_MAX_SECTORS_240;
			break;
		case 'h':
			f |= US_FL_CAPACITY_HEURISTICS;
			break;
		case 'i':
			f |= US_FL_IGNORE_DEVICE;
			break;
		case 'j':
			f |= US_FL_NO_REPORT_LUNS;
			break;
		case 'k':
			f |= US_FL_NO_SAME;
			break;
		case 'l':
			f |= US_FL_NOT_LOCKABLE;
			break;
		case 'm':
			f |= US_FL_MAX_SECTORS_64;
			break;
		case 'n':
			f |= US_FL_INITIAL_READ10;
			break;
		case 'o':
			f |= US_FL_CAPACITY_OK;
			break;
		case 'p':
			f |= US_FL_WRITE_CACHE;
			break;
		case 'r':
			f |= US_FL_IGNORE_RESIDUE;
			break;
		case 's':
			f |= US_FL_SINGLE_LUN;
			break;
		case 't':
			f |= US_FL_NO_ATA_1X;
			break;
		case 'u':
			f |= US_FL_IGNORE_UAS;
			break;
		case 'w':
			f |= US_FL_NO_WP_DETECT;
			break;
		case 'y':
			f |= US_FL_ALWAYS_SYNC;
			break;
		/* Ignore unrecognized flag characters */
		}
	}
	*fflags = (*fflags & ~mask) | f;
}
EXPORT_SYMBOL_GPL(usb_stor_adjust_quirks);

/* Get the unusual_devs entries and the string descriptors */
static int get_device_info(struct us_data *us, const struct usb_device_id *id,
		const struct us_unusual_dev *unusual_dev)
{
	struct usb_device *dev = us->pusb_dev;
	struct usb_interface_descriptor *idesc =
		&us->pusb_intf->cur_altsetting->desc;
	struct device *pdev = &us->pusb_intf->dev;

	/* Store the entries */
	us->unusual_dev = unusual_dev;
	us->subclass = (unusual_dev->useProtocol == USB_SC_DEVICE) ?
			idesc->bInterfaceSubClass :
			unusual_dev->useProtocol;
	us->protocol = (unusual_dev->useTransport == USB_PR_DEVICE) ?
			idesc->bInterfaceProtocol :
			unusual_dev->useTransport;
	us->fflags = id->driver_info;
	usb_stor_adjust_quirks(us->pusb_dev, &us->fflags);

	if (us->fflags & US_FL_IGNORE_DEVICE) {
		dev_info(pdev, "device ignored\n");
		return -ENODEV;
	}

	/*
	 * This flag is only needed when we're in high-speed, so let's
	 * disable it if we're in full-speed
	 */
	if (dev->speed != USB_SPEED_HIGH)
		us->fflags &= ~US_FL_GO_SLOW;

	if (us->fflags)
		dev_info(pdev, "Quirks match for vid %04x pid %04x: %lx\n",
				le16_to_cpu(dev->descriptor.idVendor),
				le16_to_cpu(dev->descriptor.idProduct),
				us->fflags);

	/*
	 * Log a message if a non-generic unusual_dev entry contains an
	 * unnecessary subclass or protocol override.  This may stimulate
	 * reports from users that will help us remove unneeded entries
	 * from the unusual_devs.h table.
	 */
	if (id->idVendor || id->idProduct) {
		static const char *msgs[3] = {
			"an unneeded SubClass entry",
			"an unneeded Protocol entry",
			"unneeded SubClass and Protocol entries"};
		struct usb_device_descriptor *ddesc = &dev->descriptor;
		int msg = -1;

		if (unusual_dev->useProtocol != USB_SC_DEVICE &&
			us->subclass == idesc->bInterfaceSubClass)
			msg += 1;
		if (unusual_dev->useTransport != USB_PR_DEVICE &&
			us->protocol == idesc->bInterfaceProtocol)
			msg += 2;
		if (msg >= 0 && !(us->fflags & US_FL_NEED_OVERRIDE))
			dev_notice(pdev, "This device "
					"(%04x,%04x,%04x S %02x P %02x)"
					" has %s in unusual_devs.h (kernel"
					" %s)\n"
					"   Please send a copy of this message to "
					"<linux-usb@vger.kernel.org> and "
					"<usb-storage@lists.one-eyed-alien.net>\n",
					le16_to_cpu(ddesc->idVendor),
					le16_to_cpu(ddesc->idProduct),
					le16_to_cpu(ddesc->bcdDevice),
					idesc->bInterfaceSubClass,
					idesc->bInterfaceProtocol,
					msgs[msg],
					utsname()->release);
	}

	return 0;
}

/* Get the transport settings */
static void get_transport(struct us_data *us)
{
	switch (us->protocol) {
	case USB_PR_CB:
		us->transport_name = "Control/Bulk";
		us->transport = usb_stor_CB_transport;
		us->transport_reset = usb_stor_CB_reset;
		us->max_lun = 7;
		break;

	case USB_PR_CBI:
		us->transport_name = "Control/Bulk/Interrupt";
		us->transport = usb_stor_CB_transport;
		us->transport_reset = usb_stor_CB_reset;
		us->max_lun = 7;
		break;

	case USB_PR_BULK:
		us->transport_name = "Bulk";
		us->transport = usb_stor_Bulk_transport;
		us->transport_reset = usb_stor_Bulk_reset;
		break;
	}
}

/* Get the protocol settings */
static void get_protocol(struct us_data *us)
{
	switch (us->subclass) {
	case USB_SC_RBC:
		us->protocol_name = "Reduced Block Commands (RBC)";
		us->proto_handler = usb_stor_transparent_scsi_command;
		break;

	case USB_SC_8020:
		us->protocol_name = "8020i";
		us->proto_handler = usb_stor_pad12_command;
		us->max_lun = 0;
		break;

	case USB_SC_QIC:
		us->protocol_name = "QIC-157";
		us->proto_handler = usb_stor_pad12_command;
		us->max_lun = 0;
		break;

	case USB_SC_8070:
		us->protocol_name = "8070i";
		us->proto_handler = usb_stor_pad12_command;
		us->max_lun = 0;
		break;

	case USB_SC_SCSI:
		us->protocol_name = "Transparent SCSI";
		us->proto_handler = usb_stor_transparent_scsi_command;
		break;

	case USB_SC_UFI:
		us->protocol_name = "Uniform Floppy Interface (UFI)";
		us->proto_handler = usb_stor_ufi_command;
		break;
	}
}

/* Get the pipe settings */
static int get_pipes(struct us_data *us)
{
	struct usb_host_interface *alt = us->pusb_intf->cur_altsetting;
	struct usb_endpoint_descriptor *ep_in;
	struct usb_endpoint_descriptor *ep_out;
	struct usb_endpoint_descriptor *ep_int;
	int res;

	/*
	 * Find the first endpoint of each type we need.
	 * We are expecting a minimum of 2 endpoints - in and out (bulk).
	 * An optional interrupt-in is OK (necessary for CBI protocol).
	 * We will ignore any others.
	 */
	res = usb_find_common_endpoints(alt, &ep_in, &ep_out, NULL, NULL);
	if (res) {
		usb_stor_dbg(us, "bulk endpoints not found\n");
		return res;
	}

	res = usb_find_int_in_endpoint(alt, &ep_int);
	if (res && us->protocol == USB_PR_CBI) {
		usb_stor_dbg(us, "interrupt endpoint not found\n");
		return res;
	}

	/* Calculate and store the pipe values */
	us->send_ctrl_pipe = usb_sndctrlpipe(us->pusb_dev, 0);
	us->recv_ctrl_pipe = usb_rcvctrlpipe(us->pusb_dev, 0);
	us->send_bulk_pipe = usb_sndbulkpipe(us->pusb_dev,
		usb_endpoint_num(ep_out));
	us->recv_bulk_pipe = usb_rcvbulkpipe(us->pusb_dev,
		usb_endpoint_num(ep_in));
	if (ep_int) {
		us->recv_intr_pipe = usb_rcvintpipe(us->pusb_dev,
			usb_endpoint_num(ep_int));
		us->ep_bInterval = ep_int->bInterval;
	}
	return 0;
}

/* Initialize all the dynamic resources we need */
static int usb_stor_acquire_resources(struct us_data *us)
{
	int p;
	struct task_struct *th;

	us->current_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!us->current_urb)
		return -ENOMEM;

	/*
	 * Just before we start our control thread, initialize
	 * the device if it needs initialization
	 */
	if (us->unusual_dev->initFunction) {
		p = us->unusual_dev->initFunction(us);
		if (p)
			return p;
	}

	/* Start up our control thread */
	th = kthread_run(usb_stor_control_thread, us, "usb-storage");
	if (IS_ERR(th)) {
		dev_warn(&us->pusb_intf->dev,
				"Unable to start control thread\n");
		return PTR_ERR(th);
	}
	us->ctl_thread = th;

	return 0;
}

/* Release all our dynamic resources */
static void usb_stor_release_resources(struct us_data *us)
{
	/*
	 * Tell the control thread to exit.  The SCSI host must
	 * already have been removed and the DISCONNECTING flag set
	 * so that we won't accept any more commands.
	 */
	usb_stor_dbg(us, "-- sending exit command to thread\n");
	complete(&us->cmnd_ready);
	if (us->ctl_thread)
		kthread_stop(us->ctl_thread);

	/* Call the destructor routine, if it exists */
	if (us->extra_destructor) {
		usb_stor_dbg(us, "-- calling extra_destructor()\n");
		us->extra_destructor(us->extra);
	}

	/* Free the extra data and the URB */
	kfree(us->extra);
	usb_free_urb(us->current_urb);
}

/* Dissociate from the USB device */
static void dissociate_dev(struct us_data *us)
{
	/* Free the buffers */
	kfree(us->cr);
	usb_free_coherent(us->pusb_dev, US_IOBUF_SIZE, us->iobuf, us->iobuf_dma);

	/* Remove our private data from the interface */
	usb_set_intfdata(us->pusb_intf, NULL);
}

/*
 * First stage of disconnect processing: stop SCSI scanning,
 * remove the host, and stop accepting new commands
 */
static void quiesce_and_remove_host(struct us_data *us)
{
	struct Scsi_Host *host = us_to_host(us);

	/* If the device is really gone, cut short reset delays */
	if (us->pusb_dev->state == USB_STATE_NOTATTACHED) {
		set_bit(US_FLIDX_DISCONNECTING, &us->dflags);
		wake_up(&us->delay_wait);
	}

	/*
	 * Prevent SCSI scanning (if it hasn't started yet)
	 * or wait for the SCSI-scanning routine to stop.
	 */
	cancel_delayed_work_sync(&us->scan_dwork);

	/* Balance autopm calls if scanning was cancelled */
	if (test_bit(US_FLIDX_SCAN_PENDING, &us->dflags))
		usb_autopm_put_interface_no_suspend(us->pusb_intf);

	/*
	 * Removing the host will perform an orderly shutdown: caches
	 * synchronized, disks spun down, etc.
	 */
	scsi_remove_host(host);

	/*
	 * Prevent any new commands from being accepted and cut short
	 * reset delays.
	 */
	scsi_lock(host);
	set_bit(US_FLIDX_DISCONNECTING, &us->dflags);
	scsi_unlock(host);
	wake_up(&us->delay_wait);
}

/* Second stage of disconnect processing: deallocate all resources */
static void release_everything(struct us_data *us)
{
	usb_stor_release_resources(us);
	dissociate_dev(us);

	/*
	 * Drop our reference to the host; the SCSI core will free it
	 * (and "us" along with it) when the refcount becomes 0.
	 */
	scsi_host_put(us_to_host(us));
}

/* Delayed-work routine to carry out SCSI-device scanning */
static void usb_stor_scan_dwork(struct work_struct *work)
{
	struct us_data *us = container_of(work, struct us_data,
			scan_dwork.work);
	struct device *dev = &us->pusb_intf->dev;

	dev_dbg(dev, "starting scan\n");

	/* For bulk-only devices, determine the max LUN value */
	if (us->protocol == USB_PR_BULK &&
	    !(us->fflags & US_FL_SINGLE_LUN) &&
	    !(us->fflags & US_FL_SCM_MULT_TARG)) {
		mutex_lock(&us->dev_mutex);
		us->max_lun = usb_stor_Bulk_max_lun(us);
		/*
		 * Allow proper scanning of devices that present more than 8 LUNs
		 * While not affecting other devices that may need the previous
		 * behavior
		 */
		if (us->max_lun >= 8)
			us_to_host(us)->max_lun = us->max_lun+1;
		mutex_unlock(&us->dev_mutex);
	}
	scsi_scan_host(us_to_host(us));
	dev_dbg(dev, "scan complete\n");

	/* Should we unbind if no devices were detected? */

	usb_autopm_put_interface(us->pusb_intf);
	clear_bit(US_FLIDX_SCAN_PENDING, &us->dflags);
}

static unsigned int usb_stor_sg_tablesize(struct usb_interface *intf)
{
	struct usb_device *usb_dev = interface_to_usbdev(intf);

	if (usb_dev->bus->sg_tablesize) {
		return usb_dev->bus->sg_tablesize;
	}
	return SG_ALL;
}

/* First part of general USB mass-storage probing */
int usb_stor_probe1(struct us_data **pus,
		struct usb_interface *intf,
		const struct usb_device_id *id,
		const struct us_unusual_dev *unusual_dev,
		struct scsi_host_template *sht)
{
	struct Scsi_Host *host;
	struct us_data *us;
	int result;

	dev_info(&intf->dev, "USB Mass Storage device detected\n");

	/*
	 * Ask the SCSI layer to allocate a host structure, with extra
	 * space at the end for our private us_data structure.
	 */
	host = scsi_host_alloc(sht, sizeof(*us));
	if (!host) {
		dev_warn(&intf->dev, "Unable to allocate the scsi host\n");
		return -ENOMEM;
	}

	/*
	 * Allow 16-byte CDBs and thus > 2TB
	 */
	host->max_cmd_len = 16;
	host->sg_tablesize = usb_stor_sg_tablesize(intf);
	*pus = us = host_to_us(host);
	mutex_init(&(us->dev_mutex));
	us_set_lock_class(&us->dev_mutex, intf);
	init_completion(&us->cmnd_ready);
	init_completion(&(us->notify));
	init_waitqueue_head(&us->delay_wait);
	INIT_DELAYED_WORK(&us->scan_dwork, usb_stor_scan_dwork);

	/* Associate the us_data structure with the USB device */
	result = associate_dev(us, intf);
	if (result)
		goto BadDevice;

	/* Get the unusual_devs entries and the descriptors */
	result = get_device_info(us, id, unusual_dev);
	if (result)
		goto BadDevice;

	/* Get standard transport and protocol settings */
	get_transport(us);
	get_protocol(us);

	/*
	 * Give the caller a chance to fill in specialized transport
	 * or protocol settings.
	 */
	return 0;

BadDevice:
	usb_stor_dbg(us, "storage_probe() failed\n");
	release_everything(us);
	return result;
}
EXPORT_SYMBOL_GPL(usb_stor_probe1);

/* Second part of general USB mass-storage probing */
int usb_stor_probe2(struct us_data *us)
{
	int result;
	struct device *dev = &us->pusb_intf->dev;

	/* Make sure the transport and protocol have both been set */
	if (!us->transport || !us->proto_handler) {
		result = -ENXIO;
		goto BadDevice;
	}
	usb_stor_dbg(us, "Transport: %s\n", us->transport_name);
	usb_stor_dbg(us, "Protocol: %s\n", us->protocol_name);

	if (us->fflags & US_FL_SCM_MULT_TARG) {
		/*
		 * SCM eUSCSI bridge devices can have different numbers
		 * of LUNs on different targets; allow all to be probed.
		 */
		us->max_lun = 7;
		/* The eUSCSI itself has ID 7, so avoid scanning that */
		us_to_host(us)->this_id = 7;
		/* max_id is 8 initially, so no need to set it here */
	} else {
		/* In the normal case there is only a single target */
		us_to_host(us)->max_id = 1;
		/*
		 * Like Windows, we won't store the LUN bits in CDB[1] for
		 * SCSI-2 devices using the Bulk-Only transport (even though
		 * this violates the SCSI spec).
		 */
		if (us->transport == usb_stor_Bulk_transport)
			us_to_host(us)->no_scsi2_lun_in_cdb = 1;
	}

	/* fix for single-lun devices */
	if (us->fflags & US_FL_SINGLE_LUN)
		us->max_lun = 0;

	/* Find the endpoints and calculate pipe values */
	result = get_pipes(us);
	if (result)
		goto BadDevice;

	/*
	 * If the device returns invalid data for the first READ(10)
	 * command, indicate the command should be retried.
	 */
	if (us->fflags & US_FL_INITIAL_READ10)
		set_bit(US_FLIDX_REDO_READ10, &us->dflags);

	/* Acquire all the other resources and add the host */
	result = usb_stor_acquire_resources(us);
	if (result)
		goto BadDevice;
	usb_autopm_get_interface_no_resume(us->pusb_intf);
	snprintf(us->scsi_name, sizeof(us->scsi_name), "usb-storage %s",
					dev_name(&us->pusb_intf->dev));
	result = scsi_add_host(us_to_host(us), dev);
	if (result) {
		dev_warn(dev,
				"Unable to add the scsi host\n");
		goto HostAddErr;
	}

	/* Submit the delayed_work for SCSI-device scanning */
	set_bit(US_FLIDX_SCAN_PENDING, &us->dflags);

	if (delay_use > 0)
		dev_dbg(dev, "waiting for device to settle before scanning\n");
	queue_delayed_work(system_freezable_wq, &us->scan_dwork,
			msecs_to_jiffies(delay_use));
	return 0;

	/* We come here if there are any problems */
HostAddErr:
	usb_autopm_put_interface_no_suspend(us->pusb_intf);
BadDevice:
	usb_stor_dbg(us, "storage_probe() failed\n");
	release_everything(us);
	return result;
}
EXPORT_SYMBOL_GPL(usb_stor_probe2);

/* Handle a USB mass-storage disconnect */
void usb_stor_disconnect(struct usb_interface *intf)
{
	struct us_data *us = usb_get_intfdata(intf);

	quiesce_and_remove_host(us);
	release_everything(us);
}
EXPORT_SYMBOL_GPL(usb_stor_disconnect);

static struct scsi_host_template usb_stor_host_template;

/* The main probe routine for standard devices */
static int storage_probe(struct usb_interface *intf,
			 const struct usb_device_id *id)
{
	const struct us_unusual_dev *unusual_dev;
	struct us_data *us;
	int result;
	int size;

	/* If uas is enabled and this device can do uas then ignore it. */
#if IS_ENABLED(CONFIG_USB_UAS)
	if (uas_use_uas_driver(intf, id, NULL))
		return -ENXIO;
#endif

	/*
	 * If the device isn't standard (is handled by a subdriver
	 * module) then don't accept it.
	 */
	if (usb_usual_ignore_device(intf))
		return -ENXIO;

	/*
	 * Call the general probe procedures.
	 *
	 * The unusual_dev_list array is parallel to the usb_storage_usb_ids
	 * table, so we use the index of the id entry to find the
	 * corresponding unusual_devs entry.
	 */

	size = ARRAY_SIZE(us_unusual_dev_list);
	if (id >= usb_storage_usb_ids && id < usb_storage_usb_ids + size) {
		unusual_dev = (id - usb_storage_usb_ids) + us_unusual_dev_list;
	} else {
		unusual_dev = &for_dynamic_ids;

		dev_dbg(&intf->dev, "Use Bulk-Only transport with the Transparent SCSI protocol for dynamic id: 0x%04x 0x%04x\n",
			id->idVendor, id->idProduct);
	}

	result = usb_stor_probe1(&us, intf, id, unusual_dev,
				 &usb_stor_host_template);
	if (result)
		return result;

	/* No special transport or protocol settings in the main module */

	result = usb_stor_probe2(us);
	return result;
}

static struct usb_driver usb_storage_driver = {
	.name =		DRV_NAME,
	.probe =	storage_probe,
	.disconnect =	usb_stor_disconnect,
	.suspend =	usb_stor_suspend,
	.resume =	usb_stor_resume,
	.reset_resume =	usb_stor_reset_resume,
	.pre_reset =	usb_stor_pre_reset,
	.post_reset =	usb_stor_post_reset,
	.id_table =	usb_storage_usb_ids,
	.supports_autosuspend = 1,
	.soft_unbind =	1,
};

module_usb_stor_driver(usb_storage_driver, usb_stor_host_template, DRV_NAME);
