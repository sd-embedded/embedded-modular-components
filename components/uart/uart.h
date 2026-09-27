/**
 * @file    uart.h
 * @brief   Portable, HAL-independent UART framing middleware.
 *
 * @details
 * This module provides a small, allocation-free framework for UART
 * communication built around a frame parser and per-port circular receive
 * buffers. It supports both master and slave roles and is fully decoupled
 * from any hardware abstraction layer: the application supplies the low-level
 * byte-send function and (optionally) a CRC routine via function pointers.
 *
 * Key features:
 *  - Static object pools only (no malloc/free).
 *  - Frame parser with fixed length, in-frame length byte and Modbus RTU
 *    length detection.
 *  - Pluggable CRC8 / CRC16 calculation passed explicitly per call.
 *  - Per-byte time-of-life and optional bus idle timeout for the RX buffer.
 *
 * @note Protocol frame lengths are limited to 255 bytes (carried in a single
 *       byte). The RX/TX buffer sizes are configurable and may exceed 255.
 *
 * @author  sd-embedded
 */

#ifndef UART_H_
#define UART_H_

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */

/** @brief Maximum number of UART ports held in the static pool. */
#define UART_MAX_COUNT              1U

/** @brief Maximum number of devices held in the static pool (shared). */
#define UART_DEVICE_MAX_COUNT       2U

/** @brief Receive circular buffer size, in bytes. */
#define UART_RX_BUFFER_SIZE         64U

/** @brief Transmit buffer size, in bytes. */
#define UART_TX_BUFFER_SIZE         64U

/* -------------------------------------------------------------------------- */
/* Function pointer types                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Low-level byte-array transmit function.
 * @param bytes_to_send  Pointer to the bytes to send.
 * @param byte_count     Number of bytes to send.
 */
typedef void (*uart_send_bytes_fn_t)(const uint8_t *bytes_to_send, uint8_t byte_count);

/**
 * @brief CRC calculation function.
 * @param bytes       Pointer to the bytes the CRC is computed over.
 * @param byte_count  Number of bytes.
 * @return Calculated CRC. For CRC8 only the low byte is used.
 */
typedef uint16_t (*uart_calc_crc_fn_t)(const uint8_t *bytes, uint8_t byte_count);

/**
 * @brief Builds an outgoing frame into the supplied buffer.
 * @param out_frame  Destination buffer (at least ::UART_TX_BUFFER_SIZE bytes).
 * @return Number of bytes written into @p out_frame, including the CRC slot(s).
 */
typedef uint8_t (*uart_build_frame_fn_t)(uint8_t *out_frame);

/**
 * @brief Processes a validated incoming frame.
 * @param frame        Pointer to the received frame.
 * @param frame_length Frame length in bytes.
 * @return Non-zero if the frame was accepted/handled, 0 otherwise.
 */
typedef uint8_t (*uart_process_frame_fn_t)(const uint8_t *frame, uint8_t frame_length);

/* -------------------------------------------------------------------------- */
/* Enumerations                                                               */
/* -------------------------------------------------------------------------- */

/** @brief How a frame header is matched. */
typedef enum
{
	UART_HEADER_NONE = 0,      /**< Frame has no header byte. */
	UART_HEADER_ONE_BYTE       /**< Frame starts with a single header byte. */
} uart_header_type_t;

/** @brief How a frame length is determined. */
typedef enum
{
	UART_LENGTH_CONST = 0,     /**< Length is a fixed constant. */
	UART_LENGTH_IN_BYTE,       /**< Length is carried in one of the frame bytes. */
	UART_LENGTH_MODBUS         /**< Length is derived from the Modbus RTU function code. */
} uart_length_type_t;

/** @brief CRC width used for a frame. */
typedef enum
{
	UART_CRC_8 = 0,            /**< 1-byte CRC, stored in the last byte. */
	UART_CRC_16                /**< 2-byte CRC, stored big-endian in the last two bytes. */
} uart_crc_type_t;

/** @brief Role of a UART port. */
typedef enum
{
	UART_MODE_SLAVE = 0,       /**< Waits for requests and answers them. */
	UART_MODE_MASTER           /**< Polls devices in a round-robin fashion. */
} uart_mode_t;

/** @brief Frame parser operating mode. */
typedef enum
{
	UART_PARSE_MODE_CHECK_AT_START = 0, /**< Header is expected at index 0. */
	UART_PARSE_MODE_SEARCH              /**< Header is searched for within the buffer. */
} uart_parse_mode_t;

