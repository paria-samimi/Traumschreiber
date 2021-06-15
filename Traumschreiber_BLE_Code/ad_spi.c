/**
 * Copyright (c) 2015 - 2019, Nordic Semiconductor ASA
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Nordic
 *    Semiconductor ASA integrated circuit in a product or a software update for
 *    such product, must reproduce the above copyright notice, this list of
 *    conditions and the following disclaimer in the documentation and/or other
 *    materials provided with the distribution.
 *
 * 3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Nordic Semiconductor ASA integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY NORDIC SEMICONDUCTOR ASA "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINTRAUMNGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include "ad_spi.h"

#include "nrf_drv_spi.h"
#include "app_util_platform.h"
#include "nrf_gpio.h"
#include "nrf_drv_gpiote.h"
#include "nrf_delay.h"
#include "boards.h"
#include "app_error.h"
#include <string.h>
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
#include "app_timer.h"
#include "nrf_drv_timer.h"
#include "traumschreiber_service.h"
#include "arm_math.h"


// timer
APP_TIMER_DEF(m_spi_char_timer_id);
#define SPI_CHAR_TIMER_INTERVAL     APP_TIMER_TICKS(1000) // 1000 ms intervals

//ble service handle. needs to be given to traum_eeg_update_characteristic.
bool spi_ble_connected_flag = false;
uint8_t spi_ble_notification_flag = 0;
uint8_t spi_ble_notification_threshold = 5;
ble_traum_t * spi_traum_service;
uint8_t spi_ble_encodings_sent = 0;

bool  ad_recieved[] = {false, false, false};
uint8_t  ad_converted[] = {0, 0, 0};

bool battery_read = false;


// Debug output. called every 1000ms.
static void spi_timer_timeout_handler(void * p_context)
{
   // NRF_LOG_INFO("one sec");
//    NRF_LOG_INFO("skip counter: %i", packetSkipCounter);
    if (spi_ble_connected_flag && spi_ble_notification_flag >= spi_ble_notification_threshold) {
//        NRF_LOG_INFO("r/s: %i/%i+%i\tc: %i/%i", collected_packets_counter, send_packets_counter, packetSkipCounter, stb_write_position, stb_read_capacity);
        //NRF_LOG_INFO("r/s: %i/%i+%i", collected_packets_counter, send_packets_counter, packetSkipCounter);
        //NRF_LOG_INFO("w-t: %i/%i\t\t,cw: %i\t\tcr: %i", stb_write_position, stb_read_position, stb_write_capacity, stb_read_capacity);
    }
    collected_packets_counter = 0;
    send_packets_counter = 0;
//    packetSkipCounter = 0;
}



//keeping track whether BLE is connected or not.
void spi_ble_connect(ble_traum_t * p_traum_service)
{
    spi_traum_service = p_traum_service;

    //reset to initial values
    for(int i = 0; i < SPI_CHANNEL_NUMBER_TOTAL;i++) {
        spi_encoded_values[i] = 0;
        spi_estimated_variance[i] = 1<<11; //11 is abitrary, results to 2048. needs to be positive and != 0
    }
    spi_adapt_encoding();

    spi_ble_connected_flag = true;
    spi_ble_notification_flag = 0;
    spi_ble_notification_threshold = traum_use_code_characteristic ? 2 : 1;
    NRF_LOG_INFO(" spi_ble_connect called");
    NRF_LOG_FLUSH();

    //initialize config register and read battery
//    memset(&spi_config_register.send_data, 0, 4);
    memcpy(&spi_config_register.send_data, &spi_config_register_default, 8);
    spi_read_battery_status();
}

void spi_ble_disconnect()
{
    spi_traum_service = NULL;
    spi_ble_connected_flag = false;

    //resetting buffers
    stb_write_position  = 0;
    stb_read_position   = 0;
    stb_write_capacity  = 0;
    stb_read_capacity   = 0;

    NRF_LOG_INFO(" spi_ble_disconnect called");
}

//keeping track whether BLE stack has notifications enabled.
void spi_ble_notify(uint16_t notify, ble_traum_t * p_traum_service)
{
    if (notify >= 1) { //can be 1, 2 or 3, depending on notification, indication or both
        spi_ble_notification_flag += 1;
        //resetting buffers
        stb_write_position  = 0;
        stb_read_position   = 0;
        stb_write_capacity  = 0;
        stb_read_capacity   = 0;
        stb_characteristic  = BLE_TRAUM_SAMPLES_PER_PACKAGE;
        spi_enc_estimate_factor_9 = SPI_ENC_EST_FACTOR_9_W;
        spi_enc_estimate_factor_1 = SPI_ENC_EST_FACTOR_1_W;
        spi_enc_estimate_factor_5 = SPI_ENC_EST_FACTOR_5_W;
        spi_enc_warmup = 1;
        recieved_packets_counter = 0;
        //reset stuff
        for (int i; i < SPI_CHANNEL_NUMBER_TOTAL;i++) {
            spi_encoded_values[i]     = 0;
            spi_estimated_variance[i] = 0x100;
            spi_estimated_average[i]  = 0x0;
            spi_encode_shift[i]       = 0x00;
        }
    } else {
        spi_ble_notification_flag -= 1;
        
        //if characteristic reading gets deactivated, initiate battery status read via spi
        if (spi_ble_notification_flag == spi_ble_notification_threshold - 1) {
            spi_read_battery_status();
        }
    }
//    NRF_LOG_INFO(" notify: %i -> %i", notify, spi_ble_notification_flag);
//    NRF_LOG_FLUSH();
}


//initiates SPI-read
void spi_transfer_ADread(uint8_t ad_id)
{
    if (spi_ble_connected_flag && spi_ble_notification_flag >= spi_ble_notification_threshold) { //only read data from AD if BLE connection exists

       // Reset rx buffer
       memset(m_rx_buf[ad_id], 0, m_rlength);

       APP_ERROR_CHECK(nrf_drv_spi_transfer(&spi[ad_id], m_tx_buf, m_length, m_rx_buf[ad_id], m_length));
    }
}


/**
 * @brief GPIO user event handler.
 * @param event
 * triggered when conversion of AD is complete and data can be accessed vie SPI
 * calls spi_transfer_ADread to initiate SPI-read
 */
