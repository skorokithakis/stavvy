#include "esphome_api.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esphome_api.pb.h"
#include "mdns_service.h"
#include "pb_decode.h"
#include "pb_encode.h"

static const char *TAG = "esphome_api";

#define ESPHOME_API_PORT 6053

// Message IDs from the ESPHome native API protocol. The generated nanopb
// header contains the message bodies but not these IDs, because they live in
// custom proto options that nanopb does not process.
enum {
    MESSAGE_TYPE_HELLO_REQUEST = 1,
    MESSAGE_TYPE_HELLO_RESPONSE = 2,
    MESSAGE_TYPE_DISCONNECT_REQUEST = 5,
    MESSAGE_TYPE_DISCONNECT_RESPONSE = 6,
    MESSAGE_TYPE_PING_REQUEST = 7,
    MESSAGE_TYPE_PING_RESPONSE = 8,
    MESSAGE_TYPE_DEVICE_INFO_REQUEST = 9,
    MESSAGE_TYPE_DEVICE_INFO_RESPONSE = 10,
    MESSAGE_TYPE_LIST_ENTITIES_REQUEST = 11,
    MESSAGE_TYPE_LIST_ENTITIES_DONE_RESPONSE = 19,
    MESSAGE_TYPE_SUBSCRIBE_STATES_REQUEST = 20,
    MESSAGE_TYPE_SUBSCRIBE_VOICE_ASSISTANT_REQUEST = 89,
    MESSAGE_TYPE_VOICE_ASSISTANT_REQUEST = 90,
    MESSAGE_TYPE_VOICE_ASSISTANT_RESPONSE = 91,
    MESSAGE_TYPE_VOICE_ASSISTANT_EVENT_RESPONSE = 92,
    MESSAGE_TYPE_VOICE_ASSISTANT_AUDIO = 106,
};

// From ESPHome's voice_assistant.h: FEATURE_VOICE_ASSISTANT is bit 0 and
// FEATURE_API_AUDIO is bit 2. This firmware sends microphone audio over this
// TCP connection rather than UDP, hence the API audio flag.
#define VOICE_ASSISTANT_FEATURE_VOICE_ASSISTANT (1u << 0)
#define VOICE_ASSISTANT_FEATURE_API_AUDIO (1u << 2)

// The reference client sends Flag 1 with SubscribeVoiceAssistantRequest, which
// is VOICE_ASSISTANT_SUBSCRIBE_API_AUDIO in the upstream protocol.
#define VOICE_ASSISTANT_SUBSCRIBE_API_AUDIO (1u << 0)

#define API_VERSION_MAJOR 1
#define API_VERSION_MINOR 10

#define SERVER_INFO "amoledesp32"
#define DEVICE_MODEL "ESP32-C6-Touch-AMOLED-2.16"

// The largest message we encode is VoiceAssistantAudio at 2053 bytes. The
// receive side is much larger because a voice event carries the LLM reply and
// transcript inline; anything still bigger is drained and skipped, which loses
// only that one message. Both buffers are fixed size and static.
#define MAXIMUM_RECEIVE_MESSAGE_BYTES 8192
#define MAXIMUM_SEND_MESSAGE_BYTES 2304

// A client that is waiting for Wi-Fi, or that stalls mid-frame, must not pin
// the API task forever. The deadline covers a whole frame, including draining
// an oversized one, so a peer that trickles a byte at a time cannot extend it.
// The idle timeout is separate and applies between frames: the reference client
// pings every 20 seconds, so 90 seconds of silence means it is gone.
#define FRAME_READ_TIMEOUT_SECONDS 10
#define FRAME_READ_TIMEOUT_MICROSECONDS ((int64_t)FRAME_READ_TIMEOUT_SECONDS * 1000 * 1000)
#define SEND_TIMEOUT_SECONDS 5
#define IDLE_TIMEOUT_MICROSECONDS ((int64_t)90 * 1000 * 1000)

#define API_TASK_STACK_BYTES 6144
#define API_TASK_PRIORITY 5

typedef enum {
    FRAME_RESULT_OK,
    FRAME_RESULT_SKIPPED,
    FRAME_RESULT_FAILED,
} frame_result;

