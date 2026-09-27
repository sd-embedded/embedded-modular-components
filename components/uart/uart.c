/**
 * @file    uart.c
 * @brief   Implementation of the portable UART framing middleware.
 *
 * @author  sd-embedded
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "uart.h"

/* -------------------------------------------------------------------------- */
/* Local constants                                                            */
/* -------------------------------------------------------------------------- */

/** @brief Largest frame length that fits in the single-byte length field. */
#define MAX_FRAME_LENGTH                255U

/** @brief Number of CRC bytes appended to a CRC8 frame. */
#define CRC8_BYTE_COUNT                 1U

/** @brief Number of CRC bytes appended to a CRC16 frame. */
#define CRC16_BYTE_COUNT                2U

/* Modbus RTU layout constants (offsets are relative to the header byte). */
#define MODBUS_FUNCTION_CODE_OFFSET     1U  /**< Function code follows the address. */
#define MODBUS_BYTE_COUNT_OFFSET        6U  /**< Byte-count field for 0x0F / 0x10. */
#define MODBUS_SHORT_FRAME_LENGTH       8U  /**< addr + func + 4 data + CRC(2). */
#define MODBUS_MULTI_WRITE_OVERHEAD     9U  /**< Fixed bytes around the data payload. */

/* Modbus function codes handled by the length resolver. */
#define MODBUS_FUNC_READ_COILS          0x01U
#define MODBUS_FUNC_READ_DISCRETE       0x02U
#define MODBUS_FUNC_READ_HOLDING        0x03U
#define MODBUS_FUNC_READ_INPUT          0x04U
#define MODBUS_FUNC_WRITE_SINGLE_COIL   0x05U
#define MODBUS_FUNC_WRITE_SINGLE_REG    0x06U
#define MODBUS_FUNC_WRITE_MULTI_COILS   0x0FU
#define MODBUS_FUNC_WRITE_MULTI_REGS    0x10U

/* -------------------------------------------------------------------------- */
/* Static object pools                                                        */
/* -------------------------------------------------------------------------- */

static uart_port_t   port_pool[UART_MAX_COUNT];
static uart_device_t device_pool[UART_DEVICE_MAX_COUNT];

static uint8_t used_port_count   = 0;
static uint8_t used_device_count = 0;

/* -------------------------------------------------------------------------- */
/* Circular buffer helpers                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Returns the next ring index, wrapping at the buffer size.
 */
static uint16_t next_ring_index(uint16_t index)
{
	uint16_t incremented = (uint16_t)(index + 1U);

	if (incremented >= UART_RX_BUFFER_SIZE)
	{
		incremented = 0;
	}

	return incremented;
}

/**
 * @brief Writes one byte, overwriting the oldest byte when full.
 * @param buffer   Target buffer.
 * @param data     Byte to store.
 * @param life_ms  Time-to-live to assign to the byte.
 */
static void ring_write(uart_ring_buffer_t *buffer, uint8_t data, uint8_t life_ms)
{
	uint16_t next_write_index       = next_ring_index(buffer->write_index);
	bool     overwrites_oldest_byte = (next_write_index == buffer->read_index);

	buffer->cell[buffer->write_index].data             = data;
	buffer->cell[buffer->write_index].remaining_life_ms = life_ms;

	if (overwrites_oldest_byte)
	{
		buffer->read_index = next_ring_index(buffer->read_index);
	}

	buffer->write_index = next_write_index;
}

/**
 * @brief Reads and removes the oldest byte.
 * @param buffer   Source buffer.
 * @param data     Receives the byte.
 * @param life_ms  Receives the byte's remaining time-to-live.
 * @return true on success, false if the buffer is empty.
 */
static bool ring_read(uart_ring_buffer_t *buffer, uint8_t *data, uint8_t *life_ms)
{
	bool is_buffer_empty = (buffer->write_index == buffer->read_index);

	if (is_buffer_empty)
	{
		return false;
	}

	*data    = buffer->cell[buffer->read_index].data;
	*life_ms = buffer->cell[buffer->read_index].remaining_life_ms;

	buffer->read_index = next_ring_index(buffer->read_index);

	return true;
}