void spi_trigger_pin_handler(nrf_drv_gpiote_pin_t pin, nrf_gpiote_polarity_t action)
{
    uint8_t ad_id;
    if (pin == SPI_0_DRDY) {
        ad_id = 0;
        //LED toggle forth
        nrf_gpio_pin_write(20, 0);
    } else if (pin == SPI_1_DRDY) {
        ad_id = 1;
    } else if (pin == SPI_2_DRDY) {
        ad_id = 2;
    } else {
       NRF_LOG_INFO("false pin trigger");
       return;
    }

    
    spi_transfer_ADread(ad_id);
}


/**
 * @brief SPI user event handler.
 * @param event
 * called when SPI-transfer is completed
 */
void spi_event_handler(nrf_drv_spi_evt_t const * p_event,
                       void *                    p_context)
{
    uint8_t ad_id = *(uint8_t*)p_context;
    
    //set flag for data conversion, that there is data from ad_id
    ad_recieved[ad_id] = true;
}

//this function
//- reads out values from AD buffer
//- if there is data from all 3 AD's
//  - filtering
//  - check if buffer is full
//  - if not: (data generation), encoding
//- clears AD data
//- adapts encoding, if 1000 pkg are reached
void spi_data_conversion(uint8_t ad_id) {

    uint8_t   n_channel;
    float32_t value;
    float32_t filtered_lp;
    float32_t filtered_iir;

    //LED toggle back
    if (ad_id == 0) nrf_gpio_pin_write(20, 1);

    for(int i = 0; i < SPI_READ_CHANNEL_NUMBER;i++) {
        n_channel = ad_id*SPI_READ_CHANNEL_NUMBER+i;

        //convert 24Bit SPI value to 32Bit
        //val = (int32_t)(0x007FFFFF && (uint32_t)spi_read_buf[i*SPI_READ_PER_CHANNEL] + 0x000001FF * ((uint32_t)spi_read_buf[i*SPI_READ_PER_CHANNEL] && 0x00800000));
        //it is bytes 1,2,3. pos 0 is header.
        value = (float32_t) ((m_rx_buf[ad_id][i*SPI_READ_PER_CHANNEL+1] << 24 | m_rx_buf[ad_id][i*SPI_READ_PER_CHANNEL+2] << 16 | m_rx_buf[ad_id][i*SPI_READ_PER_CHANNEL+3] << 8) >> 8); //more elegant than the line above
        //low pass filtering
        if (spi_lowpass_filter_enabled) {
            arm_biquad_cascade_df2T_f32(&lowpass_instance[n_channel], &value, &filtered_lp, 1);
        } else {
            filtered_lp = value;
        }
        //50Hz/notch filtering
        if (spi_iir_filter_enabled) {
            arm_biquad_cascade_df2T_f32(&iir_instance[n_channel], &filtered_lp, &filtered_iir, 1);
        } else {
            filtered_iir = filtered_lp;
        }
        spi_channel_values[n_channel] += filtered_iir;
    }
    ad_recieved[ad_id] = false;
    ad_converted[ad_id] += 1;
    
//    //skipping events for downsampling
//    if (triggerSkipCounter[ad_id] < triggerSkipCounterMax) {
//        triggerSkipCounter[ad_id] += 1;
//        return;
//    }
//    triggerSkipCounter[ad_id] = 0;



    //see if all channels were recieved, if yes try to send data
    if (ad_converted[0] >= spi_enc_packet_division && ad_converted[1] >= spi_enc_packet_division && ad_converted[2] >= spi_enc_packet_division) {
        collected_packets_counter += 1;

        //filter befor check if there is space
        //this is needed, because otherwise missed packages have aftershocks in the filtered signal
        //spi_filter_data();
        //instead:
        for(int i = 0; i < SPI_CHANNEL_NUMBER_TOTAL;i++) {
        
            //calculate average
            value = spi_channel_values[i] / ad_converted[i/SPI_READ_CHANNEL_NUMBER];
            spi_channel_values[i] = 0;

            //high pass filter
            if (spi_highpass_filter_enabled) {
                arm_biquad_cascade_df2T_f32(&highpass_instance[n_channel], &value, &spi_filtered_values[i], 1);
            } else if (spi_fo_hp_filter_enabled) {
                fo_hp_last_y[n_channel] = fo_hp_alpha * (fo_hp_last_y[n_channel] + value - fo_hp_last_x[n_channel]);
                fo_hp_last_x[n_channel] = value;
                spi_filtered_values[i] = fo_hp_last_y[n_channel];
            } else {
                spi_filtered_values[i] = value;
            }

        } 

        //if there is space in spi_send_buffer
        //leaves 20 packages extra space for packets currently in the BLE buffers that are not yet send
        //stb_read_capacity_safety is 20 because buffer size is 5 per eeg-characteristic (and then a little extra)
        if (stb_read_capacity + stb_read_capacity_safety + stb_packet_size_w > stb_buffer_length) {
               
            packetSkipCounter += 1;
            samplesDroppedCounter += 1;
            nrf_gpio_pin_write(18, 0);

        } else {

            if (spi_data_gen_enabled) {
                for(int8_t i = 0;i < 8; i++) {
                    //spi_data_gen_buf[i] = spi_data_gen_buf[i] + spi_data_gen_add;
                    spi_data_gen_buf[i] = spi_data_gen_buf[i]*(-1);
                    //spi_data_gen_buf[i] = spi_data_gen_buf[i] + (0x04 << (2*i));
                    spi_filtered_values[i] = spi_data_gen_buf[i];
                }
                //NRF_LOG_INFO("gen: %i/%i+%i\tc: %i\t%i-%i", spi_data_gen_buf[0], spi_data_gen_buf[1], spi_data_gen_buf[2], spi_data_gen_buf[3], spi_data_gen_buf[4], spi_data_gen_buf[5]);
            }

            //encode
            spi_encode_data();

            buffer_usage_counter += stb_read_capacity;

            //NRF_LOG_INFO("S: %02x%02x%02x%02x = %i", m_rx_buf[0][0], m_rx_buf[0][1], m_rx_buf[0][2], m_rx_buf[0][3], spi_channel_values[0]);
            
            nrf_gpio_pin_write(18, 1);

        } //end IF here to wait for next round of packets if buffer is full

        ad_converted[0] = 0;
        ad_converted[1] = 0;
        ad_converted[2] = 0;
        
        //if recieved_packets_counter is reached, update signal_scaling
        //it's out here to reset the ad_recieved more quickly
//        if (recieved_packets_counter >= 1000) {
        if (recieved_packets_counter >= 167) {
            spi_adapt_encoding();
            recieved_packets_counter = 0;
        }
    }
}


