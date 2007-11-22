/* -*- mode: C; c-basic-offset: 4 -*- */

#include <assert.h>
#include <brt.h>
#include "ydb-internal.h"
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <unistd.h>

#include "brt.h"
#include "brt-internal.h"
#include "cachetable.h"
#include "log.h"
#include "memory.h"

struct __toku_db_txn_internal {
    //TXNID txnid64; /* A sixty-four bit txn id. */
    TOKUTXN tokutxn;
    DB_TXN *parent;
};

static char *construct_full_name(const char *dir, const char *fname);
    
struct __toku_db_env_internal {
    int ref_count;
    u_int32_t open_flags;
    int open_mode;
    void (*errcall) (const char *, char *);
    char *errpfx;
    char *dir;                  /* A malloc'd copy of the directory. */
    char *tmp_dir;
    char *lg_dir;
    char *data_dir;
    //void (*noticecall)(DB_ENV *, db_notices);
    long cachetable_size;
    CACHETABLE cachetable;
    TOKULOGGER logger;
};

void __toku_db_env_err(const DB_ENV * env __attribute__ ((__unused__)), int error, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (env->i->errpfx && env->i->errpfx[0] != '\0') fprintf(stderr, "%s: ", env->i->errpfx);
    fprintf(stderr, "YDB Error %d: ", error);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

#define barf() ({ fprintf(stderr, "YDB: BARF %s:%d in %s\n", __FILE__, __LINE__, __func__); })
#define barff(fmt,...) ({ fprintf(stderr, "YDB: BARF %s:%d in %s, ", __FILE__, __LINE__, __func__); fprintf(stderr, fmt, __VA_ARGS__); })
#define note() ({ fprintf(stderr, "YDB: Note %s:%d in %s\n", __FILE__, __LINE__, __func__); })
#define notef(fmt,...) ({ fprintf(stderr, "YDB: Note %s:%d in %s, ", __FILE__, __LINE__, __func__); fprintf(stderr, fmt, __VA_ARGS__); })

static void print_flags(u_int32_t flags) {
    u_int32_t gotit = 0;
    int doneone = 0;
#define doit(flag) if (flag & flags) { if (doneone) fprintf(stderr, " | "); fprintf(stderr, "%s", #flag);  doneone=1; gotit|=flag; }
    doit(DB_INIT_LOCK);
    doit(DB_INIT_LOG);
    doit(DB_INIT_MPOOL);
    doit(DB_INIT_TXN);
    doit(DB_CREATE);
    doit(DB_THREAD);
    doit(DB_RECOVER);
    doit(DB_PRIVATE);
    if (gotit != flags)
        fprintf(stderr, "  flags 0x%x not accounted for", flags & ~gotit);
    fprintf(stderr, "\n");
}

/* TODO make these thread safe */

static void db_env_add_ref(DB_ENV *env) {
    env->i->ref_count++;
}

static void db_env_unref(DB_ENV *env) {
    env->i->ref_count--;
    if (env->i->ref_count == 0)
        env->close(env, 0);
}

static inline int db_env_opened(DB_ENV *env) {
    return env->i->cachetable != 0;
}


static int db_env_parse_config_line(DB_ENV* dbenv, char *command, char *value) {
    int r;
    
    if (!strcmp(command, "set_data_dir")) {
        r = dbenv->set_data_dir(dbenv, value);
    }
    else if (!strcmp(command, "set_tmp_dir")) {
        r = dbenv->set_tmp_dir(dbenv, value);
    }
    else if (!strcmp(command, "set_lg_dir")) {
        r = dbenv->set_lg_dir(dbenv, value);
    }
    else r = -1;
        
    return r;
}

static int db_env_read_config(DB_ENV *env, u_int32_t flags) {
    const char* config_name = "DB_CONFIG";
    char* full_name = NULL;
    char* linebuffer = NULL;
    int buffersize;
    FILE* fp = NULL;
    int r = 0;
    int r2 = 0;
    char* command;
    char* value;
    
    full_name = construct_full_name(env->i->dir, config_name);
    if (full_name == 0) {
        r = ENOMEM;
        goto cleanup;
    }
    if ((fp = fopen(full_name, "r")) == NULL) {
        //Config file is optional.
        if (errno == ENOENT) {
            r = EXIT_SUCCESS;
            goto cleanup;
        }
        r = errno;
        goto cleanup;
    }
    //Read each line, applying configuration parameters.
    //After ignoring leading white space, skip any blank lines
    //or comments (starts with #)
    //Command contains no white space.  Value may contain whitespace.
    int linenumber;
    int ch = '\0';
    BOOL eof = FALSE;
    char* temp;
    char* end;
    int index;
    
    buffersize = 1<<10; //1KB
    linebuffer = toku_malloc(buffersize);
    if (!linebuffer) {
        r = ENOMEM;
        goto cleanup;
    }
    for (linenumber = 1; !eof; linenumber++) {
        /* Read a single line. */
        for (index = 0; TRUE; index++) {
            if ((ch = getc(fp)) == EOF) {
                eof = TRUE;
                if (ferror(fp)) {
                    /* Throw away current line and print warning. */
                    r = errno;
                    goto readerror;
                }
                break;
            }
            if (ch == '\n') break;
            if (index + 1 >= buffersize) {
                //Double the buffer.
                buffersize *= 2;
                linebuffer = toku_realloc(linebuffer, buffersize);
                if (!linebuffer) {
                    r = ENOMEM;
                    goto cleanup;
                }
            }
            linebuffer[index] = ch;
        }
        linebuffer[index] = '\0';
        end = &linebuffer[index];

        /* Separate the line into command/value */
        command = linebuffer;
        //Strip leading spaces.
        while (isspace(*command) && command < end) command++;
        //Find end of command.
        temp = command;
        while (!isspace(*temp) && temp < end) temp++;
        *temp++ = '\0'; //Null terminate command.
        value = temp;
        //Strip leading spaces.
        while (isspace(*value) && value < end) value++;
        if (value < end) {
            //Strip trailing spaces.
            temp = end;
            while (isspace(*(temp-1))) temp--;
            //Null terminate value.
            *temp = '\0';
        }
        //Parse the line.
        if (strlen(command) == 0 || command[0] == '#') continue; //Ignore Comments.
        r = db_env_parse_config_line(env, command, value < end ? value : "");
        if (r != 0) goto parseerror;
    }
    if (0) {
readerror:
        env->err(env, r, "Error reading from DB_CONFIG:%d.\n", linenumber);
    }
    if (0) {
parseerror:
        env->err(env, r, "Error parsing DB_CONFIG:%d.\n", linenumber);
    }
cleanup:
    if (full_name) toku_free(full_name);
    if (linebuffer) toku_free(linebuffer);
    if (fp) r2 = fclose(fp);
    return r ? r : r2;
}

int __toku_db_env_open(DB_ENV * env, const char *home, u_int32_t flags, int mode) {
    int r;

    if (db_env_opened(env))
        return EINVAL;
    
    if (!home) return EINVAL;
    else {
	// Verify that the home exists.
	struct stat buf;
	r = stat(home, &buf);
	if (r!=0) return errno;
    }



    if (!(flags & DB_PRIVATE)) {
        // This means that we don't have to do anything with shared memory.  
        // And that's good enough for mysql. 
        return EINVAL; 
    }

    if (env->i->dir)
        toku_free(env->i->dir);
    env->i->dir = toku_strdup(home);
    if (env->i->dir == 0) 
        return ENOMEM;
    if (0) {
        died1:
        toku_free(env->i->dir);
        env->i->dir = NULL;
        return r;
    }
    if ((r = db_env_read_config(env, flags)) != 0) {
        fprintf(stderr, "FOO FOO FOO \n");
        goto died1;
    }

    env->i->open_flags = flags;
    env->i->open_mode = mode;

    if (flags & (DB_INIT_TXN | DB_INIT_LOG)) {
        char* full_dir = NULL;
        if (env->i->lg_dir) full_dir = construct_full_name(env->i->dir, env->i->lg_dir);
        r = tokulogger_create_and_open_logger(
            full_dir ? full_dir : env->i->dir, &env->i->logger);
        if (full_dir) toku_free(full_dir);
	if (r!=0) goto died1;
	if (0) {
	died2:
	    tokulogger_log_close(&env->i->logger);
	    goto died1;
	}
    }

    r = brt_create_cachetable(&env->i->cachetable, env->i->cachetable_size, ZERO_LSN, env->i->logger);
    if (r!=0) goto died2;
    return 0;
}

int __toku_db_env_close(DB_ENV * env, u_int32_t flags) {
    int r0=0,r1=0;
    if (flags) return EINVAL;
    if (env->i->cachetable)
        r0=toku_cachetable_close(&env->i->cachetable);
    if (env->i->logger)
        r1=tokulogger_log_close(&env->i->logger);
    if (env->i->data_dir)
        toku_free(env->i->data_dir);
    if (env->i->lg_dir)
        toku_free(env->i->lg_dir);
    if (env->i->tmp_dir)
        toku_free(env->i->tmp_dir);
    if (env->i->errpfx)
        toku_free(env->i->errpfx);
    toku_free(env->i->dir);
    toku_free(env->i);
    toku_free(env);
    if (flags!=0) return EINVAL;
    if (r0) return r0;
    if (r1) return r1;
    return 0;
}

int __toku_db_env_log_archive(DB_ENV * env, char **list[], u_int32_t flags) {
    *list = NULL;
    return 0;
}

int __toku_db_env_log_flush(DB_ENV * env, const DB_LSN * lsn) {
    barf();
    return 1;
}

int __toku_db_env_set_cachesize(DB_ENV * env, u_int32_t gbytes, u_int32_t bytes, int ncache) {
    env->i->cachetable_size = ((long) gbytes << 30) + bytes;
    return 0;
}

int __toku_db_env_set_data_dir(DB_ENV * env, const char *dir) {
    if (db_env_opened(env) || !dir)
        return EINVAL;
    if (env->i->data_dir)
        toku_free(env->i->data_dir);
    env->i->data_dir = toku_strdup(dir);
    return env->i->data_dir ? 0 : ENOMEM;
}

void __toku_db_env_set_errcall(DB_ENV * env, void (*errcall) (const char *, char *)) {
    env->i->errcall = errcall;
}

void __toku_db_env_set_errpfx(DB_ENV * env, const char *errpfx) {
    if (env->i->errpfx)
        toku_free(env->i->errpfx);
    env->i->errpfx = toku_strdup(errpfx ? errpfx : "");
}

int __toku_db_env_set_flags(DB_ENV * env, u_int32_t flags, int onoff) {
    assert(flags == 0);
    return 1;
}

int __toku_db_env_set_lg_bsize(DB_ENV * env, u_int32_t bsize) {
    return 1;
}

int __toku_db_env_set_lg_dir(DB_ENV * env, const char *dir) {
    if (db_env_opened(env)) return EINVAL;

    if (env->i->lg_dir) toku_free(env->i->lg_dir);
    if (dir) {
        env->i->lg_dir = toku_strdup(dir);
        if (!env->i->lg_dir) return ENOMEM;
    }
    else env->i->lg_dir = NULL;
    return 0;
}

int __toku_db_env_set_lg_max(DB_ENV * env, u_int32_t lg_max) {
    return 1;
}

int __toku_db_env_set_lk_detect(DB_ENV * env, u_int32_t detect) {
    return 1;
}

int __toku_db_env_set_lk_max(DB_ENV * env, u_int32_t lk_max) {
    return 0;
}

//void __toku_db_env_set_noticecall (DB_ENV *env, void (*noticecall)(DB_ENV *, db_notices)) {
//    env->i->noticecall = noticecall;
//}

int __toku_db_env_set_tmp_dir(DB_ENV * env, const char *tmp_dir) {
    if (db_env_opened(env)) return EINVAL;
    if (!tmp_dir) return EINVAL;
    if (env->i->tmp_dir)
        toku_free(env->i->tmp_dir);
    env->i->tmp_dir = toku_strdup(tmp_dir);
    return env->i->tmp_dir ? 0 : ENOMEM;
}

int __toku_db_env_set_verbose(DB_ENV * env, u_int32_t which, int onoff) {
    return 1;
}

int __toku_db_env_txn_checkpoint(DB_ENV * env, u_int32_t kbyte, u_int32_t min, u_int32_t flags) {
    return 0;
}

int __toku_db_env_txn_stat(DB_ENV * env, DB_TXN_STAT ** statp, u_int32_t flags) {
    return 1;
}

void __toku_default_errcall(const char *errpfx, char *msg) {
    fprintf(stderr, "YDB: %s: %s", errpfx, msg);
}

static int __toku_txn_begin(DB_ENV * env, DB_TXN * stxn, DB_TXN ** txn, u_int32_t flags);

int db_env_create(DB_ENV ** envp, u_int32_t flags) {
    DB_ENV *result = toku_malloc(sizeof(*result));
    if (result == 0)
        return ENOMEM;
    memset(result, 0, sizeof *result);
    result->err = __toku_db_env_err;
    result->open = __toku_db_env_open;
    result->close = __toku_db_env_close;
    result->txn_checkpoint = __toku_db_env_txn_checkpoint;
    result->log_flush = __toku_db_env_log_flush;
    result->set_errcall = __toku_db_env_set_errcall;
    result->set_errpfx = __toku_db_env_set_errpfx;
    //result->set_noticecall = __toku_db_env_set_noticecall;
    result->set_flags = __toku_db_env_set_flags;
    result->set_data_dir = __toku_db_env_set_data_dir;
    result->set_tmp_dir = __toku_db_env_set_tmp_dir;
    result->set_verbose = __toku_db_env_set_verbose;
    result->set_lg_bsize = __toku_db_env_set_lg_bsize;
    result->set_lg_dir = __toku_db_env_set_lg_dir;
    result->set_lg_max = __toku_db_env_set_lg_max;
    result->set_cachesize = __toku_db_env_set_cachesize;
    result->set_lk_detect = __toku_db_env_set_lk_detect;
    result->set_lk_max = __toku_db_env_set_lk_max;
    result->log_archive = __toku_db_env_log_archive;
    result->txn_stat = __toku_db_env_txn_stat;
    result->txn_begin = __toku_txn_begin;

    result->i = toku_malloc(sizeof(*result->i));
    if (result->i == 0) {
        toku_free(result);
        return ENOMEM;
    }
    memset(result->i, 0, sizeof *result->i);
    result->i->ref_count = 1;
    result->i->errcall = __toku_default_errcall;
    result->i->errpfx = toku_strdup("");

    *envp = result;
    return 0;
}

int __toku_db_txn_commit(DB_TXN * txn, u_int32_t flags) {
    //notef("flags=%d\n", flags);
    if (!txn)
        return -1;
    int r = tokulogger_log_commit(txn->i->tokutxn);
    if (r != 0)
        return r;
    if (txn->i)
        toku_free(txn->i);
    toku_free(txn);
    return 0;
}

u_int32_t __toku_db_txn_id(DB_TXN * txn) {
    barf();
    abort();
}

static TXNID next_txn = 0;

static int __toku_txn_begin(DB_ENV * env, DB_TXN * stxn, DB_TXN ** txn, u_int32_t flags) {
    DB_TXN *result = toku_malloc(sizeof(*result));
    if (result == 0)
        return ENOMEM;
    memset(result, 0, sizeof *result);
    //notef("parent=%p flags=0x%x\n", stxn, flags);
    result->commit = __toku_db_txn_commit;
    result->id = __toku_db_txn_id;
    result->i = toku_malloc(sizeof(*result->i));
    assert(result->i);
    result->i->parent = stxn;
    int r = tokutxn_begin(stxn ? stxn->i->tokutxn : 0, &result->i->tokutxn, next_txn++, env->i->logger);
    if (r != 0)
        return r;
    *txn = result;
    return 0;
}

int __toku_txn_abort(DB_TXN * txn) {
    fprintf(stderr, "__toku_txn_abort(%p)\n", txn);
    abort();
}

#if 0
int txn_commit(DB_TXN * txn, u_int32_t flags) {
    fprintf(stderr, "%s:%d\n", __FILE__, __LINE__);
    return tokulogger_log_commit(txn->i->tokutxn);
}
#endif

int log_compare(const DB_LSN * a, const DB_LSN * b) {
    fprintf(stderr, "%s:%d log_compare(%p,%p)\n", __FILE__, __LINE__, a, b);
    abort();
}

int __toku_db_close(DB * db, u_int32_t flags) {
    int r = close_brt(db->i->brt);
    if (r != 0)
        return r;
    // printf("%s:%d %d=__toku_db_close(%p)\n", __FILE__, __LINE__, r, db);
    db_env_unref(db->dbenv);
    toku_free(db->i->database_name);
    toku_free(db->i->full_fname);
    toku_free(db->i);
    toku_free(db);
    return r;
}

struct __toku_dbc_internal {
    BRT_CURSOR c;
    DB *db;
    DB_TXN *txn;
};

int __toku_c_get(DBC * c, DBT * key, DBT * data, u_int32_t flag) {
    int r = brt_cursor_get(c->i->c, key, data, flag, c->i->db, c->i->txn ? c->i->txn->i->tokutxn : 0);
    return r;
}

int __toku_c_close(DBC * c) {
    int r = brt_cursor_close(c->i->c);
    toku_free(c->i);
    toku_free(c);
    return r;
}

int __toku_c_del(DBC * c, u_int32_t flags) {
    int r = brt_cursor_delete(c->i->c, flags);
    return r;
}

int __toku_db_cursor(DB * db, DB_TXN * txn, DBC ** c, u_int32_t flags) {
    DBC *result = toku_malloc(sizeof(*result));
    if (result == 0)
        return ENOMEM;
    memset(result, 0, sizeof *result);
    result->c_get = __toku_c_get;
    result->c_close = __toku_c_close;
    result->c_del = __toku_c_del;
    result->i = toku_malloc(sizeof(*result->i));
    assert(result->i);
    result->i->db = db;
    result->i->txn = txn;
    int r = brt_cursor(db->i->brt, &result->i->c);
    assert(r == 0);
    *c = result;
    return 0;
}

int __toku_db_del(DB * db, DB_TXN * txn __attribute__ ((unused)), DBT * key, u_int32_t flags __attribute((unused))) {
    int r = brt_delete(db->i->brt, key, db);
    return r;
}

int __toku_db_get(DB * db, DB_TXN * txn __attribute__ ((unused)), DBT * key, DBT * data, u_int32_t flags) {
    assert(flags == 0);
    int r = brt_lookup(db->i->brt, key, data, db);
    return r;
}

int __toku_db_key_range(DB * db, DB_TXN * txn, DBT * dbt, DB_KEY_RANGE * kr, u_int32_t flags) {
    barf();
    abort();
}

static int construct_full_name_in_buf(const char *dir, const char *fname, char* full, int length) {
    int l;

    if (!full) return EINVAL;
    l = snprintf(full, length, "%s", dir);
    if (l >= length) return ENAMETOOLONG;
    if (l == 0 || full[l - 1] != '/') {
        if (l + 1 == length) return ENAMETOOLONG;
            
        /* Didn't put a slash down. */
        if (fname[0] != '/') {
            full[l++] = '/';
            full[l] = 0;
        }
    }
    l += snprintf(full + l, length - l, "%s", fname);
    if (l >= length) return ENAMETOOLONG;
    return 0;
}

static char *construct_full_name(const char *dir, const char *fname) {
    if (fname[0] == '/')
        dir = "";
    {
        int dirlen = strlen(dir);
        int fnamelen = strlen(fname);
        int len = dirlen + fnamelen + 2;        // One for the / between (which may not be there).  One for the trailing null.
        char *result = toku_malloc(len);
        // printf("%s:%d len(%d)=%d+%d+2\n", __FILE__, __LINE__, len, dirlen, fnamelen);
        if (construct_full_name_in_buf(dir, fname, result, len) != 0) {
            toku_free(result);
            result = NULL;
        }
        return result;
    }
}

// The decision to embedded subdatabases in files is a little bit painful.
// My original design was to simply create another file, but it turns out that we 
//  have to inherit mode bits and so forth from the first file that was created.
// Other problems may ensue (who is responsible for deleting the file?  That's not so bad actually.)
// This suggests that we really need to put the multiple databases into one file.
int __toku_db_open(DB * db, DB_TXN * txn, const char *fname, const char *dbname, DBTYPE dbtype, u_int32_t flags, int mode) {
    // Warning.  Should check arguments.  Should check return codes on malloc and open and so forth.

    int openflags = 0;
    int r;

    if ((flags & DB_EXCL) && !(flags & DB_CREATE)) return EINVAL;

    if (db->i->full_fname)
        return -1;              /* It was already open. */
    db->i->full_fname = construct_full_name(db->dbenv->i->dir, fname);
    if (db->i->full_fname == 0) {
        r = ENOMEM;
        goto error_cleanup;
    }
    // printf("Full name = %s\n", db->i->full_fname);
    db->i->database_name = toku_strdup(dbname ? dbname : "");
    if (db->i->database_name == 0) {
        r = ENOMEM;
        goto error_cleanup;
    }
    if (flags & DB_RDONLY)
        openflags |= O_RDONLY;
    else
        openflags |= O_RDWR;
    
    {
        struct stat statbuf;
        if (stat(db->i->full_fname, &statbuf) == 0) {
            /* If the database exists at the file level, and we specified no db_name, then complain here. */
            if (dbname == 0 && (flags & DB_CREATE)) {
                if (flags & DB_EXCL) {
                    r = EEXIST;
                    goto error_cleanup;
                }
                flags &= ~DB_CREATE;
            }
        } else {
            if (!(flags & DB_CREATE)) {
                r = ENOENT;
                goto error_cleanup;
            }
        }
    }
    if (flags & DB_CREATE) openflags |= O_CREAT;

    db->i->open_flags = flags;
    db->i->open_mode = mode;

    r = brt_open(db->i->brt, db->i->full_fname, fname, dbname,
		 flags & DB_CREATE, flags & DB_EXCL,
		 db->dbenv->i->cachetable,
		 txn ? txn->i->tokutxn : NULL_TXN);
    if (r != 0)
        goto error_cleanup;

    return 0;
 
error_cleanup:
    if (db->i->database_name) {
        toku_free(db->i->database_name);
        db->i->database_name = NULL;
    }
    if (db->i->full_fname) {
        toku_free(db->i->full_fname);
        db->i->full_fname = NULL;
    }
    return r;
}

int __toku_db_put(DB * db, DB_TXN * txn, DBT * key, DBT * data, u_int32_t flags) {
    int r = brt_insert(db->i->brt, key, data, db, txn ? txn->i->tokutxn : 0);
    //printf("%s:%d %d=__toku_db_put(...)\n", __FILE__, __LINE__, r);
    return r;
}

int __toku_db_remove(DB * db, const char *fname, const char *dbname, u_int32_t flags) {
    int r;
    int r2;
    char ffull[PATH_MAX];

    //TODO: DB_ENV->set_data_dir should affect db_remove's directories.
    //TODO: Verify DB* db not yet opened

    if (dbname) {
        //TODO: Verify the target db is not open
        //TODO: Use master database (instead of manual edit) when implemented.

        if ((r = db->open(db, NULL, fname, dbname, DB_BTREE, 0, 0777)) != 0) goto cleanup;
        r = brt_remove_subdb(db->i->brt, dbname, flags);
cleanup:
        r2 = db->close(db, 0);
        return r ? r : r2;
    }
    //TODO: Verify db file not in use. (all dbs in the file must be unused)
    if ((r = construct_full_name_in_buf(db->dbenv->i->dir, fname, ffull, sizeof(ffull))) != 0) {
        //Name too long.
        assert(r == ENAMETOOLONG);
        return r;
    }
    r2 = db->close(db, 0);
    if (r == 0 && r2 == 0) r = unlink(ffull);
    return r ? r : r2;
}

int __toku_db_rename(DB * db, const char *namea, const char *nameb, const char *namec, u_int32_t flags) {
    char afull[PATH_MAX], cfull[PATH_MAX];
    int r;
    assert(nameb == 0);
    r = snprintf(afull, PATH_MAX, "%s%s", db->dbenv->i->dir, namea);
    assert(r < PATH_MAX);
    r = snprintf(cfull, PATH_MAX, "%s%s", db->dbenv->i->dir, namec);
    assert(r < PATH_MAX);
    return rename(afull, cfull);
}

int __toku_db_set_bt_compare(DB * db, int (*bt_compare) (DB *, const DBT *, const DBT *)) {
    int r = brt_set_bt_compare(db->i->brt, bt_compare);
    return r;
}

int __toku_db_set_dup_compare(DB *db, int (*dup_compare)(DB *, const DBT *, const DBT *)) {
    int r = brt_set_dup_compare(db->i->brt, dup_compare);
    return r;
}

int __toku_db_set_flags(DB * db, u_int32_t flags) {
    u_int32_t tflags = 0;
    if (flags & DB_DUP)
        tflags += TOKU_DB_DUP;
    if (flags & DB_DUPSORT)
        tflags += TOKU_DB_DUPSORT;
    int r= brt_set_flags(db->i->brt, tflags);
    return r;
}

int __toku_db_set_pagesize(DB *db, u_int32_t pagesize) {
    int r = brt_set_nodesize(db->i->brt, pagesize);
    return r;
}

int __toku_db_stat(DB * db, void *v, u_int32_t flags) {
    barf();
    abort();
}

extern int toku_default_compare_fun(DB * db, const DBT * a, const DBT * b);

int db_create(DB ** db, DB_ENV * env, u_int32_t flags) {
    int r;

    /* if the env already exists then add a ref to it
       otherwise create one */
    if (env) {
        if (!db_env_opened(env))
            return EINVAL;
        db_env_add_ref(env);
    } else {
        r = db_env_create(&env, 0);
        if (r != 0)
            return r;
        r = env->open(env, ".", DB_PRIVATE + DB_INIT_MPOOL, 0);
        if (r != 0) {
            env->close(env, 0);
            return r;
        }
        assert(db_env_opened(env));
    }
    
    DB *result = toku_malloc(sizeof(*result));
    if (result == 0) {
        db_env_unref(env);
        return ENOMEM;
    }
    memset(result, 0, sizeof *result);
    result->dbenv = env;
    result->close = __toku_db_close;
    result->cursor = __toku_db_cursor;
    result->del = __toku_db_del;
    result->get = __toku_db_get;
    result->key_range = __toku_db_key_range;
    result->open = __toku_db_open;
    result->put = __toku_db_put;
    result->remove = __toku_db_remove;
    result->rename = __toku_db_rename;
    result->set_bt_compare = __toku_db_set_bt_compare;
    result->set_dup_compare = __toku_db_set_dup_compare;
    result->set_pagesize = __toku_db_set_pagesize;
    result->set_flags = __toku_db_set_flags;
    result->stat = __toku_db_stat;
    result->i = toku_malloc(sizeof(*result->i));
    if (result->i == 0) {
        toku_free(result);
        db_env_unref(env);
        return ENOMEM;
    }
    memset(result->i, 0, sizeof *result->i);
    result->i->freed = 0;
    result->i->header = 0;
    result->i->database_number = 0;
    result->i->full_fname = 0;
    result->i->database_name = 0;
    result->i->open_flags = 0;
    result->i->open_mode = 0;
    result->i->brt = 0;
    r = brt_create(&result->i->brt);
    if (r != 0) {
        toku_free(result->i);
        toku_free(result);
        db_env_unref(env);
        return ENOMEM;
    }
    *db = result;
    return 0;
}

char *db_strerror(int error) {
    char *errorstr;
    if (error >= 0) {
        errorstr = strerror(error);
        if (errorstr)
            return errorstr;
    }
    
    static char unknown_result[100];    // Race condition if two threads call this at the same time.    However even in a bad case, it should be some sort of nul-terminated string.
    errorstr = unknown_result;
    snprintf(errorstr, sizeof unknown_result, "Unknown error code: %d", error);
    return errorstr;
}

const char *db_version(int *major, int *minor, int *patch) {
    if (major)
        *major = DB_VERSION_MAJOR;
    if (minor)
        *minor = DB_VERSION_MINOR;
    if (patch)
        *patch = DB_VERSION_PATCH;
    return DB_VERSION_STRING;
}