/**
 * @brief Returns the number of bytes currently stored.
 */
static uint16_t ring_stored_count(const uart_ring_buffer_t *buffer)
{
	if (buffer->write_index >= buffer->read_index)
	{
		return (uint16_t)(buffer->write_index - buffer->read_index);
	}

	return (uint16_t)(UART_RX_BUFFER_SIZE + buffer->write_index - buffer->read_index);
}

/**
 * @brief Copies the buffer linearly so that output[0] is the oldest byte.
 * @param buffer  Source buffer.
 * @param output  Destination of at least ::UART_RX_BUFFER_SIZE bytes.
 */
static void ring_copy_linear(const uart_ring_buffer_t *buffer, uint8_t *output)
{
	for (uint16_t offset = 0; offset < UART_RX_BUFFER_SIZE; offset++)
	{
		uint16_t source_index = (uint16_t)(buffer->read_index + offset);

		if (source_index >= UART_RX_BUFFER_SIZE)
		{
			source_index -= UART_RX_BUFFER_SIZE;
		}

		output[offset] = buffer->cell[source_index].data;
	}
}

/* -------------------------------------------------------------------------- */
/* Frame parser                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Locates the frame header and returns its index.
 */
static uart_parse_result_t find_header_index(const uart_rx_frame_spec_t *spec,
                                             uart_parse_mode_t mode,
                                             const uint8_t *input, uint8_t input_length,
                                             uint8_t *header_index)
{
	*header_index = 0;

	if (spec->header_type == UART_HEADER_NONE)
	{
		return UART_PARSE_OK;
	}

	if (mode == UART_PARSE_MODE_CHECK_AT_START)
	{
		if (input[0] != spec->header_byte)
		{
			return UART_PARSE_ERROR_HEADER_MISMATCH;
		}

		return UART_PARSE_OK;
	}

	/* UART_PARSE_MODE_SEARCH: scan for the header byte. */
	for (uint8_t index = 0; index < input_length; index++)
	{
		if (input[index] == spec->header_byte)
		{
			*header_index = index;
			return UART_PARSE_OK;
		}
	}

	return UART_PARSE_ERROR_HEADER_NOT_FOUND;
}

/**
 * @brief Resolves the frame length for a Modbus RTU frame.
 */
static uart_parse_result_t resolve_modbus_length(const uint8_t *input, uint8_t input_length,
                                                 uint8_t header_index, uint8_t *frame_length)
{
	uint16_t function_code_index = (uint16_t)header_index + MODBUS_FUNCTION_CODE_OFFSET;

	if (function_code_index >= input_length)
	{
		return UART_PARSE_ERROR_LENGTH_FIELD_OUT_OF_RANGE;
	}

	switch (input[function_code_index])
	{
		case MODBUS_FUNC_READ_COILS:
		case MODBUS_FUNC_READ_DISCRETE:
		case MODBUS_FUNC_READ_HOLDING:
		case MODBUS_FUNC_READ_INPUT:
		case MODBUS_FUNC_WRITE_SINGLE_COIL:
		case MODBUS_FUNC_WRITE_SINGLE_REG:
		{
			*frame_length = MODBUS_SHORT_FRAME_LENGTH;
			break;
		}

		case MODBUS_FUNC_WRITE_MULTI_COILS:
		case MODBUS_FUNC_WRITE_MULTI_REGS:
		{
			uint16_t byte_count_index = (uint16_t)header_index + MODBUS_BYTE_COUNT_OFFSET;

			if (byte_count_index >= input_length)
			{
				return UART_PARSE_ERROR_LENGTH_FIELD_OUT_OF_RANGE;
			}

			*frame_length = (uint8_t)(input[byte_count_index] + MODBUS_MULTI_WRITE_OVERHEAD);
			break;
		}

		default:
		{
			return UART_PARSE_ERROR_LENGTH_NOT_SET;
		}
	}

	return UART_PARSE_OK;
}

/**
 * @brief Resolves the total frame length according to the specification.
 */
