// Internal to //http: the conversation with one client.

#ifndef HTTP_CONNECTION_H_
#define HTTP_CONNECTION_H_

#include "async/task.h"
#include "http/server.h"
#include "net/tcp.h"

// Answers the requests arriving on `connection` with whatever `handler`
// returns, one after another, until the client disconnects, goes idle, or
// sends something after which the connection cannot continue. Closes the
// connection when it finishes.
//
// Text that is not a valid request is answered with the appropriate error
// status and never reaches the handler.
//
// `handler` must outlive the returned task.
Task<> ServeConnection(TcpConnection connection, const Handler& handler);

#endif  // HTTP_CONNECTION_H_
