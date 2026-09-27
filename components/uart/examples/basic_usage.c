/**
 * @file    basic_usage.c
 * @brief   Minimal usage example for the uart module.
 *
 * @details
 * Shows the smallest useful configuration: a single slave port that answers a
 * fixed-length, header-delimited protocol protected by a CRC8.
 *
 * Frame layout (request and response share it):
 *
 *     +--------+---------+---------+--------+
 *     | header | command | payload |  crc8  |
 *     | 1 byte | 1 byte  | 1 byte  | 1 byte |
 *     +--------+---------+---------+--------+
 *
 * The module owns framing and CRC checking. The application only provides:
 *   - a byte sender (::send_bytes),
 *   - a CRC routine (::calc_crc8),
 *   - a request handler (::on_frame_received) and a reply builder (::build_reply).
 *
 * Replace the three platform stubs (::send_bytes, ::app_uart_rx_isr wiring and
 * ::app_systick_1ms wiring) with your BSP to run this on real hardware.
 *
 * @author  sd-embedded
 */

#include <stdint.h>
#include <stddef.h>

#include "uart.h"

/* -------------------------------------------------------------------------- */
/* Protocol definition                                                        */
/* -------------------------------------------------------------------------- */

#define FRAME_HEADER_BYTE       0xAAU  /**< Marks the start of every frame. */
#define FRAME_TOTAL_LENGTH      4U     /**< header + command + payload + CRC8. */

#define BYTE_LIFE_MS            60U    /**< Discard a byte unused for this long. */
#define BUS_IDLE_GAP_MS         5U     /**< Idle gap that flushes a partial frame. */

/* Position of each field inside a frame. */
#define FIELD_INDEX_COMMAND     1U
#define FIELD_INDEX_PAYLOAD     2U

/* Supported commands. */
#define COMMAND_PING            0x01U  /**< Reply with PONG. */
#define COMMAND_READ_STATE      0x02U  /**< Reply with the current state byte. */

/* Response codes. */
#define RESPONSE_PONG           0x81U
#define RESPONSE_STATE          0x82U
#define RESPONSE_UNKNOWN        0xEEU

/* -------------------------------------------------------------------------- */
/* Application state                                                          */
/* -------------------------------------------------------------------------- */

static uint8_t device_state = 0;  /**< Example value returned by COMMAND_READ_STATE. */

/** @brief Reply the module transmits after the current request is handled. */
static struct
{
	uint8_t response_code;
	uint8_t payload;
} pending_reply;

static uart_port_t *slave_port;

/* -------------------------------------------------------------------------- */
/* Platform glue (replace with your BSP)                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Simple CRC8 (poly 0x07, init 0x00). Matches ::uart_calc_crc_fn_t.
 */
static uint16_t calc_crc8(const uint8_t *bytes, uint8_t byte_count)
{
	uint8_t crc = 0x00U;

	for (uint8_t index = 0; index < byte_count; index++)
	{
		crc ^= bytes[index];

		for (uint8_t bit = 0; bit < 8U; bit++)
		{
			if ((crc & 0x80U) != 0U)
			{
				crc = (uint8_t)((crc << 1) ^ 0x07U);
			}
			else
			{
				crc = (uint8_t)(crc << 1);
			}
		}
	}

	return crc;
}

/**
 * @brief Low-level transmit. Matches ::uart_send_bytes_fn_t.
 */
static void send_bytes(const uint8_t *bytes_to_send, uint8_t byte_count)
{
	/* Replace with your platform's UART transmit, for example:
	 *   HAL_UART_Transmit(&huart1, (uint8_t *)bytes_to_send, byte_count, 100);
	 */
	(void)bytes_to_send;
	(void)byte_count;
}

/* -------------------------------------------------------------------------- */
/* Protocol callbacks                                                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief Handles a validated request and records the reply to send.
 * @details Called by the module only for frames that start with
 *          ::FRAME_HEADER_BYTE and carry a valid CRC8. Matches
 *          ::uart_process_frame_fn_t.
 * @return 1 to make the module build and transmit a reply.
 */
static uint8_t on_frame_received(const uint8_t *frame, uint8_t frame_length)
{
	(void)frame_length; /* Fixed by the frame spec, already validated. */

	switch (frame[FIELD_INDEX_COMMAND])
	{
		case COMMAND_PING:
		{
			pending_reply.response_code = RESPONSE_PONG;
			pending_reply.payload = 0;
			break;
		}

		case COMMAND_READ_STATE:
		{
			pending_reply.response_code = RESPONSE_STATE;
			pending_reply.payload = device_state;
			break;
		}

		default:
		{
			pending_reply.response_code = RESPONSE_UNKNOWN;
			pending_reply.payload = frame[FIELD_INDEX_COMMAND];
			break;
		}
	}

	return 1;
}

/**
 * @brief Builds the reply frame. The module appends the CRC8 and transmits it.
 * @details Matches ::uart_build_frame_fn_t.
 * @return Total frame length, including the trailing CRC slot.
 */
static uint8_t build_reply(uint8_t *out_frame)
{
	out_frame[0]                   = FRAME_HEADER_BYTE;
	out_frame[FIELD_INDEX_COMMAND] = pending_reply.response_code;
	out_frame[FIELD_INDEX_PAYLOAD] = pending_reply.payload;
	/* out_frame[3] is the CRC8 slot, filled by the module. */

	return FRAME_TOTAL_LENGTH;
}

/* -------------------------------------------------------------------------- */
/* Setup and run                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Registers the slave port and its single device.
 */
static void uart_setup(void)
{
	uart_port_t port_config = {0};
	uart_device_t device_config = {0};

	port_config.mode                = UART_MODE_SLAVE;
	port_config.tx.send_bytes       = send_bytes;
	port_config.rx.byte_life_ms     = BYTE_LIFE_MS;
	port_config.rx.max_idle_gap_ms  = BUS_IDLE_GAP_MS;

	if (uart_add_port(&slave_port, &port_config) != UART_ADD_PORT_OK)
	{
		return;
	}

	device_config.rx_spec.header_type = UART_HEADER_ONE_BYTE;
	device_config.rx_spec.header_byte = FRAME_HEADER_BYTE;
	device_config.rx_spec.length_type = UART_LENGTH_CONST;
	device_config.rx_spec.length      = FRAME_TOTAL_LENGTH;
	device_config.rx_spec.crc_type    = UART_CRC_8;
	device_config.calc_crc            = calc_crc8;
	device_config.process_frame       = on_frame_received;
	device_config.build_frame         = build_reply;
	device_config.tx_spec.crc_type    = UART_CRC_8;

	(void)uart_add_device(slave_port, &device_config);
}

/**
 * @brief Feeds received bytes into the module.
 * @details Call from the UART RX interrupt / DMA-complete callback.
 */
void app_uart_rx_isr(const uint8_t *bytes, uint8_t byte_count)
{
	(void)uart_receive(slave_port, bytes, byte_count);
}

/**
 * @brief Call exactly once per millisecond (for example from SysTick).
 */
void app_systick_1ms(void)
{
	uart_tick_1ms();
}

int main(void)
{
	/* platform_init(); -- clocks, GPIO, UART peripheral, 1 ms tick. */
	uart_setup();

	for (;;)
	{
		uart_task(); /* parse buffered frames and send replies */
		/* ... other application work ... */
	}
}