static uart_parse_result_t resolve_frame_length(const uart_rx_frame_spec_t *spec,
                                                const uint8_t *input, uint8_t input_length,
                                                uint8_t header_index, uint8_t *frame_length)
{
	*frame_length = 0;

	switch (spec->length_type)
	{
		case UART_LENGTH_CONST:
		{
			if (spec->length == 0U)
			{
				return UART_PARSE_ERROR_LENGTH_NOT_SET;
			}

			*frame_length = spec->length;
			break;
		}

		case UART_LENGTH_IN_BYTE:
		{
			uint16_t length_field_index = (uint16_t)spec->length_byte_index + header_index;

			if (length_field_index >= input_length)
			{
				return UART_PARSE_ERROR_LENGTH_FIELD_OUT_OF_RANGE;
			}

			if (((uint16_t)input[length_field_index] + header_index) > input_length)
			{
				return UART_PARSE_ERROR_LENGTH_FIELD_OUT_OF_RANGE;
			}

			if (input[length_field_index] < spec->length)
			{
				return UART_PARSE_ERROR_LENGTH_TOO_SHORT;
			}

			*frame_length = input[length_field_index];
			break;
		}

		case UART_LENGTH_MODBUS:
		{
			return resolve_modbus_length(input, input_length, header_index, frame_length);
		}

		default:
		{
			return UART_PARSE_ERROR_LENGTH_NOT_SET;
		}
	}

	if (*frame_length == 0U)
	{
		return UART_PARSE_ERROR_LENGTH_NOT_SET;
	}

	return UART_PARSE_OK;
}

/**
 * @brief Validates the trailing CRC of an extracted frame.
 */
static uart_parse_result_t validate_frame_crc(const uart_rx_frame_spec_t *spec,
                                              const uint8_t *frame, uint8_t frame_length,
                                              uart_calc_crc_fn_t calc_crc)
{
	if (calc_crc == NULL)
	{
		return UART_PARSE_ERROR_NO_CRC_FUNCTION;
	}

	if (spec->crc_type == UART_CRC_8)
	{
		uint8_t expected_crc = (uint8_t)calc_crc(frame, (uint8_t)(frame_length - CRC8_BYTE_COUNT));
		uint8_t received_crc = frame[frame_length - 1U];

		if (received_crc != expected_crc)
		{
			return UART_PARSE_ERROR_BAD_CRC;
		}
	}
	else /* UART_CRC_16 */
	{
		uint16_t expected_crc = calc_crc(frame, (uint8_t)(frame_length - CRC16_BYTE_COUNT));
		uint16_t received_crc = (uint16_t)(((uint16_t)frame[frame_length - 2U] << 8)
		                                   | frame[frame_length - 1U]);

		if (received_crc != expected_crc)
		{
			return UART_PARSE_ERROR_BAD_CRC;
		}
	}

	return UART_PARSE_OK;
}

uart_parse_result_t uart_parse_frame(const uart_rx_frame_spec_t *spec,
                                     uart_parse_mode_t mode,
                                     const uint8_t *input, uint8_t input_length,
                                     uint8_t *out_frame, uint8_t *out_frame_length,
                                     uart_calc_crc_fn_t calc_crc)
{
	uint8_t header_index = 0;
	uint8_t frame_length = 0;
	uart_parse_result_t result;

	if (input_length == 0U)
	{
		return UART_PARSE_ERROR_NO_DATA;
	}

	result = find_header_index(spec, mode, input, input_length, &header_index);

	if (result != UART_PARSE_OK)
	{
		return result;
	}

	result = resolve_frame_length(spec, input, input_length, header_index, &frame_length);

	if (result != UART_PARSE_OK)
	{
		return result;
	}

	if (((uint16_t)frame_length + header_index) > input_length)
	{
		return UART_PARSE_ERROR_NOT_ENOUGH_DATA;
	}

	for (uint8_t offset = 0; offset < frame_length; offset++)
	{
		out_frame[offset] = input[header_index + offset];
	}

	if (out_frame_length != NULL)
	{
		*out_frame_length = frame_length;
	}

	return validate_frame_crc(spec, out_frame, frame_length, calc_crc);
}

/* -------------------------------------------------------------------------- */
/* TX / frame extraction helpers                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Builds the device's frame, appends its CRC and transmits it.
 */