void spi_send_battery_status() {

        uint8_t ad_id = spi_ad_battery;
        //NRF_LOG_INFO("SPI battery reading.");
        //NRF_LOG_INFO("buffer0: %02x%02x%02x%02x%02x%02x", m_rx_buf[ad_id][0], m_rx_buf[ad_id][1], m_rx_buf[ad_id][2], m_rx_buf[ad_id][3], m_rx_buf[ad_id][4], m_rx_buf[ad_id][5]);
        //NRF_LOG_INFO("buffer1: %02x%02x%02x%02x%02x%02x", m_rx_buf[ad_id][6], m_rx_buf[ad_id][7], m_rx_buf[ad_id][8], m_rx_buf[ad_id][9], m_rx_buf[ad_id][10], m_rx_buf[ad_id][11]);

        spi_config_register.reg.battery_status = (uint16_t) (m_rx_buf[ad_id][5] << 8 | m_rx_buf[ad_id][4]);
//        NRF_LOG_INFO("S: %02x%02x%02x%02x", m_rx_buf[0][0], m_rx_buf[0][1], m_rx_buf[0][2], m_rx_buf[0][3]);
//        NRF_LOG_INFO("S: %02x%02x%02x%02x", m_rx_buf[0][4], m_rx_buf[0][5], m_rx_buf[0][6], m_rx_buf[0][7]);
    
        traum_battery_status_update(spi_traum_service, spi_config_register.send_data);

        //NRF_LOG_INFO("SPI battery send. %04x", spi_config_register.reg.battery_status);
        //NRF_LOG_INFO("SPI battery0 %02x%02x%02x%02x", spi_config_register.send_data[0], spi_config_register.send_data[1], spi_config_register.send_data[2], spi_config_register.send_data[3]);
        //NRF_LOG_INFO("SPI battery1 %02x%02x%02x%02x", spi_config_register.send_data[4], spi_config_register.send_data[5], spi_config_register.send_data[6], spi_config_register.send_data[7]);
        //NRF_LOG_FLUSH();

        battery_read = false;
}


/**
 * @brief Function to encode and write data to ring buffer
 */
