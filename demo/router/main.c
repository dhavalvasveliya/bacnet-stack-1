/**
* @file
* @author Andriy Sukhynyuk, Vasyl Tkhir, Andriy Ivasiv
* @date 2012
* @brief BACnet/IP to MS/TP Router example application.
* The Router connects two or more BACnet/IP and BACnet MS/TP networks.
* Number of netwoks is limited only by available hardware communication
* devices (or ports for Ethernet).
*
* @section LICENSE
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*
*/
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>       /* for time */
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>     /* for getopt */
#include <termios.h>    /* used in kbhit() */
#include <getopt.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <pthread.h>
#include <termios.h>
#include "msgqueue.h"
#include "portthread.h"
#include "network_layer.h"
#include "ipmodule.h"
#include "mstpmodule.h"

#define KEY_ESC 27

ROUTER_PORT *head = NULL;       /* pointer to list of router ports */

int port_count;

void print_help(
    );

bool read_config(
    char *filepath);

bool parse_cmd(
    int argc,
    char *argv[]);

void init_port_threads(
    ROUTER_PORT * port_list);

bool init_router(
    );

void cleanup(
    );

void print_msg(
    BACMSG * msg);

uint16_t process_msg(
    BACMSG * msg,
    MSG_DATA * data,
    uint8_t ** buff);

uint16_t get_next_free_dnet(
    );

int kbhit(
    );

inline bool is_network_msg(
    BACMSG * msg);

int main(
    int argc,
    char *argv[])
{
    ROUTER_PORT *port;
    BACMSG msg_storage, *bacmsg = NULL;
    MSG_DATA *msg_data = NULL;
    uint8_t *buff = NULL;
    int16_t buff_len = 0;

    atexit(cleanup);

    if (!parse_cmd(argc, argv)) {
        printf("parse cmd failed\r\n");
        return -1;
    }

    if (!init_router()) {
        printf("init_router failed\r\n");
        return -1;
    }


    send_network_message(NETWORK_MESSAGE_I_AM_ROUTER_TO_NETWORK, msg_data,
        &buff, NULL);

    while (true) {
        if (kbhit()) {
            char ch = getchar();
            if (ch == KEY_ESC) {
                PRINT(INFO, "Received shutdown. Exiting...\n");
                break;
            }
        }
          // blocking dequeue here
        bacmsg = recv_from_msgbox(head->main_id, &msg_storage, 0);
        if (bacmsg) {
            switch (bacmsg->type) {
                case DATA:
                    {
                        MSGBOX_ID msg_src = bacmsg->origin;

                        /* allocate message structure */
                        msg_data = malloc(sizeof(MSG_DATA));
                        if (!msg_data) {
                            PRINT(ERROR, "Error: Could not allocate memory\n");
                            break;
                        }

                        //print_msg(bacmsg);

                        if (is_network_msg(bacmsg)) {
                            buff_len =
                                process_network_message(bacmsg, msg_data,
                                &buff);
                            if (buff_len == 0) {
                                free_data(bacmsg->data);
                                break;
                            }
                        } else {
                            buff_len = process_msg(bacmsg, msg_data, &buff);
                        }

                        /* if buff_len */
                        /* >0 - form new message and send */
                        /* =-1 - try to find next router */
                        /* other value - discard message */

                        if (buff_len > 0) {
                            /* form new message */
                            msg_data->pdu = buff;
                            msg_data->pdu_len = buff_len;
                            msg_storage.origin = head->main_id;
                            msg_storage.type = DATA;
                            msg_storage.data = msg_data;

                            //print_msg(bacmsg);

                            if (is_network_msg(bacmsg)) {
                                msg_data->ref_count = 1;
                                send_to_msgbox(msg_src, &msg_storage);
                            } else if (msg_data->dest.net !=
                                BACNET_BROADCAST_NETWORK) {
                                msg_data->ref_count = 1;
                                port =
                                    find_dnet(msg_data->dest.net,
                                    &msg_data->dest);
                                send_to_msgbox(port->port_id, &msg_storage);
                            } else {
                                port = head;
                                msg_data->ref_count = port_count - 1;
                                while (port != NULL) {
                                    if (port->port_id == msg_src ||
                                        port->state == FINISHED) {
                                        port = port->next;
                                        continue;
                                    }
                                    send_to_msgbox(port->port_id,
                                        &msg_storage);
                                    port = port->next;
                                }
                            }
                        } else if (buff_len == -1) {
                            uint16_t net = msg_data->dest.net;  /* NET to find */
                            PRINT(INFO, "Searching NET...\n");
                            send_network_message
                                (NETWORK_MESSAGE_WHO_IS_ROUTER_TO_NETWORK,
                                msg_data, &buff, &net);
                        } else {
                            /* if invalid message send Reject-Message-To-Network */
                            PRINT(ERROR, "Error: Invalid message\n");
                            free_data(msg_data);
                        }
                    }
                    break;
                case SERVICE:
                default:
                    break;
            }
        }
    }

    return 0;

}