static void build_and_send_frame(uart_port_t *port, uart_device_t *device)
{
	uint8_t frame_length;

	if (device->build_frame == NULL)
	{
		return;
	}

	memset(port->tx.buffer, 0, UART_TX_BUFFER_SIZE);
	frame_length = device->build_frame(port->tx.buffer);
	port->tx.length = frame_length;

	if (device->calc_crc != NULL)
	{
		if (device->tx_spec.crc_type == UART_CRC_16)
		{
			uint16_t crc = device->calc_crc(port->tx.buffer,
			                                (uint8_t)(frame_length - CRC16_BYTE_COUNT));

			port->tx.buffer[frame_length - 2U] = (uint8_t)(crc >> 8);
			port->tx.buffer[frame_length - 1U] = (uint8_t)(crc & 0xFFU);
		}
		else
		{
			uint8_t crc = (uint8_t)device->calc_crc(port->tx.buffer,
			                                        (uint8_t)(frame_length - CRC8_BYTE_COUNT));

			port->tx.buffer[frame_length - 1U] = crc;
		}
	}

	if (port->tx.send_bytes != NULL)
	{
		port->tx.send_bytes(port->tx.buffer, port->tx.length);
	}
}

/**
 * @brief Returns the offset of @p frame inside @p input, or -1 if not found.
 */
static int find_frame_offset(const uint8_t *input, uint8_t input_length,
                             const uint8_t *frame, uint8_t frame_length)
{
	if (frame_length == 0U || frame_length > input_length)
	{
		return -1;
	}

	for (uint8_t start = 0; start <= (uint8_t)(input_length - frame_length); start++)
	{
		if (memcmp(&input[start], frame, frame_length) == 0)
		{
			return (int)start;
		}
	}

	return -1;
}

/**
 * @brief Advances the processing buffer past a consumed frame.
 * @param keep_last_byte When true, leaves the final byte in place so a
 *        back-to-back frame cannot be decoded before the idle gap elapses.
 */
static void consume_frame(uart_port_t *port, const uint8_t *input, uint8_t input_length,
                          const uint8_t *frame, uint8_t frame_length, bool keep_last_byte)
{
	uart_ring_buffer_t *buffer = &port->rx.bytes_to_process;
	int frame_offset = find_frame_offset(input, input_length, frame, frame_length);

	if (frame_offset < 0)
	{
		return;
	}

	buffer->read_index = (uint16_t)(buffer->read_index + (uint16_t)frame_offset + frame_length);

	if (keep_last_byte && buffer->read_index > 0U)
	{
		buffer->read_index--;
	}

	while (buffer->read_index >= UART_RX_BUFFER_SIZE)
	{
		buffer->read_index -= UART_RX_BUFFER_SIZE;
	}
}

/* -------------------------------------------------------------------------- */
/* Per-port processing                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Moves all queued input bytes into the processing buffer.
 * @return true if at least one byte was moved.
 */
static bool move_incoming_to_processing(uart_port_t *port)
{
	uint8_t data = 0;
	uint8_t life_ms = 0;
	bool moved_any_byte = false;

	while (ring_read(&port->rx.incoming_bytes, &data, &life_ms))
	{
		ring_write(&port->rx.bytes_to_process, data, life_ms);
		moved_any_byte = true;
	}

	return moved_any_byte;
}

/**
 * @brief Slave role: search every device for a matching request and reply.
 */
static void process_slave_port(uart_port_t *port, const uint8_t *input, uint8_t input_length)
{
	uint8_t frame[UART_RX_BUFFER_SIZE];
	uint8_t frame_length = 0;
	uart_device_t *device = port->device_list;

	if (device == NULL)
	{
		return;
	}

	do
	{
		uart_parse_result_t result = uart_parse_frame(&device->rx_spec,
		                                              UART_PARSE_MODE_SEARCH,
		                                              input, input_length,
		                                              frame, &frame_length,
		                                              device->calc_crc);

		bool frame_is_valid = (result == UART_PARSE_OK) && (device->process_frame != NULL);

		if (frame_is_valid)
		{
			bool idle_gap_enabled    = (port->rx.max_idle_gap_ms > 0U);
			bool frame_starts_buffer = (input[0] == frame[0]);

			/* With an idle gap configured, a valid frame must start the buffer. */
			if (idle_gap_enabled && !frame_starts_buffer)
			{
				return;
			}

			if (device->process_frame(frame, frame_length))
			{
				consume_frame(port, input, input_length, frame, frame_length, idle_gap_enabled);
				build_and_send_frame(port, device);
			}
		}

		device = device->next_device;
	}
	while (device != port->device_list);
}