void spi_encode_data(void)
{    

    //check for header. needs to happen before everything else!
    if (BLE_TRAUM_PACKAGE_HEADER_LENGTH) {
        stb_characteristic = (stb_characteristic + 2) > BLE_TRAUM_SAMPLES_PER_PACKAGE ? 0 : stb_characteristic + 1;
        if (stb_characteristic == 0) {
            stb_write_position  = (stb_write_position + BLE_TRAUM_PACKAGE_HEADER_LENGTH) % stb_buffer_length;
            stb_read_capacity  += BLE_TRAUM_PACKAGE_HEADER_LENGTH;
        }
    }

    //resetting the send buffer
    memset(&spi_send_buf[stb_write_position], 0, stb_packet_size_w);
    

    uint8_t c_bits_left = traum_bits_per_channel; //channel bits left
    uint8_t b_bits_left = 8; //byte bits left
    uint8_t byte_val = 0;
    uint8_t write_bits = 0;

    //go by channel then byte
    uint8_t n_byte = 0; //current byte
    int32_t c_value = 0;
    float32_t m_value = 0;
    float32_t s_value = 0;
    float32_t std_5 = 0;
    for(int n_channel = 0; n_channel < SPI_CHANNEL_NUMBER_TOTAL; n_channel++) { //current channel
        m_value = spi_filtered_values[n_channel] - (float32_t)spi_encoded_values[n_channel];
        std_5 = 5*spi_estimated_variance[n_channel];
        //average calculation (discarding outliers)
        if (spi_running_average_enabled) {
            if (abs(m_value - spi_estimated_average[n_channel]) > std_5) {
                if (m_value > spi_estimated_average[n_channel]) {
                    spi_estimated_average[n_channel] = spi_estimated_average[n_channel]*spi_enc_estimate_factor_9 + spi_enc_estimate_factor_1*std_5;
                } else {
                    spi_estimated_average[n_channel] = spi_estimated_average[n_channel]*spi_enc_estimate_factor_9 - spi_enc_estimate_factor_1*std_5;
                }
            } else {
                spi_estimated_average[n_channel] = spi_estimated_average[n_channel]*spi_enc_estimate_factor_9 + spi_enc_estimate_factor_1*m_value;
            }
            m_value = m_value - spi_estimated_average[n_channel];
        }
        s_value = m_value*m_value;
        //variance calculation (discarding outliers)
        if (s_value > std_5) {
            spi_estimated_variance[n_channel] = spi_estimated_variance[n_channel]*spi_enc_estimate_factor_5;
        } else {
            spi_estimated_variance[n_channel] = spi_estimated_variance[n_channel]*spi_enc_estimate_factor_9 + spi_enc_estimate_factor_1*s_value;
        }
        //bit shift
        c_value = (int32_t)m_value;
        c_value = c_value >> spi_encode_shift[n_channel];
        c_value = c_value > spi_max_difval ? spi_max_difval : (c_value < spi_min_difval ? spi_min_difval : c_value); //clipping to boarders
        
        //reconstruct encoded signal
        spi_encoded_values[n_channel] += c_value << spi_encode_shift[n_channel];

        if (debug_flag) {
          if (n_channel > 15) {
              c_value = (int32_t)(spi_filtered_values[n_channel-16] - (float32_t)spi_encoded_values[n_channel-16]);
              c_value = c_value >> spi_encode_shift[n_channel-16];
              c_value = c_value > spi_max_difval ? spi_max_difval : (c_value < spi_min_difval ? spi_min_difval : c_value); //clipping to boarders
          } else if (n_channel > 7) {
              c_value = (int32_t)(spi_estimated_average[n_channel-8]);
              c_value = c_value >> spi_encode_shift[n_channel-8];
              c_value = c_value > spi_max_difval ? spi_max_difval : (c_value < spi_min_difval ? spi_min_difval : c_value); //clipping to boarders
          }
        }


        //include channel value (c_value) in package
        c_bits_left = traum_bits_per_channel;
        while (c_bits_left) {
          write_bits = MIN(b_bits_left,c_bits_left);
          byte_val = (c_value & spi_ble_difval_mask) >> (c_bits_left - write_bits);
          byte_val <<= (b_bits_left - write_bits);
          spi_send_buf[stb_write_position + n_byte] |= byte_val;

          c_bits_left -= write_bits;
          b_bits_left -= write_bits;
          if (b_bits_left == 0) {
              n_byte++;
              b_bits_left = 8;
          }

        }
        if (n_byte > stb_packet_size_w) {
            NRF_LOG_INFO("encoding to much: %i", n_byte); //can not happen...
            break;
        }
    }
    
    //counter, when to send signal_scaling (encoding_shift) update
    recieved_packets_counter += 1;

    //update ringbuffer characteristics
    stb_write_position = (stb_write_position + stb_packet_size_w) % stb_buffer_length;
    stb_read_capacity += stb_packet_size_w;

    //NRF_LOG_INFO("ep: %i / %i (%i)", stb_read_position, stb_write_position, stb_packet_size_w);

}



/**
 * @brief Function to encode and write data to ring buffer
 */
void spi_adapt_encoding(void)
{    
    //resetting the send buffer
    memset(spi_code_send_buf, 0, CODE_CHAR_VALUE_LENGTH);

    float32_t required_bitrange;
    uint8_t odd = 0;
    
    //NRF_LOG_INFO("w-t: %i/%i\t\t,cw: %i\t\tcr: %i", stb_write_position, stb_read_position, stb_write_capacity, stb_read_capacity);


    for(int n_channel = 0; n_channel < SPI_CHANNEL_NUMBER_TOTAL; n_channel++) {
        required_bitrange = sqrtf(spi_estimated_variance[n_channel]);
        required_bitrange = log2f(required_bitrange*spi_enc_factor_safe_encoding);
        spi_encode_shift[n_channel] = (uint32_t) ceilf(required_bitrange - traum_bits_per_channel);
        spi_encode_shift[n_channel] = spi_encode_shift[n_channel] > spi_encode_max_shift ? spi_encode_max_shift : spi_encode_shift[n_channel];
        spi_encode_shift[n_channel] = spi_encode_shift[n_channel] < spi_encode_min_shift ? spi_encode_min_shift : spi_encode_shift[n_channel];
        
        //NRF_LOG_INFO("%i, ev: %i\t,rb: %i\tes: %i", n_channel, spi_estimated_variance[n_channel], required_bitrange, spi_encode_shift[n_channel]);
        
        if (odd) {
            spi_code_send_buf[n_channel/2] |= (0x0F & spi_encode_shift[n_channel]);
            odd--;
        } else {
            spi_code_send_buf[n_channel/2] |= (0x0F & spi_encode_shift[n_channel]) << 4;
            odd++;
        }


    }
    //packetSkipCounter
    spi_code_send_buf[12] = packetSkipCounter > 255 ? 255 : packetSkipCounter;
    buffer_usage_counter = buffer_usage_counter/(20*167);
    spi_code_send_buf[13] = buffer_usage_counter > 255 ? 255 : buffer_usage_counter;
    buffer_usage_counter = 0;
    packetSkipCounter = 0;

    //send packet
    if (traum_use_code_characteristic) {
//        traum_encoding_char_update(spi_traum_service, spi_code_send_buf);
        traum_code_characteristic_transmission_pending = stb_read_capacity / stb_packet_size_r; //number of packets currently pending
        traum_code_characteristic_transmission_pointer = spi_code_send_buf;
//        NRF_LOG_INFO("BLE enc new, %i, %i", stb_read_capacity, traum_code_characteristic_transmission_pending);
    }
    //reset counter
    recieved_packets_counter = 0;

    //check for warmup
    if (spi_enc_warmup == 1) {
        spi_enc_warmup = 0;
        spi_enc_estimate_factor_9 = SPI_ENC_EST_FACTOR_9;
        spi_enc_estimate_factor_1 = SPI_ENC_EST_FACTOR_1;
        spi_enc_estimate_factor_5 = SPI_ENC_EST_FACTOR_5;
    }

}


