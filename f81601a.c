#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/can/dev.h>
#include <linux/io.h>
#include <linux/version.h>
#include <linux/irqreturn.h>
#include <linux/can/dev.h>
#include <linux/can/platform/sja1000.h>
#include <linux/kfifo.h>

#define DIABLE_CANLED 1 // for RHEL9 kernel 5.14

#define DRV_VER "v1.03-20250515v2"
#define DRV_NAME "f81601a"

#ifndef GENMASK
#define GENMASK(h, l) \
	(((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#endif

#ifndef DEVICE_ATTR_RO
#define DEVICE_ATTR_RO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RO(_name)
#endif

#define F81601_FORCE_96MHZ 0
#define F81601_FORCE_MAX_SPEED 0

#define F81601_DIVIDE_CLK 1 // 1x
#define F81601_CAN_CLK_DIV2 0 // 1: reg=0, 0: reg=1
#define F81601_CAN_CLK_OUT_PLL_SEL 1 // 1: =(reg1), 0: /2(reg0)

#define F81601_PCI_MAX_CHAN 2
#define F81601_REG_SAVE_SIZE 0x20
#define F81601_DECODE_REG 0x209
#define F81601_TX_GUARD_TIME msecs_to_jiffies(100 /*00*/)
//#define F81601_RX_GUARD_TIME	usecs_to_jiffies(50)
#define F81601_RX_GUARD_TIME 0UL // 100L // us
#define F81601_BUSOFF_GUARD_TIME msecs_to_jiffies(100)
#define DEBUG_IRQ_DELAY 0
#define RMC_CHANGE_RELEASE 1

#define F81601_USE_TASKLET 1
#define F81601_NAPI_RX 4 //14
#define F81601_EXTEND_BTR 1

#define USE_CUSTOM_SJA1000

#define F81601_IS_TXING BIT(0)

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0)
#define CAN_CTRLMODE_PRESUME_ACK 0x40 /* Ignore missing CAN ACKs */
#endif

#define SJA1000_ECHO_SKB_MAX 2 /* the SJA1000 has one TX buffer object */

/* SJA1000 registers - manual section 6.4 (Pelican Mode) */
#define SJA1000_MOD 0x00
#define SJA1000_CMR 0x01
#define SJA1000_SR 0x02
#define SJA1000_IR 0x03
#define SJA1000_IER 0x04
#define SJA1000_ALC 0x0B
#define SJA1000_ECC 0x0C
#define SJA1000_EWL 0x0D
#define SJA1000_RXERR 0x0E
#define SJA1000_TXERR 0x0F
#define SJA1000_ACCC0 0x10
#define SJA1000_ACCC1 0x11
#define SJA1000_ACCC2 0x12
#define SJA1000_ACCC3 0x13
#define SJA1000_ACCM0 0x14
#define SJA1000_ACCM1 0x15
#define SJA1000_ACCM2 0x16
#define SJA1000_ACCM3 0x17
#define SJA1000_RMC 0x1D
#define SJA1000_RBSA 0x1E

/* Common registers - manual section 6.5 */
#define SJA1000_BTR0 0x06
#define SJA1000_BTR1 0x07
#define SJA1000_OCR 0x08
#define SJA1000_CDR 0x1F

#define F81601A_TX_FI(x) (0x600 + 0x80 * x)
#define F81601A_TX_ID1(x) (0x601 + 0x80 * x)
#define F81601A_TX_ID2(x) (0x602 + 0x80 * x)
#define F81601A_TX_ID3(x) (0x603 + 0x80 * x)
#define F81601A_TX_ID4(x) (0x604 + 0x80 * x)
#define F81601A_TX_BUF(x) (0x640 + 0x80 * x)

#define F81601A_RX_FI 0x180 //0x10
#define F81601A_RX_ID1 0x181
#define F81601A_RX_ID2 0x182
#define F81601A_RX_ID3 0x183
#define F81601A_RX_ID4 0x184
#define F81601A_RX_SFF_BUF 0x183
#define F81601A_RX_EFF_BUF 0x185

#define SJA1000_FI_FF 0x80
#define SJA1000_FI_RTR 0x40
#define F81601A_FI_EDL BIT(5)
#define F81601A_FI_BRS BIT(4)

#define SJA1000_CAN_RAM 0x20

/* mode register */
#define MOD_RM 0x01
#define MOD_LOM 0x02
#define MOD_STM 0x04
#define MOD_AFM 0x08
#define MOD_SM 0x10

/* commands */
#define CMD_SRR 0x10
#define CMD_CDO 0x08
#define CMD_RRB 0x04
#define CMD_AT 0x02
#define CMD_TR 0x01

/* interrupt sources */
#define IRQ_BEI 0x80
#define IRQ_ALI 0x40
#define IRQ_EPI 0x20
#define IRQ_WUI 0x10
#define IRQ_DOI 0x08
#define IRQ_EI 0x04
#define IRQ_TI 0x02
#define IRQ_RI 0x01
#define IRQ_ALL 0xFF
#define IRQ_OFF 0x00

/* status register content */
#define SR_BS 0x80
#define SR_ES 0x40
#define SR_TS 0x20
#define SR_RS 0x10
#define SR_TCS 0x08
#define SR_TBS 0x04
#define SR_DOS 0x02
#define SR_RBS 0x01

#define SR_CRIT (SR_BS | SR_ES)

/* ECC register */
#define ECC_SEG 0x1F
#define ECC_DIR 0x20
#define ECC_ERR 6
#define ECC_BIT 0x00
#define ECC_FORM 0x40
#define ECC_STUFF 0x80
#define ECC_MASK 0xc0

/*
 * SJA1000 private data structure
 */
struct sja1000_priv {
	struct can_priv can; /* must be the first member */
	struct sk_buff *echo_skb;

	/* the lower-layer is responsible for appropriate locking */
	u8 (*read_reg)(const struct sja1000_priv *priv, int reg);
	void (*write_reg)(const struct sja1000_priv *priv, int reg, u8 val);
	void (*write_mask_reg)(const struct sja1000_priv *priv, int reg,
			       u8 mask, u8 val);

	uint32_t (*read_reg_l)(const struct sja1000_priv *priv, int reg);
	void (*write_reg_l)(const struct sja1000_priv *priv, int reg,
			    uint32_t val);

	void (*pre_irq)(const struct sja1000_priv *priv);
	void (*post_irq)(const struct sja1000_priv *priv);

	void *priv; /* for board-specific data */
	struct net_device *dev;

	void __iomem *reg_base; /* ioremap'ed address to registers */
	void __iomem *reg_fd_base; /* ioremap'ed address to registers */

	unsigned long irq_flags; /* for request_irq() */
	spinlock_t tx_lock;

	u16 flags; /* custom mode flags */
	u8 ocr; /* output control register */
	u8 cdr; /* clock divider register */

	struct delayed_work tx_delayed_work;
	struct delayed_work busoff_delayed_work;
	bool is_read_more_rx;
	int rx_wait_release_cnt;
	int rmc_changed;
	unsigned int force_tx_resend, tx_resend_cnt, tx_size;

	bool is_tx_more;
	struct kfifo empty_fifo, used_fifo;
	int tx_buf_seg;
	bool is_tx_esi[SJA1000_ECHO_SKB_MAX];

#if F81601_USE_TASKLET
	struct tasklet_struct rx_tasklet;
#endif

	unsigned long rx_cnt, tx_cnt;
	unsigned char shadow_ier;
};

struct f81601_pci_card {
	int channels; /* detected channels count */
	u8 decode_cfg;
	u8 reg_table[F81601_PCI_MAX_CHAN][F81601_REG_SAVE_SIZE];
	//void __iomem *addr_io;
	//void __iomem *addr_mem;
	void __iomem *addr;
	spinlock_t lock;
	struct pci_dev *dev;
	struct net_device *net_dev[F81601_PCI_MAX_CHAN];

	int is_internal_osc;
};

static const struct pci_device_id f81601_pci_tbl[] = {
	{ PCI_DEVICE(0x1c29, 0x2004), .driver_data = 2 },
	{},
};

MODULE_DEVICE_TABLE(pci, f81601_pci_tbl);

static bool enable_msi = 1;
module_param(enable_msi, bool, S_IRUGO);
MODULE_PARM_DESC(enable_msi, "Enable device MSI handle, default 1");

static unsigned int max_msi_ch = 2;
module_param(max_msi_ch, uint, S_IRUGO);
MODULE_PARM_DESC(max_msi_ch, "Max MSI channel, default 2");

static int internal_clk = -1;
module_param(internal_clk, int, S_IRUGO);
MODULE_PARM_DESC(internal_clk, "Use internal clock, default -1, 0/1(80MHz)");

static unsigned int external_clk = 80000000;
module_param(external_clk, uint, S_IRUGO);
MODULE_PARM_DESC(external_clk,
		 "External Clock, must spec when internal_clk = 0");

static unsigned int bus_restart_ms = 0;
module_param(bus_restart_ms, uint, S_IRUGO);
MODULE_PARM_DESC(bus_restart_ms, "override default bus_restart_ms timer");

static unsigned int force_tx_send_cnt = 0;
module_param(force_tx_send_cnt, uint, S_IRUGO);
MODULE_PARM_DESC(force_tx_send_cnt, "force_tx_send_cnt");

static unsigned int rx_guard_time = 0; //F81601_RX_GUARD_TIME;
module_param(rx_guard_time, uint, S_IRUGO);
MODULE_PARM_DESC(rx_guard_time, "rx_guard_time");

static unsigned int multi_tx_queue = 1;
module_param(multi_tx_queue, uint, S_IRUGO);
MODULE_PARM_DESC(multi_tx_queue, "Enable Multi-TX queue");

static int disable_ssp = -1;
module_param(disable_ssp, int, S_IRUGO);
MODULE_PARM_DESC(disable_ssp,
		 "Disable TX delay ACKed, 1=disable, 0=enable, -1=auto");

static int ssp_value = -1;
module_param(ssp_value, int, S_IRUGO);
MODULE_PARM_DESC(ssp_value, "SSP value, must >= 2, -1=auto");

static unsigned int can_default_min_brp = 0;
module_param(can_default_min_brp, uint, S_IRUGO);
MODULE_PARM_DESC(can_default_min_brp, "can_default_min_brp");

static unsigned int more_err_report = 0;
module_param(more_err_report, uint, S_IRUGO);
MODULE_PARM_DESC(more_err_report, "more_err_report");

static irqreturn_t f81601_interrupt(int irq, void *dev_id);
static int sja1000_set_bittiming(struct net_device *dev);

static unsigned int force_sjw_max = 1;
module_param(force_sjw_max, uint, S_IRUGO);
MODULE_PARM_DESC(force_sjw_max, "force_sjw_max");

#define USEC 1000LL
#define MSEC (1000 * USEC)
#define SEC (1000 * MSEC)

#ifndef get_can_dlc
#define get_can_dlc can_cc_dlc2len
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
#define can_dlc2len can_fd_dlc2len
#define can_len2dlc can_fd_len2dlc
#endif

#if 0
static void time_start(struct timespec *start)
{
	getnstimeofday(start);
}

static unsigned long long time_end(struct timespec *start)
{
	long long secs, nsecs;
	struct timespec stop;

	getnstimeofday(&stop);
	
	secs = stop.tv_sec - start->tv_sec;
	nsecs = stop.tv_nsec - start->tv_nsec;
	if (nsecs < 0) {
		secs--;
		nsecs += SEC;
	}

	return secs * SEC + nsecs;
}
#endif

static void sja1000_write_cmdreg(struct sja1000_priv *priv, u8 val)
{
	struct can_bittiming *bt = &priv->can.bittiming;
	int i = 20000;
	u8 rmc;

	if (priv->is_read_more_rx && bt->bitrate > 250000 && val == CMD_RRB) {
#if RMC_CHANGE_RELEASE
		priv->rmc_changed = 0;
		rmc = priv->read_reg(priv, SJA1000_RMC);

		while ((priv->read_reg(priv, SJA1000_SR) & SR_RS) && --i) {
			if (priv->read_reg(priv, SJA1000_RMC) != rmc) {
				priv->rmc_changed = 1;
				break;
			}
		}
#else
		while ((priv->read_reg(priv, SJA1000_SR) & SR_RS) && --i)
			; //udelay(1);
#endif

		priv->rx_wait_release_cnt = 20000 - i;
	}

#if 0 // only TX need spec tx buffer
	if (priv->is_tx_more) {
		if (priv->tx_buf_seg != -1) {
			val |= priv->tx_buf_seg ? BIT(7) : 0;
		}
	}
#endif

#if 0
	if (val & CMD_TR) {
		uint8_t tx_data[11] = {0};

		pr_info("%s: cmd: %x flag: %x, tx_buf_seg: %x\n", __func__, val, priv->flags, priv->tx_buf_seg);

		for (i = 0; i < 3; ++i) {
			tx_data[i] = priv->read_reg(priv, F81601A_TX_FI(priv->tx_buf_seg) + i);
		}
	
		for (i = 4; i < 11; ++i) {
			tx_data[i] = priv->read_reg(priv, F81601A_TX_BUF(priv->tx_buf_seg) + i - 4);
		}
	
		pr_info("%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
			tx_data[0], tx_data[1], tx_data[2], tx_data[3],
			tx_data[4], tx_data[5], tx_data[6], tx_data[7],
			tx_data[8], tx_data[9], tx_data[10]);
	}
#endif

	//if (val & CMD_AT)
	//	udelay(130);

	//if (val & CMD_TR)
	//	val |= BIT(6);

	priv->write_reg(priv, SJA1000_CMR, val);
}

static int sja1000_err(struct net_device *dev, uint8_t isrc, uint8_t status)
{
	struct sja1000_priv *priv = netdev_priv(dev);
	struct net_device_stats *stats = &dev->stats;
	struct can_frame *cf;
	struct sk_buff *skb;
	enum can_state state = priv->can.state;
	enum can_state rx_state, tx_state;
	unsigned int rxerr, txerr;
	uint8_t ecc, alc;

	skb = alloc_can_err_skb(dev, &cf);
	if (skb == NULL)
		return -ENOMEM;

	txerr = priv->read_reg(priv, SJA1000_TXERR);
	rxerr = priv->read_reg(priv, SJA1000_RXERR);

	cf->data[6] = txerr;
	cf->data[7] = rxerr;

	if (isrc & IRQ_DOI) {
		/* data overrun interrupt */
		netdev_dbg(dev, "data overrun interrupt\n");
		cf->can_id |= CAN_ERR_CRTL;
		cf->data[1] = CAN_ERR_CRTL_RX_OVERFLOW;
		stats->rx_over_errors++;
		stats->rx_errors++;
		sja1000_write_cmdreg(priv, CMD_CDO); /* clear bit */
	}

	if (isrc & IRQ_EI) {
		/* error warning interrupt */
		netdev_dbg(dev, "error warning interrupt\n");

		if (status & SR_BS)
			state = CAN_STATE_BUS_OFF;
		else if (status & SR_ES)
			state = CAN_STATE_ERROR_WARNING;
		else
			state = CAN_STATE_ERROR_ACTIVE;
	}
	if (isrc & IRQ_BEI) {
		/* bus error interrupt */
		priv->can.can_stats.bus_error++;
		stats->rx_errors++;

		ecc = priv->read_reg(priv, SJA1000_ECC);
		//netdev_info(dev, "ecc: %02xh\n", ecc);

		cf->can_id |= CAN_ERR_PROT | CAN_ERR_BUSERROR;

		/* set error type */
		switch (ecc & ECC_MASK) {
		case ECC_BIT:
			cf->data[2] |= CAN_ERR_PROT_BIT;
			break;
		case ECC_FORM:
			cf->data[2] |= CAN_ERR_PROT_FORM;
			break;
		case ECC_STUFF:
			cf->data[2] |= CAN_ERR_PROT_STUFF;
			break;
		default:
			break;
		}

		/* set error location */
		cf->data[3] = ecc & ECC_SEG;

		/* Error occurred during transmission? */
		if ((ecc & ECC_DIR) == 0)
			cf->data[2] |= CAN_ERR_PROT_TX;
	}
	if (isrc & IRQ_EPI) {
		/* error passive interrupt */
		netdev_dbg(dev, "error passive interrupt\n");

		if (state == CAN_STATE_ERROR_PASSIVE)
			state = CAN_STATE_ERROR_WARNING;
		else
			state = CAN_STATE_ERROR_PASSIVE;
	}
	if (isrc & IRQ_ALI) {
		/* arbitration lost interrupt */
		netdev_dbg(dev, "arbitration lost interrupt\n");
		alc = priv->read_reg(priv, SJA1000_ALC);
		//netdev_info(dev, "alc: %02xh\n", alc);
		priv->can.can_stats.arbitration_lost++;
		stats->tx_errors++;
		cf->can_id |= CAN_ERR_LOSTARB;
		cf->data[0] = alc & 0x1f;
	}

	if (state != priv->can.state) {
		tx_state = txerr >= rxerr ? state : 0;
		rx_state = txerr <= rxerr ? state : 0;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
		can_change_state(dev, cf, tx_state, rx_state);
#else
		if (state == CAN_STATE_ERROR_WARNING) {
			priv->can.can_stats.error_warning++;
			cf->data[1] = (txerr > rxerr) ?
					      CAN_ERR_CRTL_TX_WARNING :
					      CAN_ERR_CRTL_RX_WARNING;
		} else {
			priv->can.can_stats.error_passive++;
			cf->data[1] = (txerr > rxerr) ?
					      CAN_ERR_CRTL_TX_PASSIVE :
					      CAN_ERR_CRTL_RX_PASSIVE;
		}
#endif

		if (state == CAN_STATE_BUS_OFF)
			can_bus_off(dev);
	}

	stats->rx_packets++;
	stats->rx_bytes += cf->can_dlc;
	netif_rx(skb);

	return 0;
}

#if 0
#define FIFO_ADDR(x) ((x) % 64)

static void sja1000_rx_from_fifo(struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);
	struct net_device_stats *stats = &dev->stats;
	struct can_frame *cf;
	struct sk_buff *skb;
	uint32_t fi, rmc, rbsa, remain_rmc;
	uint32_t dreg;
	canid_t id;
	int i, j;
	u8 tmp;
	bool debug_en = false;

	//if (dev->dev_id == 0)
	//	debug_en = true;

	remain_rmc = rmc = priv->read_reg(priv, SJA1000_RMC);
	rbsa = priv->read_reg(priv, SJA1000_RBSA);

	if (debug_en)
		netdev_info(dev, "rmc: %d 000\n", rmc);

	if (rmc > 1)
		remain_rmc = rmc = 1;

	for (i = 0; i < rmc; ++i) {
		skb = alloc_can_skb(dev, &cf);
		if (skb == NULL) {
			netdev_err(dev, "alloc_can_skb %d failed\n", i);
			return;
		}

		fi = priv->read_reg(priv, 32 + FIFO_ADDR(rbsa));
		if (fi & SJA1000_FI_FF) {
			/* extended frame format (EFF) */
			dreg = (F81601A_EFF_BUF - F81601A_TX_FI) + rbsa;
			id = (priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 1)) << 21)
			    | (priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 2)) << 13)
			    | (priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 3)) << 5)
			    | (priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 4)) >> 3);
			id |= CAN_EFF_FLAG;
		} else {
			/* standard frame format (SFF) */
			dreg = (F81601A_SFF_BUF - F81601A_TX_FI) + rbsa;
			id = (priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 1)) << 3)
			    | (priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 2)) >> 5);
		}

		cf->can_dlc = get_can_dlc(fi & 0x0F);
		if (fi & SJA1000_FI_RTR) {
			id |= CAN_RTR_FLAG;
		} else {
			for (j = 0; j < cf->can_dlc; j++)
				cf->data[j] = priv->read_reg(priv, 32 + FIFO_ADDR(dreg++));
		}

		cf->can_id = id;