/**
 * @brief Master role: poll the next device or parse the current reply.
 */
static void process_master_port(uart_port_t *port, bool has_new_data,
                                const uint8_t *input, uint8_t input_length)
{
	uint8_t frame[UART_RX_BUFFER_SIZE];
	uint8_t frame_length = 0;
	uart_device_t *device = port->device_list;
	bool time_to_poll_next;

	if (device == NULL)
	{
		return;
	}

	time_to_poll_next = (device->tx_state.poll_countdown_ms == 0U);

	if (time_to_poll_next)
	{
		port->device_list = device->next_device;
		device = port->device_list;
		device->tx_state.poll_countdown_ms = device->tx_spec.poll_period_ms;

		/* Discard any pending RX before issuing a new request. */
		port->rx.bytes_to_process.read_index = port->rx.bytes_to_process.write_index;

		build_and_send_frame(port, device);
		return;
	}

	if (!has_new_data)
	{
		return;
	}

	uart_parse_result_t result = uart_parse_frame(&device->rx_spec,
	                                              UART_PARSE_MODE_SEARCH,
	                                              input, input_length,
	                                              frame, &frame_length,
	                                              device->calc_crc);

	bool reply_is_valid = (result == UART_PARSE_OK) && (device->process_frame != NULL);

	if (reply_is_valid && device->process_frame(frame, frame_length))
	{
		port->rx.bytes_to_process.read_index = port->rx.bytes_to_process.write_index;
		consume_frame(port, input, input_length, frame, frame_length, false);
	}
}

/* -------------------------------------------------------------------------- */
/* Tick helpers                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Ages buffered bytes and drops expired ones from the tail.
 */
static void age_processing_buffer(uart_port_t *port)
{
	uart_ring_buffer_t *buffer = &port->rx.bytes_to_process;

	for (uint16_t index = 0; index < UART_RX_BUFFER_SIZE; index++)
	{
		if (buffer->cell[index].remaining_life_ms > 0U)
		{
			buffer->cell[index].remaining_life_ms--;
		}
	}

	while (buffer->write_index != buffer->read_index
	       && buffer->cell[buffer->read_index].remaining_life_ms == 0U)
	{
		uint8_t discarded_data = 0;
		uint8_t discarded_life = 0;

		(void)ring_read(buffer, &discarded_data, &discarded_life);
	}
}

/**
 * @brief Flushes the processing buffer after a bus idle gap.
 */