/**
 * @brief Function to see if there is new data available to send.
 * if yes, returns the id of the characteristic to send the data on
 */
int8_t spi_new_data(void)
{
    //if there is data available in spi_send_buffer
    if (stb_read_capacity >= stb_packet_size_r) {
        if (traum_use_only_one_characteristic) {
            return bits_per_channel; //send new data on characteristic x
        } else {
            return -1;//stb_characteristic;
        }
    } else {
        //NRF_LOG_INFO(" no new data to send");
        return -1; //no new data, don't send any
    }
}

/**
 * @brief Function to fetch the current pointer to the oldest encoded data.
 */
uint8_t* spi_get_data_pointer(void)
{    
    samplesDroppedCounter = samplesDroppedCounter > 15 ? 15 : samplesDroppedCounter;

    //fill in header
    spi_send_buf[stb_read_position] = 0;
    spi_send_buf[stb_read_position] |= (0x0F & packageID) << 4;
    spi_send_buf[stb_read_position] |= (0x0F & samplesDroppedCounter);

    //NRF_LOG_INFO("gh: %i, %i (%i/%i)", packageID, samplesDroppedCounter, stb_read_position, stb_write_position);

    return &spi_send_buf[stb_read_position];
}


//update buffer read pointer and capacity (package is now in BLE queue)
void spi_data_sent()
{
    stb_read_position = (stb_read_position + stb_packet_size_r) % stb_buffer_length;
    stb_read_capacity -= stb_packet_size_r;

    //stb_characteristic = (stb_characteristic + 1) % 3;//AD_NUMBER; //update characteristic to send on next

    //NRF_LOG_INFO("gp: %i, %i (%i)", packageID, samplesDroppedCounter, stb_read_position);

    //reset dropped counter and increment packageID
    samplesDroppedCounter = 0;
    packageID = packageID > 14 ? 0 : packageID+1;

    send_packets_counter += 1;
}


void spi_read_battery_status() {
        
        int ad_id = spi_ad_battery;
        
//    NRF_LOG_INFO("SPI BS 0.");
//    NRF_LOG_FLUSH();
        
        uint8_t tx_buf_len = 8;

        //writing in General User Config 2 to enable SAR read
        uint8_t tx_buf2[] = ADREG_GENERAL_USER_CONFIG_2; //len = 2
        tx_buf2[1] = tx_buf2[1] | ADREG_GENERAL_USER_CONFIG_2_BYTE_OR;

        bat_tx_buf[0] = tx_buf2[0];
        bat_tx_buf[1] = tx_buf2[1];
        

//    NRF_LOG_INFO("SPI BS 1.");
//    NRF_LOG_FLUSH();
        //writing in GLOBAL DIAGNOSTICS MUX REGISTER to set SAR to read out battery status
        uint8_t tx_buf_sar[] = ADREG_SAR_MUX; //len=2
        
        bat_tx_buf[2] = tx_buf_sar[0];
        bat_tx_buf[3] = tx_buf_sar[1];

//    NRF_LOG_INFO("SPI BS 2.");
//    NRF_LOG_FLUSH();
       // read battery status

        bat_tx_buf[4] = m_tx_buf[0];
        bat_tx_buf[5] = m_tx_buf[1];


//    NRF_LOG_INFO("SPI BS 3.");
//    NRF_LOG_FLUSH();

//turn back to measurements reading!!!!
        
        //disable SAR read
        tx_buf2[1] = tx_buf2[1] & ~ADREG_GENERAL_USER_CONFIG_2_BYTE_OR;

        bat_tx_buf[6] = tx_buf2[0];
        bat_tx_buf[7] = tx_buf2[1];


//    NRF_LOG_INFO("SPI BS 4.");
//    NRF_LOG_FLUSH();

       memset(m_rx_buf[ad_id], 0, tx_buf_len);
       nrf_drv_spi_transfer(&spi[ad_id], bat_tx_buf, tx_buf_len, m_rx_buf[ad_id], tx_buf_len);
       battery_read = true;

//    NRF_LOG_INFO("SPI BS 5.");
//    NRF_LOG_FLUSH();
}

