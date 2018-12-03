#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/uio.h>

#include <assert.h>

#include "redisraft.h"

#define ENTRY_CACHE_INIT_SIZE 512

#define RAFT_LOG_TRACE
#ifdef RAFT_LOG_TRACE
#  define TRACE_LOG_OP(fmt, ...) LOG_DEBUG("Log>>" fmt, ##__VA_ARGS__)
#else
#  define TRACE_LOG_OP(fmt, ...)
#endif

/*
 * Entries Cache.
 */

EntryCache *EntryCacheNew(unsigned long initial_size)
{
    EntryCache *cache = RedisModule_Calloc(1, sizeof(EntryCache));

    cache->size = initial_size;
    cache->ptrs = RedisModule_Calloc(cache->size, sizeof(raft_entry_t *));

    return cache;
}

void EntryCacheFree(EntryCache *cache)
{
    unsigned long i;

    for (i = 0; i < cache->len; i++) {
        raft_entry_release(cache->ptrs[(cache->start + i) % cache->size]);
    }

    RedisModule_Free(cache->ptrs);
    RedisModule_Free(cache);
}

void EntryCacheAppend(EntryCache *cache, raft_entry_t *ety, raft_index_t idx)
{
    if (!cache->start_idx) {
        cache->start_idx = idx;
    }

    assert(cache->start_idx + cache->len == idx);

    /* Enlrage cache if necessary */
    if (cache->len == cache->size) {
        unsigned long int new_size = cache->size * 2;
        cache->ptrs = RedisModule_Realloc(cache->ptrs, new_size * sizeof(raft_entry_t *));

        if (cache->start > 0) {
            memmove(&cache->ptrs[cache->size], &cache->ptrs[0], cache->start * sizeof(raft_entry_t *));
            memset(&cache->ptrs[0], 0, cache->start * sizeof(raft_entry_t *));
        }

        cache->size = new_size;
    }

    cache->ptrs[(cache->start + cache->len) % cache->size] = ety;
    cache->len++;
    raft_entry_hold(ety);
}

raft_entry_t *EntryCacheGet(EntryCache *cache, raft_index_t idx)
{
    if (idx < cache->start_idx) {
        return NULL;
    }

    unsigned long int relidx = idx - cache->start_idx;
    if (relidx >= cache->len) {
        return NULL;
    }

    raft_entry_t *ety = cache->ptrs[(cache->start + relidx) % cache->size];
    raft_entry_hold(ety);
    return ety;
}

long EntryCacheDeleteHead(EntryCache *cache, raft_index_t first_idx)
{
    long deleted = 0;

    if (first_idx < cache->start_idx) {
        return -1;
    }

    while (first_idx > cache->start_idx && cache->len > 0) {
        cache->start_idx++;
        raft_entry_release(cache->ptrs[cache->start]);
        cache->ptrs[cache->start] = NULL;
        cache->start++;
        if (cache->start >= cache->size) {
            cache->start = 0;
        }
        cache->len--;
        deleted++;
    }

    if (!cache->len) {
        cache->start_idx = 0;
    }

    return deleted;
}

long EntryCacheDeleteTail(EntryCache *cache, raft_index_t index)
{
    long deleted = 0;
    raft_index_t i;

    if (index >= cache->start_idx + cache->len) {
        return -1;
    }
    if (index < cache->start_idx) {
        return -1;
    }

    for (i = index; i < cache->start_idx + cache->len; i++) {
        unsigned long int relidx = i - cache->start_idx;
        unsigned long int ofs = (cache->start + relidx) % cache->size;
        raft_entry_release(cache->ptrs[ofs]);
        cache->ptrs[ofs] = NULL;
        deleted++;
    }

    cache->len -= deleted;

    if (!cache->len) {
        cache->start_idx = 0;
    }

    return deleted;
}

void RaftLogClose(RaftLog *log)
{
    fclose(log->file);
    fclose(log->idxfile);
    RedisModule_Free(log);
}

/*
 * Raw reading/writing of Raft log.
 */

static int writeBegin(FILE *logfile, int length)
{
    int n;

    if ((n = fprintf(logfile, "*%u\r\n", length)) <= 0) {
        return -1;
    }

    return n;
}

