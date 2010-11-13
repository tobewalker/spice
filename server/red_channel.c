/*
    Copyright (C) 2009 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.


    Author:
        yhalperi@redhat.com
*/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdint.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "stat.h"
#include "red_channel.h"
#include "generated_marshallers.h"

static void red_channel_client_event(int fd, int event, void *data);

/* return the number of bytes read. -1 in case of error */
static int red_peer_receive(RedsStream *stream, uint8_t *buf, uint32_t size)
{
    uint8_t *pos = buf;
    while (size) {
        int now;
        if (stream->shutdown) {
            return -1;
        }
        now = reds_stream_read(stream, pos, size);
        if (now <= 0) {
            if (now == 0) {
                return -1;
            }
            ASSERT(now == -1);
            if (errno == EAGAIN) {
                break;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EPIPE) {
                return -1;
            } else {
                red_printf("%s", strerror(errno));
                return -1;
            }
        } else {
            size -= now;
            pos += now;
        }
    }
    return pos - buf;
}

// TODO: this implementation, as opposed to the old implementation in red_worker,
// does many calls to red_peer_receive and through it cb_read, and thus avoids pointer
// arithmetic for the case where a single cb_read could return multiple messages. But
// this is suboptimal potentially. Profile and consider fixing.
static void red_peer_handle_incoming(RedsStream *stream, IncomingHandler *handler)
{
    int bytes_read;
    uint8_t *parsed;
    size_t parsed_size;
    message_destructor_t parsed_free;

    for (;;) {
        int ret_handle;
        if (handler->header_pos < sizeof(SpiceDataHeader)) {
            bytes_read = red_peer_receive(stream,
                                          ((uint8_t *)&handler->header) + handler->header_pos,
                                          sizeof(SpiceDataHeader) - handler->header_pos);
            if (bytes_read == -1) {
                handler->cb->on_error(handler->opaque);
                return;
            }
            handler->header_pos += bytes_read;

            if (handler->header_pos != sizeof(SpiceDataHeader)) {
                return;
            }
        }

        if (handler->msg_pos < handler->header.size) {
            if (!handler->msg) {
                handler->msg = handler->cb->alloc_msg_buf(handler->opaque, &handler->header);
                if (handler->msg == NULL) {
                    red_printf("ERROR: channel refused to allocate buffer.");
                    handler->cb->on_error(handler->opaque);
                    return;
                }
            }

            bytes_read = red_peer_receive(stream,
                                          handler->msg + handler->msg_pos,
                                          handler->header.size - handler->msg_pos);
            if (bytes_read == -1) {
                handler->cb->release_msg_buf(handler->opaque, &handler->header, handler->msg);
                handler->cb->on_error(handler->opaque);
                return;
            }
            handler->msg_pos += bytes_read;
            if (handler->msg_pos != handler->header.size) {
                return;
            }
        }

        if (handler->cb->parser) {
            parsed = handler->cb->parser(handler->msg,
                handler->msg + handler->header.size, handler->header.type,
                SPICE_VERSION_MINOR, &parsed_size, &parsed_free);
            if (parsed == NULL) {
                red_printf("failed to parse message type %d", handler->header.type);
                handler->cb->on_error(handler->opaque);
                return;
            }
            ret_handle = handler->cb->handle_parsed(handler->opaque, parsed_size,
                                    handler->header.type, parsed);
            parsed_free(parsed);
        } else {
            ret_handle = handler->cb->handle_message(handler->opaque, &handler->header,
                                                 handler->msg);
        }
        if (handler->shut) {
            handler->cb->on_error(handler->opaque);
            return;
        }
        handler->msg_pos = 0;
        handler->msg = NULL;
        handler->header_pos = 0;

        if (!ret_handle) {
            handler->cb->on_error(handler->opaque);
            return;
        }
    }
}

void red_channel_client_receive(RedChannelClient *rcc)
{
    red_peer_handle_incoming(rcc->stream, &rcc->incoming);
}

void red_channel_receive(RedChannel *channel)
{
    red_channel_client_receive(channel->rcc);
}