// The mutex guards the client socket, the subscription state and the outgoing
// buffers. Both the API task and any task that sends audio use it, so a frame
// is always written as one contiguous sequence of bytes.
static SemaphoreHandle_t state_mutex;
static esphome_api_callbacks callbacks;

static int listener_socket = -1;
static int client_socket = -1;
static bool voice_assistant_subscribed;
static bool voice_assistant_api_audio;
// Set by a sender when writing fails. The API task owns closing the socket, so
// the sender also shuts the socket down to wake the select() in that task.
static bool client_write_failed;
// Set when the session task wants the client gone, for example because a late
// response to a cancelled request would otherwise be accepted by the next
// session. Like a write failure, this only asks the API task to drop the
// client; the descriptor stays owned by that task.
static bool client_disconnect_requested;
static int64_t last_message_time_microseconds;

static uint8_t incoming_message_buffer[MAXIMUM_RECEIVE_MESSAGE_BYTES];
static uint8_t outgoing_message_buffer[MAXIMUM_SEND_MESSAGE_BYTES];
// Kept out of the calling task's stack: the encode step copies this into the
// outgoing buffer, so audio uses about 4 KB of static RAM in total.
static VoiceAssistantAudio outgoing_audio;
// The decoded event cannot live on the API task's 6 KB stack: nanopb fills the
// whole fixed-size message, and every value is copied into one of the buffers
// below by decode_event_value(). Per-element callback arguments make the
// mapping explicit, so a pair without a value cannot shift the next one's text.
static VoiceAssistantEventResponse incoming_event_response;
static char incoming_event_values[4][ESPHOME_API_EVENT_VALUE_MAX_BYTES + 1];

_Static_assert((int)ESPHOME_API_EVENT_ERROR == (int)VoiceAssistantEvent_VOICE_ASSISTANT_ERROR,
               "event enum no longer matches the generated protobuf enum");
_Static_assert((int)ESPHOME_API_EVENT_RUN_START == (int)VoiceAssistantEvent_VOICE_ASSISTANT_RUN_START,
               "event enum no longer matches the generated protobuf enum");
_Static_assert((int)ESPHOME_API_EVENT_STT_END == (int)VoiceAssistantEvent_VOICE_ASSISTANT_STT_END,
               "event enum no longer matches the generated protobuf enum");
_Static_assert((int)ESPHOME_API_EVENT_TTS_END == (int)VoiceAssistantEvent_VOICE_ASSISTANT_TTS_END,
               "event enum no longer matches the generated protobuf enum");
// These two limits are duplicated between esphome_api.h and the proto options;
// regenerating the proto with smaller values must fail the build rather than
// silently truncate a chunk or overrun the event data array.
_Static_assert(sizeof(((VoiceAssistantAudio *)0)->data.bytes) >= ESPHOME_API_MAX_AUDIO_CHUNK_BYTES,
               "the proto's audio data max_size is smaller than ESPHOME_API_MAX_AUDIO_CHUNK_BYTES");
_Static_assert(sizeof(((VoiceAssistantEventResponse *)0)->data) / sizeof(VoiceAssistantEventData) == 4,
               "update esphome_api_event_data[] in handle_voice_assistant_event_response()");

static void lock_state(void)
{
    configASSERT(state_mutex != NULL);
    xSemaphoreTake(state_mutex, portMAX_DELAY);
}

static void unlock_state(void)
{
    xSemaphoreGive(state_mutex);
}

static size_t encode_varint(uint8_t *buffer, uint32_t value)
{
    size_t length = 0;
    do {
        uint8_t byte = value & 0x7f;
        value >>= 7;
        if (value != 0) {
            byte |= 0x80;
        }
        buffer[length++] = byte;
    } while (value != 0);
    return length;
}

// Sends every byte or reports failure. A short send is not an error by itself;
// only a zero or negative result is.
static bool send_bytes(int socket, const uint8_t *data, size_t data_size)
{
    size_t sent_total = 0;
    while (sent_total < data_size) {
        ssize_t sent = send(socket, data + sent_total, data_size - sent_total, 0);
        if (sent <= 0) {
            return false;
        }
        sent_total += (size_t)sent;
    }
    return true;
}