static int writeEnd(FILE *logfile, bool no_fsync)
{
    if (fflush(logfile) < 0) {
        return -1;
    }
    if (no_fsync) {
        return 0;
    }
    if (fsync(fileno(logfile)) < 0) {
        return -1;
    }

    return 0;
}

static int writeBuffer(FILE *logfile, const void *buf, size_t buf_len)
{
    static const char crlf[] = "\r\n";
    int n;

    if ((n = fprintf(logfile, "$%zu\r\n", buf_len)) <= 0 ||
        fwrite(buf, 1, buf_len, logfile) < buf_len ||
        fwrite(crlf, 1, 2, logfile) < 2) {
            return -1;
    }

    return n + buf_len + 2;
}

static int writeUnsignedInteger(FILE *logfile, unsigned long value, int pad)
{
    char buf[25];
    int n;
    assert(pad < sizeof(buf));

    if (pad) {
        snprintf(buf, sizeof(buf) - 1, "%0*lu", pad, value);
    } else {
        snprintf(buf, sizeof(buf) - 1, "%lu", value);
    }

    if ((n = fprintf(logfile, "$%zu\r\n%s\r\n", strlen(buf), buf)) <= 0) {
        return -1;
    }

    return n;
}

static int writeInteger(FILE *logfile, long value, int pad)
{
    char buf[25];
    int n;
    assert(pad < sizeof(buf));

    if (pad) {
        snprintf(buf, sizeof(buf) - 1, "%0*ld", pad, value);
    } else {
        snprintf(buf, sizeof(buf) - 1, "%ld", value);
    }

    if ((n = fprintf(logfile, "$%zu\r\n%s\r\n", strlen(buf), buf)) <= 0) {
        return -1;
    }

    return n;
}


typedef struct RawElement {
    void *ptr;
    size_t len;
} RawElement;

typedef struct RawLogEntry {
    int num_elements;
    RawElement elements[];
} RawLogEntry;

static int readEncodedLength(RaftLog *log, char type, unsigned long *length)
{
    char buf[128];
    char *eptr;

    if (!fgets(buf, sizeof(buf), log->file)) {
        return -1;
    }

    if (buf[0] != type) {
        return -1;
    }

    *length = strtoul(buf + 1, &eptr, 10);
    if (*eptr != '\n' && *eptr != '\r') {
        return -1;
    }

    return 0;
}

static void freeRawLogEntry(RawLogEntry *entry)
{
    int i;

    if (!entry) {
        return;
    }

    for (i = 0; i < entry->num_elements; i++) {
        if (entry->elements[i].ptr != NULL) {
            RedisModule_Free(entry->elements[i].ptr);
            entry->elements[i].ptr = NULL;
        }
    }

    RedisModule_Free(entry);
}

static int readRawLogEntry(RaftLog *log, RawLogEntry **entry)
{
    unsigned long num_elements;
    int i;

    if (readEncodedLength(log, '*', &num_elements) < 0) {
        return -1;
    }

    *entry = RedisModule_Calloc(1, sizeof(RawLogEntry) + sizeof(RawElement) * num_elements);
    (*entry)->num_elements = num_elements;
    for (i = 0; i < num_elements; i++) {
        unsigned long len;
        char *ptr;

        if (readEncodedLength(log, '$', &len) < 0) {
            goto error;
        }
        (*entry)->elements[i].len = len;
        (*entry)->elements[i].ptr = ptr = RedisModule_Alloc(len + 2);

        /* Read extra CRLF */
        if (fread(ptr, 1, len + 2, log->file) != len + 2) {
            goto error;
        }
        ptr[len] = '\0';
        ptr[len + 1] = '\0';
    }

    return 0;
error:
    freeRawLogEntry(*entry);
    *entry = NULL;

    return -1;
}

static int updateIndex(RaftLog *log, raft_index_t index, off64_t offset)
{
    long relidx = index - log->snapshot_last_idx;

    if (fseek(log->idxfile, sizeof(off64_t) * relidx, SEEK_SET) < 0 ||
            fwrite(&offset, sizeof(off64_t), 1, log->idxfile) != 1) {
        return -1;
    }

    return 0;
}

