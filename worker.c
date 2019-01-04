#define _GNU_SOURCE

#include "buffer.h"
#include "epoll.h"
#include "errors.h"
#include "log.h"
#include "socket.h"
#include "tempconfig.h"
#include "visca.h"
#include "worker.h"
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define VOPROXYD_STRING_BUFFERS_EXTEND_LENGTH 4096
#define VOPROXYD_MAX_EPOLL_EVENTS 128
#define VOPROXYD_MAX_RX_MESSAGE_LENGTH 4096

static int handle_tcp_message(struct ap_state *state, const uint8_t *message, ssize_t length, int *close)
{
    (void)state;
    (void)message;
    (void)close;

    log("handle tcp msg of len %zu\n", length);

    return 0;
}

static int handle_udp_message(const struct ap_state *state, uint8_t *message, ssize_t length)
{
    buffer_t *message_buf = cons_buffer(length), *response;

    message_buf->data = message;

    response = visca_handle_message(message_buf);

    if (response->length != 0) {
        log("output of visca_handle_message:");
        print_buffer(response, 16);

        socket_send_message_udp(state->current, response, state->current_event->addr);

        free(response);
    }

    return 0;
}

/*
static int epoll_handle_read_queue_tcp(struct ap_state *state)
{
    ssize_t message_length;
    uint8_t rx_message[VOPROXYD_MAX_RX_MESSAGE_LENGTH];
    int close = 0;

    log("about to read on fd = %d", state->current);

    message_length = read(state->current, rx_message, sizeof(rx_message));

    log("read on fd = %d message_length = %zd %s", state->current, message_length,
            (errno == EAGAIN || errno == EWOULDBLOCK) ? "(eagain | ewouldblock)" : "");

    if (message_length == 0) {
        log("close connection on socket fd = %d", state->current);
        epoll_close_fd(state, state->current);
        return 0;
    }

    if (message_length != -1) {
        handle_tcp_message(state, rx_message, message_length, &close);

        if (close || state->close_after_read) {
            state->close_after_read = 0;
            epoll_close_fd(state, state->current);
            return 0;
        }

        return 1;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        errno = 0;
        return 0;
    }

    epoll_close_fd(state, state->current);
    die(ERR_READ, "error reading on socket fd = %d: %s", state->current, strerror(errno));
}
*/

static int epoll_handle_read_queue_udp(struct ap_state *state)
{
    ssize_t message_length;
    uint8_t rx_message[VOPROXYD_MAX_RX_MESSAGE_LENGTH];
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int close = 0;

    message_length = recvfrom(state->current, rx_message, sizeof(rx_message), MSG_DONTWAIT,
            (struct sockaddr*)&addr, &addr_len);

    state->current_event->addr = (struct sockaddr*)&addr;

    log("recvfrom fd = %d message_length = %zd %s", state->current, message_length,
            (errno == EAGAIN || errno == EWOULDBLOCK) ? "(eagain | ewouldblock)" : "");

    if (message_length == 0) {
        log("close connection on socket fd = %d", state->current);
        epoll_close_fd(state, state->current);
        return 0;
    }

    if (message_length == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            errno = 0;
            return 0;
        }

        epoll_close_fd(state, state->current);
        die(ERR_READ, "error reading on socket fd = %d: %s", state->current, strerror(errno));
    }

    handle_udp_message(state, rx_message, message_length);

    if (close || state->close_after_read) {
        state->close_after_read = 0;
        epoll_close_fd(state, state->current);
        return 0;
    }

    return 1;
}

static void epoll_handle_event(struct ap_state *state, const struct epoll_event *event)
{
    int continue_reading = 1, client_fd;

    log("new event on fd = %d", state->current);

    epoll_handle_event_errors(state, event);

    if ((event->events & (unsigned)EPOLLHUP)) {
        log("hangup on fd = %d", state->current);
    }

    state->close_after_read = !!((event->events & (unsigned)EPOLLHUP) |
            (event->events & (unsigned)EPOLLRDHUP));

    switch (state->current_event->type) {
        case FDT_TCP_LISTEN:
            client_fd = socket_accept(state->current);
            epoll_add_fd(state, client_fd, FDT_TCP, 1);
            break;
        /* case FDT_TCP: */
        /*     while (continue_reading) { */
        /*         continue_reading = epoll_handle_read_queue_tcp(state); */
        /*     } */
        /*     break; */
        case FDT_UDP:
            while (continue_reading) {
                continue_reading = epoll_handle_read_queue_udp(state);
            }
            break;
        default:
            die(ERR_EPOLL_EVENT, "epoll_handle_event: unknown event type %d",
                    state->current_event->type);
    }

}

static void main_loop(struct ap_state *state)
{
    struct epoll_event events[VOPROXYD_MAX_EPOLL_EVENTS];
    int num_events, ev_idx, running = 1;

    while (running) {
        num_events = epoll_wait(state->epoll_fd, events,
                VOPROXYD_MAX_EPOLL_EVENTS, -1);

        if (num_events == -1 && errno != EINTR) {
            die(ERR_EPOLL_WAIT, "epoll_wait() failed: %s", strerror(errno));
        }

        for (ev_idx = 0; ev_idx < num_events; ++ev_idx) {
            state->close_after_read = 0;
            state->current_event = events[ev_idx].data.ptr;
            state->current = state->current_event->fd;
            epoll_handle_event(state, &events[ev_idx]);
        }
    }

    ll_free_list(&state->tracked_events);

    close(state->epoll_fd);
}

void start_worker(void)
{
    struct ap_state state = { 0 };
    int tcp_sock_fd, udp_sock_fd;

    state.epoll_fd = epoll_create1(0);
    if (state.epoll_fd == -1) {
        die(ERR_EPOLL_CREATE, "epoll_create1() failed: %s", strerror(errno));
    }

    /* create_listening_tcp_socket(&state.tcp_sock_fd); */
    /* epoll_add_fd(&state, state.tcp_sock_fd, FDT_TCP_LISTEN, 1); */
    tcp_sock_fd = -1;

    socket_create_udp(&udp_sock_fd);
    epoll_add_fd(&state, udp_sock_fd, FDT_UDP, 1);

    log("epoll fd = %d, tcp fd = %d udp fd = %d", state.epoll_fd, tcp_sock_fd, udp_sock_fd);

    main_loop(&state);
}

