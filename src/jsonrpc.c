#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_DEFAULT_ALIGNMENT alignof(max_align_t)
#define ARENA_IMPLEMENTATION
#include "arena.h"

#include "jsonrpc.h"

constexpr size_t INITIAL_BUFFER_CAP = 4'096;
constexpr size_t MAX_MESSAGE_BYTES = 1'048'576U; // 1 MiB per JSON-RPC message
constexpr size_t MAX_BUFFER_BYTES = 2'097'152U;  // 2 MiB cap for partial lines
constexpr size_t JSONRPC_ARENA_BYTES = MAX_MESSAGE_BYTES * 2U;

constexpr int32_t JSONRPC_ERR_PARSE = -32'700;
constexpr int32_t JSONRPC_ERR_INVALID_REQUEST = -32'600;
constexpr int32_t JSONRPC_ERR_METHOD_NOT_FOUND = -32'601;
constexpr int32_t JSONRPC_ERR_INVALID_PARAMS = -32'602;
constexpr int32_t JSONRPC_ERR_INTERNAL = -32'603;

typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
} rpc_buffer_t;

typedef struct {
  uint32_t tag;
  uint32_t origin;
} jsonrpc_alloc_header_t;

constexpr uint32_t JSONRPC_ALLOC_TAG = 0x4A52'5043; // "JRPC"
constexpr uint32_t JSONRPC_ALLOC_ORIGIN_ARENA = 1U;
constexpr uint32_t JSONRPC_ALLOC_ORIGIN_HEAP = 2U;
constexpr size_t JSONRPC_ALLOC_HEADER_BYTES =
    ((sizeof(jsonrpc_alloc_header_t) + alignof(max_align_t) - 1U) /
     alignof(max_align_t)) *
    alignof(max_align_t);

typedef struct {
  Arena *prev;
  bool changed;
} jsonrpc_arena_scope_t;

static Arena *g_current_arena = nullptr;
static bool g_parson_allocator_initialized = false;

struct jsonrpc_conn_s {
  jsonrpc_transport_t transport;
  jsonrpc_callbacks_t callbacks;
  void *user_context;
  bool closed;
  rpc_buffer_t inbound;
  Arena *arena;
};

[[nodiscard]]
static void *jsonrpc_arena_malloc(size_t size) {
  if (size == 0U) {
    return nullptr;
  }

  if (size > SIZE_MAX - JSONRPC_ALLOC_HEADER_BYTES) {
    return nullptr;
  }

  const size_t total_size = size + JSONRPC_ALLOC_HEADER_BYTES;
  void *block = nullptr;
  uint32_t origin = JSONRPC_ALLOC_ORIGIN_HEAP;

  if (g_current_arena != nullptr) {
    block = arena_alloc(g_current_arena, total_size);
    if (block != nullptr) {
      origin = JSONRPC_ALLOC_ORIGIN_ARENA;
    }
  }

  if (block == nullptr) {
    block = calloc(1U, total_size);
    origin = JSONRPC_ALLOC_ORIGIN_HEAP;
  }

  if (block == nullptr) {
    return nullptr;
  }

  auto header = (jsonrpc_alloc_header_t *)block;
  header->tag = JSONRPC_ALLOC_TAG;
  header->origin = origin;

  return (uint8_t *)block + JSONRPC_ALLOC_HEADER_BYTES;
}

static void jsonrpc_arena_free(void *ptr) {
  if (ptr == nullptr) {
    return;
  }

  auto raw = (uint8_t *)ptr - JSONRPC_ALLOC_HEADER_BYTES;
  auto header = (jsonrpc_alloc_header_t *)raw;
  if (header->tag != JSONRPC_ALLOC_TAG) {
    return;
  }

  if (header->origin == JSONRPC_ALLOC_ORIGIN_HEAP) {
    free(raw);
  }
}

static void jsonrpc_init_parson_allocator() {
  if (g_parson_allocator_initialized) {
    return;
  }

  json_set_allocation_functions(jsonrpc_arena_malloc, jsonrpc_arena_free);
  g_parson_allocator_initialized = true;
}

static jsonrpc_arena_scope_t jsonrpc_arena_scope_begin(Arena *arena) {
  jsonrpc_arena_scope_t scope = {.prev = g_current_arena, .changed = false};
  if (arena == nullptr || g_current_arena == arena) {
    return scope;
  }

  g_current_arena = arena;
  scope.changed = true;
  return scope;
}