#if 0
		bool is_err = 0;

		if (cf->can_id != 0 || fi & SJA1000_FI_RTR || fi & SJA1000_FI_FF)
			is_err = 1;

		if (cf->can_dlc != 8)
			is_err = 1;

		for (i = 0; i < cf->can_dlc; i++) {
			if (cf->data[i] & 0x0f)
				is_err = 1;

			if (cf->data[i] 
!= cf->data[0])
				is_err = 1;
		}

		if (is_err) {
			/* print current */
			netdev_err(dev, "Packet\n");
			netdev_err(dev, "%x %x %02x %02x %02x %02x %02x %02x %02x %02x\n",
					cf->can_id, cf->can_dlc,
					cf->data[0],
					cf->data[1],
					cf->data[2],
					cf->data[3],
					cf->data[4],
					cf->data[5],
					cf->data[6],
					cf->data[7]);

			netdev_err(dev, "RX buff\n");
			netdev_err(dev, "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
				priv->read_reg(priv, F81601A_TX_FI + 0),
				priv->read_reg(priv, F81601A_TX_FI + 1),
				priv->read_reg(priv, F81601A_TX_FI + 2),
				priv->read_reg(priv, F81601A_TX_FI + 3),
				priv->read_reg(priv, F81601A_TX_FI + 4),
				priv->read_reg(priv, F81601A_TX_FI + 5),
				priv->read_reg(priv, F81601A_TX_FI + 6),
				priv->read_reg(priv, F81601A_TX_FI + 7),
				priv->read_reg(priv, F81601A_TX_FI + 8),
				priv->read_reg(priv, F81601A_TX_FI + 9),
				priv->read_reg(priv, F81601A_TX_FI + 10));

			netdev_err(dev, "FIFO buff\n");
			netdev_err(dev, "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
				priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 0)),
				priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 1)),
				priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 2)),
				priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 3)),
				priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 4)),
				priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 5)),
				priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 6)),
				priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 7)),
				priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 8)),
				priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 9)),
				priv->read_reg(priv, 32 + FIFO_ADDR(rbsa + 10)));		
		}
#endif

		rbsa = dreg;	

		tmp = priv->read_reg(priv, SJA1000_RMC);
		if (debug_en)
			netdev_info(dev, "rmc: %d, current RMC: %d 111\n", rmc, tmp);

		if (!(priv->read_reg(priv, SJA1000_SR) & SR_RS)) {
			priv->write_reg(priv, SJA1000_CMR, CMD_RRB);
			remain_rmc--;

			if (debug_en)
				netdev_info(dev, "rmc: %d, remain_rmc: %d 222\n", rmc, remain_rmc);
		}

		stats->rx_packets++;
		stats->rx_bytes += cf->can_dlc;
		netif_rx(skb);	
	}

	tmp = priv->read_reg(priv, SJA1000_RMC);
	if (debug_en)
		netdev_info(dev, "rmc: %d, current RMC: %d, remain_rmc: %d 333\n", rmc, tmp, remain_rmc);

	/* release receive buffer */
	for (i = 0; i < remain_rmc; ++i) {
		sja1000_write_cmdreg(priv, CMD_RRB);
		if (debug_en)
			netdev_info(dev, "i: %d released, cnt: %d, is_changed: %d\n", i,
					priv->rx_wait_release_cnt, priv->rmc_changed);
	}

	if (debug_en)
		netdev_info(dev, "remain_rmc: %d all released\n", remain_rmc);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0)
	can_led_event(dev, CAN_LED_EVENT_RX);
#endif
}
#endif

static void sja1000_rx_from_normal_l(struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);
	struct net_device_stats *stats = &dev->stats;
	struct canfd_frame *cf;
	struct sk_buff *skb;
	uint32_t fi;
	uint32_t dreg, offset;
	canid_t id;
	int i, j, len;
	uint8_t *ptr;
	uint32_t data_l[18]; // (64+5) / 4 = 17.25

#if 1
	ptr = (uint8_t *)data_l;

	data_l[0] = priv->read_reg_l(priv, F81601A_RX_FI + 4 * 0);
	data_l[1] = priv->read_reg_l(priv, F81601A_RX_FI + 4 * 1);

#if 0
	data_l[2] = priv->read_reg_l(priv, F81601A_RX_FI + 4 * 2);
	data_l[3] = priv->read_reg_l(priv, F81601A_RX_FI + 4 * 3);

	netdev_info(dev, "%x %x %x %x\n", data_l[0], data_l[1], data_l[2], data_l[3]);
#endif

	fi = ptr[F81601A_RX_FI - F81601A_RX_FI];

	if (fi & F81601A_FI_EDL)
		skb = alloc_canfd_skb(dev, &cf);
	else
		skb = alloc_can_skb(dev, (struct can_frame **)&cf);
	if (skb == NULL) {
		netdev_err(dev, "alloc_can_skb failed\n");
		return;
	}

	if (fi & SJA1000_FI_FF) {
		/* extended frame format (EFF) */
		dreg = F81601A_RX_EFF_BUF - F81601A_RX_FI;
		id = (ptr[F81601A_RX_ID1 - F81601A_RX_FI] << 21) |
		     (ptr[F81601A_RX_ID2 - F81601A_RX_FI] << 13) |
		     (ptr[F81601A_RX_ID3 - F81601A_RX_FI] << 5) |
		     (ptr[F81601A_RX_ID4 - F81601A_RX_FI] >> 3);
		id |= CAN_EFF_FLAG;
	} else {
		/* standard frame format (SFF) */
		dreg = F81601A_RX_SFF_BUF - F81601A_RX_FI;
		id = (ptr[F81601A_RX_ID1 - F81601A_RX_FI] << 3) |
		     (ptr[F81601A_RX_ID2 - F81601A_RX_FI] >> 5);
	}

	// RX ESI check
	if (fi & F81601A_FI_EDL) {
#if 0
		data_btr = priv->read_reg(priv, 0x72);
		if ((fi & F81601A_FI_EDL) && (data_btr & BIT(7)))
			cf->flags |= CANFD_ESI;
#else
		if (fi & SJA1000_FI_FF) {
			if (ptr[F81601A_RX_ID4 - F81601A_RX_FI] & BIT(0))
				cf->flags |= CANFD_ESI;
		} else {
			if (ptr[F81601A_RX_ID2 - F81601A_RX_FI] & BIT(0))
				cf->flags |= CANFD_ESI;
		}
#endif
		if (fi & F81601A_FI_BRS)
			cf->flags |= CANFD_BRS;
	}

	cf->len = can_dlc2len(fi & 0x0f);
	if (fi & SJA1000_FI_RTR) {
		id |= CAN_RTR_FLAG;
	} else {
		len = (cf->len + 3) / 4;

		for (i = 0; i < len; ++i) {
			data_l[2 + i] = priv->read_reg_l(
				priv, F81601A_RX_FI + 4 * (i + 2));
			//netdev_info(dev, "%x\n", data_l[2 + i]);

			for (j = 0; j < 4; ++j) {
				offset = i * 4 + j;

				if (offset >= cf->len)
					break;
#if 0
				if (dreg + i * 4 + j >= sizeof(data_l) * sizeof(uint32_t)) {
					netdev_info(dev, "%s: 11 %d %d %d out\n", __func__, i, j, __LINE__);
					break;
				}

				if (i * 4 + j >= 64) {
					netdev_info(dev, "%s: 22 %d %d %d out\n", __func__, i, j, __LINE__);
					break;					
				}
#endif
				//netdev_info(dev, "%d %d %x\n", i, j, ptr[dreg + i * 4 + j]);
				cf->data[offset] = ptr[dreg + offset];
			}
		}
	}

	cf->can_id = id;

	/* release receive buffer */
	sja1000_write_cmdreg(priv, CMD_RRB);

	stats->rx_packets++;
	stats->rx_bytes += can_dlc2len(fi & 0x0f);

	priv->rx_cnt++;
	netif_rx(skb);

#if !DIABLE_CANLED && LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)
	can_led_event(dev, CAN_LED_EVENT_RX);
#endif