static void apply_idle_timeout(uart_port_t *port)
{
	uart_ring_buffer_t *buffer = &port->rx.bytes_to_process;
	uint16_t last_byte_index;
	uint8_t  elapsed_since_last_byte_ms;
	bool     idle_gap_disabled = (port->rx.max_idle_gap_ms == 0U);
	bool     buffer_is_empty   = (buffer->write_index == buffer->read_index);

	if (idle_gap_disabled || buffer_is_empty)
	{
		return;
	}

	if (buffer->write_index > 0U)
	{
		last_byte_index = (uint16_t)(buffer->write_index - 1U);
	}
	else
	{
		last_byte_index = (uint16_t)(UART_RX_BUFFER_SIZE - 1U);
	}

	/* Defensive: skip if the lifetime is inconsistent with the configured base. */
	if (buffer->cell[last_byte_index].remaining_life_ms > port->rx.byte_life_ms)
	{
		return;
	}

	elapsed_since_last_byte_ms = (uint8_t)(port->rx.byte_life_ms
	                                       - buffer->cell[last_byte_index].remaining_life_ms);

	if (elapsed_since_last_byte_ms > port->rx.max_idle_gap_ms)
	{
		buffer->read_index = buffer->write_index;
	}
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

uart_add_port_result_t uart_add_port(uart_port_t **created_port, const uart_port_t *config)
{
	uart_port_t *port;

	if (config == NULL)
	{
		return UART_ADD_PORT_ERROR_NULL_CONFIG;
	}

	if (used_port_count >= UART_MAX_COUNT)
	{
		return UART_ADD_PORT_ERROR_POOL_FULL;
	}

	port = &port_pool[used_port_count];
	used_port_count++;

	port->device_list        = config->device_list;
	port->mode               = config->mode;
	port->tx.send_bytes      = config->tx.send_bytes;
	port->tx.length          = 0;
	port->rx.byte_life_ms    = config->rx.byte_life_ms;
	port->rx.max_idle_gap_ms = config->rx.max_idle_gap_ms;
	/* RX buffers are zero-initialised (static storage) and start empty. */

	if (created_port != NULL)
	{
		*created_port = port;
	}

	return UART_ADD_PORT_OK;
}

uart_add_device_result_t uart_add_device(uart_port_t *port, const uart_device_t *config)
{
	uart_device_t *device;

	if (port == NULL || config == NULL)
	{
		return UART_ADD_DEVICE_ERROR_NULL_CONFIG;
	}

	if (used_device_count >= UART_DEVICE_MAX_COUNT)
	{
		return UART_ADD_DEVICE_ERROR_POOL_FULL;
	}

	device = &device_pool[used_device_count];
	used_device_count++;

	device->tx_spec       = config->tx_spec;
	device->tx_state      = config->tx_state;
	device->rx_spec       = config->rx_spec;
	device->calc_crc      = config->calc_crc;
	device->build_frame   = config->build_frame;
	device->process_frame = config->process_frame;

	if (port->device_list == NULL)
	{
		port->device_list = device;
		device->next_device = device;            /* single-element circular list */
	}
	else
	{
		uart_device_t *last_device = port->device_list;

		while (last_device->next_device != port->device_list)
		{
			last_device = last_device->next_device;
		}

		last_device->next_device = device;
		device->next_device = port->device_list; /* close the ring */
	}

	return UART_ADD_DEVICE_OK;
}

uart_receive_result_t uart_receive(uart_port_t *port, const uint8_t *bytes, uint8_t byte_count)
{
	if (port == NULL)
	{
		return UART_RECEIVE_ERROR_NULL_PORT;
	}

	for (uint8_t index = 0; index < byte_count; index++)
	{
		ring_write(&port->rx.incoming_bytes, bytes[index], port->rx.byte_life_ms);
	}

	return UART_RECEIVE_OK;
}

void uart_task(void)
{
	uint8_t linear_input[UART_RX_BUFFER_SIZE];

	for (uint8_t port_index = 0; port_index < used_port_count; port_index++)
	{
		uart_port_t *port = &port_pool[port_index];
		bool has_new_data = move_incoming_to_processing(port);
		uint8_t input_length = 0;

		if (has_new_data)
		{
			uint16_t stored_count = ring_stored_count(&port->rx.bytes_to_process);

			ring_copy_linear(&port->rx.bytes_to_process, linear_input);
			input_length = (stored_count > MAX_FRAME_LENGTH)
			               ? (uint8_t)MAX_FRAME_LENGTH
			               : (uint8_t)stored_count;
		}

		if (port->mode == UART_MODE_SLAVE)
		{
			if (has_new_data)
			{
				process_slave_port(port, linear_input, input_length);
			}
		}
		else /* UART_MODE_MASTER */
		{
			process_master_port(port, has_new_data, linear_input, input_length);
		}
	}
}

void uart_tick_1ms(void)
{
	for (uint8_t port_index = 0; port_index < used_port_count; port_index++)
	{
		uart_port_t *port = &port_pool[port_index];
		bool has_device_to_poll = (port->device_list != NULL);

		/* Slave ports simply have no poll countdown to advance. */
		if (has_device_to_poll && port->device_list->tx_state.poll_countdown_ms > 0U)
		{
			port->device_list->tx_state.poll_countdown_ms--;
		}

		age_processing_buffer(port);
		apply_idle_timeout(port);
	}
}