RaftLog *prepareLog(const char *filename)
{
    FILE *file = fopen(filename, "a+");
    if (!file) {
        LOG_ERROR("Raft Log: %s: %s\n", filename, strerror(errno));
        return NULL;
    }

    /* Index file */
    int idx_filename_len = strlen(filename) + 10;
    char idx_filename[idx_filename_len];
    snprintf(idx_filename, idx_filename_len - 1, "%s.idx", filename);
    FILE *idxfile = fopen(idx_filename, "w+");
    if (!idxfile) {
        LOG_ERROR("Raft Log: %s: %s\n", idx_filename, strerror(errno));
        fclose(file);
        return NULL;
    }

    /* Initialize struct */
    RaftLog *log = RedisModule_Calloc(1, sizeof(RaftLog));
    log->file = file;
    log->idxfile = idxfile;
    log->filename = filename;

    return log;
}

int writeLogHeader(FILE *logfile, RaftLog *log)
{
    if (writeBegin(logfile, 7) < 0 ||
        writeBuffer(logfile, "RAFTLOG", 7) < 0 ||
        writeUnsignedInteger(logfile, RAFTLOG_VERSION, 4) < 0 ||
        writeBuffer(logfile, log->dbid, strlen(log->dbid)) < 0 ||
        writeUnsignedInteger(logfile, log->snapshot_last_term, 20) < 0 ||
        writeUnsignedInteger(logfile, log->snapshot_last_idx, 20) < 0 ||
        writeUnsignedInteger(logfile, log->term, 20) < 0 ||
        writeInteger(logfile, log->vote, 11) < 0 ||
        writeEnd(logfile, log->no_fsync) < 0) {
            return -1;
    }

    return 0;
}

int updateLogHeader(RaftLog *log)
{
    int ret;

    /* Avoid same file open twice */
    fclose(log->file);
    log->file = NULL;

    FILE *file = fopen(log->filename, "r+");
    if (!file) {
        PANIC("Failed to update log header: %s: %s",
                log->filename, strerror(errno));
    }

    ret = writeLogHeader(file, log);
    fclose(file);

    /* Reopen */
    log->file = fopen(log->filename, "a+");
    if (!log->file) {
        PANIC("Failed to reopen log file: %s: %s",
                log->filename, strerror(errno));
    }

    return ret;
}

RaftLog *RaftLogCreate(const char *filename, const char *dbid, raft_term_t term,
        raft_index_t index)
{
    RaftLog *log = prepareLog(filename);
    if (!log) {
        return NULL;
    }

    log->index = log->snapshot_last_idx = index;
    log->snapshot_last_term = term;
    log->term = 1;
    log->vote = -1;

    memcpy(log->dbid, dbid, RAFT_DBID_LEN);
    log->dbid[RAFT_DBID_LEN] = '\0';

    /* Truncate */
    ftruncate(fileno(log->file), 0);
    ftruncate(fileno(log->idxfile), 0);

    /* Write log start */
    if (writeLogHeader(log->file, log) < 0) {
        LOG_ERROR("Failed to create Raft log: %s: %s\n", filename, strerror(errno));
        RaftLogClose(log);
        log = NULL;
    }

    return log;
}

static raft_entry_t *parseRaftLogEntry(RawLogEntry *re)
{
    char *eptr;
    raft_entry_t *e;

    if (re->num_elements != 5) {
        LOG_ERROR("Log entry: invalid number of arguments: %d\n", re->num_elements);
        return NULL;
    }

    e = raft_entry_new(re->elements[4].len);
    memcpy(e->data, re->elements[4].ptr, re->elements[4].len);

    e->term = strtoul(re->elements[1].ptr, &eptr, 10);
    if (*eptr) {
        goto error;
    }

    e->id = strtoul(re->elements[2].ptr, &eptr, 10);
    if (*eptr) {
        goto error;
    }

    e->type = strtoul(re->elements[3].ptr, &eptr, 10);
    if (*eptr) {
        goto error;
    }

    return e;

error:
    raft_entry_release(e);
    return NULL;
}

