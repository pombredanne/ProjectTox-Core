
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>


#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>

#include <sys/ioctl.h>

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32) || defined(__MACH__)
#define MSG_NOSIGNAL 0
#endif

//IRC name and channel.
#define IRC_NAME "Tox_syncbot"
#define IRC_CHANNEL "#tox-real-ontopic"

//IRC server ip and port.
uint8_t ip[4] = {127, 0, 0, 1};
uint16_t port = 6667;

#define SILENT_TIMEOUT 20

int sock;

#define SERVER_CONNECT "NICK "IRC_NAME"\nUSER "IRC_NAME" 8 * :"IRC_NAME"\n"
#define CHANNEL_JOIN "JOIN "IRC_CHANNEL"\n"

/* In toxcore/network.c */
uint64_t current_time_monotonic(void);

uint64_t get_monotime_sec(void)
{
    return current_time_monotonic() / 1000;
}

int reconnect(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (sock < 0) {
        printf("error socket\n");
        return -1;
    }

    struct sockaddr_storage addr = {0};

    size_t addrsize;

    struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;

    addrsize = sizeof(struct sockaddr_in);

    addr4->sin_family = AF_INET;

    memcpy(&addr4->sin_addr, ip, 4);

    addr4->sin_port = htons(port);

    if (connect(sock, (struct sockaddr *)&addr, addrsize) != 0) {
        printf("error connect\n");
        return -1;
    }

    send(sock, SERVER_CONNECT, sizeof(SERVER_CONNECT) - 1, MSG_NOSIGNAL);

    return sock;
}

#include "../toxcore/tox.h"
#include "misc_tools.c"

int current_group = -1;

static void callback_group_invite(Tox *tox, int fid, uint8_t type, const uint8_t *data, uint16_t length, void *userdata)
{
    if (current_group == -1)
        current_group = tox_join_groupchat(tox, fid, data, length);
}

void callback_friend_message(Tox *tox, int fid, const uint8_t *message, uint16_t length, void *userdata)
{
    if (length == 1 && *message == 'c') {
        if (tox_del_groupchat(tox, current_group) == 0)
            current_group = -1;
    }

    if (length == 1 && *message == 'i') {
        tox_invite_friend(tox, fid, current_group);
    }

    if (length == 1 && *message == 'j' && sock >= 0) {
        send(sock, CHANNEL_JOIN, sizeof(CHANNEL_JOIN) - 1, MSG_NOSIGNAL);
    }
}

static void copy_groupmessage(Tox *tox, int groupnumber, int friendgroupnumber, const uint8_t *message, uint16_t length,
                              void *userdata)
{
    if (tox_group_peernumber_is_ours(tox, groupnumber, friendgroupnumber))
        return;

    uint8_t name[TOX_MAX_NAME_LENGTH];
    int namelen = tox_group_peername(tox, groupnumber, friendgroupnumber, name);

    if (namelen == 0 || namelen == -1) {
        memcpy(name, "<unknown>", 9);
        namelen = 9;
    }

    uint8_t sendbuf[2048];
    uint16_t send_len = 0;

    memcpy(sendbuf, "PRIVMSG "IRC_CHANNEL" :", sizeof("PRIVMSG "IRC_CHANNEL" :"));
    send_len += sizeof("PRIVMSG "IRC_CHANNEL" :") - 1;
    memcpy(sendbuf + send_len, name, namelen);
    send_len += namelen;
    sendbuf[send_len] = ':';
    send_len += 1;
    sendbuf[send_len] = ' ';
    send_len += 1;
    memcpy(sendbuf + send_len, message, length);
    send_len += length;
    unsigned int i;

    for (i = 0; i < send_len; ++i) {
        if (sendbuf[i] == '\n')
            sendbuf[i] = '|';

        if (sendbuf[i] == 0)
            sendbuf[i] = ' ';
    }

    sendbuf[send_len] = '\n';
    send_len += 1;

    if (sock >= 0)
        send(sock, sendbuf, send_len, MSG_NOSIGNAL);
}

void send_irc_group(Tox *tox, uint8_t *msg, uint16_t len)
{
    if (len > 1350 || len == 0 || len == 1)
        return;

    --len;

    if (*msg != ':')
        return;

    uint8_t req[len];
    unsigned int i;

    for (i = 0; i < (len - 1); ++i) {
        if (msg[i + 1] == ':') {
            break;
        }

        req[i] = msg[i + 1];
    }

    unsigned int req_len = i;
    req[i] = 0;

    uint8_t message[len];
    uint16_t length = 0;

    uint8_t *pmsg = (uint8_t *)strstr((char *)req, " PRIVMSG");

    if (pmsg == NULL)
        return;

    uint8_t *dt = req;

    for (dt = req, i = 0; dt != pmsg && *dt != '!'; ++dt, ++i) {
        message[i] = *dt;
        ++length;
    }

    message[length] = ':';
    length += 1;
    message[length] = ' ';
    length += 1;

    if ((req_len + 2) >= len)
        return;

    memcpy(message + length, msg + req_len + 2, len - (req_len + 2));
    length += len - (req_len + 2);
    tox_group_message_send(tox, current_group, message, length);
}

