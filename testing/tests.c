#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsonrpc/jsonrpc.h"

constexpr int32_t JSONRPC_ERR_PARSE = -32'700;
constexpr int32_t JSONRPC_ERR_INVALID_REQUEST = -32'600;
constexpr int32_t JSONRPC_ERR_METHOD_NOT_FOUND = -32'601;
constexpr int32_t JSONRPC_ERR_INVALID_PARAMS = -32'602;

constexpr size_t TEST_MAX_MESSAGES = 32U;

typedef struct {
  char *messages[TEST_MAX_MESSAGES];
  size_t message_count;
  bool fail_send;
  size_t close_calls;
} test_transport_state_t;

typedef struct {
  size_t open_count;
  size_t close_count;
  size_t request_count;
  size_t notification_count;
  const char *last_method;
} test_callback_state_t;

typedef struct {
  test_transport_state_t transport_state;
  test_callback_state_t callback_state;
} test_context_t;

static test_context_t *g_active_test_context = nullptr;

#define ASSERT_TRUE(expr)                                                        \
  do {                                                                           \
    if (!(expr)) {                                                               \
      fprintf(stderr, "assertion failed: %s (%s:%d)\n", #expr, __FILE__,        \
              __LINE__);                                                         \
      return false;                                                              \
    }                                                                            \
  } while (false)

static void test_transport_state_reset(test_transport_state_t *state) {
  if (state == nullptr) {
    return;
  }
  for (size_t i = 0U; i < state->message_count; ++i) {
    free(state->messages[i]);
    state->messages[i] = nullptr;
  }
  state->message_count = 0U;
  state->fail_send = false;
  state->close_calls = 0U;
}

static bool test_send_raw(jsonrpc_transport_t *self, const uint8_t *data,
                          size_t len) {
  if (self == nullptr || self->user_data == nullptr || data == nullptr ||
      len == 0U) {
    return false;
  }

  auto state = (test_transport_state_t *)self->user_data;
  if (state->fail_send) {
    return false;
  }

  if (state->message_count >= TEST_MAX_MESSAGES) {
    return false;
  }

  auto copy = (char *)calloc(len + 1U, sizeof(char));
  if (copy == nullptr) {
    return false;
  }

  memcpy(copy, data, len);
  copy[len] = '\0';
  state->messages[state->message_count] = copy;
  state->message_count += 1U;
  return true;
}

static void test_close(jsonrpc_transport_t *self) {
  if (self == nullptr || self->user_data == nullptr) {
    return;
  }
  auto state = (test_transport_state_t *)self->user_data;
  state->close_calls += 1U;
}

static void on_open(jsonrpc_conn_t *conn) {
  auto context = (test_context_t *)jsonrpc_conn_get_context(conn);
  if (context == nullptr || context != g_active_test_context) {
    return;
  }
  context->callback_state.open_count += 1U;
}

static void on_close(jsonrpc_conn_t *conn) {
  (void)conn;
  if (g_active_test_context == nullptr) {
    return;
  }
  g_active_test_context->callback_state.close_count += 1U;
}

static bool on_request(jsonrpc_conn_t *conn, const char *method,
                       const JSON_Value *params [[maybe_unused]],
                       jsonrpc_response_t *response) {
  auto context = (test_context_t *)jsonrpc_conn_get_context(conn);
  if (context == nullptr || context != g_active_test_context || method == nullptr ||
      response == nullptr) {
    return false;
  }

  context->callback_state.request_count += 1U;
  context->callback_state.last_method = method;

  if (strcmp(method, "ping") == 0) {
    response->result = json_value_init_string("pong");
    return response->result != nullptr;
  }

  return false;
}

static void on_notification(jsonrpc_conn_t *conn, const char *method,
                            const JSON_Value *params [[maybe_unused]]) {
  auto context = (test_context_t *)jsonrpc_conn_get_context(conn);
  if (context == nullptr || context != g_active_test_context) {
    return;
  }
  context->callback_state.notification_count += 1U;
  context->callback_state.last_method = method;
}

[[nodiscard]] static jsonrpc_conn_t *test_conn_new(test_context_t *context) {
  if (context == nullptr) {
    return nullptr;
  }

  jsonrpc_transport_t transport = {.user_data = &context->transport_state,
                                   .send_raw = test_send_raw,
                                   .close = test_close};

  jsonrpc_callbacks_t callbacks = {.on_open = on_open,
                                   .on_close = on_close,
                                   .on_request = on_request,
                                   .on_notification = on_notification};

  return jsonrpc_conn_new(transport, callbacks, context);
}

