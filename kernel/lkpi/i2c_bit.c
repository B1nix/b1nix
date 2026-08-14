/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Bit-banged I2C, for reading an EDID when the hardware controller will not.
 *
 * i915 does not treat a GMBUS timeout as fatal. It returns -EAGAIN so the i2c
 * core retries, and on the retry it drives the same pins by hand through the
 * callbacks in struct i2c_algo_bit_data — the "enabling bit-banging" line in
 * its log is that recovery happening. Without an implementation behind those
 * callbacks the recovery path is dead, and a GMBUS timeout becomes a monitor
 * that does not exist: every connector reports disconnected, no modes are
 * enumerated, and no modeset can run.
 *
 * Upstream's version is i2c-algo-bit.c, which is GPL-2.0 and therefore not
 * imported. This is the same protocol written from the specification.
 *
 * The lines are open-drain with pull-ups, which shapes the whole implementation:
 * a device may hold either line low, so "driving high" means releasing and then
 * checking that the line actually rose. Clock stretching — a slave holding SCL
 * low because it is not ready — is a normal part of the protocol and is waited
 * for rather than assumed away.
 *
 * What it does not do: multi-master arbitration, 10-bit addressing, and SMBus
 * block transfers. DDC is a single-master bus with 7-bit addresses, which is the
 * only thing that reaches here.
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/printk.h>
#include <lkpi/env.h>

/* Half a clock period. The driver states it per adapter; DDC runs at 100 kHz,
 * so this is 5 µs there. */
static void half_period(const struct i2c_algo_bit_data *bit)
{
	udelay(bit->udelay ? (unsigned long)bit->udelay : 5);
}

static void sda_set(const struct i2c_algo_bit_data *bit, int high)
{
	bit->setsda(bit->data, high);
	half_period(bit);
}

/*
 * Release SCL and wait for it to actually rise.
 *
 * The wait is the point: a slave that is not ready holds SCL down, and a master
 * that clocked on regardless would shift the next bit while the slave was still
 * working on the last one. Bounded, because a line stuck low is a fault rather
 * than a slow device — the adapter's own timeout is in jiffies upstream; here it
 * is a plain microsecond budget, which is what the callers actually need.
 */
static int scl_high(const struct i2c_algo_bit_data *bit)
{
	unsigned waited = 0;
	/*
	 * Milliseconds, not tens of them. A slave stretches a clock for the time it
	 * needs to get a byte ready, which is microseconds; anything beyond a
	 * couple of milliseconds is a line that is not coming back. The budget is
	 * per edge, so a generous one is not merely slow — eight bits and an ack
	 * multiply it, and a dead bus turns a probe into minutes of grinding
	 * instead of a prompt failure the caller can act on.
	 */
	const unsigned limit_us = 2000;

	bit->setscl(bit->data, 1);
	half_period(bit);

	if (!bit->getscl)
		return 0; /* the driver cannot read it back; trust the write */

	while (!bit->getscl(bit->data)) {
		if (waited >= limit_us)
			return -ETIMEDOUT;
		udelay(10);
		waited += 10;
	}
	return 0;
}

static void scl_low(const struct i2c_algo_bit_data *bit)
{
	bit->setscl(bit->data, 0);
	half_period(bit);
}

/* START: SDA falls while SCL is high. */
static int bit_start(const struct i2c_algo_bit_data *bit)
{
	int ret;

	bit->setsda(bit->data, 1);
	ret = scl_high(bit);
	if (ret)
		return ret;
	sda_set(bit, 0);
	scl_low(bit);
	return 0;
}

/* Repeated START, from the middle of a transfer: SDA must be released first. */
static int bit_repstart(const struct i2c_algo_bit_data *bit)
{
	int ret;

	bit->setsda(bit->data, 1);
	scl_low(bit);
	ret = scl_high(bit);
	if (ret)
		return ret;
	sda_set(bit, 0);
	scl_low(bit);
	return 0;
}

/* STOP: SDA rises while SCL is high. */
static void bit_stop(const struct i2c_algo_bit_data *bit)
{
	bit->setsda(bit->data, 0);
	scl_low(bit);
	(void)scl_high(bit);
	sda_set(bit, 1);
}

/*
 * Write one byte, most significant bit first, and return the acknowledgement.
 *
 * 0 means the slave acknowledged, -ENXIO that it did not. A NAK is how an
 * absent device answers, so it is a result rather than an error in the transport
 * sense — the caller decides what it means.
 */
static int write_byte(const struct i2c_algo_bit_data *bit, u8 value)
{
	int i, ret, ack;

	for (i = 7; i >= 0; i--) {
		bit->setsda(bit->data, (value >> i) & 1);
		half_period(bit);
		ret = scl_high(bit);
		if (ret)
			return ret;
		scl_low(bit);
	}

	/* Release SDA so the slave can pull it down for the ACK. */
	bit->setsda(bit->data, 1);
	half_period(bit);
	ret = scl_high(bit);
	if (ret)
		return ret;
	ack = bit->getsda ? !bit->getsda(bit->data) : 1;
	scl_low(bit);

	return ack ? 0 : -ENXIO;
}