static void red_peer_handle_outgoing(RedsStream *stream, OutgoingHandler *handler)
{
    ssize_t n;

    if (handler->size == 0) {
        handler->vec = handler->vec_buf;
        handler->size = handler->cb->get_msg_size(handler->opaque);
        if (!handler->size) {  // nothing to be sent
            return;
        }
    }

    for (;;) {
        handler->cb->prepare(handler->opaque, handler->vec, &handler->vec_size, handler->pos);
        n = reds_stream_writev(stream, handler->vec, handler->vec_size);
        if (n == -1) {
            switch (errno) {
            case EAGAIN:
                handler->cb->on_block(handler->opaque);
                return;
            case EINTR:
                continue;
            case EPIPE:
                handler->cb->on_error(handler->opaque);
                return;
            default:
                red_printf("%s", strerror(errno));
                handler->cb->on_error(handler->opaque);
                return;
            }
        } else {
            handler->pos += n;
            handler->cb->on_output(handler->opaque, n);
            if (handler->pos == handler->size) { // finished writing data
                handler->cb->on_msg_done(handler->opaque);
                handler->vec = handler->vec_buf;
                handler->pos = 0;
                handler->size = 0;
                return;
            }
        }
    }
}

static void red_channel_client_on_output(void *opaque, int n)
{
    RedChannelClient *rcc = opaque;

    stat_inc_counter(rcc->channel->out_bytes_counter, n);
}

void red_channel_client_default_peer_on_error(RedChannelClient *rcc)
{
    rcc->channel->disconnect(rcc);
}

static void red_channel_peer_on_incoming_error(RedChannelClient *rcc)
{
    rcc->channel->on_incoming_error(rcc);
}

static void red_channel_peer_on_outgoing_error(RedChannelClient *rcc)
{
    rcc->channel->on_outgoing_error(rcc);
}

static int red_channel_client_peer_get_out_msg_size(void *opaque)
{
    RedChannelClient *rcc = (RedChannelClient *)opaque;

    return rcc->send_data.size;
}

static void red_channel_client_peer_prepare_out_msg(
    void *opaque, struct iovec *vec, int *vec_size, int pos)
{
    RedChannelClient *rcc = (RedChannelClient *)opaque;

    *vec_size = spice_marshaller_fill_iovec(rcc->send_data.marshaller,
                                            vec, MAX_SEND_VEC, pos);
}

static void red_channel_client_peer_on_out_block(void *opaque)
{
    RedChannelClient *rcc = (RedChannelClient *)opaque;

    rcc->send_data.blocked = TRUE;
    rcc->channel->core->watch_update_mask(rcc->stream->watch,
                                     SPICE_WATCH_EVENT_READ |
                                     SPICE_WATCH_EVENT_WRITE);
}

static void red_channel_client_reset_send_data(RedChannelClient *rcc)
{
    spice_marshaller_reset(rcc->send_data.marshaller);
    rcc->send_data.header = (SpiceDataHeader *)
        spice_marshaller_reserve_space(rcc->send_data.marshaller, sizeof(SpiceDataHeader));
    spice_marshaller_set_base(rcc->send_data.marshaller, sizeof(SpiceDataHeader));
    rcc->send_data.header->type = 0;
    rcc->send_data.header->size = 0;
    rcc->send_data.header->sub_list = 0;
    rcc->send_data.header->serial = ++rcc->send_data.serial;
}

void red_channel_client_push_set_ack(RedChannelClient *rcc)
{
    red_channel_pipe_add_type(rcc->channel, PIPE_ITEM_TYPE_SET_ACK);
}

void red_channel_push_set_ack(RedChannel *channel)
{
    // TODO - MC, should replace with add_type_all (or whatever I'll name it)
    red_channel_pipe_add_type(channel, PIPE_ITEM_TYPE_SET_ACK);
}

static void red_channel_client_send_set_ack(RedChannelClient *rcc)
{
    SpiceMsgSetAck ack;

    ASSERT(rcc);
    red_channel_client_init_send_data(rcc, SPICE_MSG_SET_ACK, NULL);
    ack.generation = ++rcc->ack_data.generation;
    ack.window = rcc->ack_data.client_window;
    rcc->ack_data.messages_window = 0;

    spice_marshall_msg_set_ack(rcc->send_data.marshaller, &ack);

    red_channel_client_begin_send_message(rcc);
}

