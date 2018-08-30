/**
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
    (C)2013 Semtech
 ___ _____ _   ___ _  _____ ___  ___  ___ ___
/ __|_   _/_\ / __| |/ / __/ _ \| _ \/ __| __|
\__ \ | |/ _ \ (__| ' <| _| (_) |   / (__| _|
|___/ |_/_/ \_\___|_|\_\_| \___/|_|_\\___|___|
embedded.connectivity.solutions===============

Description: LoRaWAN stack layer that controls both MAC and PHY underneath

License: Revised BSD License, see LICENSE.TXT file include in the project

Maintainer: Miguel Luis ( Semtech ), Gregory Cristian ( Semtech ) and Daniel Jaeckle ( STACKFORCE )


Copyright (c) 2017, Arm Limited and affiliates.

SPDX-License-Identifier: BSD-3-Clause
*/

#include <string.h>
#include <stdlib.h>
#include "platform/Callback.h"
#include "events/EventQueue.h"

#include "LoRaWANStack.h"

#include "mbed-trace/mbed_trace.h"
#define TRACE_GROUP "LSTK"

#define INVALID_PORT                0xFF
#define MAX_CONFIRMED_MSG_RETRIES   255
/**
 * Control flags for transient states
 */
#define IDLE_FLAG                   0x00000000
#define TX_ONGOING_FLAG             0x00000001
#define MSG_RECVD_FLAG              0x00000002
#define CONNECTED_FLAG              0x00000004
#define USING_OTAA_FLAG             0x00000008
#define TX_DONE_FLAG                0x00000010
#define CONN_IN_PROGRESS_FLAG       0x00000020

using namespace mbed;
using namespace events;

/**
 * Bit mask for message flags
 */

#define MSG_FLAG_MASK                         0x0F

/*****************************************************************************
 * Constructor                                                               *
 ****************************************************************************/
LoRaWANStack::LoRaWANStack()
    : _loramac(),
      _device_current_state(DEVICE_STATE_NOT_INITIALIZED),
      _lw_session(),
      _tx_msg(),
      _rx_msg(),
      _tx_metadata(),
      _rx_metadata(),
      _num_retry(1),
      _ctrl_flags(IDLE_FLAG),
      _app_port(INVALID_PORT),
      _link_check_requested(false),
      _reset_ind_requested(false),
      _rekey_ind_needed(false),
      _rekey_ind_counter(0),
      _device_mode_ind_needed(false),
      _device_mode_ind_ongoing(false),
      _new_class_type(CLASS_A),
      _automatic_uplink_ongoing(false),
      _ready_for_rx(true),
      _queue(NULL),
      _rejoin_type1_send_period(MBED_CONF_LORA_REJOIN_TYPE1_SEND_PERIOD),
      _rejoin_type1_stamp(0),
      _rejoin_type0_counter(0),
      _forced_datarate(DR_0),
      _forced_period(0),
      _forced_retry_count(0),
      _forced_rejoin_type(REJOIN_REQUEST_TYPE0),
      _forced_counter(0)
{
    _tx_metadata.stale = true;
    _rx_metadata.stale = true;

#ifdef MBED_CONF_LORA_APP_PORT
    if (is_port_valid(MBED_CONF_LORA_APP_PORT)) {
        _app_port = MBED_CONF_LORA_APP_PORT;
    } else {
        tr_error("User defined port in .json is illegal.");
    }
#endif
}

/*****************************************************************************
 * Public Methods                                                            *
 ****************************************************************************/
void LoRaWANStack::bind_phy_and_radio_driver(LoRaRadio &radio, LoRaPHY &phy)
{
    radio_events.tx_done = mbed::callback(this, &LoRaWANStack::tx_interrupt_handler);
    radio_events.rx_done = mbed::callback(this, &LoRaWANStack::rx_interrupt_handler);
    radio_events.rx_error = mbed::callback(this, &LoRaWANStack::rx_error_interrupt_handler);
    radio_events.tx_timeout = mbed::callback(this, &LoRaWANStack::tx_timeout_interrupt_handler);
    radio_events.rx_timeout = mbed::callback(this, &LoRaWANStack::rx_timeout_interrupt_handler);

    phy.set_radio_instance(radio);
    _loramac.bind_phy(phy);

    radio.lock();
    radio.init_radio(&radio_events);
    radio.unlock();
}

lorawan_status_t LoRaWANStack::initialize_mac_layer(EventQueue *queue)
{
    if (!queue) {
        return LORAWAN_STATUS_PARAMETER_INVALID;
    }

    tr_debug("Initializing MAC layer");
    _queue = queue;

    return state_controller(DEVICE_STATE_IDLE);
}

lorawan_status_t LoRaWANStack::set_lora_callbacks(const lorawan_app_callbacks_t *callbacks)
{
    if (DEVICE_STATE_NOT_INITIALIZED == _device_current_state) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    if (!callbacks || !callbacks->events) {
        return LORAWAN_STATUS_PARAMETER_INVALID;
    }

    _callbacks.events = callbacks->events;

    if (callbacks->link_check_resp) {
        _callbacks.link_check_resp = callbacks->link_check_resp;
    }

    if (callbacks->battery_level) {
        _callbacks.battery_level = callbacks->battery_level;
        _loramac.set_batterylevel_callback(callbacks->battery_level);
    }

    return LORAWAN_STATUS_OK;
}

lorawan_status_t LoRaWANStack::connect()
{
    if (DEVICE_STATE_NOT_INITIALIZED == _device_current_state) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    if (_ctrl_flags & CONN_IN_PROGRESS_FLAG) {
        return LORAWAN_STATUS_BUSY;
    }

    if (_ctrl_flags & CONNECTED_FLAG) {
        return LORAWAN_STATUS_ALREADY_CONNECTED;
    }

    lorawan_status_t status = _loramac.prepare_join(NULL, MBED_CONF_LORA_OVER_THE_AIR_ACTIVATION);

    if (LORAWAN_STATUS_OK != status) {
        return status;
    }

    return handle_connect(MBED_CONF_LORA_OVER_THE_AIR_ACTIVATION);
}

