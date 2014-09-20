/*
 * Teensy 3.1 Digital Setting Circles
 * Copyright (C) 2014 Aaron Turner
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

#include <Arduino.h>
#include <Streaming.h>
#define ENCODER_OPTIMIZE_INTERRUPTS
#include <Encoder.h>
#include <AnySerial.h>
#include <WiFlySerial.h>
#include <Flash.h>
#include <MsTimer2.h>

#include "defaults.h"
#include "teensy_dsc.h"
#include "wifly.h"
#include "utils.h"
#include "cli.h"
#include "defaults.h"

/*
 * Init our Encoders & Serial ports
 */
Encoder EncoderRA(CHAN_A_RA, CHAN_B_RA);
Encoder EncoderDEC(CHAN_A_DEC, CHAN_B_DEC);

AnySerial UserSerial;
AnySerial WiFlySerialPort;
WiFlySerial WiFly(WiFlySerialPort);

/* our global contexts */
cli_ctx *uctx, *wctx;
common_cli_ctx *common;

/* updates the current encoder value */
void
update_encoders() {
    static int blink = 0;
    common->ra_value = EncoderRA.read();
    common->dec_value = EncoderDEC.read();
    blink = blink ? 0 : 1;
    digitalWrite(13, blink);
}

void
setup() {
    pinMode(WIFLY_RESET, INPUT);
    pinMode(13, OUTPUT);
    encoder_settings_t *encoders;

    // load the encoder settings stored in the EEPROM
    encoders = get_encoder_settings();
    common = (common_cli_ctx *)malloc(sizeof(common_cli_ctx));
    bzero(common, sizeof(common_cli_ctx));
    common->ra_cps = encoders->ra_cps;
    common->dec_cps = encoders->dec_cps;

    // Init the USB Serial port & context
    UserSerial.attach(&USER_SERIAL_PORT);
    UserSerial.begin(USER_SERIAL_BAUD);
    delay(1000);
    uctx = cli_init_cmd(&UserSerial, &common);

    MsTimer2::start();
    MsTimer2::set(UPDATE_ENCODER_MS, update_encoders);

    // Init the WiFly
//    delay(WIFLY_DELAY);   // Wait for it to come up
    // load the encoder settings stored in the EEPROM
//    WiFlySerialPort.attach(&SerialWiFly);
//    wctx = cli_init_cmd(&WiFlySerialPort, common);

#if DEBUG
    /*
     * With Teensy 3.1 it runs so fast that initial write()'s
     * to the Serial port are lost.  Hence add this delay so 
     * we will be able to read startup debugging messages
     */
    delay(1000);
#endif
}

void
loop() {
    char byte;
    if (UserSerial.available() > 0) {
        process_cmd(uctx);
    }
    /*
    if (SerialWiFly.available() > 0) {
        process_cmd(wctx);
    }
    */
}

/*
 * Reads from *serial and trys to execute the given command.
 * 
 * Returns a cmd_status based on if we ran a command
 */
cmd_status
process_cmd(cli_ctx *ctx) {
    AnySerial *serial = ctx->serial;
    static char read_buff[READBUFF_SIZE];
    static size_t pos = 0;
    char temp_buff[READBUFF_SIZE], byte;
    size_t i, len;
    cmd_status status;

    // Short cut for one char commands
    if (pos == 0) {
        i = 0;
        byte = serial->peek();
        while (ctx->one_char_cmds[i] != NULL) {
            if (byte == ctx->one_char_cmds[i]) {
                read_buff[0] = byte;
                read_buff[1] = NULL;
                status = cli_proc_cmd(ctx, read_buff, 1);
                serial->read(); // consume the byte
                read_buff[0] = NULL;
                return status;
            }
            i++;
        }
    }

    /*
     * Command is multiple bytes followed by '\r'
     */
    len = serial->readBytesUntil('\r', temp_buff, READBUFF_SIZE);
    if (((strlen(read_buff) + len) > READBUFF_SIZE) ||
        (len == READBUFF_SIZE)) {
        // Crap, someone is just sending us crap.  Just eat it.
        pos = 0;
        read_buff[0] = NULL;
        return E_CMD_TOO_LONG;
    }

    // append the last bytes to any bytes we've read before
    strcat(read_buff, temp_buff);
    pos = strlen(read_buff);


    // trim any whitespace on the end
    if (IS_WORD_END(read_buff[len-1])) {
        while (IS_WORD_END(read_buff[len-1])) {
            read_buff[len-1] = NULL;
        }
    } 

    if (strlen(read_buff) == 0) {
        return E_CMD_TOO_SHORT;
    }

    // At this point we should have a whole command.  Go process it!
    status = cli_proc_cmd(ctx, read_buff, strlen(read_buff));

    switch (status) {
        case E_CMD_TOO_LONG:
        case E_CMD_NOT_FOUND:
        case E_CMD_BAD_ARGS:
#ifdef DEBUG
            serial->printf("ERR [%d]: %s\r", (int *)&status, read_buff);
#endif
            // fall through
            pos = 0;
            read_buff[0] = NULL;
            break;
        case E_CMD_OK:
            // Eat what we've been given and return
            pos = 0;
            read_buff[0] = NULL;
            break;
        case E_CMD_TOO_SHORT:
            // Keep the buffer for next time 
            break;
    }
    return status;
}


/*
 * Resets the WiFly
 */
void
reset_wifly() {
    UserSerial.write("\nResetting the Xbee...  ");
    pinMode(WIFLY_RESET, OUTPUT);
    digitalWrite(WIFLY_RESET, LOW);
    delay(100);
    pinMode(WIFLY_RESET, INPUT);
    UserSerial.write("Done!\n");
}

/*
 * Takes the hard coded settings (see defaults.h) and
 * writes them to the EEPROM where they will be read
 * the next time we power on
 */
void
reset_all_settings() {
    network_settings_t network;
    serial_settings_t serial;
    encoder_settings_t encoders;

    strcpy(network.ip_address, IP_ADDRESS);
    strcpy(network.netmask, NETMASK);
    strcpy(network.ssid, SSID);
    strcpy(network.passphrase, WPA_PASSWORD);
    network.enable_wpa = ENABLE_WPA;
    network.port = PORT;
    network.channel = WIFLY_CHANNEL;
    network.rate = WIFLY_RATE;
    network.tx_power = TX_POWER;
    set_network_defaults(&network);

    serial.baud = SERIAL_A_BAUD;
    set_serial_defaults(SERIAL_A, &serial);
    serial.baud = SERIAL_B_BAUD;
    set_serial_defaults(SERIAL_B, &serial);

    encoders.ra_cps = RA_ENCODER_CPS;
    encoders.dec_cps = DEC_ENCODER_CPS;
    set_encoder_settings(&encoders);
}
