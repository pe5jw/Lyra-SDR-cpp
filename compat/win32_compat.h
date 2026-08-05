// compat/win32_compat.h -- master Linux/macOS compatibility header
// On Windows this header does nothing.
// On Linux/macOS the shims replace Win32 APIs with POSIX equivalents.
#pragma once
#ifndef _WIN32
#  include "win32_socket.h"
#  include "win32_timer.h"
#  include "win32_dynload.h"
#  include "win32_stubs.h"
#endif