static void red_channel_client_send_item(RedChannelClient *rcc, PipeItem *item)
{
    int handled = TRUE;

    ASSERT(red_channel_client_no_item_being_sent(rcc));
    red_channel_client_reset_send_data(rcc);
    switch (item->type) {
        case PIPE_ITEM_TYPE_SET_ACK:
            red_channel_client_send_set_ack(rcc);
            break;
        default:
            handled = FALSE;
    }
    if (!handled) {
        rcc->channel->send_item(rcc, item);
    }
}

static void red_channel_client_release_item(RedChannelClient *rcc, PipeItem *item, int item_pushed)
{
    int handled = TRUE;

    switch (item->type) {
        case PIPE_ITEM_TYPE_SET_ACK:
            free(item);
            break;
        default:
            handled = FALSE;
    }
    if (!handled) {
        rcc->channel->release_item(rcc, item, item_pushed);
    }
}

static inline void red_channel_client_release_sent_item(RedChannelClient *rcc)
{
    if (rcc->send_data.item) {
        red_channel_client_release_item(rcc,
                                        rcc->send_data.item, TRUE);
        rcc->send_data.item = NULL;
    }
}

static void red_channel_peer_on_out_msg_done(void *opaque)
{
    RedChannelClient *rcc = (RedChannelClient *)opaque;

    rcc->send_data.size = 0;
    red_channel_client_release_sent_item(rcc);
    if (rcc->send_data.blocked) {
        rcc->send_data.blocked = FALSE;
        rcc->channel->core->watch_update_mask(rcc->stream->watch,
                                         SPICE_WATCH_EVENT_READ);
    }
}

static void red_channel_add_client(RedChannel *channel, RedChannelClient *rcc)
{
    ASSERT(rcc);
	channel->rcc = rcc;
}

RedChannelClient *red_channel_client_create(
    int size,
    RedChannel *channel,
    RedsStream *stream)
{
    RedChannelClient *rcc = NULL;

    ASSERT(stream && channel && size >= sizeof(RedChannelClient));
    rcc = spice_malloc0(size);
    rcc->stream = stream;
    rcc->channel = channel;
    rcc->ack_data.messages_window = ~0;  // blocks send message (maybe use send_data.blocked +
                                             // block flags)
    rcc->ack_data.client_generation = ~0;
    rcc->ack_data.client_window = CLIENT_ACK_WINDOW;
    rcc->send_data.marshaller = spice_marshaller_new();

    rcc->incoming.opaque = rcc;
    rcc->incoming.cb = &channel->incoming_cb;

    rcc->outgoing.opaque = rcc;
    rcc->outgoing.cb = &channel->outgoing_cb;
    rcc->outgoing.pos = 0;
    rcc->outgoing.size = 0;
    if (!channel->config_socket(rcc)) {
        goto error;
    }

    stream->watch = channel->core->watch_add(stream->socket,
                                           SPICE_WATCH_EVENT_READ,
                                           red_channel_client_event, rcc);
    red_channel_add_client(channel, rcc);
    return rcc;
error:
    free(rcc);
    reds_stream_free(stream);
    return NULL;
}