lorawan_status_t LoRaWANStack::connect(const lorawan_connect_t &connect)
{
    if (DEVICE_STATE_NOT_INITIALIZED == _device_current_state) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    if (_ctrl_flags & CONN_IN_PROGRESS_FLAG) {
        return LORAWAN_STATUS_BUSY;
    }

    if (_ctrl_flags & CONNECTED_FLAG) {
        return LORAWAN_STATUS_ALREADY_CONNECTED;
    }

    if (!(connect.connect_type == LORAWAN_CONNECTION_OTAA)
            && !(connect.connect_type == LORAWAN_CONNECTION_ABP)) {
        return LORAWAN_STATUS_PARAMETER_INVALID;
    }

    bool is_otaa = (connect.connect_type == LORAWAN_CONNECTION_OTAA);

    lorawan_status_t status = _loramac.prepare_join(&connect, is_otaa);

    if (LORAWAN_STATUS_OK != status) {
        return status;
    }

    return handle_connect(is_otaa);
}

lorawan_status_t LoRaWANStack::add_channels(const lorawan_channelplan_t &channel_plan)
{
    if (_device_current_state == DEVICE_STATE_NOT_INITIALIZED) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    return _loramac.add_channel_plan(channel_plan);
}

lorawan_status_t LoRaWANStack::remove_a_channel(uint8_t channel_id)
{
    if (_device_current_state == DEVICE_STATE_NOT_INITIALIZED) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    return _loramac.remove_single_channel(channel_id);
}

lorawan_status_t LoRaWANStack::drop_channel_list()
{
    if (_device_current_state == DEVICE_STATE_NOT_INITIALIZED) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    return _loramac.remove_channel_plan();
}

lorawan_status_t LoRaWANStack::get_enabled_channels(lorawan_channelplan_t &channel_plan)
{
    if (_device_current_state == DEVICE_STATE_NOT_INITIALIZED) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    return _loramac.get_channel_plan(channel_plan);
}

lorawan_status_t LoRaWANStack::set_confirmed_msg_retry(uint8_t count)
{
    if (_device_current_state == DEVICE_STATE_NOT_INITIALIZED) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    if (count >= MAX_CONFIRMED_MSG_RETRIES) {
        return LORAWAN_STATUS_PARAMETER_INVALID;
    }

    _num_retry = count;

    return LORAWAN_STATUS_OK;
}

lorawan_status_t LoRaWANStack::set_channel_data_rate(uint8_t data_rate)
{
    if (DEVICE_STATE_NOT_INITIALIZED == _device_current_state) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    return _loramac.set_channel_data_rate(data_rate);
}


lorawan_status_t LoRaWANStack::enable_adaptive_datarate(bool adr_enabled)
{
    if (_device_current_state == DEVICE_STATE_NOT_INITIALIZED) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    _loramac.enable_adaptive_datarate(adr_enabled);
    return LORAWAN_STATUS_OK;
}

lorawan_status_t LoRaWANStack::stop_sending(void)
{
    if (_device_current_state == DEVICE_STATE_NOT_INITIALIZED) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    if (_loramac.clear_tx_pipe() == LORAWAN_STATUS_OK) {
        _ctrl_flags &= ~TX_DONE_FLAG;
        _ctrl_flags &= ~TX_ONGOING_FLAG;
        _loramac.set_tx_ongoing(false);
        _device_current_state = DEVICE_STATE_IDLE;
        return LORAWAN_STATUS_OK;
    }

    return LORAWAN_STATUS_BUSY;
}

int16_t LoRaWANStack::handle_tx(const uint8_t port, const uint8_t *data,
                                uint16_t length, uint8_t flags,
                                bool null_allowed, bool allow_port_0)
{
    if (_device_current_state == DEVICE_STATE_NOT_INITIALIZED) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    if (!null_allowed && !data) {
        return LORAWAN_STATUS_PARAMETER_INVALID;
    } else if (DEVICE_STATE_NOT_INITIALIZED == _device_current_state) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    // ResetInd is only used for ABP devices after connect, until ResetConf is received
    if (_reset_ind_requested) {
        set_reset_indication();
    } else if (_rekey_ind_needed) {
        if (_rekey_ind_counter < _loramac.get_current_adr_ack_limit()) {
            set_rekey_indication();
            _rekey_ind_counter++;
        } else {
            //TODO: Check if something else is needed also (reset settings?)
            _rekey_ind_needed = false;
            send_event_to_application(JOIN_FAILURE);
            _device_current_state = DEVICE_STATE_IDLE;
        }
    } else if (_link_check_requested) {
        // add a link check request with normal data, until the application
        // explicitly removes it.
        lorawan_status_t stat = set_link_check_request();
        if (stat != LORAWAN_STATUS_OK) {
            return stat;
        }
    }

    if (_device_mode_ind_needed) {
        set_device_mode_indication();
    }

    if (!_lw_session.active) {
        return LORAWAN_STATUS_NO_ACTIVE_SESSIONS;
    }

    if (_loramac.tx_ongoing()) {
        return LORAWAN_STATUS_WOULD_BLOCK;
    }

    lorawan_status_t status;

    if (_loramac.nwk_joined() == false) {
        return LORAWAN_STATUS_NO_NETWORK_JOINED;
    }

    status = set_application_port(port, allow_port_0);

    if (status != LORAWAN_STATUS_OK) {
        tr_error("Illegal application port definition.");
        return status;
    }

    // All the flags are mutually exclusive. In addition to that MSG_MULTICAST_FLAG cannot be
    // used for uplink.
    switch (flags & MSG_FLAG_MASK) {
        case MSG_UNCONFIRMED_FLAG:
        case MSG_CONFIRMED_FLAG:
        case MSG_PROPRIETARY_FLAG:
            break;

        default:
            tr_error("Invalid send flags");
            return LORAWAN_STATUS_PARAMETER_INVALID;
    }

    int16_t len = _loramac.prepare_ongoing_tx(port, data, length, flags, _num_retry);

    status = state_controller(DEVICE_STATE_SCHEDULING);

    // send user the length of data which is scheduled now.
    // user should take care of the pending data.
    return (status == LORAWAN_STATUS_OK) ? len : (int16_t) status;
}