void spi_config_update(const uint8_t* value_p) //it's 'const' because there is a warning otherwise
{
//    uint8_t value = value_p[0];
//    uint8_t value2 = value_p[1];

    //writing gain level to channel registers
    uint8_t cc = ADREG_CHANNEL_CONFIG_GAIN_MASK & (value_p[0] << 0);
    uint8_t tx_buf[] = {0x00, cc, 0x01, cc, 0x02, cc, 0x03, cc, 0x04, cc, 0x05, cc, 0x06, cc, 0x07, cc}; //len 16
    uint8_t tx_buf_len = 16;

    for(int i=0;i<AD_NUMBER;i++) { 
        uint32_t err_code = nrf_drv_spi_transfer(&spi[i], tx_buf, tx_buf_len, m_rx_buf[i], tx_buf_len);
        
//        NRF_LOG_INFO("spi conf 02.%i: %04x", i, err_code);
//        NRF_LOG_FLUSH();
//        APP_ERROR_CHECK(err_code);
    }

    //----------------
    //bits encoded per channel. can be 0, 1 or 2 (for 10, 14 or 16)
    bits_per_channel = (value_p[0] & 0x30) >> 4;
    bits_per_channel = bits_per_channel > 2 ? 0 : bits_per_channel;

    if (bits_per_channel == 0) {
        spi_send_buf        = spi_send_buf_10;
        stb_buffer_length   = stb_buffer_length_10; 
        stb_packet_size_w   = stb_packet_size_w_10;
        stb_packet_size_r   = stb_packet_size_r_10;
        stb_read_capacity_safety  = stb_read_capacity_safety_10;
        spi_max_difval        = (1 << (BLE_TRAUM_BASE_BITS_PER_CHANNEL_10 - 1)) - 1;
        spi_min_difval        = -1 << (BLE_TRAUM_BASE_BITS_PER_CHANNEL_10 - 1);
        spi_ble_difval_mask  = (1 << BLE_TRAUM_BASE_BITS_PER_CHANNEL_10) - 1;
        traum_bits_per_channel = BLE_TRAUM_BASE_BITS_PER_CHANNEL_10;
    } else if (bits_per_channel == 1) {
        spi_send_buf        = spi_send_buf_14;
        stb_buffer_length   = stb_buffer_length_14; 
        stb_packet_size_w   = stb_packet_size_w_14;
        stb_packet_size_r   = stb_packet_size_r_14;
        stb_read_capacity_safety  = stb_read_capacity_safety_14;
        spi_max_difval        = (1 << (BLE_TRAUM_BASE_BITS_PER_CHANNEL_14 - 1)) - 1;
        spi_min_difval        = -1 << (BLE_TRAUM_BASE_BITS_PER_CHANNEL_14 - 1);
        spi_ble_difval_mask  = (1 << BLE_TRAUM_BASE_BITS_PER_CHANNEL_14) - 1;
        traum_bits_per_channel = BLE_TRAUM_BASE_BITS_PER_CHANNEL_14;
    } else if (bits_per_channel == 2) {
        spi_send_buf        = spi_send_buf_16;
        stb_buffer_length   = stb_buffer_length_16; 
        stb_packet_size_w   = stb_packet_size_w_16;
        stb_packet_size_r   = stb_packet_size_r_16;
        stb_read_capacity_safety  = stb_read_capacity_safety_16;
        spi_max_difval        = (1 << (BLE_TRAUM_BASE_BITS_PER_CHANNEL_16 - 1)) - 1;
        spi_min_difval        = -1 << (BLE_TRAUM_BASE_BITS_PER_CHANNEL_16 - 1);
        spi_ble_difval_mask  = (1 << BLE_TRAUM_BASE_BITS_PER_CHANNEL_16) - 1;
        traum_bits_per_channel = BLE_TRAUM_BASE_BITS_PER_CHANNEL_16;
    }
    //----------------


    //running average
    spi_running_average_enabled = (value_p[0] & 0x08) >> 3;
    
    //send everything on first characteristic
    traum_use_only_one_characteristic = 1;//(value_p[0] & 0x04) >> 2;
    
    //data generation
    spi_data_gen_enabled = (value_p[0] & 0x02) >> 1;

    //debug
//    debug_flag = (value_p[0] & 0x01) >> 0;

    //transmission frequency. can be 3 or 2.
    spi_enc_packet_division = 3 - ((value_p[0] & 0x01) >> 0);

    

    //filter switching
    uint8_t hp_filter = (value_p[2] & 0x0F) >> 0;
    uint8_t lp_filter = (value_p[3] & 0xF0) >> 4;
    uint8_t notch_filter = (value_p[3] & 0x0F) >> 0;
    filter_init(hp_filter, lp_filter, notch_filter);
    
    //for the other filters, this happens in the init function
    spi_fo_hp_filter_enabled = (value_p[2] & 0xF0) >> 4;
    fo_hp_alpha = (value_p[2] & 0x10) ? fo_hp_alpha_0_5 : fo_hp_alpha_1_3;


    //min/max bitshift
    spi_encode_min_shift = (value_p[4] & 0xF0) >> 4;
    spi_encode_max_shift = (value_p[4] & 0x0F) >> 0;

    //bitshift encoding safety factor
    spi_enc_factor_safe_encoding = (value_p[5] & 0xF0) >> 4;


    NRF_LOG_INFO("SPI config updated.");
    NRF_LOG_FLUSH();     

    //write new config into config register
    for (int i = 0; i < 6; i++) {
        spi_config_register.send_data[i] = value_p[i];
    }

}


/**
 * @brief Function for configuring: PIN_IN pin for input, PIN_OUT pin for output,
 * and configures GPIOTE to give an interrupt on pin change.
 */