#else
	fi = priv->read_reg(priv, F81601A_RX_FI);
	if (fi & F81601A_FI_EDL)
		skb = alloc_canfd_skb(dev, &cf);
	else
		skb = alloc_can_skb(dev, (struct can_frame **)&cf);
	if (skb == NULL) {
		netdev_err(dev, "alloc_can_skb failed\n");
		return;
	}

	if (fi & SJA1000_FI_FF) {
		/* extended frame format (EFF) */
		dreg = F81601A_RX_EFF_BUF;
		id = (priv->read_reg(priv, F81601A_RX_ID1) << 21) |
		     (priv->read_reg(priv, F81601A_RX_ID2) << 13) |
		     (priv->read_reg(priv, F81601A_RX_ID3) << 5) |
		     (priv->read_reg(priv, F81601A_RX_ID4) >> 3);
		id |= CAN_EFF_FLAG;
	} else {
		/* standard frame format (SFF) */
		dreg = F81601A_RX_SFF_BUF;
		id = (priv->read_reg(priv, F81601A_RX_ID1) << 3) |
		     (priv->read_reg(priv, F81601A_RX_ID2) >> 5);
	}

	// RX ESI check
	if (fi & F81601A_FI_EDL) {
		data_btr = priv->read_reg(priv, 0x72);
		if ((fi & F81601A_FI_EDL) && (data_btr & BIT(7)))
			cf->flags |= CANFD_ESI;

		if (fi & F81601A_FI_BRS)
			cf->flags |= CANFD_BRS;
	}

	cf->len = can_dlc2len(fi & 0x0f);
	if (fi & SJA1000_FI_RTR) {
		id |= CAN_RTR_FLAG;
	} else {
		for (i = 0; i < cf->len; i++)
			cf->data[i] = priv->read_reg(priv, dreg++);
	}

	cf->can_id = id;

	/* release receive buffer */
	sja1000_write_cmdreg(priv, CMD_RRB);

	stats->rx_packets++;
	stats->rx_bytes += can_dlc2len(fi & 0x0f);

	priv->rx_cnt++;
	netif_rx(skb);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0)
	can_led_event(dev, CAN_LED_EVENT_RX);
#endif

#endif
}

static void sja1000_rx_from_normal(struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);
	struct net_device_stats *stats = &dev->stats;
	struct canfd_frame *cf;
	struct sk_buff *skb;
	uint32_t fi;
	uint32_t dreg;
	canid_t id;
	int i;
	uint8_t data_btr;

	//netdev_info(dev, "%s: rx: 180h\n", __func__);

	fi = priv->read_reg(priv, F81601A_RX_FI);
	if (fi & F81601A_FI_EDL)
		skb = alloc_canfd_skb(dev, &cf);
	else
		skb = alloc_can_skb(dev, (struct can_frame **)&cf);
	if (skb == NULL) {
		netdev_err(dev, "alloc_can_skb failed\n");
		return;
	}

	if (fi & SJA1000_FI_FF) {
		/* extended frame format (EFF) */
		dreg = F81601A_RX_EFF_BUF;
		id = (priv->read_reg(priv, F81601A_RX_ID1) << 21) |
		     (priv->read_reg(priv, F81601A_RX_ID2) << 13) |
		     (priv->read_reg(priv, F81601A_RX_ID3) << 5) |
		     (priv->read_reg(priv, F81601A_RX_ID4) >> 3);
		id |= CAN_EFF_FLAG;
	} else {
		/* standard frame format (SFF) */
		dreg = F81601A_RX_SFF_BUF;
		id = (priv->read_reg(priv, F81601A_RX_ID1) << 3) |
		     (priv->read_reg(priv, F81601A_RX_ID2) >> 5);
	}

	// RX ESI check
	if (fi & F81601A_FI_EDL) {
		data_btr = priv->read_reg(priv, 0x72);
		if ((fi & F81601A_FI_EDL) && (data_btr & BIT(7)))
			cf->flags |= CANFD_ESI;

		if (fi & F81601A_FI_BRS)
			cf->flags |= CANFD_BRS;
	}

	cf->len = can_dlc2len(fi & 0x0f);
	if (fi & SJA1000_FI_RTR) {
		id |= CAN_RTR_FLAG;
	} else {
		for (i = 0; i < cf->len; i++)
			cf->data[i] = priv->read_reg(priv, dreg++);
	}

	cf->can_id = id;

	/* release receive buffer */
	sja1000_write_cmdreg(priv, CMD_RRB);

	stats->rx_packets++;
	stats->rx_bytes += can_dlc2len(fi & 0x0f);

	priv->rx_cnt++;
	//netif_receive_skb(skb);
	netif_rx(skb);

#if !DIABLE_CANLED && LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)
	can_led_event(dev, CAN_LED_EVENT_RX);
#endif
}

static void sja1000_rx(struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);

	//if (priv->is_read_more_rx)
	//	sja1000_rx_from_fifo(dev);
	//else
	if (priv->read_reg_l)
		sja1000_rx_from_normal_l(dev);
	else
		sja1000_rx_from_normal(dev);
}

static int sja1000_is_absent(struct sja1000_priv *priv)
{
	return (priv->read_reg(priv, SJA1000_MOD) == 0xFF);
}

static void set_reset_mode(struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);
	unsigned char status = priv->read_reg(priv, SJA1000_MOD);
	int i;

	/* disable interrupts */
	priv->shadow_ier = IRQ_OFF;
	priv->write_reg(priv, SJA1000_IER, priv->shadow_ier);

	for (i = 0; i < 100; i++) {
		/* check reset bit */
		if (status & MOD_RM) {
			priv->can.state = CAN_STATE_STOPPED;
			return;
		}

		/* reset chip */
		priv->write_reg(priv, SJA1000_MOD, MOD_RM);
		udelay(10);
		status = priv->read_reg(priv, SJA1000_MOD);
	}

	netdev_err(dev, "setting SJA1000 into reset mode failed!\n");
}

#ifdef USE_CUSTOM_SJA1000
static /*const*/ struct can_bittiming_const sja1000_bittiming_const = {
	.name = DRV_NAME,
	.tseg1_min = 1,
	.tseg2_min = 1,

#if F81601_EXTEND_BTR
	.tseg1_max = 16 * 4,
	.tseg2_max = 8 * 4,
#else
	.tseg1_max = 16,
	.tseg2_max = 8,
#endif

	.sjw_max = 4,

	.brp_min = 8, //8, //8, //1,
	//.brp_min = 1,//1,
	.brp_max = 512, // 1024 afford 5k bitrate, 512 10k
	.brp_inc = 1,
};

static const struct can_bittiming_const sja1000_fd_bittiming_const = {
	.name = DRV_NAME,
	.tseg1_min = 1,
	.tseg2_min = 1,

#if F81601_EXTEND_BTR
	.tseg1_max = 16 * 4,
	.tseg2_max = 8 * 4,
#else
	.tseg1_max = 16,
	.tseg2_max = 8,
#endif
	.sjw_max = 4,
	.brp_min = 1,
	.brp_max = 64,
	.brp_inc = 1,
};

#if 0
static void start_hrtimer_us(struct hrtimer *hrt, unsigned long usec)
{
	//unsigned long sec = usec / 1000000;
	//unsigned long nsec = (usec % 1000000) * 1000;
	//ktime_t t = ktime_set(sec, nsec);

	//pr_info("%s: %lld\n", __func__, ktime_to_ns(t));
	hrtimer_start(hrt, ns_to_ktime(usec * 1000L), HRTIMER_MODE_REL_PINNED);
}
#endif

static int sja1000_probe_chip(struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);

	if (priv->reg_base && sja1000_is_absent(priv)) {
		netdev_err(dev, "probing failed\n");
		return 0;
	}
	return -1;
}

static void set_normal_mode(struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);
	unsigned char status = priv->read_reg(priv, SJA1000_MOD);
	u8 mod_reg_val = 0x00;
	int i;

	for (i = 0; i < 100; i++) {
		/* check reset bit */
		if ((status & MOD_RM) == 0) {
			priv->can.state = CAN_STATE_ERROR_ACTIVE;
			/* enable interrupts */
			if (priv->can.ctrlmode & CAN_CTRLMODE_BERR_REPORTING)
				priv->shadow_ier = IRQ_ALL;
			else
				priv->shadow_ier = IRQ_ALL & ~IRQ_BEI;

			if (!more_err_report)
				priv->shadow_ier &= ~(IRQ_ALI | IRQ_BEI);
			else
				priv->shadow_ier |= IRQ_ALI | IRQ_BEI;

			priv->shadow_ier &= ~IRQ_ALI;
			priv->write_reg(priv, SJA1000_IER, priv->shadow_ier);
			return;
		}

		/* set chip to normal mode */
		if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
			mod_reg_val |= MOD_LOM;
		if ((priv->can.ctrlmode & CAN_CTRLMODE_PRESUME_ACK) ||
		    (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK))
			mod_reg_val |= MOD_STM;
		priv->write_reg(priv, SJA1000_MOD, mod_reg_val);

		udelay(10);

		status = priv->read_reg(priv, SJA1000_MOD);
	}

	netdev_err(dev, "setting SJA1000 into normal mode failed!\n");
}

/*
 * initialize SJA1000 chip:
 *   - reset chip
 *   - set output mode
 *   - set baudrate
 *   - enable interrupts
 *   - start operating mode
 */
static void chipset_init(struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);

	priv->write_reg(priv, 0x70, 0x11); // enable FD/mix mode
	//priv->write_reg(priv, 0x70, 0x10); // enable FD/mix mode

	priv->write_reg(priv, 0x73,
			F81601_DIVIDE_CLK); // clk change with 1x div

	/* set clock divider and output control register */
	priv->write_reg(priv, SJA1000_CDR, priv->cdr | CDR_PELICAN);

	/* set acceptance filter (accept all) */
	priv->write_reg(priv, SJA1000_ACCC0, 0x00);
	priv->write_reg(priv, SJA1000_ACCC1, 0x00);
	priv->write_reg(priv, SJA1000_ACCC2, 0x00);
	priv->write_reg(priv, SJA1000_ACCC3, 0x00);

	priv->write_reg(priv, SJA1000_ACCM0, 0xFF);
	priv->write_reg(priv, SJA1000_ACCM1, 0xFF);
	priv->write_reg(priv, SJA1000_ACCM2, 0xFF);
	priv->write_reg(priv, SJA1000_ACCM3, 0xFF);

	priv->write_reg(priv, SJA1000_OCR, priv->ocr | OCR_MODE_NORMAL);
}

static void f81601_disable_rx_int(struct sja1000_priv *priv)
{
	//struct net_device *dev;
	//unsigned char ier;

	//dev = priv->dev;

	priv->shadow_ier &= ~IRQ_RI;
	priv->write_reg(priv, SJA1000_IER, priv->shadow_ier);

	//netdev_info(dev, "%s: disable RX: %x\n", __func__, ier);
}

static void f81601_enable_rx_int(struct sja1000_priv *priv)
{
	//struct net_device *dev;
	//unsigned char ier;

	//dev = priv->dev;

	priv->shadow_ier |= IRQ_RI;
	priv->write_reg(priv, SJA1000_IER, priv->shadow_ier);

	//netdev_info(dev, "%s: enable RX: %x\n", __func__, ier);
}

static void sja1000_start(struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);
	u8 empty_fifo[] = { 0, 1 };

	//netdev_info(dev, "%s: in\n", __func__);

	/* leave reset mode */
	//if (priv->can.state != CAN_STATE_STOPPED)
	set_reset_mode(dev);

	/* Initialize chip if uninitialized at this stage */
	//if (!(priv->read_reg(priv, SJA1000_CDR) & CDR_PELICAN))
	chipset_init(dev);

	/* Clear error counters and error code capture */
	priv->write_reg(priv, SJA1000_TXERR, 0x0);
	priv->write_reg(priv, SJA1000_RXERR, 0x0);
	priv->read_reg(priv, SJA1000_ECC);
	priv->read_reg(priv, SJA1000_ALC);

	/* clear interrupt flags */
	priv->read_reg(priv, SJA1000_IR);

	//cancel_delayed_work_sync(&priv->tx_delayed_work);

	sja1000_set_bittiming(dev);

	// init tx queue
	priv->rx_cnt = priv->tx_cnt = 0;
	priv->tx_buf_seg = -1;

	// clear TX flag
	priv->flags &= ~F81601_IS_TXING;

	kfifo_reset(&priv->empty_fifo);
	kfifo_reset(&priv->used_fifo);
	kfifo_in(&priv->empty_fifo, empty_fifo, 2);

	// if SRR, disable SSP
	if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
		priv->write_mask_reg(priv, 0x77, BIT(5) | BIT(0), 0);

	/* leave reset mode */
	set_normal_mode(dev);

	cancel_delayed_work_sync(&priv->busoff_delayed_work);

	//schedule_delayed_work(&priv->busoff_delayed_work, F81601_BUSOFF_GUARD_TIME);
}