static int handleHeader(RaftLog *log, RawLogEntry *re)
{
    if (re->num_elements != 7 ||
        strcmp(re->elements[0].ptr, "RAFTLOG")) {
        LOG_ERROR("Invalid Raft log header.");
        return -1;
    }

    char *eptr;
    unsigned long ver = strtoul(re->elements[1].ptr, &eptr, 10);
    if (*eptr != '\0' || ver != RAFTLOG_VERSION) {
        LOG_ERROR("Invalid Raft header version: %lu\n", ver);
        return -1;
    }

    if (strlen(re->elements[2].ptr) > RAFT_DBID_LEN) {
        LOG_ERROR("Invalid Raft log dbid: %s\n", re->elements[2].ptr);
        return -1;
    }
    strcpy(log->dbid, re->elements[2].ptr);

    log->snapshot_last_term = strtoul(re->elements[3].ptr, &eptr, 10);
    if (*eptr != '\0') {
        LOG_ERROR("Invalid Raft log term: %s\n", re->elements[3].ptr);
        return -1;
    }

    log->index = log->snapshot_last_idx = strtoul(re->elements[4].ptr, &eptr, 10);
    if (*eptr != '\0') {
        LOG_ERROR("Invalid Raft log index: %s\n", re->elements[4].ptr);
        return -1;
    }

    log->term = strtoul(re->elements[5].ptr, &eptr, 10);
    if (*eptr != '\0') {
        LOG_ERROR("Invalid Raft log voted term: %s\n", re->elements[5].ptr);
        return -1;
    }

    log->vote = strtol(re->elements[6].ptr, &eptr, 10);
    if (*eptr != '\0') {
        LOG_ERROR("Invalid Raft log vote: %s\n", re->elements[6].ptr);
        return -1;
    }

    return 0;
}

RaftLog *RaftLogOpen(const char *filename)
{
    RaftLog *log = prepareLog(filename);
    if (!log) {
        return NULL;
    }

    /* Read start */
    fseek(log->file, 0L, SEEK_SET);

    RawLogEntry *e = NULL;
    if (readRawLogEntry(log, &e) < 0) {
        LOG_ERROR("Failed to read Raft log: %s\n", errno ? strerror(errno) : "invalid data");
        goto error;
    }

    if (handleHeader(log, e) < 0) {
        goto error;
    }

    freeRawLogEntry(e);
    return log;

error:
    if (e != NULL) {
        freeRawLogEntry(e);
    }
    RedisModule_Free(log);
    return NULL;
}

RRStatus RaftLogReset(RaftLog *log, raft_index_t index, raft_term_t term)
{
    log->index = log->snapshot_last_idx = index;
    log->snapshot_last_term = term;
    if (log->term > term) {
        log->term = term;
        log->vote = -1;
    }

    if (ftruncate(fileno(log->file), 0) < 0 ||
        ftruncate(fileno(log->idxfile), 0) < 0 ||
        writeLogHeader(log->file, log) < 0) {

        return RR_ERROR;
    }

    return RR_OK;
}

int RaftLogLoadEntries(RaftLog *log, int (*callback)(void *, raft_entry_t *, raft_index_t), void *callback_arg)
{
    int ret = 0;

    if (fseek(log->file, 0, SEEK_SET) < 0) {
        return -1;
    }

    log->term = 1;
    log->index = 0;

    /* Read Header */
    RawLogEntry *re = NULL;
    if (readRawLogEntry(log, &re) < 0 || handleHeader(log, re) < 0)  {
        freeRawLogEntry(re);
        LOG_INFO("Failed to read Raft log header");
        return -1;
    }
    freeRawLogEntry(re);

    /* Read Entries */
    do {
        raft_entry_t *e = NULL;

        long offset = ftell(log->file);
        if (readRawLogEntry(log, &re) < 0 || !re->num_elements) {
            break;
        }

        if (!strcasecmp(re->elements[0].ptr, "ENTRY")) {
            e = parseRaftLogEntry(re);
            if (!e) {
                freeRawLogEntry(re);
                ret = -1;
                break;
            }
            log->index++;
            ret++;

            updateIndex(log, log->index, offset);
        } else {
            LOG_ERROR("Invalid log entry: %s\n", (char *) re->elements[0].ptr);
            freeRawLogEntry(re);

            ret = -1;
            break;
        }

        int cb_ret = 0;
        if (callback) {
            callback(callback_arg, e, log->index);
        }

        freeRawLogEntry(re);
        raft_entry_release(e);

        if (cb_ret < 0) {
            ret = cb_ret;
            break;
        }
    } while(1);

    if (ret > 0) {
        log->num_entries = ret;
    }
    return ret;
}