static void gpio_init(void)
{
    ret_code_t err_code;

    if (!nrf_drv_gpiote_is_init()) //check if gpiote is already initialized
    {
        err_code = nrf_drv_gpiote_init(); //if not, do so
        APP_ERROR_CHECK(err_code);
    }

    
    //seting trigger for \DRDY-signal from AD

    nrf_drv_gpiote_in_config_t in_config = GPIOTE_CONFIG_IN_SENSE_HITOLO(true);
  //  in_config.pull = NRF_GPIO_PIN_PULLUP;
    
    //set spi triggers
    nrf_drv_gpiote_in_init(SPI_0_DRDY, &in_config, spi_trigger_pin_handler);
    nrf_drv_gpiote_in_init(SPI_1_DRDY, &in_config, spi_trigger_pin_handler);
    nrf_drv_gpiote_in_init(SPI_2_DRDY, &in_config, spi_trigger_pin_handler);

    //enable spi triggers
    nrf_drv_gpiote_in_event_enable(SPI_0_DRDY, true);
    nrf_drv_gpiote_in_event_enable(SPI_1_DRDY, true);
    nrf_drv_gpiote_in_event_enable(SPI_2_DRDY, true);
}


/**
 */
void filter_init(uint8_t highpass, uint8_t lowpass, uint8_t notch)
{
    

    //switching filters
    spi_highpass_filter_enabled = 1;
    spi_lowpass_filter_enabled = 1;
    spi_iir_filter_enabled = 1;


    //todo# do we need to realloc the filter states??? currently they are just defined as max size
    uint8_t numstages;

    //notch filter
    float32_t* iir_coeffs;
    notch = notch > 4 ? 0 : notch;
    switch(notch){
        case 0:
          spi_iir_filter_enabled = 0;
          break;
        case 1:
          iir_coeffs = m_biquad_coeffs_48_o4;
          numstages = 2;
          break;
        case 2:
          iir_coeffs = m_biquad_coeffs_48_o6;
          numstages = 3;
          break;
        case 3:
          iir_coeffs = m_biquad_coeffs_46_o4;
          numstages = 2;
          break;
        case 4:
          iir_coeffs = m_biquad_coeffs_46_o6;
          numstages = 3;
          break;
        default:
          spi_iir_filter_enabled = 0;
          NRF_LOG_INFO("SPI init filter error!");
          break;
    }
    if (spi_iir_filter_enabled) {
        for(int i = 0; i < SPI_CHANNEL_NUMBER_TOTAL;i++) {
            arm_biquad_cascade_df2T_init_f32(&iir_instance[i], numstages, iir_coeffs, m_biquad_state[i]);
        }
    }
//    for(int i = 0; i < 10;i++) {
//        NRF_LOG_INFO("fc: " NRF_LOG_FLOAT_MARKER " f", NRF_LOG_FLOAT(iir_coeffs[i]));
//    }
        
    //low pass filter
    float32_t* lp_coeffs;
    lowpass = lowpass > 2 ? 0 : lowpass;
    switch(lowpass){
        case 0:
          spi_lowpass_filter_enabled = 0;
          break;
        case 1:
          lp_coeffs = m_lowpass_coeffs_o4;
          numstages = 2;
          break;
        case 2:
          lp_coeffs = m_lowpass_coeffs_o6;
          numstages = 3;
          break;
        default:
          spi_lowpass_filter_enabled = 0;
          NRF_LOG_INFO("SPI init filter error!");
          break;
    }
    if (spi_lowpass_filter_enabled) {
        for(int i = 0; i < SPI_CHANNEL_NUMBER_TOTAL;i++) {
            arm_biquad_cascade_df2T_init_f32(&lowpass_instance[i], numstages, lp_coeffs, m_lowpass_state[i]);
        }
    }
    
//    NRF_LOG_INFO("lp filter: ns: %i", lowpass_instance[0].numStages);
//    NRF_LOG_INFO("lp: " NRF_LOG_FLOAT_MARKER " f", NRF_LOG_FLOAT(lowpass_instance[0].pCoeffs[0]*10000));
//    for(int i = 0; i < 10;i++) {
//        NRF_LOG_INFO("lp: " NRF_LOG_FLOAT_MARKER " f", NRF_LOG_FLOAT(lp_coeffs[i]*10000));
//    }
//    nrf_gpio_pin_write(20, 0);

    //high pass filter
        
    //low pass filter
    float32_t* hp_coeffs;
    //selecting appropriate filter set, depending on transmission frequency
    float32_t** hp_coeffs_source = spi_enc_packet_division == 2 ? m_highpass_coeffs_250hz : m_highpass_coeffs_167hz;
    highpass = highpass > 4 ? 0 : highpass;
    switch(highpass){
        case 0:
          spi_highpass_filter_enabled = 0;
          break;
        case 1:
          hp_coeffs = hp_coeffs_source[0];
          numstages = 2;
          break;
        case 2:
          hp_coeffs = hp_coeffs_source[1];
          numstages = 2;
          break;
        case 3:
          hp_coeffs = hp_coeffs_source[2];
          numstages = 1;
          break;
        case 4:
          hp_coeffs = hp_coeffs_source[3];
          numstages = 1;
          break;
        default:
          spi_highpass_filter_enabled = 0;
          NRF_LOG_INFO("SPI init filter error!");
          break;
    }
    if (spi_highpass_filter_enabled) {
        for(int i = 0; i < SPI_CHANNEL_NUMBER_TOTAL;i++) {
            arm_biquad_cascade_df2T_init_f32(&highpass_instance[i], numstages, hp_coeffs, m_highpass_state[i]);
        }
    }

    nrf_gpio_pin_write(18, 0);
    nrf_delay_ms(500);
    if (spi_iir_filter_enabled) nrf_gpio_pin_write(18, 1);
//    nrf_gpio_pin_write(18, 1);
}


