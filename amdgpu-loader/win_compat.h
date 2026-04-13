#ifndef WIN_COMPAT_H
#define WIN_COMPAT_H

#ifdef _WIN32
#include <stdio.h>

#define flockfile(f) _lock_file(f)
#define funlockfile(f) _unlock_file(f)
#define fwrite_unlocked _fwrite_nolock
#define fread_unlocked _fread_nolock

#ifndef MATH_ERRNO
#define MATH_ERRNO 1
#endif
#ifndef MATH_ERREXCEPT
#define MATH_ERREXCEPT 2
#endif
#ifndef math_errhandling
#define math_errhandling MATH_ERRNO
#endif

#endif // _WIN32

// The internal rpc_server.h uses rpc::Buffer inside namespace LIBC_NAMESPACE,
// which doesn't resolve to the global ::rpc::Buffer. Inject the global rpc
// namespace symbols into LIBC_NAMESPACE so the internal code compiles.
#include <shared/rpc.h>
namespace LIBC_NAMESPACE_DECL {
namespace rpc {
using ::rpc::Buffer;
using ::rpc::Server;
using ::rpc::Client;
using ::rpc::Port;
using ::rpc::Status;
using ::rpc::RPC_SUCCESS;
using ::rpc::RPC_ERROR;
using ::rpc::RPC_UNHANDLED_OPCODE;
using ::rpc::MAX_PORT_COUNT;
} // namespace rpc
} // namespace LIBC_NAMESPACE_DECL

#endif // WIN_COMPAT_H