RRStatus RaftLogWriteEntry(RaftLog *log, raft_entry_t *entry)
{
    size_t written = 0;
    int n;

    if ((n = writeBegin(log->file, 5)) < 0) {
        return RR_ERROR;
    }
    written += n;
    if ((n = writeBuffer(log->file, "ENTRY", 5)) < 0) {
        return RR_ERROR;
    }
    written += n;
    if ((n = writeUnsignedInteger(log->file, entry->term, 0)) < 0) {
        return RR_ERROR;
    }
    written += n;
    if ((n = writeUnsignedInteger(log->file, entry->id, 0)) < 0) {
        return RR_ERROR;
    }
    written += n;
    if ((n = writeUnsignedInteger(log->file, entry->type, 0)) < 0) {
        return RR_ERROR;
    }
    written += n;
    if ((n = writeBuffer(log->file, entry->data, entry->data_len)) < 0) {
        return RR_ERROR;
    }
    written += n;

    /* Update index */
    off64_t offset = ftell(log->file) - written;
    log->index++;
    if (updateIndex(log, log->index, offset) < 0) {
        return RR_ERROR;
    }

    return RR_OK;
}

RRStatus RaftLogSync(RaftLog *log)
{
    if (writeEnd(log->file, log->no_fsync) < 0) {
        return RR_ERROR;
    }
    return RR_OK;
}

RRStatus RaftLogAppend(RaftLog *log, raft_entry_t *entry)
{
    if (RaftLogWriteEntry(log, entry) != RR_OK ||
            writeEnd(log->file, log->no_fsync) < 0) {
        return RR_ERROR;
    }

    log->num_entries++;
    return RR_OK;
}

static off64_t seekEntry(RaftLog *log, raft_index_t idx)
{
    /* Bounds check */
    if (idx <= log->snapshot_last_idx) {
        return 0;
    }

    if (idx > log->snapshot_last_idx + log->num_entries) {
        return 0;
    }

    raft_index_t relidx = idx - log->snapshot_last_idx;
    off64_t offset;
    if (fseek(log->idxfile, sizeof(off64_t) * relidx, SEEK_SET) < 0 ||
            fread(&offset, sizeof(offset), 1, log->idxfile) != 1) {
        return 0;
    }

    if (fseek(log->file, offset, SEEK_SET) < 0) {
        return 0;
    }

    return offset;
}

raft_entry_t *RaftLogGet(RaftLog *log, raft_index_t idx)
{
    if (seekEntry(log, idx) <= 0) {
        return NULL;
    }

    RawLogEntry *re;
    if (readRawLogEntry(log, &re) != RR_OK) {
        return NULL;
    }

    raft_entry_t *e = parseRaftLogEntry(re);
    freeRawLogEntry(re);

    if (!e) {
        return NULL;
    }

    return e;
}

RRStatus RaftLogDelete(RaftLog *log, raft_index_t from_idx, func_entry_notify_f cb, void *cb_arg)
{
    off64_t offset;
    raft_index_t idx = from_idx;
    RRStatus ret = RR_OK;

    if (!(offset = seekEntry(log, from_idx))) {
        return RR_ERROR;
    }

    do {
        RawLogEntry *re;
        raft_entry_t *e;

        if (readRawLogEntry(log, &re) < 0) {
            break;
        }

        if (!strcasecmp(re->elements[0].ptr, "ENTRY")) {
            if ((e = parseRaftLogEntry(re)) == NULL) {
                freeRawLogEntry(re);
                ret = RR_ERROR;
                break;
            }
            if (cb) {
                cb(cb_arg, e, idx);
            }
            idx++;

            raft_entry_release(e);
            freeRawLogEntry(re);
        }
    } while(1);

    ftruncate(fileno(log->file), offset);
    unsigned long removed = log->index - from_idx + 1;
    log->num_entries -= removed;
    log->index = from_idx - 1;

    return ret;
}

RRStatus RaftLogSetVote(RaftLog *log, raft_node_id_t vote)
{
    TRACE_LOG_OP("RaftLogSetVote(vote=%ld)\n", vote);
    log->vote = vote;
    if (updateLogHeader(log) < 0) {
        return RR_ERROR;
    }
    return RR_OK;
}

RRStatus RaftLogSetTerm(RaftLog *log, raft_term_t term, raft_node_id_t vote)
{
    TRACE_LOG_OP("RaftLogSetTerm(term=%lu,vote=%ld)\n", term, vote);
    log->term = term;
    log->vote = vote;
    if (updateLogHeader(log) < 0) {
        return RR_ERROR;
    }
    return RR_OK;
}