int16_t LoRaWANStack::handle_rx(uint8_t *data, uint16_t length, uint8_t &port, int &flags, bool validate_params)
{
    if (_device_current_state == DEVICE_STATE_NOT_INITIALIZED) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    if (!_lw_session.active) {
        return LORAWAN_STATUS_NO_ACTIVE_SESSIONS;
    }

    // No messages to read.
    if (!_rx_msg.receive_ready) {
        return LORAWAN_STATUS_WOULD_BLOCK;
    }

    if (data == NULL || length == 0) {
        return LORAWAN_STATUS_PARAMETER_INVALID;
    }

    int received_flags = convert_to_msg_flag(_rx_msg.msg.mcps_indication.type);
    if (validate_params) {
        // Check received message port and flags match with the ones requested by user
        received_flags &= MSG_FLAG_MASK;

        if (_rx_msg.msg.mcps_indication.port != port || !(flags & received_flags)) {
            return LORAWAN_STATUS_WOULD_BLOCK;
        }
    }

    // Report values back to user
    port = _rx_msg.msg.mcps_indication.port;
    flags = received_flags;

    const uint8_t *base_ptr = _rx_msg.msg.mcps_indication.buffer;
    uint16_t base_size = _rx_msg.msg.mcps_indication.buffer_size;
    bool read_complete = false;

    // check the length of received message whether we can fit into user
    // buffer completely or not
    if (_rx_msg.msg.mcps_indication.buffer_size > length
            && _rx_msg.prev_read_size == 0) {
        // we can't fit into user buffer. Invoke counter measures
        _rx_msg.pending_size = _rx_msg.msg.mcps_indication.buffer_size - length;
        base_size = length;
        _rx_msg.prev_read_size = base_size;
        memcpy(data, base_ptr, base_size);
    } else if (_rx_msg.prev_read_size == 0) {
        _rx_msg.pending_size = 0;
        _rx_msg.prev_read_size = 0;
        memcpy(data, base_ptr, base_size);
        read_complete = true;
    }

    // If its the pending read then we should copy only the remaining part of
    // the buffer. Due to checks above, in case of a pending read, this block
    // will be the only one to get invoked
    if (_rx_msg.pending_size > 0 && _rx_msg.prev_read_size > 0) {
        memcpy(data, base_ptr + _rx_msg.prev_read_size, base_size);
    }

    // we are done handing over received buffer to user. check if there is
    // anything pending. If not, memset the buffer to zero and indicate
    // that no read is in progress
    if (read_complete) {
        _rx_msg.msg.mcps_indication.buffer = NULL;
        _rx_msg.msg.mcps_indication.buffer_size = 0;
        _rx_msg.receive_ready = false;
    }

    return base_size;
}

lorawan_status_t LoRaWANStack::set_link_check_request()
{
    if (DEVICE_STATE_NOT_INITIALIZED == _device_current_state) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    _link_check_requested = true;
    if (!_callbacks.link_check_resp) {
        tr_error("Must assign a callback function for link check request. ");
        return LORAWAN_STATUS_PARAMETER_INVALID;
    }

    _loramac.setup_link_check_request();
    return LORAWAN_STATUS_OK;
}

void LoRaWANStack::set_reset_indication()
{
    _loramac.setup_reset_indication();
}

void LoRaWANStack::set_rekey_indication()
{
    _loramac.setup_rekey_indication();
}

void LoRaWANStack::set_device_mode_indication()
{
    _loramac.setup_device_mode_indication(_new_class_type);
}

void LoRaWANStack::remove_link_check_request()
{
    _link_check_requested = false;
}

lorawan_status_t LoRaWANStack::shutdown()
{
    if (DEVICE_STATE_NOT_INITIALIZED == _device_current_state) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    return state_controller(DEVICE_STATE_SHUTDOWN);
}

lorawan_status_t LoRaWANStack::set_device_class(const device_class_t &device_class)
{
    if (DEVICE_STATE_NOT_INITIALIZED == _device_current_state) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    if (device_class == CLASS_B) {
        return LORAWAN_STATUS_UNSUPPORTED;
    }
    // Only change the class when needed
    if (_loramac.get_device_class() != device_class) {
        if (_loramac.get_server_type() == LW1_1) {
            _new_class_type = device_class;
            set_device_mode_indication();
            _device_mode_ind_needed = true;
            _device_mode_ind_ongoing = true;
        } else {
            _loramac.set_device_class(device_class, mbed::callback(this, &LoRaWANStack::handle_ack_expiry_for_class_c));
        }

    }
    return LORAWAN_STATUS_OK;
}

lorawan_status_t  LoRaWANStack::acquire_tx_metadata(lorawan_tx_metadata &tx_metadata)
{
    if (DEVICE_STATE_NOT_INITIALIZED == _device_current_state) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    if (!_tx_metadata.stale) {
        tx_metadata = _tx_metadata;
        _tx_metadata.stale = true;
        return LORAWAN_STATUS_OK;
    }

    return LORAWAN_STATUS_METADATA_NOT_AVAILABLE;
}

lorawan_status_t LoRaWANStack::acquire_rx_metadata(lorawan_rx_metadata &metadata)
{
    if (DEVICE_STATE_NOT_INITIALIZED == _device_current_state) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    if (!_rx_metadata.stale) {
        metadata = _rx_metadata;
        _rx_metadata.stale = true;
        return LORAWAN_STATUS_OK;
    }

    return LORAWAN_STATUS_METADATA_NOT_AVAILABLE;
}

lorawan_status_t LoRaWANStack::acquire_backoff_metadata(int &backoff)
{
    if (DEVICE_STATE_NOT_INITIALIZED == _device_current_state) {
        return LORAWAN_STATUS_NOT_INITIALIZED;
    }

    int id = _loramac.get_backoff_timer_event_id();

    if (_loramac.get_backoff_timer_event_id() > 0) {
        backoff = _queue->time_left(id);
        return LORAWAN_STATUS_OK;
    }

    backoff = -1;
    return LORAWAN_STATUS_METADATA_NOT_AVAILABLE;
}

/*****************************************************************************
 * Interrupt handlers                                                        *
 ****************************************************************************/
void LoRaWANStack::tx_interrupt_handler(void)
{
    _tx_timestamp = _loramac.get_current_time();
    const int ret = _queue->call(this, &LoRaWANStack::process_transmission);
    MBED_ASSERT(ret != 0);
    (void)ret;
}

void LoRaWANStack::rx_interrupt_handler(const uint8_t *payload, uint16_t size,
                                        int16_t rssi, int8_t snr)
{
    if (!_ready_for_rx || size > sizeof _rx_payload) {
        return;
    }

    _ready_for_rx = false;
    memcpy(_rx_payload, payload, size);

    const uint8_t *ptr = _rx_payload;
    const int ret = _queue->call(this, &LoRaWANStack::process_reception,
                                 ptr, size, rssi, snr);
    MBED_ASSERT(ret != 0);
    (void)ret;
}