// Must be called with state_mutex held. shutdown() rather than close() because
// the API task owns closing the descriptor; this just makes its select() and
// recv() return so it can do that.
static void mark_client_write_failed(const char *operation)
{
    ESP_LOGW(TAG, "%s to the client failed (errno %d), dropping the client", operation, errno);
    client_write_failed = true;
    shutdown(client_socket, SHUT_RDWR);
}

// Must be called with state_mutex held.
static esp_err_t send_message_locked(uint32_t message_type, const pb_msgdesc_t *fields, const void *message)
{
    if (client_socket < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    pb_ostream_t stream = pb_ostream_from_buffer(outgoing_message_buffer, sizeof(outgoing_message_buffer));
    if (!pb_encode(&stream, fields, message)) {
        ESP_LOGE(TAG, "Encoding message type %u failed: %s", (unsigned)message_type, PB_GET_ERROR(&stream));
        return ESP_FAIL;
    }

    uint8_t header[1 + 5 + 5];
    size_t header_length = 0;
    header[header_length++] = 0x00;
    header_length += encode_varint(&header[header_length], (uint32_t)stream.bytes_written);
    header_length += encode_varint(&header[header_length], message_type);

    if (!send_bytes(client_socket, header, header_length) ||
        !send_bytes(client_socket, outgoing_message_buffer, stream.bytes_written)) {
        mark_client_write_failed("Sending a frame");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_message(uint32_t message_type, const pb_msgdesc_t *fields, const void *message)
{
    lock_state();
    esp_err_t result = send_message_locked(message_type, fields, message);
    unlock_state();
    return result;
}

// Runs in the API task, which is the only task that accepts or drops clients.
static void drop_client(const char *reason)
{
    int socket_to_close;

    lock_state();
    socket_to_close = client_socket;
    client_socket = -1;
    voice_assistant_subscribed = false;
    voice_assistant_api_audio = false;
    client_write_failed = false;
    client_disconnect_requested = false;
    unlock_state();

    if (socket_to_close < 0) {
        return;
    }

    ESP_LOGW(TAG, "Dropping client: %s", reason);
    // No sender can be using the descriptor any more: senders check
    // client_socket under the same mutex, and this task is the only one that
    // creates descriptors, so the number cannot have been reused.
    close(socket_to_close);

    if (callbacks.disconnected != NULL) {
        callbacks.disconnected(callbacks.context);
    }
}

static bool wait_for_readable(int socket, int64_t deadline_microseconds)
{
    for (;;) {
        int64_t remaining = deadline_microseconds - esp_timer_get_time();
        if (remaining <= 0) {
            return false;
        }

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(socket, &read_set);
        struct timeval timeout = {
            .tv_sec = (time_t)(remaining / 1000000),
            .tv_usec = (suseconds_t)(remaining % 1000000),
        };
        int ready = select(socket + 1, &read_set, NULL, NULL, &timeout);
        if (ready > 0) {
            return true;
        }
        if (ready == 0) {
            return false;
        }
        if (errno != EINTR) {
            ESP_LOGW(TAG, "Waiting for a frame failed (errno %d)", errno);
            return false;
        }
    }
}

// The deadline is absolute for the whole frame. A per-recv() timeout is not
// enough: a peer that sends one byte just before every timeout would never let
// the API task go back to accepting connections or dropping the client.
static bool receive_bytes(int socket, uint8_t *buffer, size_t length, int64_t deadline_microseconds)
{
    size_t received_total = 0;
    while (received_total < length) {
        if (!wait_for_readable(socket, deadline_microseconds)) {
            ESP_LOGW(TAG, "Frame read deadline expired");
            return false;
        }
        ssize_t received = recv(socket, buffer + received_total, length - received_total, 0);
        if (received == 0) {
            ESP_LOGW(TAG, "Client closed the connection");
            return false;
        }
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // The absolute deadline decides when to give up, so just retry.
                continue;
            }
            ESP_LOGW(TAG, "Reading from the client failed (errno %d)", errno);
            return false;
        }
        received_total += (size_t)received;
    }
    return true;
}

static bool receive_varint(int socket, uint32_t *value, int64_t deadline_microseconds)
{
    uint32_t decoded = 0;
    unsigned shift = 0;
    for (unsigned byte_index = 0; byte_index < 5; byte_index++) {
        uint8_t byte;
        if (!receive_bytes(socket, &byte, 1, deadline_microseconds)) {
            return false;
        }
        decoded |= (uint32_t)(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            *value = decoded;
            return true;
        }
        shift += 7;
    }
    ESP_LOGW(TAG, "Frame varint is longer than five bytes");
    return false;
}

static bool discard_bytes(int socket, uint32_t length, int64_t deadline_microseconds)
{
    uint8_t scratch[64];
    while (length > 0) {
        size_t chunk = length < sizeof(scratch) ? length : sizeof(scratch);
        if (!receive_bytes(socket, scratch, chunk, deadline_microseconds)) {
            return false;
        }
        length -= (uint32_t)chunk;
    }
    return true;
}

static frame_result receive_frame(int socket, uint32_t *message_type, uint8_t *body, size_t body_capacity,
                                  size_t *body_size)
{
    int64_t deadline_microseconds = esp_timer_get_time() + FRAME_READ_TIMEOUT_MICROSECONDS;

    uint8_t preamble;
    if (!receive_bytes(socket, &preamble, 1, deadline_microseconds)) {
        return FRAME_RESULT_FAILED;
    }
    if (preamble != 0x00) {
        // A non-zero preamble means noise or encryption, neither of which this
        // server implements. The byte stream cannot be resynchronised safely.
        ESP_LOGW(TAG, "Unexpected preamble 0x%02x, dropping the connection", preamble);
        return FRAME_RESULT_FAILED;
    }

    uint32_t declared_size;
    if (!receive_varint(socket, &declared_size, deadline_microseconds) ||
        !receive_varint(socket, message_type, deadline_microseconds)) {
        return FRAME_RESULT_FAILED;
    }

    if (declared_size > body_capacity) {
        ESP_LOGW(TAG, "Discarding message type %u of %u bytes (buffer is %u bytes)", (unsigned)*message_type,
                 (unsigned)declared_size, (unsigned)body_capacity);
        return discard_bytes(socket, declared_size, deadline_microseconds) ? FRAME_RESULT_SKIPPED
                                                                          : FRAME_RESULT_FAILED;
    }

    if (!receive_bytes(socket, body, declared_size, deadline_microseconds)) {
        return FRAME_RESULT_FAILED;
    }
    *body_size = declared_size;
    return FRAME_RESULT_OK;
}

static void copy_string(char *destination, size_t destination_size, const char *source)
{
    size_t length = strlen(source);
    configASSERT(length < destination_size);
    memcpy(destination, source, length + 1);
}

static void format_mac_address(char *buffer, size_t buffer_size)
{
    uint8_t mac_address[6];
    ESP_ERROR_CHECK(esp_read_mac(mac_address, ESP_MAC_WIFI_STA));
    snprintf(buffer, buffer_size, "%02X:%02X:%02X:%02X:%02X:%02X", mac_address[0], mac_address[1], mac_address[2],
             mac_address[3], mac_address[4], mac_address[5]);
}

static void send_hello_response(void)
{
    char hostname[64];
    mdns_service_get_hostname(hostname, sizeof(hostname));

    HelloResponse response = HelloResponse_init_zero;
    response.api_version_major = API_VERSION_MAJOR;
    response.api_version_minor = API_VERSION_MINOR;
    copy_string(response.server_info, sizeof(response.server_info), SERVER_INFO);
    copy_string(response.name, sizeof(response.name), hostname);
    send_message(MESSAGE_TYPE_HELLO_RESPONSE, HelloResponse_fields, &response);
}

static void send_device_info_response(void)
{
    char hostname[64];
    mdns_service_get_hostname(hostname, sizeof(hostname));

    DeviceInfoResponse response = DeviceInfoResponse_init_zero;
    copy_string(response.name, sizeof(response.name), hostname);
    format_mac_address(response.mac_address, sizeof(response.mac_address));
    copy_string(response.model, sizeof(response.model), DEVICE_MODEL);
    response.voice_assistant_feature_flags =
        VOICE_ASSISTANT_FEATURE_VOICE_ASSISTANT | VOICE_ASSISTANT_FEATURE_API_AUDIO;
    send_message(MESSAGE_TYPE_DEVICE_INFO_RESPONSE, DeviceInfoResponse_fields, &response);
}

static void handle_hello_request(const uint8_t *body, size_t body_size)
{
    HelloRequest request = HelloRequest_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(body, body_size);
    if (pb_decode(&stream, HelloRequest_fields, &request)) {
        ESP_LOGI(TAG, "HelloRequest from %s, API %u.%u", request.client_info,
                 (unsigned)request.api_version_major, (unsigned)request.api_version_minor);
    } else {
        ESP_LOGW(TAG, "Ignoring malformed HelloRequest: %s", PB_GET_ERROR(&stream));
    }
    send_hello_response();
}

static void handle_subscribe_voice_assistant(const uint8_t *body, size_t body_size)
{
    SubscribeVoiceAssistantRequest request = SubscribeVoiceAssistantRequest_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(body, body_size);
    if (!pb_decode(&stream, SubscribeVoiceAssistantRequest_fields, &request)) {
        ESP_LOGW(TAG, "Ignoring malformed SubscribeVoiceAssistantRequest: %s", PB_GET_ERROR(&stream));
        return;
    }

    lock_state();
    voice_assistant_subscribed = request.subscribe;
    voice_assistant_api_audio = (request.flags & VOICE_ASSISTANT_SUBSCRIBE_API_AUDIO) != 0;
    unlock_state();

    ESP_LOGI(TAG, "Voice assistant subscription: subscribe=%d flags=%u", request.subscribe,
             (unsigned)request.flags);
}

static void handle_voice_assistant_response(const uint8_t *body, size_t body_size)
{
    VoiceAssistantResponse response = VoiceAssistantResponse_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(body, body_size);
    if (!pb_decode(&stream, VoiceAssistantResponse_fields, &response)) {
        ESP_LOGW(TAG, "Ignoring malformed VoiceAssistantResponse: %s", PB_GET_ERROR(&stream));
        return;
    }
    ESP_LOGI(TAG, "VoiceAssistantResponse: port=%u error=%d", (unsigned)response.port, response.error);

    if (callbacks.voice_response != NULL) {
        callbacks.voice_response(response.port, response.error, callbacks.context);
    }
}

// Returns a prefix of text that ends on a UTF-8 character boundary. It is only
// called when the value was cut at the copy limit, so the last character may be
// incomplete. Finding the start of that character and checking whether its full
// sequence fits keeps the result valid UTF-8 without altering values that
// already end on a boundary.
static size_t truncate_at_utf8_boundary(const char *text, size_t length)
{
    size_t character_start = length;
    while (character_start > 0 && (text[character_start - 1] & 0xC0) == 0x80) {
        character_start--;
    }
    if (character_start == 0) {
        // Every copied byte was a continuation byte, so no character begins in
        // the prefix and nothing can be kept.
        return 0;
    }

    size_t lead_index = character_start - 1;
    uint8_t lead_byte = (uint8_t)text[lead_index];
    size_t sequence_length;
    if (lead_byte < 0x80) {
        sequence_length = 1;
    } else if ((lead_byte & 0xE0) == 0xC0) {
        sequence_length = 2;
    } else if ((lead_byte & 0xF0) == 0xE0) {
        sequence_length = 3;
    } else if ((lead_byte & 0xF8) == 0xF0) {
        sequence_length = 4;
    } else {
        // Not a valid lead byte, so there is no safe boundary to find. Keeping
        // the bytes as copied is the least destructive option.
        return length;
    }

    if (lead_index + sequence_length <= length) {
        return length;
    }
    return lead_index;
}

// Copies a bounded prefix of an event value and consumes the remainder. The
// remainder has to be consumed here: nanopb calls a string callback repeatedly
// while bytes are left, which would overwrite the prefix with later chunks.
static bool decode_event_value(pb_istream_t *stream, const pb_field_t *field, void **argument)
{
    (void)field;
    char *destination = *argument;

    size_t available = stream->bytes_left;
    size_t copied = available < ESPHOME_API_EVENT_VALUE_MAX_BYTES ? available : ESPHOME_API_EVENT_VALUE_MAX_BYTES;
    if (!pb_read(stream, (pb_byte_t *)destination, copied)) {
        return false;
    }
    if (copied < available) {
        copied = truncate_at_utf8_boundary(destination, copied);
    }
    destination[copied] = '\0';

    if (stream->bytes_left > 0 && !pb_read(stream, NULL, stream->bytes_left)) {
        return false;
    }
    return true;
}

// nanopb sets a repeated submessage's callback fields to NULL by default, so
// they are installed before every decode. The callback argument ties each array
// element to its own buffer, which keeps multiple data pairs independent.
static void reset_incoming_event_response(void)
{
    memset(&incoming_event_response, 0, sizeof(incoming_event_response));
    for (size_t index = 0; index < 4; index++) {
        incoming_event_response.data[index].value.funcs.decode = decode_event_value;
        incoming_event_response.data[index].value.arg = incoming_event_values[index];
        incoming_event_values[index][0] = '\0';
    }
}

static void handle_voice_assistant_event_response(const uint8_t *body, size_t body_size)
{
    reset_incoming_event_response();
    pb_istream_t stream = pb_istream_from_buffer(body, body_size);
    if (!pb_decode(&stream, VoiceAssistantEventResponse_fields, &incoming_event_response)) {
        ESP_LOGW(TAG, "Ignoring malformed VoiceAssistantEventResponse: %s", PB_GET_ERROR(&stream));
        return;
    }

    esphome_api_event_data event_data[4];
    for (pb_size_t index = 0; index < incoming_event_response.data_count; index++) {
        event_data[index].name = incoming_event_response.data[index].name;
        event_data[index].value = incoming_event_values[index];
    }

    if (callbacks.voice_event != NULL) {
        callbacks.voice_event((int)incoming_event_response.event_type, event_data,
                              incoming_event_response.data_count, callbacks.context);
    }
}

static void handle_message(uint32_t message_type, const uint8_t *body, size_t body_size)
{
    switch (message_type) {
    case MESSAGE_TYPE_HELLO_REQUEST:
        handle_hello_request(body, body_size);
        break;
    case MESSAGE_TYPE_DEVICE_INFO_REQUEST:
        send_device_info_response();
        break;
    case MESSAGE_TYPE_LIST_ENTITIES_REQUEST: {
        // The device has no entities; the client only waits for the done marker.
        ListEntitiesDoneResponse done = ListEntitiesDoneResponse_init_zero;
        send_message(MESSAGE_TYPE_LIST_ENTITIES_DONE_RESPONSE, ListEntitiesDoneResponse_fields, &done);
        break;
    }
    case MESSAGE_TYPE_SUBSCRIBE_STATES_REQUEST:
        break;
    case MESSAGE_TYPE_SUBSCRIBE_VOICE_ASSISTANT_REQUEST:
        handle_subscribe_voice_assistant(body, body_size);
        break;
    case MESSAGE_TYPE_PING_REQUEST: {
        PingResponse response = PingResponse_init_zero;
        send_message(MESSAGE_TYPE_PING_RESPONSE, PingResponse_fields, &response);
        break;
    }
    case MESSAGE_TYPE_PING_RESPONSE:
        break;
    case MESSAGE_TYPE_DISCONNECT_REQUEST: {
        DisconnectResponse response = DisconnectResponse_init_zero;
        send_message(MESSAGE_TYPE_DISCONNECT_RESPONSE, DisconnectResponse_fields, &response);
        drop_client("client sent DisconnectRequest");
        break;
    }
    case MESSAGE_TYPE_VOICE_ASSISTANT_RESPONSE:
        handle_voice_assistant_response(body, body_size);
        break;
    case MESSAGE_TYPE_VOICE_ASSISTANT_EVENT_RESPONSE:
        handle_voice_assistant_event_response(body, body_size);
        break;
    default:
        // Includes VoiceAssistantAudio, which this server never receives, and
        // every message type that the reduced protocol does not implement.
        ESP_LOGW(TAG, "Ignoring message type %u of %u bytes", (unsigned)message_type, (unsigned)body_size);
        break;
    }
}

static void set_socket_option(int socket, int level, int option, const void *value, socklen_t value_size,
                              const char *name)
{
    if (setsockopt(socket, level, option, value, value_size) != 0) {
        ESP_LOGW(TAG, "Setting %s failed (errno %d)", name, errno);
    }
}

static void accept_client(void)
{
    struct sockaddr_in peer_address;
    socklen_t peer_address_size = sizeof(peer_address);
    int new_socket = accept(listener_socket, (struct sockaddr *)&peer_address, &peer_address_size);
    if (new_socket < 0) {
        ESP_LOGW(TAG, "accept failed (errno %d)", errno);
        return;
    }

    // Protocol frames are small and latency matters for voice, so do not wait
    // to coalesce them. The timeouts stop a stalled peer from blocking the API
    // task indefinitely.
    int no_delay = 1;
    set_socket_option(new_socket, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay), "TCP_NODELAY");
    struct timeval receive_timeout = {.tv_sec = FRAME_READ_TIMEOUT_SECONDS, .tv_usec = 0};
    set_socket_option(new_socket, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout),
                      "SO_RCVTIMEO");
    struct timeval send_timeout = {.tv_sec = SEND_TIMEOUT_SECONDS, .tv_usec = 0};
    set_socket_option(new_socket, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout), "SO_SNDTIMEO");

    // After a network drop the client reconnects while the old socket can still
    // look open, so the newest connection always wins.
    drop_client("replaced by a new connection");

    lock_state();
    client_socket = new_socket;
    voice_assistant_subscribed = false;
    voice_assistant_api_audio = false;
    client_write_failed = false;
    client_disconnect_requested = false;
    last_message_time_microseconds = esp_timer_get_time();
    unlock_state();

    ESP_LOGI(TAG, "Client connected from %s, free heap %u bytes", inet_ntoa(peer_address.sin_addr),
             (unsigned)esp_get_free_heap_size());

    if (callbacks.connected != NULL) {
        callbacks.connected(callbacks.context);
    }
}

