// Error type for the app-facing `adb` component. See README.md ("Errors").
// Synchronous methods return adb::Error; async completions take it as their
// first argument.
#pragma once

namespace adb {

enum class Error {
    Ok = 0,
    NotConnected,  // not Online yet, or already closed
    StreamClosed,  // operation on a stream the peer/we already closed
    QueueFull,     // non-blocking write backpressure limit hit
    Timeout,
    Rejected,    // the service refused to open
    Protocol,    // malformed packet / unexpected response
    Transport,   // USB / transfer-level error
    Cancelled,   // in-flight op aborted by Client::close()
};

const char* to_string(Error e);

}  // namespace adb