RedChannel *red_channel_create(int size,
                               SpiceCoreInterface *core,
                               int migrate, int handle_acks,
                               channel_configure_socket_proc config_socket,
                               channel_disconnect_proc disconnect,
                               channel_handle_message_proc handle_message,
                               channel_alloc_msg_recv_buf_proc alloc_recv_buf,
                               channel_release_msg_recv_buf_proc release_recv_buf,
                               channel_hold_pipe_item_proc hold_item,
                               channel_send_pipe_item_proc send_item,
                               channel_release_pipe_item_proc release_item,
                               channel_handle_migrate_flush_mark_proc handle_migrate_flush_mark,
                               channel_handle_migrate_data_proc handle_migrate_data,
                               channel_handle_migrate_data_get_serial_proc handle_migrate_data_get_serial)
{
    RedChannel *channel;

    ASSERT(size >= sizeof(*channel));
    ASSERT(config_socket && disconnect && handle_message && alloc_recv_buf &&
           release_item);
    channel = spice_malloc0(size);
    channel->handle_acks = handle_acks;
    channel->disconnect = disconnect;
    channel->send_item = send_item;
    channel->release_item = release_item;
    channel->hold_item = hold_item;
    channel->handle_migrate_flush_mark = handle_migrate_flush_mark;
    channel->handle_migrate_data = handle_migrate_data;
    channel->handle_migrate_data_get_serial = handle_migrate_data_get_serial;
    channel->config_socket = config_socket;

    channel->core = core;
    channel->migrate = migrate;
    ring_init(&channel->pipe);

    channel->incoming_cb.alloc_msg_buf = (alloc_msg_recv_buf_proc)alloc_recv_buf;
    channel->incoming_cb.release_msg_buf = (release_msg_recv_buf_proc)release_recv_buf;
    channel->incoming_cb.handle_message = (handle_message_proc)handle_message;
    channel->incoming_cb.on_error =
        (on_incoming_error_proc)red_channel_client_default_peer_on_error;
    channel->outgoing_cb.get_msg_size = red_channel_client_peer_get_out_msg_size;
    channel->outgoing_cb.prepare = red_channel_client_peer_prepare_out_msg;
    channel->outgoing_cb.on_block = red_channel_client_peer_on_out_block;
    channel->outgoing_cb.on_error =
        (on_outgoing_error_proc)red_channel_client_default_peer_on_error;
    channel->outgoing_cb.on_msg_done = red_channel_peer_on_out_msg_done;
    channel->outgoing_cb.on_output = red_channel_client_on_output;

    channel->shut = 0; // came here from inputs, perhaps can be removed? XXX
    channel->out_bytes_counter = 0;
    return channel;
}

static void do_nothing_disconnect(RedChannelClient *rcc)
{
}

static int do_nothing_handle_message(RedChannelClient *rcc, SpiceDataHeader *header, uint8_t *msg)
{
    return TRUE;
}

RedChannel *red_channel_create_parser(int size,
                               SpiceCoreInterface *core,
                               int migrate, int handle_acks,
                               channel_configure_socket_proc config_socket,
                               spice_parse_channel_func_t parser,
                               channel_handle_parsed_proc handle_parsed,
                               channel_alloc_msg_recv_buf_proc alloc_recv_buf,
                               channel_release_msg_recv_buf_proc release_recv_buf,
                               channel_hold_pipe_item_proc hold_item,
                               channel_send_pipe_item_proc send_item,
                               channel_release_pipe_item_proc release_item,
                               channel_on_incoming_error_proc incoming_error,
                               channel_on_outgoing_error_proc outgoing_error,
                               channel_handle_migrate_flush_mark_proc handle_migrate_flush_mark,
                               channel_handle_migrate_data_proc handle_migrate_data,
                               channel_handle_migrate_data_get_serial_proc handle_migrate_data_get_serial)
{
    RedChannel *channel = red_channel_create(size,
        core, migrate, handle_acks, config_socket, do_nothing_disconnect,
        do_nothing_handle_message, alloc_recv_buf, release_recv_buf, hold_item,
        send_item, release_item, handle_migrate_flush_mark, handle_migrate_data,
        handle_migrate_data_get_serial);

    if (channel == NULL) {
        return NULL;
    }
    channel->incoming_cb.handle_parsed = (handle_parsed_proc)handle_parsed;
    channel->incoming_cb.parser = parser;
    channel->incoming_cb.on_error = (on_incoming_error_proc)red_channel_peer_on_incoming_error;
    channel->outgoing_cb.on_error = (on_outgoing_error_proc)red_channel_peer_on_outgoing_error;
    channel->on_incoming_error = incoming_error;
    channel->on_outgoing_error = outgoing_error;
    return channel;
}

void red_channel_client_destroy(RedChannelClient *rcc)
{
    red_channel_client_disconnect(rcc);
    spice_marshaller_destroy(rcc->send_data.marshaller);
    free(rcc);
}

void red_channel_destroy(RedChannel *channel)
{
    if (!channel) {
        return;
    }
    if (channel->rcc) {
        red_channel_client_destroy(channel->rcc);
    }
    free(channel);
}

static void red_channel_client_shutdown(RedChannelClient *rcc)
{
    if (rcc->stream && !rcc->stream->shutdown) {
        rcc->channel->core->watch_remove(rcc->stream->watch);
        rcc->stream->watch = NULL;
        shutdown(rcc->stream->socket, SHUT_RDWR);
        rcc->stream->shutdown = TRUE;
        rcc->incoming.shut = TRUE;
    }
    red_channel_client_release_sent_item(rcc);
}