/* Read one byte, acknowledging it unless it is the last of the message. */
static int read_byte(const struct i2c_algo_bit_data *bit, u8 *out, bool ack)
{
	int i, ret;
	u8 value = 0;

	bit->setsda(bit->data, 1); /* release, the slave drives it */

	for (i = 7; i >= 0; i--) {
		ret = scl_high(bit);
		if (ret)
			return ret;
		if (bit->getsda && bit->getsda(bit->data))
			value |= (u8)(1u << i);
		scl_low(bit);
	}

	/* ACK pulls SDA low; NAK leaves it released and ends the read. */
	bit->setsda(bit->data, ack ? 0 : 1);
	half_period(bit);
	ret = scl_high(bit);
	if (ret)
		return ret;
	scl_low(bit);
	bit->setsda(bit->data, 1);

	*out = value;
	return 0;
}

/* Address phase: 7-bit address, low bit set for a read. */
static int send_address(const struct i2c_algo_bit_data *bit,
                        const struct i2c_msg *msg)
{
	return write_byte(bit, (u8)((msg->addr << 1) |
	                            ((msg->flags & I2C_M_RD) ? 1 : 0)));
}

static int bit_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct i2c_algo_bit_data *bit = adap ? adap->algo_data : 0;
	int i, ret = 0;

	if (!bit || !bit->setsda || !bit->setscl)
		return -ENODEV;

	if (bit->pre_xfer) {
		ret = bit->pre_xfer(adap);
		if (ret)
			return ret;
	}

	/*
	 * What the lines read once the driver has released them.
	 *
	 * pre_xfer leaves both high, so both should read back high; a pull-up and
	 * an idle bus give nothing else. Reading low here does not mean the bus is
	 * busy — it means the readback itself is not working, and then no amount of
	 * protocol on top can succeed. Worth one line, because it separates "the
	 * monitor is not answering" from "this machine cannot see its own pins".
	 */
	if (bit->getscl && bit->getsda && !bit->getscl(bit->data) &&
	    !bit->getsda(bit->data)) {
		static bool once;

		if (!once) {
			once = true;
			pr_info("lkpi: bit-bang unusable: both lines read low while released\n");
		}
		/*
		 * Refused rather than attempted.
		 *
		 * pre_xfer has just released both lines, and a pull-up makes an idle
		 * bus read high; both reading low means the readback itself is not
		 * working, not that the bus is busy. Every edge would then wait out its
		 * stretch budget and every byte would fail, so attempting the protocol
		 * costs seconds per probe and cannot succeed. Reporting no device lets
		 * the caller fall back to GMBUS immediately, which is the path that
		 * does work on this hardware.
		 *
		 * Observed on a passed-through IGD: GMBUS transfers complete, the GPIO
		 * readback does not.
		 */
		ret = -ENODEV;
		goto out;
	}

	for (i = 0; i < num; i++) {
		struct i2c_msg *msg = &msgs[i];
		u16 b;

		/*
		 * A message flagged NOSTART continues the previous one rather than
		 * addressing the slave again — that is what makes an EDID read a
		 * write of the offset followed by a read, on one bus cycle.
		 */
		if (!(msg->flags & I2C_M_NOSTART)) {
			ret = i ? bit_repstart(bit) : bit_start(bit);
			if (ret)
				goto out;
			ret = send_address(bit, msg);
			if (ret) {
				/* A NAK on the address is the normal answer from an
				 * empty bus, and not worth a log line per probe. */
				if (ret == -ENXIO && (msg->flags & I2C_M_IGNORE_NAK))
					ret = 0;
				else
					goto out;
			}
		}

		for (b = 0; b < msg->len; b++) {
			if (msg->flags & I2C_M_RD) {
				ret = read_byte(bit, &msg->buf[b], b + 1 < msg->len);
			} else {
				ret = write_byte(bit, msg->buf[b]);
				if (ret == -ENXIO && (msg->flags & I2C_M_IGNORE_NAK))
					ret = 0;
			}
			if (ret)
				goto out;
		}
	}

	ret = num;

out:
	bit_stop(bit);
	if (bit->post_xfer)
		bit->post_xfer(adap);

	return ret;
}

static u32 bit_functionality(struct i2c_adapter *adap)
{
	(void)adap;
	/* Plain I2C with repeated start. No SMBus emulation is claimed, because
	 * none is implemented and a caller that believed otherwise would get
	 * silence instead of an error. */
	return I2C_FUNC_I2C | I2C_FUNC_NOSTART;
}

const struct i2c_algorithm i2c_bit_algo = {
	.master_xfer = bit_xfer,
	.functionality = bit_functionality,
};

/*
 * Adopt the bit-banged algorithm for an adapter that filled in algo_data.
 *
 * i915 does not take this route — it keeps GMBUS as its algorithm and calls
 * i2c_bit_algo directly when it forces bit-banging — but a driver that does
 * gets the same implementation.
 */
int i2c_bit_add_bus(struct i2c_adapter *adap)
{
	if (!adap || !adap->algo_data)
		return -EINVAL;
	adap->algo = &i2c_bit_algo;
	return i2c_add_adapter(adap);
}