void print_help(
    )
{
    printf("Usage: router <init_method> [init_parameters]\n" "\ninit_method:\n"
        "-c, --config <filepath>\n\tinitialize router with a configuration file (.cfg) located at <filepath>\n"
        "-D, --device <dev_type> [params]\n\tinitialize a <dev_type> device specified with\n\t[params]\n"
        "\ninit_parameters:\n"
        "-i, --interface <iface>\n\tusing an <iface> interface\n"
        "-n, --network <net>\n\tspecify device network number\n"
        "-P, --port <port>\n\tspecify udp port for BIP device\n"
        "-m, --mac <mac_address> [max_master] [max_frames]\n\tspecify MSTP port parameters\n"
        "-b, --baud <baud>\n\tspecify MSTP port baud rate\n"
        "-p, --parity <None|Even|Odd>\n\tspecify MSTP port parity\n"
        "-d, --databits <5|6|7|8>\n\tspecify MSTP port databits\n"
        "-s, --stopbits <1|2>\n\tspecify MSTP port stopbits\n");
}

bool parse_cmd(
    int argc,
    char *argv[])
{
    const char *optString = "hD:";
    const char *bipString = "p:n:D:i:";
    const char *mstpString = "m:b:p:d:s:n:D:i:";
    const struct option Options[] = {
        {"device", required_argument, NULL, 'D'},
        {"interface", required_argument, NULL, 'i'},
        {"network", required_argument, NULL, 'n'},
        {"port", required_argument, NULL, 'P'},
        {"mac", required_argument, NULL, 'm'},
        {"baud", required_argument, NULL, 'b'},
        {"parity", required_argument, NULL, 'p'},
        {"databits", required_argument, NULL, 'd'},
        {"stopbits", required_argument, NULL, 's'},
        {"help", no_argument, NULL, 'h'},
        {NULL, no_argument, NULL, 0},
    };

    int opt, dev_opt, index, result, fd;
    ROUTER_PORT *current = head;

    if (argc < 2)
        print_help();

    /* begin checking cmd parameters */
    opt = getopt_long(argc, argv, optString, Options, &index);
    printf("opt = %c\r\n", opt);
    while (opt != -1) {
        switch (opt) {
            case 'h':
                print_help();
                return false;
                break;
            case 'D':

                /* create new list node to store port information */
                if (head == NULL) {
                    head = (ROUTER_PORT *)calloc(sizeof(ROUTER_PORT), 1);
                    head->next = NULL;
                    current = head;
                } else {
                    ROUTER_PORT *tmp = current;
                    current = current->next;
                    head = (ROUTER_PORT *)calloc(sizeof(ROUTER_PORT), 1);
                    current->next = NULL;
                    tmp->next = current;
                }

                port_count++;
                PRINT(ERROR,
                   "Info: Port %i\n",port_count);
                if (strcmp(optarg, "bip") == 0) {
                    current->type = BIP;

                    /* setup default parameters */
                    current->iface = "eth0";
                    current->params.bip_params.port = 0xBAC0;   /* 47808 */
                    current->route_info.net = get_next_free_dnet();

                    dev_opt =
                        getopt_long(argc, argv, bipString, Options, &index);
                    while (dev_opt != -1 && dev_opt != 'D') {
                        switch (dev_opt) {
                           case 'i':
                                PRINT(ERROR,"info: argc %s\n",optarg);
                                current->iface = optarg;
                                /* check if interface is valid */
                                fd = socket(AF_INET, SOCK_DGRAM, 0);
                                if (fd) {
                                    struct ifreq ifr;
                                    strncpy(ifr.ifr_name, current->iface,
                                        sizeof(ifr.ifr_name) - 1);
                                    result = ioctl(fd, SIOCGIFADDR, &ifr);
                                    if (result != -1) {
                                        close(fd);
                                    } else {
                                        PRINT(ERROR,
                                            "Error: Invalid interface for BIP device \n");
                                        return false;
                                    }
                                }
                            case 'P':
                                result = atoi(optarg);
                                if (result) {
                                    current->params.bip_params.port =
                                        (uint16_t) result;
                                } else {
                                    current->params.bip_params.port = 0xBAC0;   /* 47808 */
                                }
                                break;
                            case 'n':
                                result = atoi(optarg);
                                if (result) {
                                    current->route_info.net =
                                        (uint16_t) result;
                                } else {
                                    current->route_info.net = port_count;
                                }
                                break;
                        }
                        dev_opt =
                            getopt_long(argc, argv, bipString, Options,
                            &index);
                    }
                    opt = dev_opt;
                } else if (strcmp(optarg, "mstp") == 0) {
                    current->type = MSTP;

                    /* setup default parameters */
                    current->iface = "/dev/ttyUSB0";
                    current->route_info.mac[0] = 127;
                    current->route_info.mac_len = 1;
                    current->params.mstp_params.max_master = 127;
                    current->params.mstp_params.max_frames = 1;
                    current->params.mstp_params.baudrate = 9600;
                    current->params.mstp_params.parity = PARITY_NONE;
                    current->params.mstp_params.databits = 8;
                    current->params.mstp_params.stopbits = 1;
                    current->route_info.net = get_next_free_dnet();

                    dev_opt =
                        getopt_long(argc, argv, mstpString, Options, &index);
                    while (dev_opt != -1 && dev_opt != 'D') {
                        switch (dev_opt) {
                            case 'i':
                                PRINT(ERROR,"info: argc %s\n",optarg);
                                current->iface = optarg;
                                /* check if interface is valid */
                                fd = open(current->iface, O_NOCTTY | O_NONBLOCK);
                                if (fd != -1) {
                                    close(fd);
                                } else {
                                    PRINT(ERROR,
                                        "Error: Invalid interface for MSTP device\n");
                                    return false;
                                }
                            case 'm':
                                result = atoi(optarg);
                                if (result) {
                                    current->route_info.mac[0] =
                                        (uint8_t) result;
                                }
                                if (argv[optind][0] != '-') {
                                    current->params.mstp_params.max_master =
                                        (uint8_t) atoi(argv[optind]);
                                    if (current->params.mstp_params.
                                        max_master <
                                        current->route_info.mac[0])
                                        current->params.mstp_params.
                                            max_master =
                                            current->route_info.mac[0];

                                    if (argv[optind + 1][0] != '-') {
                                        current->params.mstp_params.
                                            max_frames =
                                            (uint8_t) atoi(argv[optind + 1]);
                                    }
                                }
                                break;
                            case 'b':
                                result = atoi(optarg);
                                if (result) {
                                    current->params.mstp_params.baudrate =
                                        (uint32_t) result;
                                }
                                break;
                            case 'p':
                                switch (optarg[0]) {
                                    case 'E':
                                        current->params.mstp_params.parity =
                                            PARITY_EVEN;
                                        break;
                                    case 'O':
                                        current->params.mstp_params.parity =
                                            PARITY_ODD;
                                        break;
                                    default:
                                        current->params.mstp_params.parity =
                                            PARITY_NONE;
                                        break;
                                }
                                break;
                            case 'd':
                                result = atoi(optarg);
                                if (result >= 5 && result <= 8) {
                                    current->params.mstp_params.databits =
                                        (uint8_t) result;
                                }
                                break;
                            case 's':
                                result = atoi(optarg);
                                if (result >= 1 && result <= 2) {
                                    current->params.mstp_params.stopbits =
                                        (uint8_t) result;
                                }
                                break;
                            case 'n':
                                result = atoi(optarg);
                                if (result) {
                                    current->route_info.net =
                                        (uint16_t) result;
                                }
                                break;
                        }
                        dev_opt =
                            getopt_long(argc, argv, mstpString, Options,
                            &index);
                    }
                    opt = dev_opt;
                } else {
                    PRINT(ERROR, "Error: %s unknown\n", optarg);
                    return false;
                }
                break;
        }
    }
    return true;
}