void LoRaWANStack::rx_error_interrupt_handler(void)
{
    const int ret = _queue->call(this, &LoRaWANStack::process_reception_timeout,
                                 false);
    MBED_ASSERT(ret != 0);
    (void)ret;
}

void LoRaWANStack::tx_timeout_interrupt_handler(void)
{
    const int ret = _queue->call(this, &LoRaWANStack::process_transmission_timeout);
    MBED_ASSERT(ret != 0);
    (void)ret;
}

void LoRaWANStack::rx_timeout_interrupt_handler(void)
{
    const int ret = _queue->call(this, &LoRaWANStack::process_reception_timeout,
                                 true);
    MBED_ASSERT(ret != 0);
    (void)ret;
}

/*****************************************************************************
 * Processors for deferred interrupts                                        *
 ****************************************************************************/
void LoRaWANStack::process_transmission_timeout()
{
    // this is a fatal error and should not happen
    tr_debug("TX Timeout");
    _loramac.on_radio_tx_timeout();
    _ctrl_flags &= ~TX_ONGOING_FLAG;
    _ctrl_flags &= ~TX_DONE_FLAG;
    if (_device_current_state == DEVICE_STATE_JOINING) {
        _device_current_state = DEVICE_STATE_IDLE;
        tr_error("Joining abandoned: Radio failed to transmit");
        send_event_to_application(TX_TIMEOUT);
    } else {
        state_controller(DEVICE_STATE_STATUS_CHECK);
    }

    state_machine_run_to_completion();
}

void LoRaWANStack::process_transmission(void)
{
    tr_debug("Transmission completed");
    _loramac.on_radio_tx_done(_tx_timestamp);

    if (_loramac.get_server_type() == LW1_1 && _device_mode_ind_ongoing) {
        _device_mode_ind_ongoing = false;
        _loramac.set_device_class(device_class_t(_new_class_type), mbed::callback(this, &LoRaWANStack::handle_ack_expiry_for_class_c));
        send_event_to_application(CLASS_CHANGED);
    }

    make_tx_metadata_available();

    if (_device_current_state == DEVICE_STATE_JOINING) {
        _device_current_state = DEVICE_STATE_AWAITING_JOIN_ACCEPT;
    }

    if (_device_current_state == DEVICE_STATE_SENDING) {
        if (_loramac.get_mcps_confirmation()->req_type == MCPS_CONFIRMED) {
            _ctrl_flags |= TX_ONGOING_FLAG;
            _ctrl_flags &= ~TX_DONE_FLAG;
            tr_debug("Awaiting ACK");
            _device_current_state = DEVICE_STATE_AWAITING_ACK;
        } else if (_loramac.get_device_class() == CLASS_A) {
            // Class A unconfirmed message sent, TX_DONE event will be sent to
            // application when RX2 windows is elapsed, i.e., in process_reception_timeout()
            _ctrl_flags &= ~TX_ONGOING_FLAG;
            _ctrl_flags |= TX_DONE_FLAG;
        } else if (_loramac.get_device_class() == CLASS_C) {
            // In Class C, reception timeout never happens, so we handle the state
             // progression for TX_DONE in UNCONFIRMED case here
            _loramac.post_process_mcps_req();
            state_controller(DEVICE_STATE_STATUS_CHECK);
            state_machine_run_to_completion();
        }
    }
}

void LoRaWANStack::handle_ack_expiry_for_class_c(void)
{
    _ctrl_flags &= ~TX_DONE_FLAG;
    _ctrl_flags |= TX_ONGOING_FLAG;
    tr_error("Retries exhausted for Class C device");
    state_controller(DEVICE_STATE_STATUS_CHECK);
}

void LoRaWANStack::handle_scheduling_failure(void)
{
    tr_error("Failed to schedule transmission");
    state_controller(DEVICE_STATE_STATUS_CHECK);
    state_machine_run_to_completion();
}

void LoRaWANStack::process_reception(const uint8_t *const payload, uint16_t size,
                                     int16_t rssi, int8_t snr)
{
    _device_current_state = DEVICE_STATE_RECEIVING;
    _ctrl_flags &= ~MSG_RECVD_FLAG;
    _rejoin_type0_counter++;

    bool joined = _loramac.nwk_joined();

    _loramac.on_radio_rx_done(payload, size, rssi, snr, callback(this, &LoRaWANStack::mlme_confirm_handler));

    make_rx_metadata_available();

    if (!joined) {
        _ready_for_rx = true;
        return;
    }

    // if the outgoing message was of CONFIRMED type
    if (_loramac.get_mcps_confirmation()->req_type == MCPS_CONFIRMED) {
        // if ack was not received, we will try retransmission after
        // ACK_TIMEOUT. handle_data_frame() already disables ACK_TIMEOUT timer
        // if ack was received. Otherwise, following method will be called in
        // LoRaMac.cpp, on_ack_timeout_timer_event().
        if (_loramac.get_mcps_indication()->is_ack_recvd) {
            tr_debug("Ack=OK, NbTrials=%d",
                     _loramac.get_mcps_confirmation()->nb_retries);
            _loramac.post_process_mcps_req();
            _ctrl_flags |= TX_DONE_FLAG;
            _ctrl_flags &= ~TX_ONGOING_FLAG;
            state_controller(DEVICE_STATE_STATUS_CHECK);
        } else {
            if (!_loramac.continue_sending_process() &&
                _loramac.get_current_slot() != RX_SLOT_WIN_1) {
                tr_error("Retries exhausted for Class A device");
                _ctrl_flags &= ~TX_DONE_FLAG;
                _ctrl_flags |= TX_ONGOING_FLAG;
                state_controller(DEVICE_STATE_STATUS_CHECK);
            }
        }
    } else if (_loramac.get_device_class() == CLASS_A) {
        // handle UNCONFIRMED case here, RX slots were turned off due to
        // valid packet reception. For Class C, an outgoing UNCONFIRMED message
        // gets its handling in process_transmission.
        _loramac.post_process_mcps_req();
        _ctrl_flags |= TX_DONE_FLAG;
        state_controller(DEVICE_STATE_STATUS_CHECK);
    }

    // handle any pending MCPS indication
    if (_loramac.get_mcps_indication()->pending) {
        _loramac.post_process_mcps_ind();
        _ctrl_flags |= MSG_RECVD_FLAG;
        state_controller(DEVICE_STATE_STATUS_CHECK);
    }

    // change the state only if a TX cycle completes for Class A
    // For class C it's not needed as it will already be in receiving
    // state, no matter if the TX cycle completed or not.
    if (!(_ctrl_flags & TX_ONGOING_FLAG)) {
        // we are done here, update the state
        state_machine_run_to_completion();
    }

    if (_loramac.get_mlme_indication()->pending) {
        tr_debug("MLME Indication pending");
        _loramac.post_process_mlme_ind();
        tr_debug("Immediate Uplink requested");
        mlme_indication_handler();
    }

    if (((_loramac.get_lora_time()->get_current_time()/1000) -
        _rejoin_type1_stamp) > _rejoin_type1_send_period) {
        _rejoin_type1_stamp = _loramac.get_lora_time()->get_current_time()/1000;
        process_rejoin(REJOIN_REQUEST_TYPE1, false);
    }
    uint32_t max_time;
    uint32_t max_count;
    _loramac.get_rejoin_parameters(max_time, max_count);
    if (_rejoin_type0_counter >= max_count) {
        //This causes excactly same handling as a timeout
        process_rejoin_type0();
    }

    _ready_for_rx = true;
}

