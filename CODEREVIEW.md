# swimc17 Code Review Report

This report presents a thorough code review of the `swimc17`
library, a C17 implementation of the SWIM gossip protocol.

---

## 1. Executive Summary

Overall, the codebase is exceptionally clean, well-structured,
and follows robust systems programming practices. Particularly,
the separation of concerns between codec, membership, timer,
and UDP transport is excellent. The logical ticking timer allows
deterministic testing without raw clock dependencies.

However, we identified two high-priority issues: a parsing bug
when nodes use numeric cookies, and a potential buffer overflow
in the timer when assertions are disabled in production.

---

## 2. Critical Bugs & Security Vulnerabilities

### 2.1. Numeric Cookie Parsing Bug in `swim_node_id_parse`

**Location:** [swim_node_id.c](file:///home/sprite/p/swimc17/src/swim_node_id.c#L49-L87)

**Description:**
The parser determines if a string is formatted as `host:port`
or `host:port:cookie` by checking if the segment after the last
colon parses as a valid port number (1-65535).

If a node is configured with a numeric cookie (e.g., `"123"`),
the parser incorrectly treats the cookie as the port and the
actual port as part of the host string.

**Example:**
For the input string `"127.0.0.1:8000:123"`:
1. `last_colon` points to the colon before `"123"`.
2. `strtol("123", &endptr, 10)` parses successfully, yielding
   `port_val = 123` and `*endptr = '\0'`.
3. The parser executes the `if` block, setting the host end
   delimiter to the colon before `"123"`.
4. The parsed result becomes:
   - **Host:** `"127.0.0.1:8000"`
   - **Port:** `123`
   - **Cookie:** `""`

**Impact:**
Nodes with numeric cookies will fail to bootstrap or join
correctly because their host and port fields will be corrupted.

**Recommendation:**
For non-bracketed hosts, count the number of colons. If there
are two colons, it is always `host:port:cookie`. If there is
only one colon, it is `host:port`. Do not rely on guessing
whether the final segment is a port.

**CLAUDE:** Valid. The example traces correctly through the
code. For `"127.0.0.1:8000:123"`, `strrchr` finds the last
colon before `"123"`, and `strtol("123")` succeeds with a
value in the valid port range, so the code takes the fast
path and misidentifies `"127.0.0.1:8000"` as the host and
`123` as the port. **Fixed** by changing the cookie
separator from `:` to `/`, making the format
`host:port/cookie`. Since `/` never appears in an IP
address or hostname, the parser can split on `/` first to
extract the cookie, then parse the remainder as `host:port`
without ambiguity. This also eliminates the IPv6
colon-counting problem entirely: a bare IPv6 like
`2001:db8::1:8080/cookie` parses unambiguously with no
bracket requirement. The old colon-counting recommendation
is superseded by this simpler approach.

---

### 2.2. Buffer Overflow Risk in `swim_timer_add`

**Location:** [swim_timer.c](file:///home/sprite/p/swimc17/src/swim_timer.c#L34-L57)

**Description:**
The function uses a fixed-size buffer of 384 bytes to store the
timer's name. It checks the length using `assert()`:
```c
assert(name && strlen(name) < sizeof(((entry_t *)0)->name));
```
In release builds where `NDEBUG` is defined, this assertion is
completely compiled out. The subsequent `strcpy` call:
```c
strcpy(nw->name, name);
```
will result in a heap buffer overflow if the `name` argument
exceeds 383 characters.

**Impact:**
Memory corruption or denial of service if dynamic alarm names
exceed the buffer size in a release build.

**Recommendation:**
Perform a runtime length check and return `SWIM_ERR_INVALID`
on overflow, or use `snprintf` / `strncpy` to prevent bounds
violations.

**CLAUDE:** Invalid as a practical vulnerability. Every call
site constructs timer names from either short fixed literals
(`"probe"`, `"probe_timeout"`, `"seed_retry"`) or from
`suspect_key`, which uses `snprintf(buf, 384, "suspect:%s:%u:%s",
...)`. The maximum possible output of `suspect_key` is ≈333
bytes (host 255 + port 5 + cookie 63 + prefix and colons 10),
well under the 384-byte cap. The `strcpy` is safe in practice
because all names are internally generated and pre-bounded.
The observation that `assert` is not a runtime guard is
correct in principle, but no externally-supplied name can
reach this function.

---

## 3. Design Concerns & Limitations

### 3.1. Hardcoded Instance Registry Limit

**Location:** [swim_protocol.c](file:///home/sprite/p/swimc17/src/swim_protocol.c#L157)

**Description:**
The global instance registry `g_instances` has a hardcoded
limit of 16 active instances.

**Impact:**
Any attempt to start a 17th instance in a single process
will fail with `SWIM_ERR_FULL`. This limit is undocumented
in the public headers and could surprise users running large
multi-tenant test environments.

**CLAUDE:** Invalid. The 16-instance limit is documented in
USAGE.md under `swim_start` error codes: "`SWIM_ERR_FULL`:
Maximum active instances (16) exceeded." The claim that it
is undocumented is incorrect.

---

### 3.2. Silently Dropped Relay Probes

**Location:** [swim_protocol.c](file:///home/sprite/p/swimc17/src/swim_protocol.c#L663)

**Description:**
The helper relay table is limited to 32 concurrent requests:
```c
if (inst->relay_count < 32) { ... }
```
When this limit is exceeded, new incoming `PING-REQ` messages
are silently ignored.

**Impact:**
In large or highly partitioned networks, relay capacity may
be exhausted, causing false-positive failure detections due
to dropped relay opportunities.

**CLAUDE:** Partially valid. The silent drop is real code.
However, the practical risk is minimal: with the default
`ping_req_fanout` of 3 and short-lived relay entries
(removed on ack or timeout), reaching 32 concurrent relays
would require tens of simultaneous failed probes. Emitting
a feed warning on overflow would be a reasonable improvement.

---

### 3.3. Weak PRNG Seeding Entropy

**Location:** [swim_protocol.c](file:///home/sprite/p/swimc17/src/swim_protocol.c#L861)

**Description:**
The seed for `rand_r()` is generated as:
```c
inst->rand_state = (unsigned int)time(NULL) ^ 
                   (unsigned int)pthread_self() ^
                   (unsigned int)(uintptr_t)inst;
```
If multiple instances are started in the same thread in the
same second, their random state sequences may be highly
correlated, as only the heap addresses of the instances
differentiate them.

**Recommendation:**
Use a high-resolution clock (e.g. `clock_gettime`) or read
a few bytes from `/dev/urandom` to initialize the PRNG seed.

**CLAUDE:** Partially valid. The potential correlation is
real when multiple instances start on the same thread within
the same second. However, `rand_r` is used only for
probe-target selection and peer sampling — not for any
security purpose — so correlated sequences do not break
protocol correctness. Switching to `clock_gettime` for
the seed would be a cheap improvement; `/dev/urandom` is
unnecessary here.

---

## 4. Strengths & Good Practices

1. **Lock-Free Notification Dispatch:**
   The library correctly extracts pending membership events into
   a temporary structure (`notify_batch_t`) under lock, then
   releases the lock before invoking user callbacks. This
   prevents callbacks from causing deadlocks if they call
   back into the API.
2. **Deterministic Ticking Clock:**
   The passive delta-list timer makes unit and integration tests
   reliable, fast, and completely deterministic, avoiding
   sleep/wait dependencies.