static int sja1000_set_mode(struct net_device *dev, enum can_mode mode)
{
	switch (mode) {
	case CAN_MODE_START:
		sja1000_start(dev);
		if (netif_queue_stopped(dev))
			netif_wake_queue(dev);
		break;

	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int sja1000_set_data_bittiming(struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);
	const struct can_bittiming *dbt = &priv->can.data_bittiming;
	//const struct can_bittiming *bt = &priv->can.bittiming;
	u8 btr0, btr1, seg1, seg2, force_sjw, brp;

	netdev_dbg(dev, "%s: dbt->bitrate: %d\n", __func__, dbt->bitrate);
	netdev_dbg(dev, "%s: priv->can.ctrlmode: %x\n", __func__,
		   priv->can.ctrlmode);

	netdev_dbg(dev, "%s: dbt->brp: %d\n", __func__, dbt->brp - 1);
	netdev_dbg(dev, "%s: dbt->sjw: %x\n", __func__, dbt->sjw - 1);
	netdev_dbg(dev, "%s: data tseg1: %x\n", __func__,
		   dbt->prop_seg + dbt->phase_seg1 - 1);
	netdev_dbg(dev, "%s: data tseg2: %x\n", __func__, dbt->phase_seg2 - 1);

	//priv->write_reg(priv, 0x71, 0x01);	// FD BRP fix to 4M
	//priv->write_reg(priv, 0x72, 0x27);	// FD BTR fix to 4M

	seg1 = dbt->prop_seg + dbt->phase_seg1 - 1;
	seg2 = dbt->phase_seg2 - 1;
	brp = dbt->brp - 1;

#if F81601_FORCE_MAX_SPEED
	seg1 = 10; // 176 // 176
	seg2 = 1;
	brp = 0;

	seg1 = 7; // fail
	seg2 = 1;
	brp = 0;

	seg1 = 10; // 176 // 176
	seg2 = 1;
	brp = 0;
#endif

	btr0 = (brp & 0x3f) | (((dbt->sjw - 1) & 0x3) << 6);
	btr1 = (seg1 & 0xf) | ((seg2 & 0x7) << 4);

	if (force_sjw_max) {
		force_sjw = min_t(int, seg2, 3);

		btr0 &= ~(BIT(7) | BIT(6));
		btr0 |= force_sjw << 6;

		netdev_info(dev, "CANFD SJW: %xh\n", force_sjw);
	}

	netdev_dbg(dev, "%s: can tseg1: %x\n", __func__, seg1);
	netdev_dbg(dev, "%s: can tseg2: %x\n", __func__, seg2);

#if 0
	switch (dbt->bitrate) {
	case 8000000:
		if (disable_ssp == -1)
			priv->write_mask_reg(priv, 0x77, BIT(5) | BIT(0),
					     BIT(5) | BIT(0));
		else if (disable_ssp == 1)
			priv->write_mask_reg(priv, 0x77, BIT(5) | BIT(0), 0);
		else
			priv->write_mask_reg(priv, 0x77, BIT(5) | BIT(0),
					     BIT(5) | BIT(0));

		priv->write_reg(priv, 0x78,
				dbt->phase_seg2 + 4); // +4 12m seems good
		priv->write_reg(priv, 0x7a, 0x00); //brs tseg2
		//priv->write_reg(priv, 0x7b, 0x00); // brs rx
		priv->write_reg(priv, 0x7b, 0x85);
		break;

	default:
		if (disable_ssp == -1)
			priv->write_mask_reg(priv, 0x77, BIT(5) | BIT(0), 0);
		else if (disable_ssp == 1)
			priv->write_mask_reg(priv, 0x77, BIT(5) | BIT(0), 0);
		else
			priv->write_mask_reg(priv, 0x77, BIT(5) | BIT(0),
					     BIT(5) | BIT(0));

		priv->write_reg(priv, 0x78,
				dbt->phase_seg2 + 4); // +4 12m seems good
		priv->write_reg(priv, 0x7a, 0x00); //brs tseg2
		priv->write_reg(priv, 0x7b, 0x00); // brs rx

		break;
	}
#else
	if (disable_ssp == 1) {
		priv->write_mask_reg(priv, 0x77, BIT(5) | BIT(0), 0);
	} else {
		if (ssp_value == -1 || ssp_value < 2)
			priv->write_reg(priv, 0x78, 8);
		else
			priv->write_reg(priv, 0x78, ssp_value);

		priv->write_mask_reg(priv, 0x77, BIT(5) | BIT(0),
				     BIT(5) | BIT(0));
	}

#endif
	//netdev_info(dev, "%s: fd seg1: %x, seg2: %x\n", __func__, dbt->phase_seg1, dbt->phase_seg2);
	//netdev_info(dev, "%s: fd reg seg1: %x, seg2: %x, 78h: %x\n", __func__, seg1, seg2, priv->read_reg(priv, 0x78));

	netdev_info(dev, "%s: fd seg1: %x, seg2: %x, brp: %x\n", __func__,
		    seg1, seg2, brp);

	priv->write_reg(priv, 0x71, btr0);
	priv->write_reg(priv, 0x72, btr1);

	// bit 1-0: tseg1 bit5~4
	// bit 3-2: tseg2 bit4~3
	// bit 5-4: FD tseg1 bit5~4
	// bit 7-6: FD tseg2 bit4~3
	// priv->write_reg(priv, 0x79, 0x00);

#if F81601_EXTEND_BTR
	// bit 1-0: tseg1 bit5~4
	// bit 3-2: tseg2 bit4~3
	// bit 5-4: FD tseg1 bit5~4
	// bit 7-6: FD tseg2 bit4~3
	// priv->write_reg(priv, 0x79, 0x00);

	priv->write_mask_reg(priv, 0x79, GENMASK(5, 4),
			     ((seg1 >> 4) & 0x03) << 4);
	priv->write_mask_reg(priv, 0x79, GENMASK(7, 6),
			     ((seg2 >> 3) & 0x03) << 6);
#else
	priv->write_mask_reg(priv, 0x79, GENMASK(5, 4), 0);
	priv->write_mask_reg(priv, 0x79, GENMASK(7, 6), 0);
#endif

	if (priv->can.ctrlmode & CAN_CTRLMODE_FD_NON_ISO)
		priv->write_mask_reg(priv, 0x77, BIT(3), 0);
	else
		priv->write_mask_reg(priv, 0x77, BIT(3), BIT(3));

	//netdev_info(dev, "priv->can.ctrlmode: %x %x %x %x\n", priv->can.ctrlmode, CAN_CTRLMODE_FD_NON_ISO, CAN_CTRLMODE_FD, tmp);
	netdev_info(dev, "FD DATA: %d\n", dbt->bitrate);
	netdev_info(dev, "setting DATA BTR0=0x%02x BTR1=0x%02x\n", btr0, btr1);

	// fpga debug
	//priv->write_mask_reg(priv, 0x78, BIT(0), BIT(0));
	//priv->write_mask_reg(priv, 0x7a, BIT(1) | BIT(0), 0);

	return 0;
}

static int sja1000_set_bittiming(struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);
	const struct can_bittiming *bt = &priv->can.bittiming;
	//const struct can_bittiming *dbt = &priv->can.data_bittiming;
	u8 btr0, btr1, extra_btr0, tmp, seg1, seg2, force_sjw, brp;

	seg1 = bt->prop_seg + bt->phase_seg1 - 1;
	seg2 = bt->phase_seg2 - 1;
	brp = bt->brp;

	btr0 = ((brp - 1) & 0x3f) | (((bt->sjw - 1) & 0x3) << 6);
	btr1 = (seg1 & 0xf) | ((seg2 & 0x7) << 4);
	extra_btr0 = ((brp - 1) >> 6) & 0x07;

	if (priv->can.ctrlmode & CAN_CTRLMODE_3_SAMPLES)
		btr1 |= 0x80;

	if (force_sjw_max) {
		force_sjw = min_t(int, seg2, 3);

		btr0 &= ~(BIT(7) | BIT(6));
		btr0 |= force_sjw << 6;

		netdev_info(dev, "CAN SJW: %xh\n", force_sjw);
	}

#if 0
	seg2 = (btr1 >> 0) & 0x0f;
	seg1 = (btr1 >> 4) & 0x0f;

	if (seg2 < 4) {
		seg2 = 4;
		seg1 += (4 - seg2);

		btr1 = (seg2 << 4) | seg1;
	}
#endif

	netdev_info(dev, "CAN: %d, seg1: %d, seg2: %d\n", bt->bitrate,
		    bt->prop_seg + bt->phase_seg1, bt->phase_seg2);
	netdev_info(
		dev,
		"setting CAN BTR0=0x%02x BTR1=0x%02x, ExtraBRP:0x%02x, BRP: 0x%x\n",
		btr0, btr1, extra_btr0, bt->brp);

	priv->write_reg(priv, SJA1000_BTR0, btr0);
	priv->write_reg(priv, SJA1000_BTR1, btr1);

	tmp = priv->read_reg(priv, 0x73);
	tmp = (tmp & ~0x70) | ((extra_btr0 & 0x07) << 4);
	priv->write_reg(priv, 0x73, tmp);

#if F81601_EXTEND_BTR
	// bit 1-0: tseg1 bit5~4
	// bit 3-2: tseg2 bit4~3
	// bit 5-4: FD tseg1 bit5~4
	// bit 7-6: FD tseg2 bit4~3
	// priv->write_reg(priv, 0x79, 0x00);

	priv->write_mask_reg(priv, 0x79, GENMASK(1, 0), seg1 >> 4);
	priv->write_mask_reg(priv, 0x79, GENMASK(3, 2), seg2 >> 1);
#else
	priv->write_mask_reg(priv, 0x79, GENMASK(1, 0), 0);
	priv->write_mask_reg(priv, 0x79, GENMASK(3, 2), 0);
#endif

	//netdev_info(dev, "79h: %02x\n", priv->read_reg(priv, 0x79));
#if 0
	if (priv->can.ctrlmode & CAN_CTRLMODE_FD || priv->can.ctrlmode & CAN_CTRLMODE_FD_NON_ISO) {
		sja1000_set_data_bittiming(dev);
	}
#endif

	priv->write_mask_reg(priv, 0x77, BIT(4), BIT(4)); // can2.0 delay ack
	//priv->write_mask_reg(priv, 0x77, BIT(4), 0);

	return 0;
}

static int sja1000_get_berr_counter(const struct net_device *dev,
				    struct can_berr_counter *bec)
{
	struct sja1000_priv *priv = netdev_priv(dev);

	bec->txerr = priv->read_reg(priv, SJA1000_TXERR);
	bec->rxerr = priv->read_reg(priv, SJA1000_RXERR);

	return 0;
}

static netdev_tx_t sja1000_start_xmit_single(struct sk_buff *skb,
					     struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);
	struct canfd_frame *cf = (struct canfd_frame *)skb->data;
	uint32_t fi;
	uint8_t dlc;
	canid_t id;
	uint32_t dreg;
	u8 cmd_reg_val = 0x00;
	int i;
	unsigned char status;
	int max_wait = 20000;
	unsigned long flags;

	if (can_dropped_invalid_skb(dev, skb)) {
		netdev_info(dev, "%s: can_dropped_invalid_skb\n", __func__);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&priv->tx_lock, flags);

	//cancel_delayed_work(&priv->tx_delayed_work);
	netif_stop_queue(dev);

	for (i = 0; i < max_wait; ++i) {
		if (priv->can.state >= CAN_STATE_BUS_OFF) {
			netdev_info(dev, "%s: busoff\n", __func__);
			spin_unlock_irqrestore(&priv->tx_lock, flags);
			return NETDEV_TX_OK;
		}

		status = priv->read_reg(priv, SJA1000_SR);
		if ((status & (SR_TBS | SR_TS)) == SR_TBS)
			break;
	}

	if (i >= max_wait) {
		dev->stats.tx_dropped++;
		netdev_dbg(dev, "%s: bus busy: %d, %d\n", __func__, i,
			   priv->can.state);
		spin_unlock_irqrestore(&priv->tx_lock, flags);

		//schedule_delayed_work(&priv->tx_delayed_work, F81601_TX_GUARD_TIME);

		return NETDEV_TX_BUSY;
	}

	id = cf->can_id;

	if (can_is_canfd_skb(skb)) {
		fi = dlc = can_len2dlc(cf->len);
		priv->tx_size = can_dlc2len(dlc);

		fi |= F81601A_FI_EDL;

		if (cf->flags & CANFD_BRS)
			fi |= F81601A_FI_BRS;

	} else {
		priv->tx_size = fi = dlc = cf->len;

		if (id & CAN_RTR_FLAG)
			fi |= SJA1000_FI_RTR;
	}

	dreg = F81601A_TX_BUF(0);

	if (id & CAN_EFF_FLAG) {
		fi |= SJA1000_FI_FF;
		priv->write_reg(priv, F81601A_TX_FI(0), fi);
		priv->write_reg(priv, F81601A_TX_ID1(0),
				(id & 0x1fe00000) >> 21);
		priv->write_reg(priv, F81601A_TX_ID2(0),
				(id & 0x001fe000) >> 13);
		priv->write_reg(priv, F81601A_TX_ID3(0),
				(id & 0x00001fe0) >> 5);
		priv->write_reg(priv, F81601A_TX_ID4(0),
				(id & 0x0000001f) << 3);
	} else {
		priv->write_reg(priv, F81601A_TX_FI(0), fi);
		priv->write_reg(priv, F81601A_TX_ID1(0),
				(id & 0x000007f8) >> 3);
		priv->write_reg(priv, F81601A_TX_ID2(0),
				(id & 0x00000007) << 5);
	}

	for (i = 0; i < can_dlc2len(dlc); i++)
		priv->write_reg(priv, dreg++, cf->data[i]);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
	can_put_echo_skb(skb, dev, 0, can_dlc2len(dlc));
#else
	can_put_echo_skb(skb, dev, 0);
#endif

	if (priv->can.ctrlmode & CAN_CTRLMODE_ONE_SHOT)
		cmd_reg_val |= CMD_AT;

	if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
		cmd_reg_val |= CMD_SRR;
	else
		cmd_reg_val |= CMD_TR;

	if ((fi & F81601A_FI_EDL) && (cf->flags & CANFD_ESI))
		cmd_reg_val |= BIT(6);

	priv->flags |= F81601_IS_TXING;
	sja1000_write_cmdreg(priv, cmd_reg_val);
	priv->tx_cnt++;
	//schedule_delayed_work(&priv->tx_delayed_work, F81601_TX_GUARD_TIME);

	spin_unlock_irqrestore(&priv->tx_lock, flags);

	return NETDEV_TX_OK;
}

static netdev_tx_t sja1000_start_xmit_dual_write_l(struct sk_buff *skb,
						   struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);
	struct canfd_frame *cf = (struct canfd_frame *)skb->data;
	uint32_t fi;
	uint8_t dlc;
	canid_t id;
	uint32_t dreg;
	u8 cmd_reg_val = 0x00;
	int i;
	unsigned long flags;
	int len;
	uint8_t tx_idx;

	if (can_dropped_invalid_skb(dev, skb)) {
		netdev_info(dev, "%s: can_dropped_invalid_skb\n", __func__);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&priv->tx_lock, flags);

	//netdev_info(dev, "%s: empty_fifo: %d, used_fifo: %d, %d\n", __func__,
	//	kfifo_len(&priv->empty_fifo),
	//	kfifo_len(&priv->used_fifo),
	//	__LINE__);

	len = kfifo_len(&priv->empty_fifo);
	switch (len) {
	case 0:
		netdev_err(dev, "%s: empty_fifo len == 0\n", __func__);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return NETDEV_TX_OK;
	case 1:
		/* last fifo, stop queue */
		//netdev_info(dev, "%s: stop queue\n", __func__);
		netif_stop_queue(dev);
	case 2:
		break;
	default:
		//netdev_err(dev, "%s: empty_fifo len == %d\n", __func__, len);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return NETDEV_TX_OK;
	};

	// get tx fifo idx from empty
	len = kfifo_out(&priv->empty_fifo, &tx_idx, 1);

	// push tx fifo idx to used
	len = kfifo_in(&priv->used_fifo, &tx_idx, 1);

	switch (tx_idx) {
	case 0:
		priv->write_reg(priv, 0x74, 0x01);
		break;
	case 1:
		priv->write_reg(priv, 0x74, 0x11);
		break;
	default:
		netdev_err(dev, "%s: tx_idx == %d\n", __func__, tx_idx);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return NETDEV_TX_OK;
	}

	id = cf->can_id;

	if (can_is_canfd_skb(skb)) {
		fi = dlc = can_len2dlc(cf->len);
		priv->tx_size = can_dlc2len(dlc);

		fi |= F81601A_FI_EDL;

		if (cf->flags & CANFD_BRS)
			fi |= F81601A_FI_BRS;

		priv->is_tx_esi[tx_idx] = !!(cf->flags & CANFD_ESI);
	} else {
		priv->tx_size = fi = dlc = cf->len;

		if (id & CAN_RTR_FLAG)
			fi |= SJA1000_FI_RTR;
	}

	dreg = F81601A_TX_BUF(tx_idx);

	if (id & CAN_EFF_FLAG) {
		fi |= SJA1000_FI_FF;
		priv->write_reg(priv, F81601A_TX_FI(tx_idx), fi);
		priv->write_reg(priv, F81601A_TX_ID1(tx_idx),
				(id & 0x1fe00000) >> 21);
		priv->write_reg(priv, F81601A_TX_ID2(tx_idx),
				(id & 0x001fe000) >> 13);
		priv->write_reg(priv, F81601A_TX_ID3(tx_idx),
				(id & 0x00001fe0) >> 5);
		priv->write_reg(priv, F81601A_TX_ID4(tx_idx),
				(id & 0x0000001f) << 3);
	} else {
		priv->write_reg(priv, F81601A_TX_FI(tx_idx), fi);
		priv->write_reg(priv, F81601A_TX_ID1(tx_idx),
				(id & 0x000007f8) >> 3);
		priv->write_reg(priv, F81601A_TX_ID2(tx_idx),
				(id & 0x00000007) << 5);
	}

