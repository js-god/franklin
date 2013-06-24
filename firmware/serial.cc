#include "firmware.hh"

// Commands which can be sent:
// Packet: n*4 bytes, of which n*3 bytes are data.
// Ack: 1 byte.
// Nack: 1 byte.

// The first byte of a packet is the flip-flop and length: f0llllll
// All other commands have bit 6 set, so they cannot be mistaken for a packet.
// They have 4 bit data and 3 bit parity: p1ppdddd
// Codes (defined in firmware.hh):
// ack:		0000	-> 0x40	0100
// nack:	0001	-> 0xe1	1110
// event:	0010	-> 0xd2	1101
// pause:	0011	-> 0x73	0111
// continue:	0100	-> 0xf4	1111
// stall:	0101	-> 0x55	0101
// sync:	0110	-> 0x66	0110
// unused:	0111	-> 0xc7	1100
// static const uint8_t MASK1[3] = {0x8f, 0x2d, 0x1e}
// reply is the arrival notification.  It doesn't really belong there, but
// the system requires a synchronous and asynchronous buffer.  The asynchronous
// buffer only needs to be able to send arrival notifications, so it doesn't
// really need to exist, if those are sent as 1 byte.
// Because it doesn't have 0/1-encoding, the host cannot respond with ack.  It
// must therefore respond to it with the same single byte.

static uint8_t ff_in = 0;
static uint8_t ff_out = 0;
static bool sent_notification = false;
static unsigned long long last_millis = 0;

// Parity masks for decoding.
static const uint8_t MASK[5][4] = {
	{0xc0, 0xc3, 0xff, 0x09},
	{0x38, 0x3a, 0x7e, 0x13},
	{0x26, 0xb5, 0xb9, 0x23},
	{0x95, 0x6c, 0xd5, 0x43},
	{0x4b, 0xdc, 0xe2, 0x83}};

void send_notification ()
{
	if (!num_cbs++)
	{
		Serial.write (EVENT);
		sent_notification = true;
	}
}

// There is serial data available.
void serial ()
{
	if (command_end > 0 && millis () >= last_millis + 2)
	{
		Serial.write (NACK);
		command_end = 0;
	}
	while (command_end == 0)
	{
		if (!Serial.available ())
			return;
		command[0] = Serial.read ();
		// If this is a 1-byte command, handle it.
		switch (command[0])
		{
		case ACK:
			// Ack: everything was ok; flip the flipflop.
			ff_out ^= 0x80;
			continue;
		case NACK:
			// Nack: the host didn't properly receive the notification: resend.
			if (sent_notification)
				Serial.write (EVENT);
			else
				send_packet ();
			continue;
		case EVENT:
			// Server has seen arrival notification.
			if (!--num_cbs)
				sent_notification = false;
			else
				Serial.write (EVENT);
			continue;
		case PAUSE:
			pause_all = true;
			Serial.write (ACK);
			continue;
		case CONTINUE:
			pause_all = false;
			Serial.write (ACK);
			continue;
		case STALL:
			// Emergency reset.
			queue_start = 0;
			queue_end = 0;
			for (uint8_t o = 0; o < MAXOBJECT; ++o)
			{
				if (motors[o])
				{
					motors[o]->steps_total = 0;
					motors[o]->continuous = false;
					digitalWrite (motors[o]->sleep_pin, LOW);
				}
				if (temps[o])
				{
					temps[o]->target = NAN;
					if (temps[o]->is_on)
					{
						digitalWrite (temps[o]->power_pin, LOW);
						temps[o]->is_on = false;
					}
				}
			}
			Serial.write (ACK);
			continue;
		case SYNC:
			Serial.write (SYNC);
			continue;
		case UNUSED:
			Serial.write (STALL);
			continue;
		default:
			break;
		}
		command_end = 1;
		last_millis = millis ();
	}
	int len = Serial.available ();
	if (len == 0)
		return;
	if (len + command_end > COMMAND_SIZE)
		len = COMMAND_SIZE - command_end;
	uint8_t cmd_len = (((command[0] & COMMAND_LEN_MASK) + 2) / 3) * 4;
	if (command_end + len > cmd_len)
		len = cmd_len - command_end;
	Serial.readBytes (&command[command_end], len);
	last_millis = millis ();
	command_end += len;
	if (command_end < cmd_len)
		return;
	command_end = 0;
	// Check packet integrity.
	// Size must be good.
	if (command[0] < 2 || command[0] == 4)
	{
		Serial.write (NACK);
		return;
	}
	// Checksum must be good.
	len = command[0] & ~0x80;
	for (uint8_t t = 0; t < (len + 2) / 3; ++t)
	{
		uint8_t sum = command[len + t];
		if ((sum & 0x7) != (t & 0x7))
		{
			Serial.write (NACK);
			return;
		}
		for (uint8_t bit = 0; bit < 5; ++bit)
		{
			uint8_t check = sum & MASK[bit][3];
			for (uint8_t p = 0; p < 3; ++p)
				check ^= command[3 * t + p] & MASK[bit][p];
			check ^= check >> 4;
			check ^= check >> 2;
			check ^= check >> 1;
			if (check & 1)
			{
				Serial.write (NACK);
				return;
			}
		}
	}
	// Packet is good.
	// Flip-flop must have good state.
	if ((command[0] & 0x80) != ff_in)
	{
		// Wrong: this must be a retry to send the previous packet, so our ack was lost.
		// Resend the ack, but don't do anything (the action has already been taken).
		Serial.write (ACK);
		return;
	}
	// Right: update flip-flop and send ack.
	ff_in ^= 0x80;
	packet ();
}

// Send packet to host.
void send_packet ()
{
	// Set flipflop bit.
	outcommand[0] &= ~0xc0;
	outcommand[0] ^= ff_out;
	// Compute the checksums.  This doesn't work for size in (1, 2, 4), so
	// the protocol doesn't allow those.
	// For size % 3 != 0, the first checksums are part of the data for the
	// last checksum.  This means they must have been filled in at that
	// point.  (This is also the reason (1, 2, 4) are not allowed.)
	uint8_t cmd_len = outcommand[0] & COMMAND_LEN_MASK;
	for (uint8_t t = 0; t < (cmd_len + 2) / 3; ++t)
	{
		uint8_t sum = t & 7;
		for (unsigned bit = 0; bit < 5; ++bit)
		{
			uint8_t check = 0;
			for (uint8_t p = 0; p < 3; ++p)
				check ^= outcommand[3 * t + p] & MASK[bit][p];
			check ^= sum & MASK[bit][3];
			check ^= check >> 4;
			check ^= check >> 2;
			check ^= check >> 1;
			if (check & 1)
				sum |= 1 << (bit + 3);
		}
		outcommand[outcommand[0] + t] = sum;
	}
	for (uint8_t t = 0; t < cmd_len + (cmd_len + 2) / 3; ++t)
		Serial.write (outcommand[t]);
}