void LoRaWANStack::process_reception_timeout(bool is_timeout)
{
    rx_slot_t slot = _loramac.get_current_slot();
    _rejoin_type0_counter++;

    // when is_timeout == false, a CRC error took place in the received frame
    // we treat that erroneous frame as no frame received at all, hence handle
    // it exactly as we would handle timeout
    _loramac.on_radio_rx_timeout(is_timeout);

    if (slot == RX_SLOT_WIN_2 && !_loramac.nwk_joined()) {
        state_controller(DEVICE_STATE_JOINING);
        return;
    }

    /**
     * LoRaWAN Specification 1.0.2. Section 3.3.6
     * Main point:
     *     We indicate successful transmission
     * of UNCONFIRMED message after RX windows are done with.
     *     For a CONFIRMED message, it means that we have not received
     * ack (actually nothing was received), and we should retransmit if we can.
     *
     * NOTE: This code block doesn't get hit for Class C as in Class C, RX2 timeout
     * never occurs.
     */
    if (slot == RX_SLOT_WIN_2) {
        _loramac.post_process_mcps_req();

        if (_loramac.get_mcps_confirmation()->req_type == MCPS_CONFIRMED) {
            if (_loramac.continue_sending_process()) {
                return;
            } else {
                tr_error("Retries exhausted for Class A device");
            }
        }

        state_controller(DEVICE_STATE_STATUS_CHECK);
        state_machine_run_to_completion();

        if ((_loramac.get_lora_time()->get_current_time()/1000) -
            _rejoin_type1_stamp > _rejoin_type1_send_period) {
            _rejoin_type1_stamp = _loramac.get_lora_time()->get_current_time()/1000;
            process_rejoin(REJOIN_REQUEST_TYPE1, false);
        }

        uint32_t max_time;
        uint32_t max_count;
        _loramac.get_rejoin_parameters(max_time, max_count);
        if (_rejoin_type0_counter >= max_count) {
            //This causes excactly same handling as a timeout
            process_rejoin_type0();
        }
    }
}

/*****************************************************************************
 * Private methods                                                           *
 ****************************************************************************/
void LoRaWANStack::make_tx_metadata_available(void)
{
    _tx_metadata.stale = false;
    _tx_metadata.channel = _loramac.get_mcps_confirmation()->channel;
    _tx_metadata.data_rate = _loramac.get_mcps_confirmation()->data_rate;
    _tx_metadata.tx_power = _loramac.get_mcps_confirmation()->tx_power;
    _tx_metadata.tx_toa = _loramac.get_mcps_confirmation()->tx_toa;
    _tx_metadata.nb_retries = _loramac.get_mcps_confirmation()->nb_retries;
}

void LoRaWANStack::make_rx_metadata_available(void)
{
    _rx_metadata.stale = false;
    _rx_metadata.rx_datarate = _loramac.get_mcps_indication()->rx_datarate;
    _rx_metadata.rssi = _loramac.get_mcps_indication()->rssi;
    _rx_metadata.snr = _loramac.get_mcps_indication()->snr;
}

bool LoRaWANStack::is_port_valid(const uint8_t port, bool allow_port_0)
{
    //Application should not use reserved and illegal port numbers.
    if (port == 0) {
        return allow_port_0;
    } else {
        return true;
    }
}

lorawan_status_t LoRaWANStack::set_application_port(const uint8_t port, bool allow_port_0)
{
    if (is_port_valid(port, allow_port_0)) {
        _app_port = port;
        return LORAWAN_STATUS_OK;
    }

    return LORAWAN_STATUS_PORT_INVALID;
}

void LoRaWANStack::state_machine_run_to_completion()
{
    if (_loramac.get_device_class() == CLASS_C) {
        _device_current_state = DEVICE_STATE_RECEIVING;
        return;
    }

    _device_current_state = DEVICE_STATE_IDLE;
}

void LoRaWANStack::send_event_to_application(const lorawan_event_t event) const
{
    if (_callbacks.events) {
        const int ret = _queue->call(_callbacks.events, event);
        MBED_ASSERT(ret != 0);
        (void)ret;
    }
}

void LoRaWANStack::send_automatic_uplink_message(const uint8_t port)
{
    // we will silently ignore the automatic uplink event if the user is already
    // sending something
    const int16_t ret = handle_tx(port, NULL, 0, MSG_CONFIRMED_FLAG, true, true);
    if (ret == LORAWAN_STATUS_WOULD_BLOCK) {
        _automatic_uplink_ongoing = false;
    } else if (ret < 0) {
        tr_debug("Failed to generate AUTOMATIC UPLINK, error code = %d", ret);
        send_event_to_application(AUTOMATIC_UPLINK_ERROR);
    }
}