void red_channel_shutdown(RedChannel *channel)
{
    if (channel->rcc) {
        red_channel_client_shutdown(channel->rcc);
    }
    red_channel_pipe_clear(channel);
}

void red_channel_client_send(RedChannelClient *rcc)
{
    red_peer_handle_outgoing(rcc->stream, &rcc->outgoing);
}

void red_channel_send(RedChannel *channel)
{
    if (channel->rcc) {
        red_channel_client_send(channel->rcc);
    }
}

static inline int red_channel_client_waiting_for_ack(RedChannelClient *rcc)
{
    return (rcc->channel->handle_acks &&
            (rcc->ack_data.messages_window > rcc->ack_data.client_window * 2));
}

// TODO: add refs and target to PipeItem. Right now this only works for a
// single client (or actually, it's worse - first come first served)
static inline PipeItem *red_channel_client_pipe_get(RedChannelClient *rcc)
{
    PipeItem *item;

    if (!rcc || rcc->send_data.blocked
             || red_channel_client_waiting_for_ack(rcc)
             || !(item = (PipeItem *)ring_get_tail(&rcc->channel->pipe))) {
        return NULL;
    }
    --rcc->channel->pipe_size;
    ring_remove(&item->link);
    return item;
}

static void red_channel_client_push(RedChannelClient *rcc)
{
    PipeItem *pipe_item;

    if (!rcc->during_send) {
        rcc->during_send = TRUE;
    } else {
        return;
    }

    if (rcc->send_data.blocked) {
        red_channel_client_send(rcc);
    }

    while ((pipe_item = red_channel_client_pipe_get(rcc))) {
        red_channel_client_send_item(rcc, pipe_item);
    }
    rcc->during_send = FALSE;
}

void red_channel_push(RedChannel *channel)
{
    if (!channel || !channel->rcc) {
        return;
    }
    red_channel_client_push(channel->rcc);
}

static void red_channel_client_init_outgoing_messages_window(RedChannelClient *rcc)
{
    rcc->ack_data.messages_window = 0;
    red_channel_client_push(rcc);
}

// TODO: this function doesn't make sense because the window should be client (WAN/LAN)
// specific
void red_channel_init_outgoing_messages_window(RedChannel *channel)
{
    red_channel_client_init_outgoing_messages_window(channel->rcc);
}

static void red_channel_handle_migrate_flush_mark(RedChannel *channel)
{
    if (channel->handle_migrate_flush_mark) {
        channel->handle_migrate_flush_mark(channel->rcc);
    }
}

// TODO: the whole migration is broken with multiple clients. What do we want to do?
// basically just
//  1) source send mark to all
//  2) source gets at various times the data (waits for all)
//  3) source migrates to target
//  4) target sends data to all
// So need to make all the handlers work with per channel/client data (what data exactly?)
static void red_channel_handle_migrate_data(RedChannelClient *rcc, uint32_t size, void *message)
{
    if (!rcc->channel->handle_migrate_data) {
        return;
    }
    ASSERT(red_channel_client_get_message_serial(rcc) == 0);
    red_channel_client_set_message_serial(rcc,
        rcc->channel->handle_migrate_data_get_serial(rcc, size, message));
    rcc->channel->handle_migrate_data(rcc, size, message);
}

int red_channel_client_handle_message(RedChannelClient *rcc, uint32_t size,
                               uint16_t type, void *message)
{
    switch (type) {
    case SPICE_MSGC_ACK_SYNC:
        if (size != sizeof(uint32_t)) {
            red_printf("bad message size");
            return FALSE;
        }
        rcc->ack_data.client_generation = *(uint32_t *)(message);
        break;
    case SPICE_MSGC_ACK:
        if (rcc->ack_data.client_generation == rcc->ack_data.generation) {
            rcc->ack_data.messages_window -= rcc->ack_data.client_window;
            red_channel_client_push(rcc);
        }
        break;
    case SPICE_MSGC_DISCONNECTING:
        break;
    case SPICE_MSGC_MIGRATE_FLUSH_MARK:
        red_channel_handle_migrate_flush_mark(rcc->channel);
        break;
    case SPICE_MSGC_MIGRATE_DATA:
        red_channel_handle_migrate_data(rcc, size, message);
        break;
    default:
        red_printf("invalid message type %u", type);
        return FALSE;
    }
    return TRUE;
}

