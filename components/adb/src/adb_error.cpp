#include "adb_error.hpp"

namespace adb {

const char* to_string(Error e) {
    switch (e) {
        case Error::Ok: return "Ok";
        case Error::NotConnected: return "NotConnected";
        case Error::StreamClosed: return "StreamClosed";
        case Error::QueueFull: return "QueueFull";
        case Error::Timeout: return "Timeout";
        case Error::Rejected: return "Rejected";
        case Error::Protocol: return "Protocol";
        case Error::Transport: return "Transport";
        case Error::Cancelled: return "Cancelled";
    }
    return "?";
}

}  // namespace adb
