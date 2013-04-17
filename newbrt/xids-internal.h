/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "Copyright (c) 2007, 2008 Tokutek Inc.  All rights reserved."


#ifndef XIDS_INTERNAL_H
#define XIDS_INTERNAL_H

// Variable size list of transaction ids (known in design doc as xids<>).
// ids[0] is the outermost transaction.
// ids[num_xids - 1] is the innermost transaction.
// Should only be accessed by accessor functions xids_xxx, not directly.
typedef struct xids_t {
    u_int8_t  num_stored_xids;    // maximum value of MAX_TRANSACTION_RECORDS - 1 ...
				    // ... because transaction 0 is implicit
    TXNID     ids[];
} XIDS_S;

#endif