void init_port_threads(
    ROUTER_PORT * port_list)
{
    ROUTER_PORT *port = port_list;
    pthread_t *thread;

    while (port != NULL) {
        switch (port->type) {
            case BIP:
                port->func = &dl_ip_thread;
                break;
            case MSTP:
                port->func = &dl_mstp_thread;
                break;
        }

        port->state = INIT;
        thread = (pthread_t *) malloc(sizeof(pthread_t));
        pthread_create(thread, NULL, port->func, port);

        pthread_detach(*thread);        /* for proper thread termination */

        port = port->next;
    }
}

bool init_router(
    )
{
    MSGBOX_ID msgboxid;
    ROUTER_PORT *port;

    msgboxid = create_msgbox();
    if (msgboxid == INVALID_MSGBOX_ID)
        return false;

    port = head;
    /* add main message box id to all ports */
    while (port != NULL) {
        port->main_id = msgboxid;
        port = port->next;
    }

    init_port_threads(head);

    /* wait for port initialization */
    port = head;
    while (port != NULL) {
        if (port->state == RUNNING) {
            port = port->next;
            continue;
        } else if (port->state == INIT_FAILED) {
            PRINT(ERROR, "Error: Failed to initialize %s\n", port->iface);
            return false;
        } else {
            PRINT(INFO, "Initializing...\n");
            sleep(1);
            continue;
        }
    }

    return true;
}