void spi_init(void)
{

    NRF_LOG_INFO("timer init");
    NRF_LOG_FLUSH();

    
    // Initialize timer module.
    APP_ERROR_CHECK(app_timer_create(&m_spi_char_timer_id, APP_TIMER_MODE_REPEATED, spi_timer_timeout_handler));
    ret_code_t err_code = app_timer_start(m_spi_char_timer_id, SPI_CHAR_TIMER_INTERVAL, NULL);
    APP_ERROR_CHECK(err_code);


      
    //NRF_LOG_INFO("SPI init for write General User Register.");
    //NRF_LOG_FLUSH();
    
    for(int i=0;i<AD_NUMBER;i++) {

        NRF_LOG_INFO("Initialising SPI%i", i);

        //setting Settings in AD
        //SPI Interface setup
        nrf_drv_spi_config_t spi_config = NRF_DRV_SPI_DEFAULT_CONFIG;
        if (i == 0) {
            spi_config.ss_pin   = SPI_0_CS;
            spi_config.miso_pin = SPI_0_SDO;
            spi_config.mosi_pin = SPI_0_SDI;
            spi_config.sck_pin  = SPI_0_SCLK;
        } else if (i == 1) {
            spi_config.ss_pin   = SPI_1_CS;
            spi_config.miso_pin = SPI_1_SDO;
            spi_config.mosi_pin = SPI_1_SDI;
            spi_config.sck_pin  = SPI_1_SCLK;
        } else if (i == 2) {
            spi_config.ss_pin   = SPI_2_CS;
            spi_config.miso_pin = SPI_2_SDO;
            spi_config.mosi_pin = SPI_2_SDI;
            spi_config.sck_pin  = SPI_2_SCLK;
        }
        spi_config.frequency = SPI_FREQUENCY_FREQUENCY_M8;
        APP_ERROR_CHECK(nrf_drv_spi_init(&spi[i], &spi_config, NULL, NULL));


        //writing in SRC Config to set AD ORD to 250Hz
        uint8_t tx_buf_src[] = ADREG_DECIMATION_RATE_N_500; //len=4
        uint8_t tx_buf_len = 4;
        APP_ERROR_CHECK(nrf_drv_spi_transfer(&spi[i], tx_buf_src, tx_buf_len, m_rx_buf[i], tx_buf_len));

        //writing in SRC Update to update AD ORD to 250Hz
        uint8_t tx_buf_src_u[] = ADREG_SRC_MODE_UPDATE_SET; //len=2
        tx_buf_len = 2;
        APP_ERROR_CHECK(nrf_drv_spi_transfer(&spi[i], tx_buf_src_u, tx_buf_len, m_rx_buf[i], tx_buf_len));
        nrf_delay_ms(20); //this should be 'at least two MCLK periods'
        uint8_t tx_buf_src_c[] = ADREG_SRC_MODE_UPDATE_CLEAR; //len=2
        tx_buf_len = 2;
        APP_ERROR_CHECK(nrf_drv_spi_transfer(&spi[i], tx_buf_src_c, tx_buf_len, m_rx_buf[i], tx_buf_len));

        //add GUC1 change
        //writing in General User Config 1 to enable internal reference
        uint8_t tx_buf1[] = ADREG_GENERAL_USER_CONFIG_1; //len = 2
        tx_buf_len = 2;
        //NRF_LOG_INFO(" R_tx: %04x", *(uint16_t*) tx_buf1); 
        //NRF_LOG_FLUSH(); 
        tx_buf1[1] = tx_buf1[1] | ADREG_GENERAL_USER_CONFIG_1_BYTE_OR; //##commented out while testing    
        //NRF_LOG_INFO(" R_tx: %04x", *(uint16_t*) tx_buf1);
        APP_ERROR_CHECK(nrf_drv_spi_transfer(&spi[i], tx_buf1, tx_buf_len, m_rx_buf[i], tx_buf_len));

        //writing in General User Config to enable AD for data output via SPI
        uint8_t tx_buf[] = ADREG_GENERAL_USER_CONFIG_3; //len = 2
        tx_buf_len = 2;
        //NRF_LOG_INFO(" R_tx: %04x", *(uint16_t*) tx_buf); 
        //NRF_LOG_FLUSH(); 
        tx_buf[1] = tx_buf[1] | ADREG_GENERAL_USER_CONFIG_3_BYTE_OR; //##commented out while testing    
        //NRF_LOG_INFO(" R_tx: %04x", *(uint16_t*) tx_buf);
        APP_ERROR_CHECK(nrf_drv_spi_transfer(&spi[i], tx_buf, tx_buf_len, m_rx_buf[i], tx_buf_len));
   
        nrf_delay_ms(200);
        nrf_drv_spi_uninit(&spi[i]) ; //need to uninititialize because otherwise reinitialisation crashes
        //NRF_LOG_INFO("SPI init2.");
        //NRF_LOG_FLUSH();
    
        //initializing SPI for cyclic read operation
        //the pointer to spi_instances is needed to distinguish, which AD send the trigger
        APP_ERROR_CHECK(nrf_drv_spi_init(&spi[i], &spi_config, spi_event_handler, &spi_instances_id[i]));
    }

    //init GPIO and setting trigger for \DRDY-signal
    gpio_init();
  
    //init filters
    filter_init(1, 1, 1); //default
     
    //NRF_LOG_INFO(" len service, write: %i, %i, %08x", TRAUM_SERVICE_VALUE_LENGTH, SPI_BLE_BUFFER_WTRITE_LENGTH, spi_ble_difval_mask);

    NRF_LOG_INFO("SPI init fin.");
    NRF_LOG_FLUSH();
}