#if 1
	if (priv->write_reg_l) {
		u32 *data_ptr;
		int cnt;

		cnt = (can_dlc2len(dlc) + 3) / 4;

		// to do buggy
		for (i = 0; i < cnt; i++) {
			data_ptr = (u32 *)&cf->data[i * 4];
			priv->write_reg_l(priv, dreg + i * 4, *data_ptr);
		}
	} else {
		for (i = 0; i < can_dlc2len(dlc); i++)
			priv->write_reg(priv, dreg++, cf->data[i]);
	}

#else
	for (i = 0; i < can_dlc2len(dlc); i++)
		priv->write_reg(priv, dreg++, cf->data[i]);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
	can_put_echo_skb(skb, dev, tx_idx, can_dlc2len(dlc));
#else
	can_put_echo_skb(skb, dev, tx_idx);
#endif

	if (!(priv->flags & F81601_IS_TXING)) {
		priv->flags |= F81601_IS_TXING;

		if (priv->can.ctrlmode & CAN_CTRLMODE_ONE_SHOT)
			cmd_reg_val |= CMD_AT;

		if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
			cmd_reg_val |= CMD_SRR;
		else
			cmd_reg_val |= CMD_TR;

		if (priv->is_tx_esi[tx_idx])
			cmd_reg_val |= BIT(6);

		//pr_info("id: %x, esi: %x\n", id, priv->is_tx_esi[tx_idx]);
		cmd_reg_val |= tx_idx ? BIT(7) : 0; // tx idx

		if (priv->tx_buf_seg == -1) {
			priv->tx_buf_seg = tx_idx;
		} else {
#if 0		
			status = priv->read_reg(priv, SJA1000_SR);
			if (!(status & SR_TCS)) {
				netdev_dbg(dev, "%s: TCS err: %d %x\n", __func__, __LINE__, status);
			}		

			if (!(status & SR_TBS)) {
				netdev_dbg(dev, "%s: TBS err: %d %x\n", __func__, __LINE__, status);
			}

			if (status & SR_TS) {
				netdev_dbg(dev, "%s: SR_TS err: %d %x\n", __func__, __LINE__, status);
			}
#endif

			if (priv->tx_buf_seg != tx_idx) {
				priv->tx_buf_seg = tx_idx;
			} else {
				netdev_info(dev, "%s: err: %d %d %d\n",
					    __func__, __LINE__,
					    priv->tx_buf_seg, tx_idx);
			}
		}

		sja1000_write_cmdreg(priv, cmd_reg_val);
		priv->tx_cnt++;
	}

	spin_unlock_irqrestore(&priv->tx_lock, flags);

	return NETDEV_TX_OK;
}

static netdev_tx_t sja1000_start_xmit_dual(struct sk_buff *skb,
					   struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);
	struct canfd_frame *cf = (struct canfd_frame *)skb->data;
	uint32_t fi;
	uint8_t dlc;
	canid_t id;
	uint32_t dreg;
	u8 cmd_reg_val = 0x00;
	int i;
	unsigned long flags;
	int len;
	uint8_t tx_idx;

	if (can_dropped_invalid_skb(dev, skb)) {
		netdev_info(dev, "%s: can_dropped_invalid_skb\n", __func__);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&priv->tx_lock, flags);

	//netdev_info(dev, "%s: empty_fifo: %d, used_fifo: %d, %d\n", __func__,
	//	kfifo_len(&priv->empty_fifo),
	//	kfifo_len(&priv->used_fifo),
	//	__LINE__);

	len = kfifo_len(&priv->empty_fifo);
	switch (len) {
	case 0:
		netdev_err(dev, "%s: empty_fifo len == 0\n", __func__);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return NETDEV_TX_OK;
	case 1:
		/* last fifo, stop queue */
		//netdev_info(dev, "%s: stop queue\n", __func__);
		netif_stop_queue(dev);
	case 2:
		break;
	default:
		//netdev_err(dev, "%s: empty_fifo len == %d\n", __func__, len);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return NETDEV_TX_OK;
	};

	// get tx fifo idx from empty
	len = kfifo_out(&priv->empty_fifo, &tx_idx, 1);

	// push tx fifo idx to used
	len = kfifo_in(&priv->used_fifo, &tx_idx, 1);

	switch (tx_idx) {
	case 0:
		priv->write_reg(priv, 0x74, 0x01);
		break;
	case 1:
		priv->write_reg(priv, 0x74, 0x11);
		break;
	default:
		netdev_err(dev, "%s: tx_idx == %d\n", __func__, tx_idx);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return NETDEV_TX_OK;
	}

	id = cf->can_id;

	if (can_is_canfd_skb(skb)) {
		fi = dlc = can_len2dlc(cf->len);
		priv->tx_size = can_dlc2len(dlc);

		fi |= F81601A_FI_EDL;

		if (cf->flags & CANFD_BRS)
			fi |= F81601A_FI_BRS;

		priv->is_tx_esi[tx_idx] = !!(cf->flags & CANFD_ESI);
	} else {
		priv->tx_size = fi = dlc = cf->len;

		if (id & CAN_RTR_FLAG)
			fi |= SJA1000_FI_RTR;
	}

	dreg = F81601A_TX_BUF(tx_idx);

	if (id & CAN_EFF_FLAG) {
		fi |= SJA1000_FI_FF;
		priv->write_reg(priv, F81601A_TX_FI(tx_idx), fi);
		priv->write_reg(priv, F81601A_TX_ID1(tx_idx),
				(id & 0x1fe00000) >> 21);
		priv->write_reg(priv, F81601A_TX_ID2(tx_idx),
				(id & 0x001fe000) >> 13);
		priv->write_reg(priv, F81601A_TX_ID3(tx_idx),
				(id & 0x00001fe0) >> 5);
		priv->write_reg(priv, F81601A_TX_ID4(tx_idx),
				(id & 0x0000001f) << 3);
	} else {
		priv->write_reg(priv, F81601A_TX_FI(tx_idx), fi);
		priv->write_reg(priv, F81601A_TX_ID1(tx_idx),
				(id & 0x000007f8) >> 3);
		priv->write_reg(priv, F81601A_TX_ID2(tx_idx),
				(id & 0x00000007) << 5);
	}

#if 0
	if (priv->write_reg_l) {
		cnt = priv->tx_size / 4;
		if (priv->tx_size < 8)
			cnt += 1;

		// to do buggy
		for (i = 0; i < cnt; i++) {
			data = (cf->data[0 + i * 4] << 0) |
				(cf->data[1 + i * 4] << 8) |
				(cf->data[2 + i * 4] << 16) |
				(cf->data[3 + i * 4] << 24);
			priv->write_reg_l(priv, dreg + i * 4, data);
		}		
	} else {
		for (i = 0; i < can_dlc2len(dlc); i++)
			priv->write_reg(priv, dreg++, cf->data[i]);
	}

#else
	for (i = 0; i < can_dlc2len(dlc); i++)
		priv->write_reg(priv, dreg++, cf->data[i]);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
	can_put_echo_skb(skb, dev, tx_idx, can_dlc2len(dlc));
#else
	can_put_echo_skb(skb, dev, tx_idx);
#endif

	if (!(priv->flags & F81601_IS_TXING)) {
		priv->flags |= F81601_IS_TXING;

		if (priv->can.ctrlmode & CAN_CTRLMODE_ONE_SHOT)
			cmd_reg_val |= CMD_AT;

		if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
			cmd_reg_val |= CMD_SRR;
		else
			cmd_reg_val |= CMD_TR;

		if (priv->is_tx_esi[tx_idx])
			cmd_reg_val |= BIT(6);

		//pr_info("id: %x, esi: %x\n", id, priv->is_tx_esi[tx_idx]);
		cmd_reg_val |= tx_idx ? BIT(7) : 0; // tx idx

		if (priv->tx_buf_seg == -1) {
			priv->tx_buf_seg = tx_idx;
		} else {
#if 0		
			status = priv->read_reg(priv, SJA1000_SR);
			if (!(status & SR_TCS)) {
				netdev_dbg(dev, "%s: TCS err: %d %x\n", __func__, __LINE__, status);
			}		

			if (!(status & SR_TBS)) {
				netdev_dbg(dev, "%s: TBS err: %d %x\n", __func__, __LINE__, status);
			}

			if (status & SR_TS) {
				netdev_dbg(dev, "%s: SR_TS err: %d %x\n", __func__, __LINE__, status);
			}
#endif

			if (priv->tx_buf_seg != tx_idx) {
				priv->tx_buf_seg = tx_idx;
			} else {
				netdev_info(dev, "%s: err: %d %d %d\n",
					    __func__, __LINE__,
					    priv->tx_buf_seg, tx_idx);
			}
		}

		sja1000_write_cmdreg(priv, cmd_reg_val);
		priv->tx_cnt++;
	}

	spin_unlock_irqrestore(&priv->tx_lock, flags);

	return NETDEV_TX_OK;
}

/*
 * transmit a CAN message
 * message layout in the sk_buff should be like this:
 * xx xx xx xx	 ff	 ll   00 11 22 33 44 55 66 77
 * [  can-id ] [flags] [len] [can data (up to 8 bytes]
 */
static netdev_tx_t sja1000_start_xmit(struct sk_buff *skb,
				      struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);
	netdev_tx_t ret;

	if (priv->is_tx_more &&
	    (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK) == 0) {
		if (priv->write_reg_l)
			ret = sja1000_start_xmit_dual_write_l(skb, dev);
		else
			ret = sja1000_start_xmit_dual(skb, dev);
	} else {
		ret = sja1000_start_xmit_single(skb, dev);
	}

	return ret;
}

static int sja1000_open(struct net_device *dev)
{
	int err;

	/* set chip into reset mode */
	set_reset_mode(dev);

	/* common open */
	err = open_candev(dev);
	if (err)
		return err;

	/* init and start chi */
	sja1000_start(dev);

#if !DIABLE_CANLED && LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)
	can_led_event(dev, CAN_LED_EVENT_OPEN);
#endif

	netif_start_queue(dev);

	return 0;
}

static int sja1000_close(struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);

	//netdev_info(dev, "%s: in\n", __func__);

	set_reset_mode(dev);
	//cancel_delayed_work_sync(&priv->tx_delayed_work);
	netif_stop_queue(dev);

	//synchronize_irq(dev->irq);
	//free_irq(dev->irq, (void *)dev);
	cancel_delayed_work(&priv->busoff_delayed_work);
	close_candev(dev);

#if !DIABLE_CANLED && LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)
	can_led_event(dev, CAN_LED_EVENT_STOP);
#endif

	//netdev_info(dev, "%s: priv->rx_cnt: %lu\n", __func__, priv->rx_cnt);

	return 0;
}

static struct net_device *alloc_sja1000dev(int sizeof_priv)
{
	struct net_device *dev;
	struct sja1000_priv *priv;

	dev = alloc_candev(sizeof(struct sja1000_priv) + sizeof_priv,
			   SJA1000_ECHO_SKB_MAX);
	if (!dev)
		return NULL;

	priv = netdev_priv(dev);

	if (can_default_min_brp) {
		sja1000_bittiming_const.brp_min = can_default_min_brp;
		pr_info("%s: customize can_default_min_brp: %d\n", __func__,
			can_default_min_brp);
	}

	priv->dev = dev;
	priv->can.bittiming_const = &sja1000_bittiming_const;
	priv->can.data_bittiming_const = &sja1000_fd_bittiming_const;
	priv->can.do_set_bittiming = sja1000_set_bittiming;
	priv->can.do_set_data_bittiming = sja1000_set_data_bittiming;
	priv->can.do_set_mode = sja1000_set_mode;
	priv->can.do_get_berr_counter = sja1000_get_berr_counter;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
	//priv->can.bitrate_max = 5000000;
#endif
	priv->can.ctrlmode_supported =
		CAN_CTRLMODE_LOOPBACK | CAN_CTRLMODE_LISTENONLY |
		CAN_CTRLMODE_3_SAMPLES | CAN_CTRLMODE_ONE_SHOT |
		CAN_CTRLMODE_BERR_REPORTING | CAN_CTRLMODE_FD |
		CAN_CTRLMODE_FD_NON_ISO | CAN_CTRLMODE_PRESUME_ACK;

	spin_lock_init(&priv->tx_lock);

	if (sizeof_priv)
		priv->priv = (void *)priv + sizeof(struct sja1000_priv);

	return dev;
}

static void free_sja1000dev(struct net_device *dev)
{
	free_candev(dev);
}