int LoRaWANStack::convert_to_msg_flag(const mcps_type_t type)
{
    int msg_flag = MSG_UNCONFIRMED_FLAG;
    switch (type) {
        case MCPS_UNCONFIRMED:
            msg_flag = MSG_UNCONFIRMED_FLAG;
            break;

        case MCPS_CONFIRMED:
            msg_flag = MSG_CONFIRMED_FLAG;
            break;

        case MCPS_MULTICAST:
            msg_flag = MSG_MULTICAST_FLAG;
            break;

        case MCPS_PROPRIETARY:
            msg_flag = MSG_PROPRIETARY_FLAG;
            break;

        default:
            tr_error("Unknown message type!");
            MBED_ASSERT(0);
    }

    return msg_flag;
}

lorawan_status_t LoRaWANStack::handle_connect(bool is_otaa)
{
    _ctrl_flags |= CONN_IN_PROGRESS_FLAG;

    if (is_otaa) {
        tr_debug("Initiating OTAA");

        // In 1.0.2 spec, counters are always set to zero for new connection.
        // This section is common for both normal and
        // connection restore at this moment. Will change in future with 1.1 support.
        _lw_session.downlink_counter = 0;
        _lw_session.uplink_counter = 0;
        _ctrl_flags |= USING_OTAA_FLAG;
        //We cannot set _rekey_ind_needed here, because server might not support LW1.1
    } else {
        // If current state is SHUTDOWN, device may be trying to re-establish
        // communication. In case of ABP specification is meddled about frame counters.
        // It says to reset counters to zero but there is no mechanism to tell the
        // network server that the device was disconnected or restarted.
        // At the moment, this implementation does not support a non-volatile
        // memory storage.
        //_lw_session.downlink_counter; //Get from NVM
        //_lw_session.uplink_counter; //Get from NVM

        if (MBED_CONF_LORA_VERSION == LORAWAN_VERSION_1_1) {
            _reset_ind_requested = true;
            //TODO: Switch back to default MAC and radio parameters, but leave counters untouched
        }

        tr_debug("Initiating ABP");
        tr_debug("Frame Counters. UpCnt=%lu, DownCnt=%lu",
                 _lw_session.uplink_counter, _lw_session.downlink_counter);
        _ctrl_flags &= ~USING_OTAA_FLAG;
    }

    return state_controller(DEVICE_STATE_CONNECTING);
}

void LoRaWANStack::mlme_indication_handler()
{
    if (_loramac.get_mlme_indication()->indication_type == MLME_SCHEDULE_UPLINK) {
        // The MAC signals that we shall provide an uplink as soon as possible
#if MBED_CONF_LORA_AUTOMATIC_UPLINK_MESSAGE
        _automatic_uplink_ongoing = true;
        tr_debug("mlme indication: sending empty uplink to port 0 to acknowledge MAC commands...");
        const uint8_t port = 0;
        const int ret = _queue->call(this, &LoRaWANStack::send_automatic_uplink_message, port);
        MBED_ASSERT(ret != 0);
        (void)ret;
#else
        send_event_to_application(UPLINK_REQUIRED);
#endif
        return;
    }

    tr_error("Unknown MLME Indication type.");
}

void LoRaWANStack::mlme_confirm_handler(loramac_mlme_confirm_t& mlme_confirm)
{
    if (mlme_confirm.type == MLME_LINK_CHECK) {
        if (mlme_confirm.status == LORAMAC_EVENT_INFO_STATUS_OK) {
            if (_callbacks.link_check_resp) {
                const int ret = _queue->call(_callbacks.link_check_resp,
                                             mlme_confirm.demod_margin,
                                             mlme_confirm.nb_gateways);
                MBED_ASSERT(ret != 0);
                (void)ret;
            }
        }
    } else if (mlme_confirm.type == MLME_RESET) {
        _reset_ind_requested = false;
    } else if (mlme_confirm.type == MLME_REKEY) {
        _rekey_ind_needed = false;
        _rekey_ind_counter = 0;
    } else if (mlme_confirm.type == MLME_DEVICE_MODE) {
        _device_mode_ind_needed = false;
        if (_loramac.get_device_class() == mlme_confirm.classType) {
            send_event_to_application(SERVER_ACCEPTED_CLASS_IN_USE);
        } else {
            send_event_to_application(SERVER_DOES_NOT_SUPPORT_CLASS_IN_USE);
        }
    } else if (mlme_confirm.type == MLME_JOIN_ACCEPT) {
        switch (mlme_confirm.status) {
            case LORAMAC_EVENT_INFO_STATUS_OK:
                if (_loramac.get_server_type() == LW1_1) {
                    _rekey_ind_needed = true;
                    _rekey_ind_counter = 0;
                    // THIS IS NOT ALLOWED HERE!
                    // We might get JOIN_ACCEPT for rejoin type 1,
                    // which points to different server!
                    //reset_forced_rejoin();
                } else {
                    _loramac.get_lora_time()->stop(_forced_timer);
                    _loramac.get_lora_time()->stop(_rejoin_type0_timer);
                }
                state_controller(DEVICE_STATE_CONNECTED);
                break;

            case LORAMAC_EVENT_INFO_STATUS_CRYPTO_FAIL:
                // fatal error
                _device_current_state = DEVICE_STATE_IDLE;
                tr_error("Joining abandoned: CRYPTO_ERROR");
                send_event_to_application(CRYPTO_ERROR);
                break;
            default:
                // non-fatal, retry if possible
                _device_current_state = DEVICE_STATE_AWAITING_JOIN_ACCEPT;
                state_controller(DEVICE_STATE_JOINING);
        }
    } else if (mlme_confirm.type == MLME_FORCE_REJOIN) {
        if (join_req_type_t(mlme_confirm.rejoin_type) <= REJOIN_REQUEST_TYPE2 &&
            _loramac.get_server_type() == LW1_1 ) {
            _forced_datarate  = mlme_confirm.datarate;
            _forced_period = ((1 << mlme_confirm.period)*32 + (rand()%33))*1000;
            _forced_retry_count = mlme_confirm.max_retries;
            if (_forced_retry_count) {
                _forced_retry_count += 1;
            }
            _forced_rejoin_type = join_req_type_t(mlme_confirm.rejoin_type);
            // See LW 1.1 chapter 5.13 - RejoinType
            if (join_req_type_t(mlme_confirm.rejoin_type) == REJOIN_REQUEST_TYPE1) {
                _forced_rejoin_type = REJOIN_REQUEST_TYPE0;
            }
            reset_forced_rejoin();
            process_rejoin(_forced_rejoin_type, true);
            if (_forced_retry_count) {
                _loramac.get_lora_time()->start(_forced_timer, _forced_period);
            }
        }
    }
}

