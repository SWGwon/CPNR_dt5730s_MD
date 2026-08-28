#ifndef TESTS_MOCKS_ZMQ_H
#define TESTS_MOCKS_ZMQ_H

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

constexpr int ZMQ_PUB = 1;
constexpr int ZMQ_SNDHWM = 2;
constexpr int ZMQ_LINGER = 3;
constexpr int ZMQ_DONTWAIT = 1;

namespace zmq_mock {
inline bool bind_failure = false;
inline int send_failure = 0;
inline int last_error = EAGAIN;
inline int last_send_hwm = 0;

inline void SetBindFailure(bool enabled) {
  bind_failure = enabled;
  last_error = enabled ? EADDRINUSE : EAGAIN;
}

inline void SetSendFailure(int error) {
  send_failure = error;
  last_error = error == 0 ? EAGAIN : error;
}
}  // namespace zmq_mock

inline void* zmq_ctx_new() {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(1));
}

inline int zmq_ctx_destroy(void*) { return 0; }

inline void* zmq_socket(void*, int) {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(2));
}

inline int zmq_close(void*) { return 0; }
inline int zmq_setsockopt(void*, int option, const void* value,
                          std::size_t value_size) {
  if (option == ZMQ_SNDHWM && value_size == sizeof(int)) {
    std::memcpy(&zmq_mock::last_send_hwm, value, sizeof(int));
  }
  return 0;
}
inline int zmq_bind(void*, const char*) {
  return zmq_mock::bind_failure ? -1 : 0;
}
inline int zmq_send(void*, const void*, std::size_t size, int) {
  if (zmq_mock::send_failure != 0) return -1;
  return static_cast<int>(size);
}
inline int zmq_errno() { return zmq_mock::last_error; }
inline const char* zmq_strerror(int error) { return std::strerror(error); }

#endif  // TESTS_MOCKS_ZMQ_H