static void red_channel_client_event(int fd, int event, void *data)
{
    RedChannelClient *rcc = (RedChannelClient *)data;

    if (event & SPICE_WATCH_EVENT_READ) {
        red_channel_client_receive(rcc);
    }
    if (event & SPICE_WATCH_EVENT_WRITE) {
        red_channel_client_push(rcc);
    }
}

void red_channel_client_init_send_data(RedChannelClient *rcc, uint16_t msg_type, PipeItem *item)
{
    ASSERT(red_channel_client_no_item_being_sent(rcc));
    ASSERT(msg_type != 0);
    rcc->send_data.header->type = msg_type;
    rcc->send_data.item = item;
    if (item) {
        rcc->channel->hold_item(rcc, item);
    }
}

void red_channel_client_begin_send_message(RedChannelClient *rcc)
{
    SpiceMarshaller *m = rcc->send_data.marshaller;

    // TODO - better check: type in channel_allowed_types. Better: type in channel_allowed_types(channel_state)
    if (rcc->send_data.header->type == 0) {
        red_printf("BUG: header->type == 0");
        return;
    }
    spice_marshaller_flush(m);
    rcc->send_data.size = spice_marshaller_get_total_size(m);
    rcc->send_data.header->size = rcc->send_data.size - sizeof(SpiceDataHeader);
    rcc->ack_data.messages_window++;
    rcc->send_data.header = NULL; /* avoid writing to this until we have a new message */
    red_channel_client_send(rcc);
}

uint64_t red_channel_client_get_message_serial(RedChannelClient *rcc)
{
    return rcc->send_data.serial;
}

void red_channel_client_set_message_serial(RedChannelClient *rcc, uint64_t serial)
{
    rcc->send_data.serial = serial;
}

void red_channel_pipe_item_init(RedChannel *channel, PipeItem *item, int type)
{
    ring_item_init(&item->link);
    item->type = type;
}

void red_channel_pipe_add(RedChannel *channel, PipeItem *item)
{
    ASSERT(channel);

    channel->pipe_size++;
    ring_add(&channel->pipe, &item->link);
}

void red_channel_pipe_add_push(RedChannel *channel, PipeItem *item)
{
    ASSERT(channel);

    channel->pipe_size++;
    ring_add(&channel->pipe, &item->link);
    red_channel_push(channel);
}

void red_channel_pipe_add_after(RedChannel *channel, PipeItem *item, PipeItem *pos)
{
    ASSERT(channel);
    ASSERT(pos);
    ASSERT(item);

    channel->pipe_size++;
    ring_add_after(&item->link, &pos->link);
}

int red_channel_pipe_item_is_linked(RedChannel *channel, PipeItem *item)
{
    return ring_item_is_linked(&item->link);
}

void red_channel_pipe_item_remove(RedChannel *channel, PipeItem *item)
{
    ring_remove(&item->link);
}

void red_channel_pipe_add_tail(RedChannel *channel, PipeItem *item)
{
    ASSERT(channel);
    channel->pipe_size++;
    ring_add_before(&item->link, &channel->pipe);

    red_channel_push(channel);
}

void red_channel_pipe_add_type(RedChannel *channel, int pipe_item_type)
{
    PipeItem *item = spice_new(PipeItem, 1);
    red_channel_pipe_item_init(channel, item, pipe_item_type);
    red_channel_pipe_add(channel, item);

    red_channel_push(channel);
}

int red_channel_is_connected(RedChannel *channel)
{
    return channel->rcc != NULL;
}

void red_channel_client_clear_sent_item(RedChannelClient *rcc)
{
    if (rcc->send_data.item) {
        red_channel_client_release_item(rcc, rcc->send_data.item, TRUE);
        rcc->send_data.item = NULL;
    }
    rcc->send_data.blocked = FALSE;
    rcc->send_data.size = 0;
}

void red_channel_pipe_clear(RedChannel *channel)
{
    PipeItem *item;

    ASSERT(channel);
    if (channel->rcc) {
        red_channel_client_clear_sent_item(channel->rcc);
    }
    while ((item = (PipeItem *)ring_get_head(&channel->pipe))) {
        ring_remove(&item->link);
        red_channel_client_release_item(channel->rcc, item, FALSE);
    }
    channel->pipe_size = 0;
}