void cleanup(
    )
{
    ROUTER_PORT *port;
    BACMSG msg;

    if (head == NULL)
        return;

    msg.origin = head->main_id;
    msg.type = SERVICE;
    msg.subtype = SHUTDOWN;

    del_msgbox(head->main_id);  /* close routers message box */

    /* send shutdown message to all router ports */
    port = head;
    while (port != NULL) {
        if (port->state == RUNNING)
            send_to_msgbox(port->port_id, &msg);
        port = port->next;
    }

    port = head;
    while (port != NULL) {
        if (port->state == FINISHED) {
            cleanup_dnets(port->route_info.dnets);
            port = port->next;
            free(head->iface);
            free(head);
            head = port;
        }
    }

    pthread_mutex_destroy(&msg_lock);
}

void print_msg(
    BACMSG * msg)
{
    if (msg->type == DATA) {
        int i;
        MSG_DATA *data = (MSG_DATA *) msg->data;

        if (data->pdu_len) {
            PRINT(DEBUG, "Message PDU: ");
            for (i = 0; i < data->pdu_len; i++)
                PRINT(DEBUG, "%02X ", data->pdu[i]);
            PRINT(DEBUG, "\n");
        }
    }
}

uint16_t process_msg(
    BACMSG * msg,
    MSG_DATA * data,
    uint8_t ** buff)
{

    BACNET_ADDRESS addr;
    BACNET_NPDU_DATA npdu_data;
    ROUTER_PORT *srcport;
    ROUTER_PORT *destport;
    uint8_t npdu[MAX_NPDU];
    int16_t buff_len = 0;
    int apdu_offset;
    int apdu_len;
    int npdu_len;

    memmove(data, msg->data, sizeof(MSG_DATA));

    apdu_offset = npdu_decode(data->pdu, &data->dest, &addr, &npdu_data);
    apdu_len = data->pdu_len - apdu_offset;

    srcport = find_snet(msg->origin);
    destport = find_dnet(data->dest.net, NULL);
    assert(srcport);

    if (srcport && destport) {
        data->src.net = srcport->route_info.net;

        /* if received from another router save real source address (not other router source address) */
        if (addr.net > 0 && addr.net < BACNET_BROADCAST_NETWORK &&
            data->src.net != addr.net)
            memmove(&data->src, &addr, sizeof(BACNET_ADDRESS));

        /* encode both source and destination for broadcast and router-to-router communication */
        if (data->dest.net == BACNET_BROADCAST_NETWORK ||
            destport->route_info.net != data->dest.net) {
            npdu_len =
                npdu_encode_pdu(npdu, &data->dest, &data->src, &npdu_data);
        } else {
            npdu_len = npdu_encode_pdu(npdu, NULL, &data->src, &npdu_data);
        }

        buff_len = npdu_len + data->pdu_len - apdu_offset;

        *buff = (uint8_t *) malloc(buff_len);
        memmove(*buff, npdu, npdu_len); /* copy newly formed NPDU */
        memmove(*buff + npdu_len, &data->pdu[apdu_offset], apdu_len);   /* copy APDU */

    } else {
        /* request net search */
        return -1;
    }

    /* delete received message */
    free_data((MSG_DATA *) msg->data);

    return buff_len;
}

int kbhit(
    )
{
    static const int STDIN = 0;
    static bool initialized = false;

    if (!initialized) {
        /* use termios to turn off line buffering */
        struct termios term;
        tcgetattr(STDIN, &term);
        term.c_lflag &= ~ICANON;
        tcsetattr(STDIN, TCSANOW, &term);
        setbuf(stdin, NULL);
        initialized = true;
    }

    int bytesWaiting;
    ioctl(STDIN, FIONREAD, &bytesWaiting);
    return bytesWaiting;
}

bool is_network_msg(
    BACMSG * msg)
{

    uint8_t control_byte;       /* NPDU control byte */
    MSG_DATA *data = (MSG_DATA *) msg->data;

    control_byte = data->pdu[1];

    return control_byte & 0x80; /* check 7th bit */
}

uint16_t get_next_free_dnet(
    )
{

    ROUTER_PORT *port = head;
    uint16_t i = 1;
    while (port) {
        if (port->route_info.net == i) {
            port = head;
            i++;
            continue;
        }

        port = port->next;
    }
    return i;
}