[[nodiscard]] static JSON_Value *test_parse_sent_json(
    const test_transport_state_t *state, size_t index) {
  if (state == nullptr || index >= state->message_count) {
    return nullptr;
  }

  const char *message = state->messages[index];
  if (message == nullptr) {
    return nullptr;
  }

  const size_t message_len = strlen(message);
  if (message_len == 0U || message[message_len - 1U] != '\n') {
    return nullptr;
  }

  auto copy = (char *)calloc(message_len, sizeof(char));
  if (copy == nullptr) {
    return nullptr;
  }

  memcpy(copy, message, message_len - 1U);
  copy[message_len - 1U] = '\0';
  auto value = json_parse_string(copy);
  free(copy);
  return value;
}

static bool test_basic_request_response() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);
  ASSERT_TRUE(context.callback_state.open_count == 1U);

  const char *request = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n";
  jsonrpc_conn_feed(conn, (const uint8_t *)request, strlen(request));

  ASSERT_TRUE(context.callback_state.request_count == 1U);
  ASSERT_TRUE(context.transport_state.message_count == 1U);

  auto response = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(response != nullptr);
  ASSERT_TRUE(json_value_get_type(response) == JSONObject);

  auto response_obj = json_value_get_object(response);
  ASSERT_TRUE(strcmp(json_object_get_string(response_obj, "jsonrpc"), "2.0") ==
              0);
  ASSERT_TRUE(json_object_get_number(response_obj, "id") == 1.0);
  ASSERT_TRUE(strcmp(json_object_get_string(response_obj, "result"), "pong") ==
              0);

  json_value_free(response);
  jsonrpc_conn_free(conn);
  ASSERT_TRUE(context.callback_state.close_count == 1U);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_notification_has_no_response() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  const char *request = "{\"jsonrpc\":\"2.0\",\"method\":\"note\"}\n";
  jsonrpc_conn_feed(conn, (const uint8_t *)request, strlen(request));

  ASSERT_TRUE(context.callback_state.notification_count == 1U);
  ASSERT_TRUE(strcmp(context.callback_state.last_method, "note") == 0);
  ASSERT_TRUE(context.transport_state.message_count == 0U);

  jsonrpc_conn_free(conn);
  ASSERT_TRUE(context.callback_state.close_count == 1U);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_parse_error_response() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  const char *request = "{not-valid-json}\n";
  jsonrpc_conn_feed(conn, (const uint8_t *)request, strlen(request));

  ASSERT_TRUE(context.transport_state.message_count == 1U);
  auto response = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(response != nullptr);
  auto response_obj = json_value_get_object(response);
  auto error_obj = json_object_get_object(response_obj, "error");
  ASSERT_TRUE((int32_t)json_object_get_number(error_obj, "code") ==
              JSONRPC_ERR_PARSE);
  ASSERT_TRUE(strcmp(json_object_get_string(error_obj, "message"),
                     "Parse error") == 0);
  ASSERT_TRUE(json_object_get_value(response_obj, "id") != nullptr);
  ASSERT_TRUE(json_value_get_type(json_object_get_value(response_obj, "id")) ==
              JSONNull);

  json_value_free(response);
  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_method_not_found_response() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  const char *request = "{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":"
                        "\"unknown\"}\n";
  jsonrpc_conn_feed(conn, (const uint8_t *)request, strlen(request));

  ASSERT_TRUE(context.callback_state.request_count == 1U);
  ASSERT_TRUE(context.transport_state.message_count == 1U);

  auto response = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(response != nullptr);
  auto response_obj = json_value_get_object(response);
  auto error_obj = json_object_get_object(response_obj, "error");
  ASSERT_TRUE((int32_t)json_object_get_number(error_obj, "code") ==
              JSONRPC_ERR_METHOD_NOT_FOUND);
  ASSERT_TRUE(strcmp(json_object_get_string(error_obj, "message"),
                     "Method not found") == 0);
  ASSERT_TRUE(strcmp(json_object_get_string(response_obj, "id"), "abc") == 0);

  json_value_free(response);
  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_invalid_params_response() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  const char *request =
      "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\",\"params\":123}\n";
  jsonrpc_conn_feed(conn, (const uint8_t *)request, strlen(request));

  ASSERT_TRUE(context.transport_state.message_count == 1U);
  auto response = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(response != nullptr);
  auto response_obj = json_value_get_object(response);
  auto error_obj = json_object_get_object(response_obj, "error");
  ASSERT_TRUE((int32_t)json_object_get_number(error_obj, "code") ==
              JSONRPC_ERR_INVALID_PARAMS);
  ASSERT_TRUE(strcmp(json_object_get_string(error_obj, "message"),
                     "Invalid params") == 0);

  json_value_free(response);
  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_batch_mixed_notification_and_request() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  const char *request =
      "[{\"jsonrpc\":\"2.0\",\"method\":\"note\"},"
      "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}]\n";
  jsonrpc_conn_feed(conn, (const uint8_t *)request, strlen(request));

  ASSERT_TRUE(context.callback_state.notification_count == 1U);
  ASSERT_TRUE(context.callback_state.request_count == 1U);
  ASSERT_TRUE(context.transport_state.message_count == 1U);

  auto response = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(response != nullptr);
  ASSERT_TRUE(json_value_get_type(response) == JSONArray);

  auto response_array = json_value_get_array(response);
  ASSERT_TRUE(json_array_get_count(response_array) == 1U);
  auto response_item = json_array_get_object(response_array, 0U);
  ASSERT_TRUE(response_item != nullptr);
  ASSERT_TRUE(json_object_get_number(response_item, "id") == 9.0);
  ASSERT_TRUE(strcmp(json_object_get_string(response_item, "result"), "pong") ==
              0);

  json_value_free(response);
  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_send_result_and_send_error_api() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  auto id = json_value_init_string("r1");
  ASSERT_TRUE(id != nullptr);
  auto result = json_value_init_number(42.0);
  ASSERT_TRUE(result != nullptr);
  ASSERT_TRUE(jsonrpc_conn_send_result(conn, id, result));
  json_value_free(id);

  ASSERT_TRUE(context.transport_state.message_count == 1U);
  auto result_response = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(result_response != nullptr);
  auto result_obj = json_value_get_object(result_response);
  ASSERT_TRUE(strcmp(json_object_get_string(result_obj, "id"), "r1") == 0);
  ASSERT_TRUE(json_object_get_number(result_obj, "result") == 42.0);
  json_value_free(result_response);

  ASSERT_TRUE(
      jsonrpc_conn_send_error(conn, nullptr, JSONRPC_ERR_INVALID_REQUEST, nullptr));
  ASSERT_TRUE(context.transport_state.message_count == 2U);
  auto error_response = test_parse_sent_json(&context.transport_state, 1U);
  ASSERT_TRUE(error_response != nullptr);
  auto error_obj = json_object_get_object(json_value_get_object(error_response),
                                          "error");
  ASSERT_TRUE((int32_t)json_object_get_number(error_obj, "code") ==
              JSONRPC_ERR_INVALID_REQUEST);
  ASSERT_TRUE(strcmp(json_object_get_string(error_obj, "message"),
                     "Invalid Request") == 0);
  json_value_free(error_response);

  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_send_failure_triggers_transport_close() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);
  context.transport_state.fail_send = true;

  auto result = json_value_init_string("value");
  ASSERT_TRUE(result != nullptr);
  ASSERT_TRUE(!jsonrpc_conn_send_result(conn, nullptr, result));
  ASSERT_TRUE(context.transport_state.close_calls >= 1U);
  ASSERT_TRUE(context.transport_state.message_count == 0U);

  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