static void f81601_busoff_delayed_work(struct work_struct *work)
{
	struct sja1000_priv *priv;
	struct net_device *netdev;
	struct net_device_stats *stats;
	u8 sr;
	//bool debug = false;
	struct sk_buff *skb;
	struct can_frame *cf;

	priv = container_of(work, struct sja1000_priv,
			    busoff_delayed_work.work);
	netdev = priv->dev;
	stats = &netdev->stats;

	//if (netdev->dev_id == 0)
	//	debug = true;

	cancel_delayed_work(&priv->busoff_delayed_work);

	if (priv->can.state >= CAN_STATE_BUS_OFF) {
		netdev_dbg(netdev, "%s: busoff\n", __func__);
		return;
	}

	sr = priv->read_reg(priv, SJA1000_SR);
	if ((sr & SR_CRIT) != SR_CRIT) {
		schedule_delayed_work(&priv->busoff_delayed_work,
				      F81601_BUSOFF_GUARD_TIME);
		return;
	}

	skb = alloc_can_err_skb(netdev, &cf);
	if (skb == NULL) {
		schedule_delayed_work(&priv->busoff_delayed_work,
				      F81601_BUSOFF_GUARD_TIME);
		netdev_warn(netdev, "%s: nomem\n", __func__);
		return;
	}

	priv->can.state = CAN_STATE_BUS_OFF;

	cf->can_id |= CAN_ERR_BUSOFF;
	cf->data[6] = priv->read_reg(priv, SJA1000_TXERR);
	cf->data[7] = priv->read_reg(priv, SJA1000_RXERR);

	can_bus_off(netdev);

	//if (debug)
	netdev_warn(netdev, "%s: busoff, restart timer: %d\n", __func__,
		    priv->can.restart_ms);

	stats->rx_packets++;
	stats->rx_bytes += cf->can_dlc;
	netif_rx(skb);
}

static const struct net_device_ops sja1000_netdev_ops = {
	.ndo_open = sja1000_open,
	.ndo_stop = sja1000_close,
	.ndo_start_xmit = sja1000_start_xmit,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 15, 0)
	.ndo_change_mtu = can_change_mtu,
#endif
};

static int register_sja1000dev(struct net_device *dev)
{
	int ret;

	if (!sja1000_probe_chip(dev))
		return -ENODEV;

	dev->flags |= IFF_ECHO; /* we support local echo */
	dev->netdev_ops = &sja1000_netdev_ops;

	set_reset_mode(dev);
	chipset_init(dev);

	ret = register_candev(dev);

#if !DIABLE_CANLED && LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)
	if (!ret)
		devm_can_led_init(dev);
#endif
	return ret;
}

static void unregister_sja1000dev(struct net_device *dev)
{
	set_reset_mode(dev);
	unregister_candev(dev);
}
#endif

static irqreturn_t f81601_interrupt_dual_tx(int irq, void *dev_id)
{
	struct net_device *dev = (struct net_device *)dev_id;
	//struct pci_dev *pdev = to_pci_dev(dev->dev.parent);
	//struct f81601_pci_card *card = pci_get_drvdata(pdev);
	struct sja1000_priv *priv = netdev_priv(dev);
	struct net_device_stats *stats = &dev->stats;
	uint8_t isrc, status;
	int n = 0, r = 0;
	bool is_err = false;
	unsigned long flags;
	bool en_debuf = false;
	int do_wakeup;
	uint8_t tx_idx;
	int len, tmp;
	uint8_t next_tx_idx = 0;
	uint8_t cmd_reg_val = 0x00;
	//unsigned int max_retry = 4;

#if DEBUG_IRQ_DELAY
	unsigned long long elapse;
	struct timespec start;

	if (dev->dev_id == 0) {
		//en_debuf = true;
		time_start(&start);
	}
#endif

#if 0
	if (dev->dev_id == 0) {
		netdev_info(dev, "%s: dev->dev_id: %d force end\n", __func__, dev->dev_id);
		return IRQ_HANDLED;
	}
#endif

	/* Shared interrupts and IRQ off? */
	if (priv->read_reg(priv, SJA1000_IER) == IRQ_OFF) {
		//netdev_info(dev, "IRQ_OFF\n");
		goto out;
	}

	if (en_debuf)
		netdev_info(dev, "IRQ in\n");

	while (/*(n < max_retry) &&*/
	       (isrc = priv->read_reg(priv, SJA1000_IR))) {
		status = priv->read_reg(priv, SJA1000_SR);
		if (en_debuf)
			netdev_info(dev, "isrc: %02x, sr: %02x\n", isrc,
				    status);

		/* check for absent controller due to hw unplug */
		if (status == 0xFF && sja1000_is_absent(priv)) {
			netdev_info(dev, "sja1000_is_absent\n");
			goto out;
		}

		if (isrc & IRQ_RI) {
			status = priv->read_reg(priv, SJA1000_SR);

			/* receive interrupt */
			while (status & SR_RBS) {
				//netdev_info(dev, "RX\n");
				sja1000_rx(dev);

				status = priv->read_reg(priv, SJA1000_SR);
				/* check for absent controller */
				if (status == 0xFF &&
				    sja1000_is_absent(priv)) {
					netdev_info(dev,
						    "sja1000_is_absent\n");
					goto out;
				}

				n++;
				r++;

#if F81601_USE_TASKLET
				if (0 && r >= 20) {
					f81601_disable_rx_int(priv);
					tasklet_schedule(&priv->rx_tasklet);
				}
#endif
			}
		}

		if (isrc & IRQ_WUI) {
			netdev_dbg(dev, "wakeup interrupt\n");
			n++;
		}

		if (isrc & IRQ_TI) {
			n++;
			do_wakeup = 0;
			next_tx_idx = 0;
			cmd_reg_val = 0x00;

			//netdev_info(dev, "%s: TI sr: %x\n", __func__, status);
			if ((priv->can.ctrlmode & CAN_CTRLMODE_ONE_SHOT) ||
			    priv->force_tx_resend) {
				netdev_info(dev, "%s: TI sr: %x\n", __func__,
					    status);
				priv->force_tx_resend = 0;
				do_wakeup = 1;
			} else if (status & SR_TCS) {
				do_wakeup = 1;
			} else {
				priv->tx_resend_cnt++;
				stats->tx_errors++;
				netdev_dbg(dev, "%s: tx_resend_cnt: %d\n",
					   __func__, priv->tx_resend_cnt);
			}

			if (force_tx_send_cnt > 1 &&
			    priv->tx_resend_cnt >= force_tx_send_cnt) {
				priv->force_tx_resend = 1;
				priv->tx_resend_cnt = 0;
				netdev_dbg(dev, "%s: force abort\n", __func__);

				// to do multi-tx clear
				sja1000_write_cmdreg(priv, CMD_AT | CMD_TR);
			}

			if (do_wakeup) {
				spin_lock_irqsave(&priv->tx_lock, flags);

				tx_idx = 0;
				cmd_reg_val = 0;

				//netdev_info(dev, "%s: empty_fifo: %d, used_fifo: %d, %d\n", __func__,
				//	kfifo_len(&priv->empty_fifo),
				//	kfifo_len(&priv->used_fifo),
				//	__LINE__);

				len = kfifo_len(&priv->used_fifo);
				switch (len) {
				default:
				case 0:
					netdev_err(
						dev,
						"%s: used_fifo len == %d\n",
						__func__,
						kfifo_len(&priv->used_fifo));
					netdev_err(
						dev,
						"%s: empty_fifo len == %d\n",
						__func__,
						kfifo_len(&priv->empty_fifo));
					break;
				case 1:
				case 2:
					// get tx fifo idx from used
					tmp = kfifo_out(&priv->used_fifo,
							&tx_idx, 1);
					if (tmp != 1)
						netdev_err(
							dev,
							"%s: kfifo_out len != 1 %d\n",
							__func__, __LINE__);

					// push to empty
					tmp = kfifo_in(&priv->empty_fifo,
						       &tx_idx, 1);
					if (tmp != 1)
						netdev_err(
							dev,
							"%s: kfifo_in len != 1 %d\n",
							__func__, __LINE__);

					if (len != 2) {
						priv->flags &=
							~F81601_IS_TXING;
						break;
					}

					tmp = kfifo_peek(&priv->used_fifo,
							 &next_tx_idx);
					if (tmp != 1)
						netdev_err(
							dev,
							"%s: kfifo_peek len != 1 %d\n",
							__func__, __LINE__);

					if (priv->can.ctrlmode &
					    CAN_CTRLMODE_ONE_SHOT)
						cmd_reg_val |= CMD_AT;

					if (priv->can.ctrlmode &
					    CAN_CTRLMODE_LOOPBACK)
						cmd_reg_val |= CMD_SRR;
					else
						cmd_reg_val |= CMD_TR;

					if (priv->is_tx_esi[next_tx_idx])
						cmd_reg_val |= BIT(6);

					cmd_reg_val |= next_tx_idx ?
							       BIT(7) :
							       0; // tx idx

					//netdev_info(dev, "%s: sent tx: %x\n", __func__, cmd_reg_val);

					if (priv->tx_buf_seg == -1) {
						priv->tx_buf_seg = next_tx_idx;
					} else {
						if (priv->tx_buf_seg !=
						    next_tx_idx) {
							priv->tx_buf_seg =
								next_tx_idx;
						} else {
							netdev_info(
								dev,
								"%s: err: %d %d %d\n",
								__func__,
								__LINE__,
								priv->tx_buf_seg,
								next_tx_idx);
						}
					}

					sja1000_write_cmdreg(priv,
							     cmd_reg_val);
					priv->tx_cnt++;
					break;
				}

				/* transmission buffer released */
				if ((priv->can.ctrlmode &
				     CAN_CTRLMODE_ONE_SHOT) ||
				    (status & SR_TCS)) {
					int err;

					/* transmission complete */
					stats->tx_bytes += priv->tx_size;
					stats->tx_packets++;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
					err = can_get_echo_skb(dev, tx_idx,
							       NULL);
					(void)err;
#else
					can_get_echo_skb(dev, tx_idx);
#endif
					//netdev_info(dev, "%s: tx int tcs success\n", __func__);
				} else {
					stats->tx_errors++;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
					can_free_echo_skb(dev, tx_idx, NULL);
#else
					can_free_echo_skb(dev, tx_idx);
#endif
					//netdev_info(dev, "%s: tx int tcs fail\n", __func__);
				}

				spin_unlock_irqrestore(&priv->tx_lock, flags);

				//netdev_info(dev, "%s: tx complete: %x\n", __func__, tx_idx);

				//cancel_delayed_work(&priv->tx_delayed_work);

				//netdev_info(dev, "%s: wake queue\n", __func__);
				netif_wake_queue(dev);
#if !DIABLE_CANLED && LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)
				can_led_event(dev, CAN_LED_EVENT_TX);
#endif
			}
		}

		if (isrc & (IRQ_DOI | IRQ_EI | IRQ_BEI | IRQ_EPI | IRQ_ALI)) {
			n++;
			is_err = true;

			/* error interrupt */
			sja1000_err(dev, isrc, status);
		}

#if 0
		if (isrc & IRQ_DOI) {
			netdev_info(dev, "IRQ_DOI, isrc: %02x, n: %d, r: %d\n", isrc, n, r);
			set_reset_mode(dev);
		}
#endif
	}
out:

#if DEBUG_IRQ_DELAY
	if (en_debuf) {
		elapse = time_end(&start);

		netdev_info(dev, "IRQ OUT elapse: %llu, n: %d, r: %d\n",
			    elapse, n, r);
	}
#else
	if (en_debuf)
		netdev_info(dev, "IRQ OUT\n");
#endif

	//if (!n)
	//	netdev_info(dev, "%s: irq not handle: %x %x\n", __func__, isrc, status);

	return (n) ? IRQ_HANDLED : IRQ_NONE;
}

static irqreturn_t f81601_interrupt_single_tx(int irq, void *dev_id)
{
	struct net_device *dev = (struct net_device *)dev_id;
	//struct pci_dev *pdev = to_pci_dev(dev->dev.parent);
	//struct f81601_pci_card *card = pci_get_drvdata(pdev);
	struct sja1000_priv *priv = netdev_priv(dev);
	struct net_device_stats *stats = &dev->stats;
	uint8_t isrc, status;
	int n = 0, r = 0;
	unsigned long flags;
	bool en_debuf = false;
	int do_wakeup;
	//unsigned int max_retry = 10;

#if DEBUG_IRQ_DELAY
	unsigned long long elapse;
	struct timespec start;

	if (dev->dev_id == 0) {
		//en_debuf = true;
		time_start(&start);
	}
#endif

	/* Shared interrupts and IRQ off? */
	if (priv->read_reg(priv, SJA1000_IER) == IRQ_OFF) {
		//netdev_info(dev, "IRQ_OFF\n");
		goto out;
	}

	if (en_debuf)
		netdev_info(dev, "IRQ in\n");

	while (/*(n < max_retry) &&*/
	       (isrc = priv->read_reg(priv, SJA1000_IR))) {
		status = priv->read_reg(priv, SJA1000_SR);
		if (en_debuf)
			netdev_info(dev, "isrc: %02x, sr: %02x\n", isrc,
				    status);

		/* check for absent controller due to hw unplug */
		if (status == 0xFF && sja1000_is_absent(priv)) {
			netdev_info(dev, "sja1000_is_absent\n");
			goto out;
		}

		if (isrc & (IRQ_DOI | IRQ_EI | IRQ_BEI | IRQ_EPI | IRQ_ALI)) {
			n++;

			/* error interrupt */
			sja1000_err(dev, isrc, status);
		}

		if (isrc & IRQ_RI) {
			status = priv->read_reg(priv, SJA1000_SR);

			/* receive interrupt */
			while (status & SR_RBS) {
				//netdev_info(dev, "RX\n");
				sja1000_rx(dev);

				status = priv->read_reg(priv, SJA1000_SR);
				/* check for absent controller */
				if (status == 0xFF &&
				    sja1000_is_absent(priv)) {
					netdev_info(dev,
						    "sja1000_is_absent\n");
					goto out;
				}

				n++;
				r++;
			}
		}

		if (isrc & IRQ_WUI) {
			netdev_dbg(dev, "wakeup interrupt\n");
			n++;
		}

		if (isrc & IRQ_TI) {
			n++;
			do_wakeup = 0;

			if ((priv->can.ctrlmode & CAN_CTRLMODE_ONE_SHOT) ||
			    priv->force_tx_resend) {
				//netdev_info(dev, "%s: TI sr: %x\n", __func__, status);
				priv->force_tx_resend = 0;
				do_wakeup = 1;
			} else if (status & SR_TCS) {
				do_wakeup = 1;
			} else {
				priv->tx_resend_cnt++;
				stats->tx_errors++;
				netdev_dbg(dev, "%s: tx_resend_cnt: %d\n",
					   __func__, priv->tx_resend_cnt);
			}

			if (force_tx_send_cnt > 1 &&
			    priv->tx_resend_cnt >= force_tx_send_cnt) {
				priv->force_tx_resend = 1;
				priv->tx_resend_cnt = 0;
				netdev_dbg(dev, "%s: force abort\n", __func__);
				sja1000_write_cmdreg(priv, CMD_AT | CMD_TR);
			}

			if (do_wakeup) {
				spin_lock_irqsave(&priv->tx_lock, flags);

				if (priv->flags & F81601_IS_TXING) {
					/* transmission buffer released */
					if ((priv->can.ctrlmode &
					     CAN_CTRLMODE_ONE_SHOT) ||
					    (status & SR_TCS)) {
						int err;

						/* transmission complete */
						stats->tx_bytes +=
							priv->tx_size;
						stats->tx_packets++;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
						err = can_get_echo_skb(dev, 0,
								       NULL);
						(void)err;
#else
						can_get_echo_skb(dev, 0);
#endif
						//netdev_info(dev, "%s: tx int tcs success\n", __func__);
					} else {
						stats->tx_errors++;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
						can_free_echo_skb(dev, 0,
								  NULL);
#else
						can_free_echo_skb(dev, 0);
#endif
						//netdev_info(dev, "%s: tx int tcs fail\n", __func__);
					}

					//cancel_delayed_work(&priv->tx_delayed_work);
					netif_wake_queue(dev);
#if !DIABLE_CANLED && LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)
					can_led_event(dev, CAN_LED_EVENT_TX);
#endif
					priv->flags &= ~F81601_IS_TXING;
				}

				spin_unlock_irqrestore(&priv->tx_lock, flags);
			}
		}

#if 0
		if (isrc & IRQ_DOI) {
			netdev_info(dev, "IRQ_DOI, isrc: %02x, n: %d, r: %d\n", isrc, n, r);
			set_reset_mode(dev);
		}
#endif
	}
out:

#if DEBUG_IRQ_DELAY
	if (en_debuf) {
		elapse = time_end(&start);

		netdev_info(dev, "IRQ OUT elapse: %llu, n: %d, r: %d\n",
			    elapse, n, r);
	}
#endif

	return (n) ? IRQ_HANDLED : IRQ_NONE;
}