static void jsonrpc_arena_scope_end(Arena *arena, jsonrpc_arena_scope_t scope) {
  if (!scope.changed) {
    return;
  }

  if (arena != nullptr) {
    arena_clear(arena);
  }
  g_current_arena = scope.prev;
}

static const char *jsonrpc_default_message(int32_t code) {
  switch (code) {
  case JSONRPC_ERR_PARSE:
    return "Parse error";
  case JSONRPC_ERR_INVALID_REQUEST:
    return "Invalid Request";
  case JSONRPC_ERR_METHOD_NOT_FOUND:
    return "Method not found";
  case JSONRPC_ERR_INVALID_PARAMS:
    return "Invalid params";
  case JSONRPC_ERR_INTERNAL:
    return "Internal error";
  default:
    return "Server error";
  }
}

static void rpc_buffer_free(rpc_buffer_t *buffer) {
  if (buffer->data != nullptr) {
    free(buffer->data);
    buffer->data = nullptr;
  }
  buffer->len = 0U;
  buffer->cap = 0U;
}

[[nodiscard]]
static bool rpc_buffer_reserve(rpc_buffer_t *buffer, size_t desired) {
  if (desired <= buffer->cap) {
    return true;
  }

  auto new_cap = buffer->cap == 0U ? INITIAL_BUFFER_CAP : buffer->cap;
  while (new_cap < desired) {
    new_cap *= 2U;
  }

  auto new_data = (uint8_t *)calloc(new_cap, sizeof(uint8_t));
  if (new_data == nullptr) {
    return false;
  }

  if (buffer->data != nullptr && buffer->len > 0U) {
    memcpy(new_data, buffer->data, buffer->len);
    free(buffer->data);
  }

  buffer->data = new_data;
  buffer->cap = new_cap;
  return true;
}

[[nodiscard]]
static bool rpc_buffer_append(rpc_buffer_t *buffer, const uint8_t *data,
                              size_t len) {
  if (len == 0U) {
    return true;
  }
  const size_t required = buffer->len + len;
  if (required > MAX_BUFFER_BYTES) {
    return false;
  }
  if (!rpc_buffer_reserve(buffer, required)) {
    return false;
  }

  memcpy(buffer->data + buffer->len, data, len);
  buffer->len += len;
  return true;
}

static void rpc_buffer_consume(rpc_buffer_t *buffer, size_t count) {
  if (count == 0U || buffer->len < count) {
    return;
  }
  const size_t remaining = buffer->len - count;
  if (remaining > 0U) {
    memmove(buffer->data, buffer->data + count, remaining);
  }
  buffer->len = remaining;
}

[[nodiscard]]
static bool jsonrpc_send_value(jsonrpc_conn_t *conn, const JSON_Value *value) {
  if (conn == nullptr || conn->transport.send_raw == nullptr ||
      value == nullptr) {
    return false;
  }

  auto serialized = json_serialize_to_string(value);
  if (serialized == nullptr) {
    return false;
  }

  const size_t len = strlen(serialized);
  auto payload = (uint8_t *)jsonrpc_arena_malloc(len + 1U);
  if (payload == nullptr) {
    json_free_serialized_string(serialized);
    return false;
  }

  memcpy(payload, serialized, len);
  payload[len] = '\n';
  conn->transport.send_raw(&conn->transport, payload, len + 1U);
  jsonrpc_arena_free(payload);
  json_free_serialized_string(serialized);
  return true;
}

[[nodiscard]]
static JSON_Value *jsonrpc_copy_id(const JSON_Value *id) {
  if (id == nullptr) {
    return json_value_init_null();
  }

  const JSON_Value_Type type = json_value_get_type(id);
  if (type != JSONString && type != JSONNumber && type != JSONNull) {
    return json_value_init_null();
  }

  auto copy = json_value_deep_copy(id);
  if (copy == nullptr) {
    return json_value_init_null();
  }
  return copy;
}