int main() {
  typedef struct {
    const char *name;
    bool (*run)();
  } test_case_t;

  const test_case_t tests[] = {
      {.name = "basic_request_response", .run = test_basic_request_response},
      {.name = "notification_has_no_response",
       .run = test_notification_has_no_response},
      {.name = "parse_error_response", .run = test_parse_error_response},
      {.name = "method_not_found_response", .run = test_method_not_found_response},
      {.name = "invalid_params_response", .run = test_invalid_params_response},
      {.name = "batch_mixed_notification_and_request",
       .run = test_batch_mixed_notification_and_request},
      {.name = "send_result_and_send_error_api",
       .run = test_send_result_and_send_error_api},
      {.name = "send_failure_triggers_transport_close",
       .run = test_send_failure_triggers_transport_close},
  };

  size_t failures = 0U;
  const size_t total = sizeof(tests) / sizeof(tests[0]);
  for (size_t i = 0U; i < total; ++i) {
    if (tests[i].run()) {
      printf("[PASS] %s\n", tests[i].name);
    } else {
      printf("[FAIL] %s\n", tests[i].name);
      failures += 1U;
    }
  }

  if (failures != 0U) {
    fprintf(stderr, "%zu/%zu tests failed\n", failures, total);
    return EXIT_FAILURE;
  }

  printf("All %zu tests passed\n", total);
  return EXIT_SUCCESS;
}