raft_index_t RaftLogFirstIdx(RaftLog *log)
{
    return log->snapshot_last_idx;
}

raft_index_t RaftLogCurrentIdx(RaftLog *log)
{
    return log->index;
}

raft_index_t RaftLogCount(RaftLog *log)
{
    return log->num_entries;
}

/*
 * Interface to Raft library.
 */

static void *logImplInit(void *raft, void *arg)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) arg;

    if (!rr->logcache) {
        rr->logcache = EntryCacheNew(ENTRY_CACHE_INIT_SIZE);
    }

    return rr;
}

static void logImplFree(void *rr_)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) rr_;

    RaftLogClose(rr->log);
    EntryCacheFree(rr->logcache);
}

static void logImplReset(void *rr_, raft_index_t index, raft_term_t term)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) rr_;
    RaftLogReset(rr->log, index, term);

    TRACE_LOG_OP("Reset(index=%lu,term=%lu)\n", index, term);

    EntryCacheFree(rr->logcache);
    rr->logcache = EntryCacheNew(ENTRY_CACHE_INIT_SIZE);
}

static int logImplAppend(void *rr_, raft_entry_t *ety)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) rr_;
    TRACE_LOG_OP("Append(id=%d, term=%lu) -> index %lu\n", ety->id, ety->term, rr->log->index + 1);
    if (RaftLogAppend(rr->log, ety) != RR_OK) {
        return -1;
    }
    EntryCacheAppend(rr->logcache, ety, rr->log->index);
    return 0;
}

static int logImplPoll(void *rr_, raft_index_t first_idx)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) rr_;
    TRACE_LOG_OP("Poll(first_idx=%lu)\n", first_idx);
    EntryCacheDeleteHead(rr->logcache, first_idx);
    return 0;
}

static int logImplPop(void *rr_, raft_index_t from_idx, func_entry_notify_f cb, void *cb_arg)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) rr_;
    TRACE_LOG_OP("Delete(from_idx=%lu)\n", from_idx);
    EntryCacheDeleteTail(rr->logcache, from_idx);
    if (RaftLogDelete(rr->log, from_idx, cb, cb_arg) != RR_OK) {
        return -1;
    }
    return 0;
}

static raft_entry_t *logImplGet(void *rr_, raft_index_t idx)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) rr_;
    raft_entry_t *ety;

    ety = EntryCacheGet(rr->logcache, idx);
    if (ety != NULL) {
        TRACE_LOG_OP("Get(idx=%lu) -> (cache) id=%d, term=%lu\n",
                idx, ety->id, ety->term);
        return ety;
    }

    ety = RaftLogGet(rr->log, idx);
    TRACE_LOG_OP("Get(idx=%lu) -> (file) id=%d, term=%lu\n",
            idx, ety ? ety->id : -1, ety ? ety->term : 0);
    return ety;
}

static int logImplGetBatch(void *rr_, raft_index_t idx, int entries_n, raft_entry_t **entries)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) rr_;
    int n = 0;
    raft_index_t i = idx;

    while (n < entries_n) {
        raft_entry_t *e = EntryCacheGet(rr->logcache, i);
        if (!e) {
            e = RaftLogGet(rr->log, i);
        }
        if (!e) {
            break;
        }

        entries[n] = e;
        n++;
        i++;
    }

    TRACE_LOG_OP("GetBatch(idx=%lu entries_n=%d) -> %d\n", idx, entries_n, n);
    return n;
}

static raft_index_t logImplFirstIdx(void *rr_)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) rr_;
    return RaftLogFirstIdx(rr->log);
}

static raft_index_t logImplCurrentIdx(void *rr_)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) rr_;
    return RaftLogCurrentIdx(rr->log);
}

static raft_index_t logImplCount(void *rr_)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) rr_;
    return RaftLogCount(rr->log);
}

raft_log_impl_t RaftLogImpl = {
    .init = logImplInit,
    .free = logImplFree,
    .reset = logImplReset,
    .append = logImplAppend,
    .poll = logImplPoll,
    .pop = logImplPop,
    .get = logImplGet,
    .get_batch = logImplGetBatch,
    .first_idx = logImplFirstIdx,
    .current_idx = logImplCurrentIdx,
    .count = logImplCount
};