void LoRaWANStack::mcps_confirm_handler()
{
    switch (_loramac.get_mcps_confirmation()->status) {

        case LORAMAC_EVENT_INFO_STATUS_OK:
            _lw_session.uplink_counter = _loramac.get_mcps_confirmation()->ul_frame_counter;
            send_event_to_application(TX_DONE);
            break;

        case LORAMAC_EVENT_INFO_STATUS_TX_TIMEOUT:
            tr_error("Fatal Error, Radio failed to transmit");
            send_event_to_application(TX_TIMEOUT);
            break;

        case LORAMAC_EVENT_INFO_STATUS_TX_DR_PAYLOAD_SIZE_ERROR:
            send_event_to_application(TX_SCHEDULING_ERROR);
            break;

        default:
            // if no ack was received after enough retries, send TX_ERROR
            send_event_to_application(TX_ERROR);
    }
}

void LoRaWANStack::mcps_indication_handler()
{
    const loramac_mcps_indication_t *mcps_indication = _loramac.get_mcps_indication();
    if (mcps_indication->status != LORAMAC_EVENT_INFO_STATUS_OK) {
        tr_error("RX_ERROR: mcps_indication status = %d", mcps_indication->status);
        send_event_to_application(RX_ERROR);
        return;
    }

    _lw_session.downlink_counter = mcps_indication->dl_frame_counter;

    if (mcps_indication->port == 224) {
        tr_info("Compliance test disabled.");
    } else {
        if (mcps_indication->is_data_recvd) {
            // Valid message arrived.
            _rx_msg.type = LORAMAC_RX_MCPS_INDICATION;
            _rx_msg.msg.mcps_indication.buffer_size = mcps_indication->buffer_size;
            _rx_msg.msg.mcps_indication.port = mcps_indication->port;
            _rx_msg.msg.mcps_indication.buffer = mcps_indication->buffer;
            _rx_msg.msg.mcps_indication.type = mcps_indication->type;

            // Notify application about received frame..
            tr_debug("Packet Received %d bytes, Port=%d",
                     _rx_msg.msg.mcps_indication.buffer_size,
                     mcps_indication->port);
            _rx_msg.receive_ready = true;
            send_event_to_application(RX_DONE);
        }

        /*
         * If fPending bit is set we try to generate an empty packet
         * with CONFIRMED flag set. We always set a CONFIRMED flag so
         * that we could retry a certain number of times if the uplink
         * failed for some reason
         * or
         * Class C and node received a confirmed message so we need to
         * send an empty packet to acknowledge the message.
         * This scenario is unspecified by LoRaWAN 1.0.2 specification,
         * but version 1.1.0 says that network SHALL not send any new
         * confirmed messages until ack has been sent
         */
        if ((_loramac.get_device_class() != CLASS_C && mcps_indication->fpending_status)
                ||
                (_loramac.get_device_class() == CLASS_C && mcps_indication->type == MCPS_CONFIRMED)) {
#if (MBED_CONF_LORA_AUTOMATIC_UPLINK_MESSAGE)
            tr_debug("Sending empty uplink message...");
            _automatic_uplink_ongoing = true;
            const int ret = _queue->call(this, &LoRaWANStack::send_automatic_uplink_message, mcps_indication->port);
            MBED_ASSERT(ret != 0);
            (void)ret;
#else
            send_event_to_application(UPLINK_REQUIRED);
#endif
        }
    }
}

lorawan_status_t LoRaWANStack::state_controller(device_states_t new_state)
{
    lorawan_status_t status = LORAWAN_STATUS_OK;

    switch (new_state) {
        case DEVICE_STATE_IDLE:
            process_idle_state(status);
            break;
        case DEVICE_STATE_CONNECTING:
            process_connecting_state(status);
            break;
        case DEVICE_STATE_JOINING:
            process_joining_state(status);
            break;
        case DEVICE_STATE_CONNECTED:
            process_connected_state();
            break;
        case DEVICE_STATE_SCHEDULING:
            process_scheduling_state(status);
            break;
        case DEVICE_STATE_STATUS_CHECK:
            process_status_check_state();
            break;
        case DEVICE_STATE_SHUTDOWN:
            process_shutdown_state(status);
            break;
        default:
            tr_debug("state_controller: Unknown state!");
            status = LORAWAN_STATUS_SERVICE_UNKNOWN;
            break;
    }

    return status;
}

void LoRaWANStack::process_shutdown_state(lorawan_status_t &op_status)
{
    /**
     * Remove channels
     * Radio will be put to sleep by the APIs underneath
     */
    drop_channel_list();
    _loramac.disconnect();
    _lw_session.active = false;
    _device_current_state = DEVICE_STATE_SHUTDOWN;
    op_status = LORAWAN_STATUS_DEVICE_OFF;
    _ctrl_flags &= ~CONNECTED_FLAG;
    _ctrl_flags &= ~CONN_IN_PROGRESS_FLAG;
    send_event_to_application(DISCONNECTED);
}

void LoRaWANStack::process_status_check_state()
{
    if (_device_current_state == DEVICE_STATE_SENDING ||
            _device_current_state == DEVICE_STATE_AWAITING_ACK) {
        // If there was a successful transmission, this block gets a kick after
        // RX2 slot is exhausted. We may or may not have a successful UNCONFIRMED transmission
        // here. In CONFIRMED case this block is invoked only
        // when the MAX number of retries are exhausted, i.e., only error
        // case will fall here. Moreover, it will happen for Class A only.
        // Another possibility is the case when the stack fails to schedule a
        // deferred transmission and a scheduling failure handler is invoked.
        _ctrl_flags &= ~TX_DONE_FLAG;
        _ctrl_flags &= ~TX_ONGOING_FLAG;
        _loramac.set_tx_ongoing(false);
        _loramac.reset_ongoing_tx();
        mcps_confirm_handler();

    } else if (_device_current_state == DEVICE_STATE_RECEIVING) {

        if ((_ctrl_flags & TX_DONE_FLAG) || (_ctrl_flags & TX_ONGOING_FLAG)) {
            // for CONFIRMED case, ack validity is already checked
            // If it was a successful transmission, TX_ONGOING_FLAG will not be set.
            // If it was indeed set, that means the device was in Class C mode and
            // CONFIRMED transmission was in place and the ack retries maxed out.
            _ctrl_flags &= ~TX_DONE_FLAG;
            _ctrl_flags &= ~TX_ONGOING_FLAG;
            _loramac.set_tx_ongoing(false);
            _loramac.reset_ongoing_tx();
            // if an automatic uplink is ongoing, we should not send a TX_DONE
            // event to application
            if (_automatic_uplink_ongoing) {
                _automatic_uplink_ongoing = false;
            } else {
                mcps_confirm_handler();
            }
        }

        // handle any received data and send event accordingly
        if (_ctrl_flags & MSG_RECVD_FLAG) {
            _ctrl_flags &= ~MSG_RECVD_FLAG;
            mcps_indication_handler();
        }
    }
}

