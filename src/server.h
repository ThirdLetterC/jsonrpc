#ifndef SERVER_H
#define SERVER_H

#include "jsonrpc.h"
#include <stdint.h>

void server_set_callbacks(jsonrpc_callbacks_t callbacks);
[[nodiscard]] jsonrpc_callbacks_t server_get_callbacks();
void start_jsonrpc_server(int32_t port, jsonrpc_callbacks_t callbacks);
void server_request_shutdown();

#endif // SERVER_H
