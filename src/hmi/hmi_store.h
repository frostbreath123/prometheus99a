/**
 * @file hmi_store.h
 * @brief Storage worker: one thread that owns every durable file write of the
 *        runtime (audit file, alarm journal, alarm state), so that no other
 *        thread blocks on a disk.
 *
 * Requests go into a bounded queue in a fixed order and are processed one at
 * a time. A request is made durable by the platform layer (data written and
 * flushed to stable storage) before it is reported as done. Nothing is
 * allocated: the queue, the outcome table and the file names live inside the
 * hmi_store_t object.
 *
 * Callers that must not proceed without a durable record (a PLC write, an
 * alarm acknowledgement) ask for a completion callback or wait for the ticket,
 * and treat anything but "done" as failure (fail closed). Callers that only
 * log (the alarm journal) ignore the outcome. A request that cannot be queued
 * is refused at once; the queue never grows and never drops silently.
 *
 * Before the first append to a file, and after any failed append, the worker
 * looks at the last byte of the file: if it is not a line break (a torn last
 * record) it writes one, so a new record never joins a damaged one.
 *
 * Append targets rotate at a size cap: the file is renamed to name.1, the
 * older generations move up, the oldest is replaced, and a marker line starts
 * the new file. A failed write or rotation sets a visible fault flag that the
 * next success clears. The failure count is never reset.
 *
 * Threading: hmi_store_enqueue(), hmi_store_ticket(), the stats getters and
 * hmi_store_fault() may be called from any thread. hmi_store_wait() and
 * hmi_store_flush() are for one waiting thread at a time. The store lock is
 * never held while a file is touched or a callback runs.
 */
#ifndef HMI_STORE_H
#define HMI_STORE_H

#include "hmi_platform.h"

#define HMI_STORE_QUEUE_LEN 64
#define HMI_STORE_REC_MAX   704  /* longest record in bytes; the acknowledgement line is the longest at 640 */
#define HMI_STORE_RESULTS   128  /* ticket outcomes remembered; at least twice the queue length */
#define HMI_STORE_PATH_MAX  256
#define HMI_STORE_TARGETS   3

typedef enum {
    HMI_STORE_APPEND = 1,  /* append a record, rotate at the size cap */
    HMI_STORE_REPLACE      /* replace the whole file atomically; the newest queued request wins */
} hmi_store_kind_t;

/* The file operations the worker uses. The default is the platform layer. A
 * test passes its own table to inject failures and torn writes. */
typedef struct {
    bool (*append)(const char *path, const void *data, size_t len);
    bool (*replace)(const char *path, const void *data, size_t len);
    bool (*size)(const char *path, uint64_t *size);
    bool (*rename)(const char *from, const char *to);
} hmi_store_io_t;

typedef struct {
    const char *path[HMI_STORE_TARGETS];   /* NULL or empty: target not in use */
    hmi_store_kind_t kind[HMI_STORE_TARGETS];
    bool marker[HMI_STORE_TARGETS];        /* write a LOG_ROTATED line at the start of a rotated file */
    uint64_t max_bytes;                    /* size cap of an append target, 0 = never rotate */
    uint32_t generations;                  /* rotated files kept (name.1 .. name.N), 1 to 9 */
    const hmi_store_io_t *io;              /* NULL = platform */
} hmi_store_cfg_t;

typedef enum {
    HMI_STORE_PENDING = 0, /* queued or being written */
    HMI_STORE_DONE,        /* durable */
    HMI_STORE_FAILED,      /* could not be made durable */
    HMI_STORE_UNKNOWN      /* ticket not (or no longer) known: treat as failed */
} hmi_store_status_t;

/* Called on the worker thread after the outcome is recorded. */
typedef void (*hmi_store_done_fn)(void *user, uint32_t arg, bool ok);

typedef struct {
    uint32_t enqueued, written, failed, refused, rotations, max_depth;
    bool fault;
} hmi_store_stats_t;

typedef struct {
    uint8_t target;
    uint8_t kind;
    uint16_t len;
    uint32_t ticket;
    hmi_store_done_fn done;
    void *user;
    uint32_t arg;
    char data[HMI_STORE_REC_MAX];
} hmi_store_item_t;

typedef struct {
    uint32_t ticket;
    uint8_t status;
} hmi_store_result_t;

typedef struct {
    char path[HMI_STORE_TARGETS][HMI_STORE_PATH_MAX];
    hmi_store_cfg_t cfg;
    hmi_store_io_t io;

    hmi_mutex_t lock;
    hmi_store_item_t q[HMI_STORE_QUEUE_LEN];
    uint8_t q_head, q_len;
    bool busy;                       /* an item has been taken and is not finished */
    uint32_t next_ticket;
    hmi_store_result_t res[HMI_STORE_RESULTS];
    hmi_store_stats_t stats;
    uint64_t file_size[HMI_STORE_TARGETS];
    bool size_known[HMI_STORE_TARGETS];

    hmi_event_t wake;                /* worker: something queued, or stop */
    hmi_event_t done;                /* waiter: an item finished */
    hmi_atomic_flag_t stop;
    hmi_atomic_flag_t fault;
    hmi_thread_t th;
    bool running, ready;
} hmi_store_t;

/* Validates the configuration (at least one target, kinds set, generations 1
 * to 9 when max_bytes is set) and copies the file names. Does not start the
 * worker, so a test can drive it with hmi_store_run_once(). */
bool hmi_store_init(hmi_store_t *s, const hmi_store_cfg_t *cfg);
void hmi_store_destroy(hmi_store_t *s);

bool hmi_store_start(hmi_store_t *s);
/* Refuses new requests, lets the worker finish what is queued, and joins it.
 * Returns true when the worker exited within timeout_ms. */
bool hmi_store_stop(hmi_store_t *s, uint32_t timeout_ms);

/* Queue one record. Returns false, and changes nothing, if the queue is full,
 * the store is stopping, the target is unused, the record is empty or longer
 * than HMI_STORE_REC_MAX. done may be NULL. *ticket may be NULL. */
bool hmi_store_enqueue(hmi_store_t *s, unsigned target, const void *data, size_t len, hmi_store_done_fn done,
                       void *user, uint32_t arg, uint32_t *ticket);

hmi_store_status_t hmi_store_ticket(hmi_store_t *s, uint32_t ticket);
/* Block until the ticket is no longer pending or timeout_ms passes. Returns
 * HMI_STORE_PENDING on timeout. Needs a running worker. */
hmi_store_status_t hmi_store_wait(hmi_store_t *s, uint32_t ticket, uint32_t timeout_ms);
/* Block until the queue is empty and nothing is being written. */
bool hmi_store_flush(hmi_store_t *s, uint32_t timeout_ms);

/* Process the oldest queued item on the calling thread. Returns false if the
 * queue was empty. For tests and for the worker; do not call while the worker
 * runs. */
bool hmi_store_run_once(hmi_store_t *s);

/* True while the last file operation failed. */
bool hmi_store_fault(hmi_store_t *s);
void hmi_store_stats(hmi_store_t *s, hmi_store_stats_t *out);

#endif /* HMI_STORE_H */