[[nodiscard]]
static JSON_Value *jsonrpc_build_error(const JSON_Value *id, int32_t code,
                                       const char *message) {
  auto response = json_value_init_object();
  if (response == nullptr) {
    return nullptr;
  }

  auto response_obj = json_value_get_object(response);
  if (json_object_set_string(response_obj, "jsonrpc", "2.0") != JSONSuccess) {
    json_value_free(response);
    return nullptr;
  }

  auto id_copy = jsonrpc_copy_id(id);
  if (id_copy == nullptr ||
      json_object_set_value(response_obj, "id", id_copy) != JSONSuccess) {
    if (id_copy != nullptr) {
      json_value_free(id_copy);
    }
    json_value_free(response);
    return nullptr;
  }

  auto error_value = json_value_init_object();
  if (error_value == nullptr) {
    json_value_free(response);
    return nullptr;
  }

  auto error_obj = json_value_get_object(error_value);
  if (json_object_set_number(error_obj, "code", (double)code) != JSONSuccess) {
    json_value_free(error_value);
    json_value_free(response);
    return nullptr;
  }

  const char *use_message =
      message != nullptr ? message : jsonrpc_default_message(code);
  if (json_object_set_string(error_obj, "message", use_message) !=
      JSONSuccess) {
    json_value_free(error_value);
    json_value_free(response);
    return nullptr;
  }

  if (json_object_set_value(response_obj, "error", error_value) !=
      JSONSuccess) {
    json_value_free(error_value);
    json_value_free(response);
    return nullptr;
  }

  return response;
}

[[nodiscard]]
static JSON_Value *jsonrpc_build_result(const JSON_Value *id,
                                        JSON_Value *result) {
  if (result == nullptr) {
    return nullptr;
  }

  auto response = json_value_init_object();
  if (response == nullptr) {
    json_value_free(result);
    return nullptr;
  }

  auto response_obj = json_value_get_object(response);
  if (json_object_set_string(response_obj, "jsonrpc", "2.0") != JSONSuccess) {
    json_value_free(result);
    json_value_free(response);
    return nullptr;
  }

  auto id_copy = jsonrpc_copy_id(id);
  if (id_copy == nullptr ||
      json_object_set_value(response_obj, "id", id_copy) != JSONSuccess) {
    if (id_copy != nullptr) {
      json_value_free(id_copy);
    }
    json_value_free(result);
    json_value_free(response);
    return nullptr;
  }

  if (json_object_set_value(response_obj, "result", result) != JSONSuccess) {
    json_value_free(result);
    json_value_free(response);
    return nullptr;
  }

  return response;
}

[[nodiscard]]
static bool jsonrpc_id_is_valid(const JSON_Value *id) {
  if (id == nullptr) {
    return false;
  }
  const JSON_Value_Type type = json_value_get_type(id);
  return type == JSONString || type == JSONNumber || type == JSONNull;
}

[[nodiscard]]
static bool jsonrpc_params_is_valid(const JSON_Value *params) {
  if (params == nullptr) {
    return true;
  }
  const JSON_Value_Type type = json_value_get_type(params);
  return type == JSONArray || type == JSONObject;
}

[[nodiscard]]
static JSON_Value *jsonrpc_process_object(jsonrpc_conn_t *conn,
                                          const JSON_Value *value) {
  auto obj = json_value_get_object(value);
  if (obj == nullptr) {
    return jsonrpc_build_error(nullptr, JSONRPC_ERR_INVALID_REQUEST, nullptr);
  }

  const char *version = json_object_get_string(obj, "jsonrpc");
  if (version == nullptr || strcmp(version, "2.0") != 0) {
    return jsonrpc_build_error(nullptr, JSONRPC_ERR_INVALID_REQUEST, nullptr);
  }

  const char *method = json_object_get_string(obj, "method");
  if (method == nullptr) {
    return jsonrpc_build_error(nullptr, JSONRPC_ERR_INVALID_REQUEST, nullptr);
  }

  const bool has_id = json_object_has_value(obj, "id");
  const JSON_Value *id = has_id ? json_object_get_value(obj, "id") : nullptr;
  if (has_id && !jsonrpc_id_is_valid(id)) {
    return jsonrpc_build_error(nullptr, JSONRPC_ERR_INVALID_REQUEST, nullptr);
  }

  const JSON_Value *params = json_object_get_value(obj, "params");
  if (!jsonrpc_params_is_valid(params)) {
    if (!has_id) {
      return nullptr;
    }
    return jsonrpc_build_error(id, JSONRPC_ERR_INVALID_PARAMS, nullptr);
  }

  if (!has_id) {
    if (conn->callbacks.on_notification != nullptr) {
      conn->callbacks.on_notification(conn, method, params);
    }
    return nullptr;
  }

  if (conn->callbacks.on_request == nullptr) {
    return jsonrpc_build_error(id, JSONRPC_ERR_METHOD_NOT_FOUND, nullptr);
  }

  jsonrpc_response_t response = {
      .result = nullptr, .error_code = 0, .error_message = nullptr};
  const bool handled =
      conn->callbacks.on_request(conn, method, params, &response);

  if (!handled) {
    if (response.result != nullptr) {
      json_value_free(response.result);
    }
    return jsonrpc_build_error(id, JSONRPC_ERR_METHOD_NOT_FOUND, nullptr);
  }

  if (response.error_code != 0) {
    if (response.result != nullptr) {
      json_value_free(response.result);
    }
    return jsonrpc_build_error(id, response.error_code, response.error_message);
  }

  if (response.result == nullptr) {
    return jsonrpc_build_error(id, JSONRPC_ERR_INTERNAL,
                               "Handler returned no result");
  }

  return jsonrpc_build_result(id, response.result);
}

