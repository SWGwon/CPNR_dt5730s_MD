#ifndef TESTS_MOCKS_ZMQ_H
#define TESTS_MOCKS_ZMQ_H

#include <cerrno>
#include <cstddef>
#include <cstdint>

constexpr int ZMQ_PUB = 1;
constexpr int ZMQ_SNDHWM = 2;
constexpr int ZMQ_LINGER = 3;
constexpr int ZMQ_DONTWAIT = 1;

inline void* zmq_ctx_new() {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(1));
}

inline int zmq_ctx_destroy(void*) { return 0; }

inline void* zmq_socket(void*, int) {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(2));
}

inline int zmq_close(void*) { return 0; }
inline int zmq_setsockopt(void*, int, const void*, std::size_t) { return 0; }
inline int zmq_bind(void*, const char*) { return 0; }
inline int zmq_send(void*, const void*, std::size_t size, int) {
  return static_cast<int>(size);
}
inline int zmq_errno() { return EAGAIN; }

#endif  // TESTS_MOCKS_ZMQ_H