void LoRaWANStack::process_scheduling_state(lorawan_status_t &op_status)
{
    if (_device_current_state != DEVICE_STATE_IDLE) {
        if (_device_current_state != DEVICE_STATE_RECEIVING
                && _loramac.get_device_class() != CLASS_C) {
            op_status = LORAWAN_STATUS_BUSY;
            return;
        }
    }

    op_status = _loramac.send_ongoing_tx();
    if (op_status == LORAWAN_STATUS_OK) {
        _ctrl_flags |= TX_ONGOING_FLAG;
        _ctrl_flags &= ~TX_DONE_FLAG;
        _loramac.set_tx_ongoing(true);
        _device_current_state = DEVICE_STATE_SENDING;
    }
}

void LoRaWANStack::process_joining_state(lorawan_status_t &op_status)
{
    if (_device_current_state == DEVICE_STATE_CONNECTING) {
        _device_current_state = DEVICE_STATE_JOINING;
        tr_debug("Sending Join Request ...");
        op_status = _loramac.join(true);
        return;
    }

    if (_device_current_state == DEVICE_STATE_AWAITING_JOIN_ACCEPT &&
        _loramac.get_current_slot() != RX_SLOT_WIN_1) {
        _device_current_state = DEVICE_STATE_JOINING;
        // retry join
        bool can_continue = _loramac.continue_joining_process();

        if (!can_continue) {
            _ctrl_flags &= ~CONN_IN_PROGRESS_FLAG;
            send_event_to_application(JOIN_FAILURE);
            _device_current_state = DEVICE_STATE_IDLE;
            return;
        }
    }
}

void LoRaWANStack::process_connected_state()
{
    _ctrl_flags |= CONNECTED_FLAG;
    _ctrl_flags &= ~CONN_IN_PROGRESS_FLAG;

    if (_ctrl_flags & USING_OTAA_FLAG) {
        tr_debug("OTAA Connection OK!");
    }

    _lw_session.active = true;
    send_event_to_application(CONNECTED);

    _device_current_state = DEVICE_STATE_IDLE;
}

void LoRaWANStack::process_connecting_state(lorawan_status_t &op_status)
{
    MBED_ASSERT(_device_current_state == DEVICE_STATE_IDLE ||
                _device_current_state == DEVICE_STATE_SHUTDOWN);

    _device_current_state = DEVICE_STATE_CONNECTING;

    if (_ctrl_flags & USING_OTAA_FLAG) {
        process_joining_state(op_status);
        return;
    }

    op_status = _loramac.join(false);
    tr_debug("ABP connection OK.");
    process_connected_state();
}

void LoRaWANStack::process_idle_state(lorawan_status_t &op_status)
{
    if (_device_current_state == DEVICE_STATE_NOT_INITIALIZED) {
        _device_current_state = DEVICE_STATE_IDLE;
        process_uninitialized_state(op_status);
        return;
    }

    _device_current_state = DEVICE_STATE_IDLE;
    op_status = LORAWAN_STATUS_OK;
}

void LoRaWANStack::process_uninitialized_state(lorawan_status_t &op_status)
{
    op_status = _loramac.initialize(_queue, mbed::callback(this,
                                                           &LoRaWANStack::handle_scheduling_failure));

    if (op_status == LORAWAN_STATUS_OK) {
        _device_current_state = DEVICE_STATE_IDLE;
    }

    if (MBED_CONF_LORA_VERSION == LORAWAN_VERSION_1_1) {
        _loramac.get_lora_time()->init(_forced_timer,
                        mbed::callback(this, &LoRaWANStack::forced_timer_expiry));

        _loramac.get_lora_time()->init(_rejoin_type0_timer,
                        mbed::callback(this, &LoRaWANStack::process_rejoin_type0));

        _rejoin_type1_stamp = _loramac.get_lora_time()->get_current_time()/1000;
    }
}

void LoRaWANStack::process_rejoin(join_req_type_t rejoin_type, bool is_forced)
{
    if (_loramac.get_server_type() == LW1_1 ) {
        _loramac.rejoin(rejoin_type, is_forced, _forced_datarate);
        if (rejoin_type == REJOIN_REQUEST_TYPE0) {
            _loramac.get_lora_time()->stop(_rejoin_type0_timer);
            _rejoin_type0_counter = 0;
            uint32_t max_time;
            uint32_t max_count;
            _loramac.get_rejoin_parameters(max_time, max_count);
            _loramac.get_lora_time()->start(_rejoin_type0_timer, max_time);
        }
    }
}

void LoRaWANStack::reset_forced_rejoin()
{
    _forced_counter = 0;
    _loramac.get_lora_time()->stop(_forced_timer);
}

void LoRaWANStack::forced_timer_expiry()
{
    if (_loramac.get_server_type() == LW1_1 ) {
        if (_forced_counter < _forced_retry_count) {
            process_rejoin(_forced_rejoin_type, true);
            _loramac.get_lora_time()->start(_forced_timer, _forced_period);
        } else {
            reset_forced_rejoin();
        }
    }
}

void LoRaWANStack::process_rejoin_type0()
{
    if (_loramac.get_server_type() == LW1_1 ) {
        //stop in case counter was exceeded
        _loramac.get_lora_time()->stop(_rejoin_type0_timer);
        _rejoin_type0_counter = 0;
        process_rejoin(REJOIN_REQUEST_TYPE0, false);
        uint32_t max_time;
        uint32_t max_count;
        _loramac.get_rejoin_parameters(max_time, max_count);
        _loramac.get_lora_time()->start(_rejoin_type0_timer, max_time);
    }
}