Tox *init_tox(int argc, char *argv[])
{
    uint8_t ipv6enabled = TOX_ENABLE_IPV6_DEFAULT; /* x */
    int argvoffset = cmdline_parsefor_ipv46(argc, argv, &ipv6enabled);

    if (argvoffset < 0)
        exit(1);

    /* with optional --ipvx, now it can be 1-4 arguments... */
    if ((argc != argvoffset + 2) && (argc != argvoffset + 4)) {
        printf("Usage: %s [--ipv4|--ipv6] ip port public_key (of the DHT bootstrap node)\n", argv[0]);
        exit(0);
    }

    Tox *tox = tox_new(0);

    if (!tox)
        exit(1);

    tox_set_name(tox, (uint8_t *)IRC_NAME, sizeof(IRC_NAME) - 1);
    tox_callback_friend_message(tox, &callback_friend_message, 0);
    tox_callback_group_invite(tox, &callback_group_invite, 0);
    tox_callback_group_message(tox, &copy_groupmessage, 0);
    tox_callback_group_action(tox, &copy_groupmessage, 0);

    uint16_t port = atoi(argv[argvoffset + 2]);
    unsigned char *binary_string = hex_string_to_bin(argv[argvoffset + 3]);
    int res = tox_bootstrap_from_address(tox, argv[argvoffset + 1], port, binary_string);
    free(binary_string);

    char temp_id[128];
    printf("\nEnter the address of irc_syncbots master (38 bytes HEX format):\n");

    if (scanf("%s", temp_id) != 1) {
        exit (1);
    }

    uint8_t *bin_id = hex_string_to_bin(temp_id);
    int num = tox_add_friend(tox, bin_id, (uint8_t *)"Install Gentoo", sizeof("Install Gentoo") - 1);
    free(bin_id);

    if (num < 0) {
        printf("\nSomething went wrong when adding friend.\n");
        exit(1);
    }

    return tox;
}

int main(int argc, char *argv[])
{
    Tox *tox = init_tox(argc, argv);

    sock = reconnect();

    if (sock < 0)
        return 1;

    uint64_t last_get = get_monotime_sec();
    int connected = 0, ping_sent = 0;

    while (1) {
        int count = 0;
        ioctl(sock, FIONREAD, &count);

        if (count > 0) {
            last_get = get_monotime_sec();
            ping_sent = 0;
            uint8_t data[count + 1];
            data[count] = 0;
            recv(sock, data, count, MSG_NOSIGNAL);
            printf("%s", data);

            if (!connected)
                connected = 1;

            if (count > 6 && data[0] == 'P' && data[1] == 'I' && data[2] == 'N' && data[3] == 'G') {
                data[1] = 'O';
                unsigned int i;

                for (i = 0; i < count; ++i) {
                    if (data[i] == '\n') {
                        ++i;
                        break;
                    }
                }

                send(sock, data, i, MSG_NOSIGNAL);
            }

            unsigned int i, p_i = 0;

            for (i = 1; data[0] == ':' && i < count; ++i) {
                if (data[i] == ' ') {
                    if (i + 5 < count && memcmp(data + i, " 404 ", 5) == 0) {
                        connected = 1;
                    }

                    break;
                }

                if (data[i] == ':')
                    break;
            }

            for (i = 0; i < count; ++i) {
                if (data[i] == '\n' && i != 0) {
                    send_irc_group(tox, data + p_i, i - p_i);
                    p_i = i + 1;
                }
            }
        }

        if (connected == 1) {
            send(sock, CHANNEL_JOIN, sizeof(CHANNEL_JOIN) - 1, MSG_NOSIGNAL);
            connected = 2;
        }

        if (!ping_sent && last_get + (SILENT_TIMEOUT / 2) < get_monotime_sec()) {
            unsigned int p_s = sizeof("PING :test\n") - 1;

            if (send(sock, "PING :test\n", p_s, MSG_NOSIGNAL) == p_s)
                ping_sent = 1;
        }

        int error = 0;
        socklen_t len = sizeof (error);

        if (sock < 0 || last_get + SILENT_TIMEOUT < get_monotime_sec()
                || getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len ) != 0) {
            close(sock);
            printf("reconnect\n");
            sock = reconnect();

            if (sock >= 0) {
                last_get = get_monotime_sec();
                connected = 0;
                ping_sent = 0;
            }
        }

        tox_do(tox);
        usleep(1000 * 50);
    }

    return 0;
}