static irqreturn_t f81601_interrupt(int irq, void *dev_id)
{
	struct net_device *dev = (struct net_device *)dev_id;
	struct sja1000_priv *priv = netdev_priv(dev);
	irqreturn_t ret;

	if (priv->is_tx_more &&
	    (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK) == 0) {
		ret = f81601_interrupt_dual_tx(irq, dev_id);
	} else {
		ret = f81601_interrupt_single_tx(irq, dev_id);
	}

	return ret;
}

static void f81601_tx_delayed_work(struct work_struct *work)
{
	struct sja1000_priv *priv;
	struct net_device *netdev;
	unsigned long flags;
	int r;
	u8 sr;

	priv = container_of(work, struct sja1000_priv, tx_delayed_work.work);
	netdev = priv->dev;

	netdev_dbg(netdev, "%s: into\n", __func__);

	if (priv->can.state >= CAN_STATE_BUS_OFF) {
		netdev_dbg(netdev, "%s: busoff\n", __func__);
		return;
	}

	spin_lock_irqsave(&priv->tx_lock, flags);

	if (!(priv->flags & F81601_IS_TXING)) {
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return;
	}

	sr = priv->read_reg(priv, SJA1000_SR);
	if ((sr & (SR_TBS | SR_TS)) != SR_TBS) {
		netdev_dbg(netdev, "%s: not idle, schedule next\n", __func__);

		cancel_delayed_work(&priv->tx_delayed_work);
		schedule_delayed_work(&priv->tx_delayed_work,
				      F81601_TX_GUARD_TIME);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return;
	}

	if (!(priv->can.ctrlmode & CAN_CTRLMODE_ONE_SHOT) && !(sr & SR_TCS)) {
		netdev_dbg(netdev, "%s: not completed tx, schedule next\n",
			   __func__);

		cancel_delayed_work(&priv->tx_delayed_work);
		schedule_delayed_work(&priv->tx_delayed_work,
				      F81601_TX_GUARD_TIME);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return;
	}

	netdev_dbg(netdev, "%s: wake tx queue\n", __func__);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
	r = can_get_echo_skb(netdev, 0, NULL);
	(void)r;
#else
	can_get_echo_skb(netdev, 0);
#endif

	netif_wake_queue(netdev);
#if !DIABLE_CANLED && LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)
	can_led_event(netdev, CAN_LED_EVENT_TX);
#endif

	priv->flags &= ~F81601_IS_TXING;
	spin_unlock_irqrestore(&priv->tx_lock, flags);
}

#if 0
static u8 f81601_pci_io_read_reg(const struct sja1000_priv *priv, int port)
{
	void __iomem *reg;

	if (port < 0x80)
		reg = priv->reg_base + port;
	else
		reg = priv->reg_fd_base + port;

	return ioread8(reg);
}

static void f81601_pci_io_write_reg(const struct sja1000_priv *priv, int port, u8 val)
{
	void __iomem *reg;

	if (port < 0x80)
		reg = priv->reg_base + port;
	else
		reg = priv->reg_fd_base + port;

	iowrite8(val, reg);
}

void f81601_pci_io_write_mask_reg(const struct sja1000_priv *priv, int reg, u8 mask, u8 val)
{
	//int status;
	u8 tmp;

	tmp = f81601_pci_io_read_reg(priv, reg);

	tmp &= ~mask;
	tmp |= (mask & val);

	f81601_pci_io_write_reg(priv, reg, tmp);
}

static uint32_t f81601_pci_io_read_reg_l(const struct sja1000_priv *priv, int port)
{
	void __iomem *reg;

	if (port < 0x80)
		reg = priv->reg_base + port;
	else
		reg = priv->reg_fd_base + port;

	return ioread32(reg);
}

static void f81601_pci_io_write_reg_l(const struct sja1000_priv *priv, int port, uint32_t val)
{
	void __iomem *reg;

	if (port < 0x80)
		reg = priv->reg_base + port;
	else
		reg = priv->reg_fd_base + port;

	iowrite32(val, reg);
}
#endif

static u8 f81601_pci_mmio_read_reg(const struct sja1000_priv *priv, int port)
{
	void __iomem *reg;
	u8 tmp;

	if (port < 0x80)
		reg = priv->reg_base + port;
	else
		reg = priv->reg_fd_base + port;

	tmp = readb(reg);
	//netdev_info(priv->can.dev, "%s: read reg: %02xh, data: %02xh\n", __func__, port, tmp);
#if 0
	struct net_device *dev = priv->dev;
	dev_info(dev, "Removing %s\n", dev->name);

#endif
	return tmp;
}

static void f81601_pci_mmio_write_reg(const struct sja1000_priv *priv,
				      int port, u8 val)
{
	volatile void __iomem *reg;

	if (port < 0x80)
		reg = priv->reg_base + port;
	else
		reg = priv->reg_fd_base + port;

		//netdev_info(priv->can.dev, "%s: write reg: %02xh, data: %02xh\n", __func__, port, val);
#if 0
	struct net_device *dev = priv->dev;

	if (!dev->dev_id)
		netdev_info(dev, "write %02x: %02x\n", port, val);
#endif
	writeb(val, reg);
}

static void f81601_pci_mmio_write_mask_reg(const struct sja1000_priv *priv,
					   int reg, u8 mask, u8 val)
{
	//int status;
	u8 tmp;

	tmp = f81601_pci_mmio_read_reg(priv, reg);

	tmp &= ~mask;
	tmp |= (mask & val);

	f81601_pci_mmio_write_reg(priv, reg, tmp);
}

static uint32_t f81601_pci_mmio_read_reg_l(const struct sja1000_priv *priv,
					   int port)
{
	void __iomem *reg;

	if (port < 0x80)
		reg = priv->reg_base + port;
	else
		reg = priv->reg_fd_base + port;

	return readl(reg);
}

#if 1
static void f81601_pci_mmio_write_reg_l(const struct sja1000_priv *priv,
					int port, uint32_t val)
{
	volatile void __iomem *reg;

	if (port < 0x80)
		reg = priv->reg_base + port;
	else
		reg = priv->reg_fd_base + port;

	writel(val, reg);
}
#endif

static ssize_t read_id_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);
	u8 tmp;

	pci_read_config_byte(pdev, 0x20a, &tmp);
	tmp &= GENMASK(2, 0);

	return sprintf(buf, "%x\n", tmp);
}

static DEVICE_ATTR_RO(read_id);

#if 0
static ssize_t tx_cnt_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct net_device *netdev = container_of(dev, struct net_device, dev);
	struct sja1000_priv *priv = netdev_priv(netdev);

	return sprintf(buf, "%lu\n", priv->tx_cnt);
}

static DEVICE_ATTR_RO(tx_cnt);
#endif

static void f81601_pci_del_card(struct pci_dev *pdev)
{
	struct f81601_pci_card *card = pci_get_drvdata(pdev);
	struct sja1000_priv *priv;
	struct net_device *dev;
	int i = 0;

	device_remove_file(&pdev->dev, &dev_attr_read_id);

	for (i = 0; i < F81601_PCI_MAX_CHAN; i++) {
		dev = card->net_dev[i];
		if (!dev)
			continue;

		dev_info(&pdev->dev, "Removing %s\n", dev->name);
		//device_remove_file(&dev->dev, &dev_attr_tx_cnt);

		priv = netdev_priv(dev);
		synchronize_irq(dev->irq);
		free_irq(dev->irq, (void *)dev);

		unregister_sja1000dev(dev);

		kfifo_free(&priv->used_fifo);
		kfifo_free(&priv->empty_fifo);

#if F81601_USE_TASKLET
		tasklet_disable(&priv->rx_tasklet);
		tasklet_kill(&priv->rx_tasklet);
#endif

		free_sja1000dev(dev);
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 8, 0)
	if (pdev->msi_enabled)
		pci_free_irq_vectors(pdev);

#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)
	if (pdev->msi_enabled)
		pci_disable_msi(pdev);
#else
	if (pdev->msi_enabled)
		pci_disable_msi(pdev);
#endif
}

static int f81601_can_pre_config(struct net_device *dev)
{
	struct sja1000_priv *priv = netdev_priv(dev);
	unsigned char data;
	unsigned char key[4] = { 0x32, 0x5d, 0x42, 0xac };
	int i;

	priv->write_reg(priv, 0x7c, 0x35); // force deactive

	for (i = 0; i < ARRAY_SIZE(key); ++i)
		priv->write_reg(priv, 0x7c, key[i]);

	data = priv->read_reg(priv, 0x7f);
	//pr_info("%s data: %x\n", __func__, data);

	//data = BIT(2); // write clear
	data = BIT(5) | BIT(4); // 256
	//data = 0;

	priv->write_reg(priv, 0x7f, data);

	data = 0;
	data = priv->read_reg(priv, 0x7f);
	//pr_info("%s data: %x\n", __func__, data);

	priv->write_reg(priv, 0x7c, 0x35); // force deactive

	if (multi_tx_queue)
		priv->write_reg(priv, 0x74, 0x01); // en multi tx
	else
		priv->write_reg(priv, 0x74, 0x00); // dis multi tx
	data = priv->read_reg(priv, 0x74);

	if (data == 0x01)
		priv->is_tx_more = true;
	//pr_info("%s: is_tx_more: %x\n", __func__, priv->is_tx_more);

	return 0;
}

#if F81601_USE_TASKLET
static void f81601_rx_tasklet(unsigned long data)
{
	struct sja1000_priv *priv = (void *)data;
	struct net_device *dev = priv->dev;

	//netdev_info(dev, "%s: in\n", __func__);

	while (priv->read_reg(priv, SJA1000_SR) & SR_RBS) {
		sja1000_rx(dev);
	}

	if (priv->can.state != CAN_STATE_BUS_OFF)
		f81601_enable_rx_int(priv);
}
#endif

#ifndef pci_clear_and_set_config_byte
static void pci_clear_and_set_config_byte(const struct pci_dev *dev, int pos,
					  u8 clear, u8 set)
{
	u8 val;

	pci_read_config_byte(dev, pos, &val);
	val &= ~clear;
	val |= set;
	pci_write_config_byte(dev, pos, val);
}
#endif

static int f81601_detect_osc(struct pci_dev *pdev)
{
	struct f81601_pci_card *card;
	u8 tmp, mask;

	card = pci_get_drvdata(pdev);
	mask = GENMASK(3, 2);

	// force external
	pci_clear_and_set_config_byte(pdev, F81601_DECODE_REG, mask, 0);

	// must read once only, when no external osc and read
	// more times will occur hangup
	tmp = readb(card->addr + SJA1000_MOD);
	if (tmp != 0xff) {
		// external

		card->is_internal_osc = 0;

		dev_info(
			&pdev->dev,
			"F81601 running with auto/external clock: %dMhz, div: %d\n",
			external_clk / 1000000, F81601_DIVIDE_CLK);

	} else {
		// internal

		card->is_internal_osc = 1;
		pci_clear_and_set_config_byte(pdev, F81601_DECODE_REG, mask,
					      mask);

		dev_info(
			&pdev->dev,
			"F81601 running with auto/internal clock: 80Mhz, div: %d\n",
			F81601_DIVIDE_CLK);
	}

	return 0;
}

/*
 * Probe F8160x based device for the SJA1000 chips and register each
 * available CAN channel to SJA1000 Socket-CAN subsystem.
 */