/** @brief Result of ::uart_parse_frame. */
typedef enum
{
	UART_PARSE_OK = 0,                       /**< Valid frame found. */
	UART_PARSE_ERROR_NO_DATA,                /**< Input length was 0. */
	UART_PARSE_ERROR_HEADER_MISMATCH,        /**< Header mismatch in CHECK_AT_START mode. */
	UART_PARSE_ERROR_HEADER_NOT_FOUND,       /**< Header not found in SEARCH mode. */
	UART_PARSE_ERROR_LENGTH_NOT_SET,         /**< Resolved length was 0. */
	UART_PARSE_ERROR_LENGTH_TOO_SHORT,       /**< In-byte length below the configured minimum. */
	UART_PARSE_ERROR_NO_CRC_FUNCTION,        /**< CRC requested but no function supplied. */
	UART_PARSE_ERROR_BAD_CRC,                /**< CRC check failed. */
	UART_PARSE_ERROR_NOT_ENOUGH_DATA,        /**< Buffer holds fewer bytes than the frame length. */
	UART_PARSE_ERROR_LENGTH_FIELD_OUT_OF_RANGE /**< Length field index points outside the buffer. */
} uart_parse_result_t;

/** @brief Result of ::uart_add_port. */
typedef enum
{
	UART_ADD_PORT_OK = 0,            /**< Port created. */
	UART_ADD_PORT_ERROR_NULL_CONFIG, /**< Supplied config was NULL. */
	UART_ADD_PORT_ERROR_POOL_FULL    /**< Static pool exhausted. */
} uart_add_port_result_t;

/** @brief Result of ::uart_add_device. */
typedef enum
{
	UART_ADD_DEVICE_OK = 0,            /**< Device added. */
	UART_ADD_DEVICE_ERROR_NULL_CONFIG, /**< Supplied config was NULL. */
	UART_ADD_DEVICE_ERROR_POOL_FULL    /**< Static pool exhausted. */
} uart_add_device_result_t;

/** @brief Result of ::uart_receive. */
typedef enum
{
	UART_RECEIVE_OK = 0,           /**< Bytes queued. */
	UART_RECEIVE_ERROR_NULL_PORT   /**< Supplied port was NULL. */
} uart_receive_result_t;

/* -------------------------------------------------------------------------- */
/* Frame specification / state                                                */
/* -------------------------------------------------------------------------- */

/** @brief Transmit-side, per-device frame specification. */
typedef struct
{
	unsigned int    poll_period_ms; /**< Master only: interval between polls of this device. */
	uart_crc_type_t crc_type;       /**< CRC width appended to outgoing frames. */
} uart_tx_frame_spec_t;

/** @brief Transmit-side, per-device runtime state. */
typedef struct
{
	unsigned int poll_countdown_ms; /**< Master only: time left until next poll. */
} uart_tx_frame_state_t;

/** @brief Receive-side, per-device frame specification. */
typedef struct
{
	uart_header_type_t header_type;        /**< Header matching strategy. */
	uint8_t            header_byte;        /**< Expected header byte. */
	uart_length_type_t length_type;        /**< Length resolution strategy. */
	uint8_t            length;             /**< Fixed length, or minimum length for IN_BYTE mode. */
	uint8_t            length_byte_index;  /**< IN_BYTE mode: index of the byte carrying the length. */
	uart_crc_type_t    crc_type;           /**< CRC width expected in incoming frames. */
} uart_rx_frame_spec_t;

/* -------------------------------------------------------------------------- */
/* Circular buffer                                                            */
/* -------------------------------------------------------------------------- */

/** @brief One slot of the receive circular buffer. */
typedef struct
{
	uint8_t data;                /**< Received byte. */
	uint8_t remaining_life_ms;   /**< Remaining time-to-live, in milliseconds. */
} uart_ring_cell_t;

/** @brief Single-producer/single-consumer circular byte buffer. */
typedef struct
{
	uint16_t         read_index;                    /**< Index of the oldest stored byte. */
	uint16_t         write_index;                   /**< Index where the next byte is written. */
	uart_ring_cell_t cell[UART_RX_BUFFER_SIZE];     /**< Storage. */
} uart_ring_buffer_t;

/* -------------------------------------------------------------------------- */
/* Device / TX / RX / port                                                    */
/* -------------------------------------------------------------------------- */

