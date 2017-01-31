#include <errno.h>

#include "uapi/plugins.h"

#include "uapi/std/syscall.h"
#include "uapi/std/string.h"
#include "uapi/std/log.h"
#include "uapi/std/fds.h"

#include "std-priv.h"

#define pr_err(fmt, ...)

#include "common/compiler.h"
#include "common/bug.h"

#define __sys(foo)	sys_##foo
#define __sys_err(ret)	ret

#include "common/scm-code.c"