static int f81601_pci_add_card(struct pci_dev *pdev,
			       const struct pci_device_id *ent)
{
	struct sja1000_priv *priv;
	struct net_device *dev;
	struct f81601_pci_card *card;
	int err, i;
	int irq_count = 1;
	u8 tmp;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
	unsigned int int_flag = PCI_IRQ_INTX;
#else
	unsigned int int_flag = PCI_IRQ_LEGACY;
#endif

	dev_info(&pdev->dev, "Fintek F81601 Driver version: %s\n", DRV_VER);

	if (pcim_enable_device(pdev) < 0) {
		dev_err(&pdev->dev, "Failed to enable PCI device\n");
		return -ENODEV;
	}

	/* Allocate card structures to hold addresses, ... */
	card = devm_kzalloc(&pdev->dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	pci_set_drvdata(pdev, card);

	card->channels = 0;
	card->dev = pdev;
	card->is_internal_osc = internal_clk;
	spin_lock_init(&card->lock);

	// enable 2 CAN
	pci_clear_and_set_config_byte(pdev, F81601_DECODE_REG, GENMASK(1, 0),
				      GENMASK(1, 0));

	dev_info(&pdev->dev, "Using MMIO interface\n");
	card->addr = pcim_iomap(pdev, 0, pci_resource_len(pdev, 0));
	pci_clear_and_set_config_byte(pdev, F81601_DECODE_REG, BIT(6), BIT(6));

	if (internal_clk >= 0) {
		if (internal_clk == 0) {
			dev_info(
				&pdev->dev,
				"F81601 running with force/external clock: %dMhz, div: %d\n",
				external_clk / 1000000, F81601_DIVIDE_CLK);

			pci_clear_and_set_config_byte(pdev, F81601_DECODE_REG,
						      GENMASK(3, 2), 0);
		} else {
			dev_info(
				&pdev->dev,
				"F81601 running with force/internal clock: 80Mhz, div: %d\n",
				F81601_DIVIDE_CLK);

			pci_clear_and_set_config_byte(pdev, F81601_DECODE_REG,
						      GENMASK(3, 2),
						      GENMASK(3, 2));
		}
	} else {
		err = f81601_detect_osc(pdev);
		if (err)
			return -ENODEV;
	}

	// 209h src_clk_no_div  1 => no div

#if F81601_CAN_CLK_DIV2
	pci_clear_and_set_config_byte(pdev, F81601_DECODE_REG, BIT(4), 0);
	//card->decode_cfg &= ~BIT(4);
#else
	pci_clear_and_set_config_byte(pdev, F81601_DECODE_REG, BIT(4), BIT(4));
	//card->decode_cfg |= BIT(4);
#endif

	// force external clock
	//card->decode_cfg &= ~0x0c;

	if (disable_ssp)
		dev_info(&pdev->dev, "CAN/FD Not using TX delay ACKed\n");

	pci_read_config_byte(pdev, F81601_DECODE_REG, &card->decode_cfg);
	//pci_write_config_byte(pdev, F81601_DECODE_REG, card->decode_cfg);

	if (card->is_internal_osc) {
		/* clk_out_pll_sel (48/96,80Mhz) */
		pci_read_config_byte(pdev, 0x296, &tmp);

#if F81601_FORCE_96MHZ
		// force 96MHz
		pci_write_config_byte(pdev, 0x296, 0x80);
#if F81601_CAN_CLK_OUT_PLL_SEL
		pci_write_config_byte(pdev, 0x296, 0xb0); // ok
#else
		pci_write_config_byte(pdev, 0x296, 0x70);
#endif

#else
#if F81601_CAN_CLK_OUT_PLL_SEL
		// force 80MHz
		//pci_write_config_byte(pdev, 0x296, 0x84);
		//pci_write_config_byte(pdev, 0x296, 0xd4);
		//pci_write_config_byte(pdev, 0x296, 0x80); // ok
		pci_write_config_byte(pdev, 0x296, 0xc0);
#else
		// force 40MHz
		//pci_write_config_byte(pdev, 0x296, 0x00);
		//pci_write_config_byte(pdev, 0x296, 0x10);
		pci_write_config_byte(pdev, 0x296, 0x40);
#endif

#endif

		msleep(1000);
	}

	if (!card->addr) {
		err = -ENOMEM;
		dev_err(&pdev->dev, "Failed to remap BAR\n");
		goto failure_cleanup;
	}

	if (enable_msi) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 8, 0)
		irq_count = pci_alloc_irq_vectors(pdev, 1, max_msi_ch,
						  int_flag | PCI_IRQ_MSI |
							  PCI_IRQ_AFFINITY);
		if (irq_count < 0)
			return irq_count;

#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)
		irq_count = pci_enable_msi_range(pdev, 1, max_msi_ch);
		if (irq_count < 0) {
			irq_count = 1;
			pci_intx(pdev, 1);
		}

#else
		irq_count = pci_enable_msi_block(pdev, max_msi_ch);
		if (irq_count < 0) {
			irq_count = 1;
			pci_intx(pdev, 1);
		} else if (irq_count == 0) { // fully support
			irq_count = max_msi_ch;
		} else {
			// partial support, irq_count is count
		}
#endif
	}

#if 0
	unsigned long flags;
	static spinlock_t lock;
	int test_cnt = 10000;
	s64 start, end;

	spin_lock_init(&lock);
	
	spin_lock_irqsave(&lock, flags);

	// speed test
	pr_info("mmio read test\n");

	start = ktime_get();
	for (i = 0; i < test_cnt; ++i)
		readb(card->addr);
	end = ktime_get();
	pr_info("mmio read test finish: %lld, cnt: %d, rate: %lld\n", end - start,
			test_cnt, (end - start) / test_cnt);


	pr_info("mmio write test\n");

	start = ktime_get();
	for (i = 0; i < test_cnt; ++i)
		writeb(0x01, card->addr);
	end = ktime_get();
	pr_info("mmio write test finish: %lld, cnt: %d, rate: %lld\n", end - start,
			test_cnt, (end - start) / test_cnt);

	pr_info("config read test\n");

	start = ktime_get();
	for (i = 0; i < test_cnt; ++i)
		pci_read_config_byte(pdev, 0x296, &tmp);
	end = ktime_get();
	pr_info("config read test finish: %lld, cnt: %d, rate: %lld\n", end - start,
			test_cnt, (end - start) / test_cnt);

	pr_info("config write test\n");

	start = ktime_get();
	for (i = 0; i < test_cnt; ++i)
		pci_write_config_byte(pdev, 0x296, 0x84);
	end = ktime_get();
	pr_info("config write test finish: %lld, cnt: %d, rate: %lld\n", end - start,
			test_cnt, (end - start) / test_cnt);

	pci_write_config_byte(pdev, 0x296, 0xd4);

	spin_unlock_irqrestore(&lock, flags);

	msleep(1000);
#endif

	/* Detect available channels */
	for (i = 0; i < ent->driver_data; i++) {
		/* read CAN2_HW_EN strap pin */
		pci_read_config_byte(pdev, 0x20a, &tmp);
		if (i == 1 && !(tmp & BIT(4)))
			break;

		dev = alloc_sja1000dev(0);
		if (!dev) {
			err = -ENOMEM;
			goto failure_cleanup;
		}

		card->net_dev[i] = dev;
		priv = netdev_priv(dev);
		priv->priv = card;
		priv->irq_flags = /*IRQF_ONESHOT |*/ IRQF_SHARED;

		INIT_DELAYED_WORK(&priv->tx_delayed_work,
				  f81601_tx_delayed_work);
		INIT_DELAYED_WORK(&priv->busoff_delayed_work,
				  f81601_busoff_delayed_work);
#if F81601_USE_TASKLET
		tasklet_init(&priv->rx_tasklet, f81601_rx_tasklet,
			     (unsigned long)priv);
#endif

		if (pci_dev_msi_enabled(pdev))
			priv->irq_flags |= IRQF_NO_SUSPEND;

		if (pdev->msi_enabled) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 8, 0)
			dev->irq = pci_irq_vector(pdev, i % irq_count);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)
			dev->irq = pdev->irq + i % irq_count;
#else
			dev->irq = pdev->irq + i % irq_count;
#endif
		} else {
			dev->irq = pdev->irq;
		}

		priv->reg_base = card->addr + 0x80 * i;
		priv->reg_fd_base = card->addr + 0x800 * i;

		priv->read_reg = f81601_pci_mmio_read_reg;
		priv->write_reg = f81601_pci_mmio_write_reg;

		priv->read_reg_l = f81601_pci_mmio_read_reg_l;
		priv->write_reg_l = f81601_pci_mmio_write_reg_l;
		priv->write_mask_reg = f81601_pci_mmio_write_mask_reg;

		if (card->is_internal_osc) {
#if F81601_FORCE_96MHZ
			priv->can.clock.freq = 96000000 / F81601_DIVIDE_CLK;
#else
			priv->can.clock.freq = 80000000 / F81601_DIVIDE_CLK;
#endif
			//priv->can.clock.freq = 125000000;
		} else {
			priv->can.clock.freq =
				external_clk / F81601_DIVIDE_CLK;
		}

#if F81601_CAN_CLK_DIV2
		priv->can.clock.freq = priv->can.clock.freq / 2;
#endif

#if !F81601_CAN_CLK_OUT_PLL_SEL
		priv->can.clock.freq = priv->can.clock.freq / 2;
#endif

		priv->ocr = OCR_TX0_PUSHPULL | OCR_TX1_PUSHPULL;
		priv->cdr = CDR_CBP;

		SET_NETDEV_DEV(dev, &pdev->dev);
		dev->dev_id = i;

		err = kfifo_alloc(&priv->empty_fifo, 2, GFP_KERNEL);
		if (err)
			goto failure_cleanup;

		err = kfifo_alloc(&priv->used_fifo, 2, GFP_KERNEL);
		if (err)
			goto failure_cleanup;

		f81601_can_pre_config(dev);

		priv->is_read_more_rx = true;
		tmp = priv->read_reg(priv, 0x7f);
		if (tmp & BIT(6))
			priv->is_read_more_rx = false;

		//pr_info("is_read_more_rx: %d\n", priv->is_read_more_rx);

		if (bus_restart_ms)
			priv->can.restart_ms = bus_restart_ms;

		/* Register SJA1000 device */
		err = register_sja1000dev(dev);
		if (err) {
			dev_err(&pdev->dev,
				"Registering device failed "
				"(err=%d)\n",
				err);
			goto failure_cleanup;
		}

		/* force into reset mode */
		priv->write_reg(priv, SJA1000_MOD, MOD_RM);
#if 0
		err = request_threaded_irq(dev->irq, NULL, f81601_interrupt, priv->irq_flags,
						dev->name, (void *)dev);
#else
		err = request_irq(dev->irq, f81601_interrupt, priv->irq_flags,
				  dev->name, (void *)dev);
#endif
		if (err)
			goto failure_cleanup;

		card->channels++;

		//device_create_file(&dev->dev, &dev_attr_tx_cnt);

		dev_info(&pdev->dev,
			 "Channel #%d at 0x%p, irq %d "
			 "registered as %s\n",
			 i + 1, priv->reg_base, dev->irq, dev->name);
	}

	if (!card->channels) {
		err = -ENODEV;
		goto failure_cleanup;
	}

	device_create_file(&pdev->dev, &dev_attr_read_id);

	return 0;

failure_cleanup:
	dev_err(&pdev->dev, "Error: %d. Cleaning Up.\n", err);
	f81601_pci_del_card(pdev);

	return err;
}

static int f81601_pci_suspend(struct device *device)
{
	struct f81601_pci_card *card;
	struct sja1000_priv *priv;
	struct net_device *dev;
	struct pci_dev *pdev;
	int i, j;

	pdev = to_pci_dev(device);
	card = pci_get_drvdata(pdev);

	for (i = 0; i < ARRAY_SIZE(card->net_dev); i++) {
		dev = card->net_dev[i];
		if (!dev)
			continue;

		priv = netdev_priv(dev);
		netif_stop_queue(dev);

		/* force into reset mode */
		priv->write_reg(priv, SJA1000_MOD, MOD_RM);

		/* save necessary register data */
		for (j = 0; j < ARRAY_SIZE(card->reg_table[i]); ++j)
			card->reg_table[i][j] = priv->read_reg(priv, j);

		/* disable interrupt */
		priv->shadow_ier = 0;
		priv->write_reg(priv, SJA1000_IER, 0);
		synchronize_irq(dev->irq);
	}

	return 0;
}

static int f81601_pci_resume(struct device *device)
{
	struct pci_dev *pdev;
	struct f81601_pci_card *card;
	struct sja1000_priv *priv;
	struct net_device *dev;
	int i;
#if 0
	u8 restore_reg_table[] = { SJA1000_BTR0, SJA1000_BTR1, SJA1000_CDR,
		SJA1000_OCR, SJA1000_ACCC0, SJA1000_ACCC1, SJA1000_ACCC2,
		SJA1000_ACCC3, SJA1000_ACCM0, SJA1000_ACCM1, SJA1000_ACCM2,
		SJA1000_ACCM3, SJA1000_IER,
	};
#endif

	pdev = to_pci_dev(device);
	card = pci_get_drvdata(pdev);

	/* recovery all needed configure */
	pci_write_config_byte(pdev, F81601_DECODE_REG, card->decode_cfg);

	/* todo internal clock tune */

	for (i = 0; i < ARRAY_SIZE(card->net_dev); i++) {
		dev = card->net_dev[i];
		if (!dev)
			continue;

		priv = netdev_priv(dev);
		if (priv->can.state == CAN_STATE_STOPPED ||
		    priv->can.state == CAN_STATE_BUS_OFF)
			continue;

		/* force into reset mode */
		priv->write_reg(priv, SJA1000_MOD, MOD_RM);

		dev_dbg(&pdev->dev, "%s: ctrlmode: %x\n", __func__,
			priv->can.ctrlmode);

		if ((priv->can.ctrlmode & CAN_CTRLMODE_FD) ||
		    (priv->can.ctrlmode & CAN_CTRLMODE_FD_NON_ISO)) {
			sja1000_set_data_bittiming(dev);
		}

		sja1000_start(dev);

		if (netif_queue_stopped(dev))
			netif_wake_queue(dev);
	}

	return 0;
}

static const struct dev_pm_ops f81601_pm_ops = { SET_SYSTEM_SLEEP_PM_OPS(
	f81601_pci_suspend, f81601_pci_resume) };

static struct pci_driver f81601_pci_driver = {
	.name = DRV_NAME,
	.id_table = f81601_pci_tbl,
	.probe = f81601_pci_add_card,
	.remove = f81601_pci_del_card,
	.driver.pm = &f81601_pm_ops,
};

MODULE_LICENSE("GPL v2");

#if 1
module_pci_driver(f81601_pci_driver);
#else
static int __init f81601_pci_driver_init(void)
{
	return pci_register_driver(&f81601_pci_driver);
}

static void __exit f81601_pci_driver_exit(void)
{
	pci_unregister_driver(&f81601_pci_driver);
}

module_init(f81601_pci_driver_init);
module_exit(f81601_pci_driver_exit);
#endif