static void handle_readable_client(int socket)
{
    uint32_t message_type;
    size_t body_size = 0;
    frame_result result = receive_frame(socket, &message_type, incoming_message_buffer, sizeof(incoming_message_buffer),
                                        &body_size);
    if (result == FRAME_RESULT_FAILED) {
        drop_client("failed to read a frame");
        return;
    }

    // A discarded oversize frame still proves the client is alive.
    lock_state();
    last_message_time_microseconds = esp_timer_get_time();
    unlock_state();

    if (result == FRAME_RESULT_OK) {
        handle_message(message_type, incoming_message_buffer, body_size);
    }
}

static void api_task(void *argument)
{
    (void)argument;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    while (true) {
        // The select below wakes at least once a second, so this is always far
        // inside the timeout. A frame that stalls mid-read is bounded by
        // FRAME_READ_TIMEOUT_SECONDS, which is why the watchdog timeout is
        // larger than that plus a send.
        esp_task_wdt_reset();

        bool write_failed;
        bool disconnect_requested;
        int socket_to_watch;

        lock_state();
        write_failed = client_write_failed;
        disconnect_requested = client_disconnect_requested;
        unlock_state();
        if (write_failed) {
            drop_client("a write to the client failed");
        } else if (disconnect_requested) {
            drop_client("the session asked for the client to be disconnected");
        }

        lock_state();
        socket_to_watch = client_socket;
        unlock_state();

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(listener_socket, &read_set);
        int highest_socket = listener_socket;
        if (socket_to_watch >= 0) {
            FD_SET(socket_to_watch, &read_set);
            if (socket_to_watch > highest_socket) {
                highest_socket = socket_to_watch;
            }
        }

        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        int ready = select(highest_socket + 1, &read_set, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGE(TAG, "select failed (errno %d)", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        bool accepted = false;
        if (ready > 0 && FD_ISSET(listener_socket, &read_set)) {
            accept_client();
            accepted = true;
        }
        // When a new client was accepted, the socket number in read_set is the
        // one that was just closed; wait for the next iteration instead.
        if (!accepted && ready > 0 && socket_to_watch >= 0 && FD_ISSET(socket_to_watch, &read_set)) {
            handle_readable_client(socket_to_watch);
        }

        lock_state();
        int64_t last_message_time = last_message_time_microseconds;
        bool connected = client_socket >= 0;
        unlock_state();
        if (connected && esp_timer_get_time() - last_message_time > IDLE_TIMEOUT_MICROSECONDS) {
            drop_client("no messages for 90 seconds");
        }
    }
}

void esphome_api_set_callbacks(const esphome_api_callbacks *new_callbacks)
{
    // Deliberately not locked: this must be called before esphome_api_start(),
    // and the API task is the only reader afterwards.
    callbacks = *new_callbacks;
}

void esphome_api_start(void)
{
    state_mutex = xSemaphoreCreateMutex();
    configASSERT(state_mutex != NULL);

    listener_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_socket < 0) {
        ESP_LOGE(TAG, "Creating the API socket failed (errno %d)", errno);
        abort();
    }

    struct sockaddr_in listen_address = {
        .sin_family = AF_INET,
        .sin_port = htons(ESPHOME_API_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    int reuse_address = 1;
    set_socket_option(listener_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address),
                      "SO_REUSEADDR");
    if (bind(listener_socket, (struct sockaddr *)&listen_address, sizeof(listen_address)) != 0) {
        ESP_LOGE(TAG, "Binding to port %d failed (errno %d)", ESPHOME_API_PORT, errno);
        abort();
    }
    if (listen(listener_socket, 1) != 0) {
        ESP_LOGE(TAG, "Listening on port %d failed (errno %d)", ESPHOME_API_PORT, errno);
        abort();
    }

    BaseType_t created = xTaskCreate(api_task, "esphome_api", API_TASK_STACK_BYTES, NULL, API_TASK_PRIORITY, NULL);
    configASSERT(created == pdPASS);

    ESP_LOGI(TAG, "Listening on port %d", ESPHOME_API_PORT);
}

bool esphome_api_client_connected(void)
{
    lock_state();
    bool connected = client_socket >= 0;
    unlock_state();
    return connected;
}

void esphome_api_disconnect_client(void)
{
    lock_state();
    if (client_socket >= 0) {
        ESP_LOGW(TAG, "Disconnecting the client at the session's request");
        client_disconnect_requested = true;
        // shutdown() rather than close(): the API task owns the descriptor, so
        // this only wakes its select() and lets it run the normal drop path.
        shutdown(client_socket, SHUT_RDWR);
    }
    unlock_state();
}

bool esphome_api_voice_assistant_subscribed(void)
{
    lock_state();
    bool subscribed = client_socket >= 0 && voice_assistant_subscribed && voice_assistant_api_audio;
    unlock_state();
    return subscribed;
}

esp_err_t esphome_api_send_voice_assistant_request(bool start, const char *wake_word_phrase)
{
    VoiceAssistantRequest request = VoiceAssistantRequest_init_zero;
    request.start = start;
    if (wake_word_phrase != NULL) {
        size_t phrase_length = strlen(wake_word_phrase);
        if (phrase_length >= sizeof(request.wake_word_phrase)) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(request.wake_word_phrase, wake_word_phrase, phrase_length + 1);
    }

    return send_message(MESSAGE_TYPE_VOICE_ASSISTANT_REQUEST, VoiceAssistantRequest_fields, &request);
}

esp_err_t esphome_api_send_audio(const uint8_t *data, size_t data_size, bool end)
{
    if (data_size > ESPHOME_API_MAX_AUDIO_CHUNK_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (data_size > 0 && data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result;
    lock_state();
    if (client_socket < 0) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        // The audio message is static, and the encoder reads it while the lock
        // is held, so no second audio sender can overwrite it mid-encode.
        outgoing_audio.data.size = (pb_size_t)data_size;
        if (data_size > 0) {
            memcpy(outgoing_audio.data.bytes, data, data_size);
        }
        outgoing_audio.end = end;
        result = send_message_locked(MESSAGE_TYPE_VOICE_ASSISTANT_AUDIO, VoiceAssistantAudio_fields, &outgoing_audio);
    }
    unlock_state();
    return result;
}