[[nodiscard]]
static JSON_Value *jsonrpc_process_value(jsonrpc_conn_t *conn,
                                         const JSON_Value *value) {
  if (value == nullptr) {
    return jsonrpc_build_error(nullptr, JSONRPC_ERR_INVALID_REQUEST, nullptr);
  }

  const JSON_Value_Type type = json_value_get_type(value);
  if (type == JSONArray) {
    auto array = json_value_get_array(value);
    const size_t count = json_array_get_count(array);
    if (count == 0U) {
      return jsonrpc_build_error(nullptr, JSONRPC_ERR_INVALID_REQUEST, nullptr);
    }

    auto response_array_value = json_value_init_array();
    if (response_array_value == nullptr) {
      return jsonrpc_build_error(nullptr, JSONRPC_ERR_INTERNAL, nullptr);
    }
    auto response_array = json_value_get_array(response_array_value);
    size_t response_count = 0U;

    for (size_t i = 0U; i < count; ++i) {
      const JSON_Value *item = json_array_get_value(array, i);
      JSON_Value *response = jsonrpc_process_object(conn, item);
      if (response == nullptr) {
        continue;
      }
      if (json_array_append_value(response_array, response) != JSONSuccess) {
        json_value_free(response);
        json_value_free(response_array_value);
        return jsonrpc_build_error(nullptr, JSONRPC_ERR_INTERNAL, nullptr);
      }
      response_count += 1U;
    }

    if (response_count == 0U) {
      json_value_free(response_array_value);
      return nullptr;
    }

    return response_array_value;
  }

  return jsonrpc_process_object(conn, value);
}

[[nodiscard]] jsonrpc_conn_t *jsonrpc_conn_new(jsonrpc_transport_t transport,
                                               jsonrpc_callbacks_t callbacks,
                                               void *external_context) {
  jsonrpc_init_parson_allocator();

  auto conn = (jsonrpc_conn_t *)calloc(1, sizeof(jsonrpc_conn_t));
  if (conn == nullptr) {
    return nullptr;
  }

  conn->transport = transport;
  conn->callbacks = callbacks;
  conn->user_context = external_context;
  conn->closed = false;
  conn->inbound.data = nullptr;
  conn->inbound.len = 0U;
  conn->inbound.cap = 0U;
  conn->arena = arena_create(JSONRPC_ARENA_BYTES);

  if (conn->callbacks.on_open != nullptr) {
    conn->callbacks.on_open(conn);
  }

  return conn;
}

void jsonrpc_conn_free(jsonrpc_conn_t *conn) {
  if (conn == nullptr) {
    return;
  }

  if (!conn->closed && conn->callbacks.on_close != nullptr) {
    conn->callbacks.on_close(conn);
  }

  conn->closed = true;
  rpc_buffer_free(&conn->inbound);
  if (conn->arena != nullptr) {
    if (g_current_arena == conn->arena) {
      g_current_arena = nullptr;
    }
    arena_destroy(conn->arena);
    conn->arena = nullptr;
  }
  free(conn);
}