void red_channel_client_ack_zero_messages_window(RedChannelClient *rcc)
{
    rcc->ack_data.messages_window = 0;
}

void red_channel_ack_zero_messages_window(RedChannel *channel)
{
    red_channel_client_ack_zero_messages_window(channel->rcc);
}

void red_channel_client_ack_set_client_window(RedChannelClient *rcc, int client_window)
{
    rcc->ack_data.client_window = client_window;
}

void red_channel_ack_set_client_window(RedChannel* channel, int client_window)
{
    if (channel->rcc) {
        red_channel_client_ack_set_client_window(channel->rcc, client_window);
    }
}

void red_channel_client_disconnect(RedChannelClient *rcc)
{
    red_printf("%p (channel %p)", rcc, rcc->channel);

    if (rcc->send_data.item) {
        rcc->channel->release_item(rcc, rcc->send_data.item, FALSE);
    }
    // TODO: clear our references from the pipe
    reds_stream_free(rcc->stream);
    rcc->send_data.item = NULL;
    rcc->send_data.blocked = FALSE;
    rcc->send_data.size = 0;
    rcc->channel->rcc = NULL;
}

void red_channel_disconnect(RedChannel *channel)
{
    red_channel_pipe_clear(channel);
    if (channel->rcc) {
        red_channel_client_disconnect(channel->rcc);
    }
}

int red_channel_all_clients_serials_are_zero(RedChannel *channel)
{
    return (!channel->rcc || channel->rcc->send_data.serial == 0);
}

void red_channel_apply_clients(RedChannel *channel, channel_client_visitor v)
{
    if (channel->rcc) {
        v(channel->rcc);
    }
}

void red_channel_apply_clients_data(RedChannel *channel, channel_client_visitor_data v, void *data)
{
    if (channel->rcc) {
        v(channel->rcc, data);
    }
}

void red_channel_set_shut(RedChannel *channel)
{
    if (channel->rcc) {
        channel->rcc->incoming.shut = TRUE;
    }
}

int red_channel_all_blocked(RedChannel *channel)
{
    return !channel || !channel->rcc || channel->rcc->send_data.blocked;
}

int red_channel_any_blocked(RedChannel *channel)
{
    return !channel || !channel->rcc || channel->rcc->send_data.blocked;
}

int red_channel_client_blocked(RedChannelClient *rcc)
{
    return rcc && rcc->send_data.blocked;
}

int red_channel_client_send_message_pending(RedChannelClient *rcc)
{
    return rcc->send_data.header->type != 0;
}

/* accessors for RedChannelClient */
SpiceMarshaller *red_channel_client_get_marshaller(RedChannelClient *rcc)
{
    return rcc->send_data.marshaller;
}

RedsStream *red_channel_client_get_stream(RedChannelClient *rcc)
{
    return rcc->stream;
}

SpiceDataHeader *red_channel_client_get_header(RedChannelClient *rcc)
{
    return rcc->send_data.header;
}
/* end of accessors */

int red_channel_get_first_socket(RedChannel *channel)
{
    if (!channel->rcc || !channel->rcc->stream) {
        return -1;
    }
    return channel->rcc->stream->socket;
}

int red_channel_client_item_being_sent(RedChannelClient *rcc, PipeItem *item)
{
    return rcc->send_data.item == item;
}

int red_channel_item_being_sent(RedChannel *channel, PipeItem *item)
{
    return channel->rcc && red_channel_client_item_being_sent(channel->rcc, item);
}

int red_channel_no_item_being_sent(RedChannel *channel)
{
    return !channel->rcc || red_channel_client_no_item_being_sent(channel->rcc);
}

int red_channel_client_no_item_being_sent(RedChannelClient *rcc)
{
    return !rcc || (rcc->send_data.size == 0);
}

static void red_channel_client_pipe_remove(RedChannelClient *rcc, PipeItem *item)
{
    rcc->channel->pipe_size--;
    ring_remove(&item->link);
}

void red_channel_client_pipe_remove_and_release(RedChannelClient *rcc,
                                                PipeItem *item)
{
    red_channel_client_pipe_remove(rcc, item);
    red_channel_client_release_item(rcc, item, FALSE);
}