/** @brief A logical device reachable over a UART port. */
typedef struct uart_device
{
	uart_tx_frame_spec_t    tx_spec;         /**< Outgoing frame specification. */
	uart_tx_frame_state_t   tx_state;        /**< Outgoing frame runtime state. */
	uart_rx_frame_spec_t    rx_spec;         /**< Incoming frame specification. */
	uart_calc_crc_fn_t      calc_crc;        /**< CRC routine for this device. */
	uart_build_frame_fn_t   build_frame;     /**< Builds the frame to send. */
	uart_process_frame_fn_t process_frame;   /**< Handles a received frame. */
	struct uart_device     *next_device;     /**< Next device in the circular list. */
} uart_device_t;

/** @brief Transmit channel of a UART port. */
typedef struct
{
	uint8_t              buffer[UART_TX_BUFFER_SIZE]; /**< Outgoing byte buffer. */
	uint8_t              length;                      /**< Number of valid bytes in @c buffer. */
	uart_send_bytes_fn_t send_bytes;                 /**< Low-level transmit function. */
} uart_tx_channel_t;

/** @brief Receive channel of a UART port. */
typedef struct
{
	uart_ring_buffer_t incoming_bytes;    /**< Filled from the ISR via ::uart_receive. */
	uart_ring_buffer_t bytes_to_process;  /**< Snapshot consumed by ::uart_task. */
	uint8_t            byte_life_ms;      /**< Initial time-to-life assigned to each byte. */
	uint8_t            max_idle_gap_ms;   /**< 0 = disabled; otherwise idle gap that flushes the buffer. */
} uart_rx_channel_t;

/** @brief A UART port instance. */
typedef struct
{
	uart_device_t     *device_list;  /**< Circular list of devices on this port. */
	uart_mode_t        mode;         /**< Master or slave role. */
	uart_tx_channel_t  tx;           /**< Transmit channel. */
	uart_rx_channel_t  rx;           /**< Receive channel. */
} uart_port_t;

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Registers a new UART port from a configuration template.
 * @param[out] created_port  Receives a pointer to the created port (pool-backed).
 * @param[in]  config        Configuration to copy into the new port.
 * @return ::UART_ADD_PORT_OK on success, otherwise an error code.
 */
uart_add_port_result_t uart_add_port(uart_port_t **created_port, const uart_port_t *config);

/**
 * @brief Adds a device to a UART port's circular device list.
 * @param[in] port    Target port.
 * @param[in] config  Device configuration to copy.
 * @return ::UART_ADD_DEVICE_OK on success, otherwise an error code.
 */
uart_add_device_result_t uart_add_device(uart_port_t *port, const uart_device_t *config);

/**
 * @brief Queues received bytes into a port's input buffer.
 * @details Intended to be called from the UART RX interrupt.
 * @param[in] port         Target port.
 * @param[in] bytes        Bytes to queue.
 * @param[in] byte_count   Number of bytes.
 * @return ::UART_RECEIVE_OK on success, otherwise an error code.
 */
uart_receive_result_t uart_receive(uart_port_t *port, const uint8_t *bytes, uint8_t byte_count);

/**
 * @brief Validates / locates a frame in a byte buffer.
 * @param[in]  spec              Receive frame specification.
 * @param[in]  mode              Whether the header is checked at index 0 or searched.
 * @param[in]  input             Input bytes.
 * @param[in]  input_length      Number of input bytes.
 * @param[out] out_frame         Buffer receiving the extracted frame.
 * @param[out] out_frame_length  Optional: receives the frame length (may be NULL).
 * @param[in]  calc_crc          CRC routine, or NULL if @p spec uses no CRC.
 * @return ::UART_PARSE_OK if a valid frame is found, otherwise an error code.
 */
uart_parse_result_t uart_parse_frame(const uart_rx_frame_spec_t *spec,
                                     uart_parse_mode_t mode,
                                     const uint8_t *input, uint8_t input_length,
                                     uint8_t *out_frame, uint8_t *out_frame_length,
                                     uart_calc_crc_fn_t calc_crc);

/**
 * @brief Main UART processing routine.
 * @details Call from the application super-loop. Drains the input buffer,
 *          parses frames and drives master/slave logic for every port.
 */
void uart_task(void);

/**
 * @brief 1 ms time base for all UART ports.
 * @details Advances poll countdowns, ages buffered bytes and applies the
 *          optional bus idle timeout. Call exactly once per millisecond.
 */
void uart_tick_1ms(void);

#endif /* UART_H_ */