void jsonrpc_conn_feed(jsonrpc_conn_t *conn, const uint8_t *data, size_t len) {
  if (conn == nullptr || data == nullptr || len == 0U || conn->closed) {
    return;
  }

  jsonrpc_init_parson_allocator();

  if (!rpc_buffer_append(&conn->inbound, data, len)) {
    const bool sent = jsonrpc_conn_send_error(
        conn, nullptr, JSONRPC_ERR_INVALID_REQUEST, "Request too large");
    if (!sent && conn->transport.close != nullptr) {
      conn->transport.close(&conn->transport);
      return;
    }
    if (conn->transport.close != nullptr) {
      conn->transport.close(&conn->transport);
    }
    return;
  }

  while (true) {
    void *newline = memchr(conn->inbound.data, '\n', conn->inbound.len);
    if (newline == nullptr) {
      return;
    }

    const size_t newline_index =
        (size_t)((uint8_t *)newline - conn->inbound.data);
    size_t line_len = newline_index;
    const size_t consume_len = line_len + 1U;

    if (line_len > 0U && conn->inbound.data[line_len - 1U] == '\r') {
      line_len -= 1U;
    }

    if (line_len == 0U) {
      rpc_buffer_consume(&conn->inbound, consume_len);
      continue;
    }

    if (line_len > MAX_MESSAGE_BYTES) {
      const bool sent = jsonrpc_conn_send_error(
          conn, nullptr, JSONRPC_ERR_INVALID_REQUEST, "Request too large");
      if (!sent && conn->transport.close != nullptr) {
        conn->transport.close(&conn->transport);
        return;
      }
      if (conn->transport.close != nullptr) {
        conn->transport.close(&conn->transport);
      }
      return;
    }

    const jsonrpc_arena_scope_t scope =
        jsonrpc_arena_scope_begin(conn->arena);
    JSON_Value *request = nullptr;
    JSON_Value *response = nullptr;
    bool close_connection = false;

    auto line = (char *)jsonrpc_arena_malloc(line_len + 1U);
    if (line == nullptr) {
      const bool sent =
          jsonrpc_conn_send_error(conn, nullptr, JSONRPC_ERR_INTERNAL, nullptr);
      if (!sent && conn->transport.close != nullptr) {
        conn->transport.close(&conn->transport);
        close_connection = true;
      }
      rpc_buffer_consume(&conn->inbound, consume_len);
      goto cleanup_message;
    }

    memcpy(line, conn->inbound.data, line_len);
    line[line_len] = '\0';
    rpc_buffer_consume(&conn->inbound, consume_len);

    request = json_parse_string(line);
    jsonrpc_arena_free(line);

    if (request == nullptr) {
      const bool sent =
          jsonrpc_conn_send_error(conn, nullptr, JSONRPC_ERR_PARSE, nullptr);
      if (!sent && conn->transport.close != nullptr) {
        conn->transport.close(&conn->transport);
        close_connection = true;
        goto cleanup_message;
      }
      goto cleanup_message;
    }

    response = jsonrpc_process_value(conn, request);
    if (response != nullptr) {
      const bool sent = jsonrpc_send_value(conn, response);
      if (!sent && conn->transport.close != nullptr) {
        conn->transport.close(&conn->transport);
        close_connection = true;
        goto cleanup_message;
      }
    }

  cleanup_message:
    if (response != nullptr) {
      json_value_free(response);
    }
    if (request != nullptr) {
      json_value_free(request);
    }
    jsonrpc_arena_scope_end(conn->arena, scope);
    if (close_connection) {
      return;
    }
  }
}

[[nodiscard]] bool jsonrpc_conn_send_result(jsonrpc_conn_t *conn,
                                            const JSON_Value *id,
                                            JSON_Value *result) {
  if (conn == nullptr || result == nullptr) {
    if (result != nullptr) {
      json_value_free(result);
    }
    return false;
  }

  jsonrpc_init_parson_allocator();
  const jsonrpc_arena_scope_t scope =
      jsonrpc_arena_scope_begin(conn->arena);

  JSON_Value *response = jsonrpc_build_result(id, result);
  if (response == nullptr) {
    jsonrpc_arena_scope_end(conn->arena, scope);
    return false;
  }

  const bool sent = jsonrpc_send_value(conn, response);
  json_value_free(response);
  jsonrpc_arena_scope_end(conn->arena, scope);
  return sent;
}

[[nodiscard]] bool jsonrpc_conn_send_error(jsonrpc_conn_t *conn,
                                           const JSON_Value *id, int32_t code,
                                           const char *message) {
  if (conn == nullptr) {
    return false;
  }

  jsonrpc_init_parson_allocator();
  const jsonrpc_arena_scope_t scope =
      jsonrpc_arena_scope_begin(conn->arena);

  JSON_Value *response = jsonrpc_build_error(id, code, message);
  if (response == nullptr) {
    jsonrpc_arena_scope_end(conn->arena, scope);
    return false;
  }

  const bool sent = jsonrpc_send_value(conn, response);
  json_value_free(response);
  jsonrpc_arena_scope_end(conn->arena, scope);
  return sent;
}

[[nodiscard]] void *jsonrpc_conn_get_context(jsonrpc_conn_t *conn) {
  if (conn == nullptr) {
    return nullptr;
  }
  return conn->user_context;
}
