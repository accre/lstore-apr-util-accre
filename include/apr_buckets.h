/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000-2001 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Software Foundation.  For more
 * information on the Apache Software Foundation, please see
 * <http://www.apache.org/>.
 */

#ifndef APR_BUCKETS_H
#define APR_BUCKETS_H

#include "apu.h"
#include "apr.h"
#include "apr_network_io.h"
#include "apr_file_io.h"
#include "apr_general.h"
#include "apr_mmap.h"
#include "apr_errno.h"
#include "apr_ring.h"
#include "apr_sms.h"
#if APR_HAVE_SYS_UIO_H
#include <sys/uio.h>	/* for struct iovec */
#endif
#if APR_HAVE_STDARG_H
#include <stdarg.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @package Bucket Brigades
 */

#define APR_BUCKET_BUFF_SIZE 8192

typedef enum {
    APR_BLOCK_READ,
    APR_NONBLOCK_READ
} apr_read_type_e;

/*
 * The one-sentence buzzword-laden overview: Bucket brigades represent
 * a complex data stream that can be passed through a layered IO
 * system without unnecessary copying. A longer overview follows...
 *
 * A bucket brigade is a doubly linked list (ring) of buckets, so we
 * aren't limited to inserting at the front and removing at the end.
 * Buckets are only passed around as members of a brigade, although
 * singleton buckets can occur for short periods of time.
 *
 * Buckets are data stores of various types. They can refer to data in
 * memory, or part of a file or mmap area, or the output of a process,
 * etc. Buckets also have some type-dependent accessor functions:
 * read, split, copy, setaside, and destroy.
 *
 * read returns the address and size of the data in the bucket. If the
 * data isn't in memory then it is read in and the bucket changes type
 * so that it can refer to the new location of the data. If all the
 * data doesn't fit in the bucket then a new bucket is inserted into
 * the brigade to hold the rest of it.
 *
 * split divides the data in a bucket into two regions. After a split
 * the original bucket refers to the first part of the data and a new
 * bucket inserted into the brigade after the original bucket refers
 * to the second part of the data. Reference counts are maintained as
 * necessary.
 *
 * setaside ensures that the data in the bucket has a long enough
 * lifetime. Sometimes it is convenient to create a bucket referring
 * to data on the stack in the expectation that it will be consumed
 * (output to the network) before the stack is unwound. If that
 * expectation turns out not to be valid, the setaside function is
 * called to move the data somewhere safer.
 *
 * copy makes a duplicate of the bucket structure as long as it's
 * possible to have multiple references to a single copy of the
 * data itself.  Not all bucket types can be copied.
 *
 * destroy maintains the reference counts on the resources used by a
 * bucket and frees them if necessary.
 *
 * Note: all of the above functions have wrapper macros (apr_bucket_read(),
 * apr_bucket_destroy(), etc), and those macros should be used rather
 * than using the function pointers directly.
 *
 * To write a bucket brigade, they are first made into an iovec, so that we
 * don't write too little data at one time.  Currently we ignore compacting the
 * buckets into as few buckets as possible, but if we really want good
 * performance, then we need to compact the buckets before we convert to an
 * iovec, or possibly while we are converting to an iovec.
 */

/**
 * Forward declaration of the main types.
 */

typedef struct apr_bucket_brigade apr_bucket_brigade;

typedef struct apr_bucket apr_bucket;

typedef struct apr_bucket_type_t apr_bucket_type_t;
struct apr_bucket_type_t {
    /**
     * The name of the bucket type
     */
    const char *name;
    /** 
     * The number of functions this bucket understands.  Can not be less than
     * five.
     */
    int num_func;
    /**
     * Free the private data and any resources used by the bucket (if they
     *  aren't shared with another bucket).  This function is required to be
     *  implemented for all bucket types, though it might be a no-op on some
     *  of them (namely ones that never allocate any private data structures).
     * @param data The private data pointer from the bucket to be destroyed
     */
    void (*destroy)(void *data);

    /**
     * Read the data from the bucket. This is required to be implemented
     *  for all bucket types.
     * @param b The bucket to read from
     * @param str A place to store the data read.  Allocation should only be
     *            done if absolutely necessary. 
     * @param len The amount of data read.
     * @param block Should this read function block if there is more data that
     *              cannot be read immediately.
     * @deffunc apr_status_t read(apr_bucket *b, const char **str, apr_size_t *len, apr_read_type_e block)
     */
    apr_status_t (*read)(apr_bucket *b, const char **str, apr_size_t *len, 
                         apr_read_type_e block);
    
    /**
     * Make it possible to set aside the data for at least as long as the
     *  given pool. Buckets containing data that could potentially die before
     *  this pool (e.g. the data resides on the stack, in a child pool of
     *  the given pool, or in a disjoint pool) must somehow copy, shift, or
     *  transform the data to have the proper lifetime.
     * @param e The bucket to convert
     * @deffunc apr_status_t setaside(apr_bucket *e)
     * @tip Some bucket types contain data that will always outlive the
     *      bucket itself. For example no data (EOS and FLUSH), or the data
     *      resides in global, constant memory (IMMORTAL), or the data is on
     *      the heap (HEAP). For these buckets, apr_bucket_setaside_noop can
     *      be used.
     */
    apr_status_t (*setaside)(apr_bucket *e, apr_pool_t *pool);

    /**
     * Split one bucket in two at the specified position by duplicating
     *  the bucket structure (not the data) and modifying any necessary
     *  start/end/offset information.  If it's not possible to do this
     *  for the bucket type (perhaps the length of the data is indeterminate,
     *  as with pipe and socket buckets), then APR_ENOTIMPL is returned.
     * @param e The bucket to split
     * @param point The offset of the first byte in the new bucket
     * @deffunc apr_status_t split(apr_bucket *e, apr_size_t point)
     */
    apr_status_t (*split)(apr_bucket *e, apr_size_t point);

    /**
     * Copy the bucket structure (not the data), assuming that this is
     *  possible for the bucket type. If it's not, APR_ENOTIMPL is returned.
     * @param e The bucket to copy
     * @param c Returns a pointer to the new bucket
     * @deffunc apr_status_t copy
     */
    apr_status_t (*copy)(apr_bucket *e, apr_bucket **c);

};

/**
 * apr_bucket structures are allocated from an SMS with apr_sms_malloc()
 * and their lifetime is controlled by the parent apr_bucket_brigade
 * structure. Buckets can move from one brigade to another e.g. by
 * calling APR_BRIGADE_CONCAT(). In general the data in a bucket has
 * the same lifetime as the bucket and is freed when the bucket is
 * destroyed; if the data is shared by more than one bucket (e.g.
 * after a split) the data is freed when the last bucket goes away.
 */
struct apr_bucket {
    /** Links to the rest of the brigade */
    APR_RING_ENTRY(apr_bucket) link;
    /** The type of bucket.  */
    const apr_bucket_type_t *type;
    /** The length of the data in the bucket.  This could have been implemented
     *  with a function, but this is an optimization, because the most
     *  common thing to do will be to get the length.  If the length is unknown,
     *  the value of this field will be (apr_size_t)(-1).
     */
    apr_size_t length;
    /** The start of the data in the bucket relative to the private base
     *  pointer.  The vast majority of bucket types allow a fixed block of
     *  data to be referenced by multiple buckets, each bucket pointing to
     *  a different segment of the data.  That segment starts at base+start
     *  and ends at base+start+length.  
     *  If the length == (apr_size_t)(-1), then start == -1.
     */
    apr_off_t start;
    /** type-dependent data hangs off this pointer */
    void *data;	
    /**
     * Pointer to SMS in which bucket is allocated.  Used when freeing
     * the bucket and when allocating for private data structures.
     */
    apr_sms_t *sms;
};

/** A list of buckets */
struct apr_bucket_brigade {
    /** The pool to associate the brigade with.  The data is not allocated out
     *  of the pool, but a cleanup is registered with this pool.  If the 
     *  brigade is destroyed by some mechanism other than pool destruction,
     *  the destroying function is responsible for killing the cleanup.
     */
    apr_pool_t *p;
    /** The buckets in the brigade are on this list. */
    /*
     * The apr_bucket_list structure doesn't actually need a name tag
     * because it has no existence independent of struct apr_bucket_brigade;
     * the ring macros are designed so that you can leave the name tag
     * argument empty in this situation but apparently the Windows compiler
     * doesn't like that.
     */
    APR_RING_HEAD(apr_bucket_list, apr_bucket) list;
};

/* temporary */
APU_DECLARE_DATA extern apr_sms_t *apr_bucket_global_sms;

typedef apr_status_t (*apr_brigade_flush)(apr_bucket_brigade *bb, void *ctx);

/**
 * Wrappers around the RING macros to reduce the verbosity of the code
 * that handles bucket brigades.
 */
/**
 * The magic pointer value that indicates the head of the brigade
 * @tip This is used to find the beginning and end of the brigade, eg:
 * <pre>
 *      while (e != APR_BRIGADE_SENTINEL(b)) {
 *          ...
 *          e = APR_BUCKET_NEXT(e);
 *      }
 * </pre>
 * @param  b The brigade
 * @return The magic pointer value
 * @deffunc apr_bucket *APR_BRIGADE_SENTINEL(apr_bucket_brigade *b)
 */
#define APR_BRIGADE_SENTINEL(b)	APR_RING_SENTINEL(&(b)->list, apr_bucket, link)

/**
 * Determine if the bucket brigade is empty
 * @param b The brigade to check
 * @return true or false
 * @deffunc int APR_BRIGADE_EMPTY(apr_bucket_brigade *b)
 */
#define APR_BRIGADE_EMPTY(b)	APR_RING_EMPTY(&(b)->list, apr_bucket, link)

/**
 * Return the first bucket in a brigade
 * @param b The brigade to query
 * @return The first bucket in the brigade
 * @deffunc apr_bucket *APR_BRIGADE_FIRST(apr_bucket_brigade *b)
 */
#define APR_BRIGADE_FIRST(b)	APR_RING_FIRST(&(b)->list)
/**
 * Return the last bucket in a brigade
 * @param b The brigade to query
 * @return The last bucket in the brigade
 * @deffunc apr_bucket *APR_BRIGADE_LAST(apr_bucket_brigade *b)
 */
#define APR_BRIGADE_LAST(b)	APR_RING_LAST(&(b)->list)

/**
 * Iterate through a bucket brigade
 * @param e The current bucket
 * @param b The brigade to iterate over
 * @tip This is the same as either:
 * <pre>
 *	e = APR_BRIGADE_FIRST(b);
 * 	while (e != APR_BRIGADE_SENTINEL(b)) {
 *	    ...
 * 	    e = APR_BUCKET_NEXT(e);
 * 	}
 *  OR
 * 	for (e = APR_BRIGADE_FIRST(b);
 *           e != APR_BRIGADE_SENTINEL(b);
 *           e = APR_BUCKET_NEXT(e)) {
 *	    ...
 * 	}
 * </pre>
 * @warning Be aware that you cannot change the value of e within
 * the foreach loop, nor can you destroy the bucket it points to.
 * Modifying the prev and next pointers of the bucket is dangerous
 * but can be done if you're careful.  If you change e's value or
 * destroy the bucket it points to, then APR_BRIGADE_FOREACH
 * will have no way to find out what bucket to use for its next
 * iteration.  The reason for this can be seen by looking closely
 * at the equivalent loops given in the tip above.  So, for example,
 * if you are writing a loop that empties out a brigade one bucket
 * at a time, APR_BRIGADE_FOREACH just won't work for you.  Do it
 * by hand, like so:
 * <pre>
 *      while (!APR_BRIGADE_EMPTY(b)) {
 *          e = APR_BRIGADE_FIRST(b);
 *          ...
 *          apr_bucket_delete(e);
 *      }
 * </pre>
 * @deffunc void APR_BRIGADE_FOREACH(apr_bucket *e, apr_bucket_brigade *b)
 */
#define APR_BRIGADE_FOREACH(e, b)					\
	APR_RING_FOREACH((e), &(b)->list, apr_bucket, link)

/**
 * Insert a list of buckets at the front of a brigade
 * @param b The brigade to add to
 * @param e The first bucket in a list of buckets to insert
 * @deffunc void APR_BRIGADE_INSERT_HEAD(apr_bucket_brigade *b, apr_bucket *e)
 */
#define APR_BRIGADE_INSERT_HEAD(b, e) do {				\
	apr_bucket *ap__b = (e);                                        \
	APR_RING_INSERT_HEAD(&(b)->list, ap__b, apr_bucket, link);	\
    } while (0)

/**
 * Insert a list of buckets at the end of a brigade
 * @param b The brigade to add to
 * @param e The first bucket in a list of buckets to insert
 * @deffunc void APR_BRIGADE_INSERT_TAIL(apr_bucket_brigade *b, apr_bucket *e)
 */
#define APR_BRIGADE_INSERT_TAIL(b, e) do {				\
	apr_bucket *ap__b = (e);					\
	APR_RING_INSERT_TAIL(&(b)->list, ap__b, apr_bucket, link);	\
    } while (0)

/**
 * Concatenate brigade b onto the end of brigade a, leaving brigade b empty
 * @param a The first brigade
 * @param b The second brigade
 * @deffunc void APR_BRIGADE_CONCAT(apr_bucket_brigade *a, apr_bucket_brigade *b)
 */
#define APR_BRIGADE_CONCAT(a, b)					\
	APR_RING_CONCAT(&(a)->list, &(b)->list, apr_bucket, link)

/**
 * Insert a list of buckets before a specified bucket
 * @param a The bucket to insert before
 * @param b The buckets to insert
 * @deffunc void APR_BUCKET_INSERT_BEFORE(apr_bucket *a, apr_bucket *b)
 */
#define APR_BUCKET_INSERT_BEFORE(a, b) do {				\
	apr_bucket *ap__a = (a), *ap__b = (b);				\
	APR_RING_INSERT_BEFORE(ap__a, ap__b, link);			\
    } while (0)

/**
 * Insert a list of buckets after a specified bucket
 * @param a The bucket to insert after
 * @param b The buckets to insert
 * @deffunc void APR_BUCKET_INSERT_AFTER(apr_bucket *a, apr_bucket *b)
 */
#define APR_BUCKET_INSERT_AFTER(a, b) do {				\
	apr_bucket *ap__a = (a), *ap__b = (b);				\
	APR_RING_INSERT_AFTER(ap__a, ap__b, link);			\
    } while (0)

/**
 * Get the next bucket in the list
 * @param e The current bucket
 * @return The next bucket
 * @deffunc apr_bucket *APR_BUCKET_NEXT(apr_bucket *e)
 */
#define APR_BUCKET_NEXT(e)	APR_RING_NEXT((e), link)
/**
 * Get the previous bucket in the list
 * @param e The current bucket
 * @return The previous bucket
 * @deffunc apr_bucket *APR_BUCKET_PREV(apr_bucket *e)
 */
#define APR_BUCKET_PREV(e)	APR_RING_PREV((e), link)

/**
 * Remove a bucket from its bucket brigade
 * @param e The bucket to remove
 * @deffunc void APR_BUCKET_REMOVE(apr_bucket *e)
 */
#define APR_BUCKET_REMOVE(e)	APR_RING_REMOVE((e), link)

/**
 * Initialize a new bucket's prev/next pointers
 * @param e The bucket to initialize
 * @deffunc void APR_BUCKET_INIT(apr_bucket *e)
 */
#define APR_BUCKET_INIT(e)	APR_RING_ELEM_INIT((e), link);

/**
 * Determine if a bucket is a FLUSH bucket
 * @param e The bucket to inspect
 * @return true or false
 * @deffunc int APR_BUCKET_IS_FLUSH(apr_bucket *e)
 */
#define APR_BUCKET_IS_FLUSH(e)       (e->type == &apr_bucket_type_flush)
/**
 * Determine if a bucket is an EOS bucket
 * @param e The bucket to inspect
 * @return true or false
 * @deffunc int APR_BUCKET_IS_EOS(apr_bucket *e)
 */
#define APR_BUCKET_IS_EOS(e)         (e->type == &apr_bucket_type_eos)
/**
 * Determine if a bucket is a FILE bucket
 * @param e The bucket to inspect
 * @return true or false
 * @deffunc int APR_BUCKET_IS_FILE(apr_bucket *e)
 */
#define APR_BUCKET_IS_FILE(e)        (e->type == &apr_bucket_type_file)
/**
 * Determine if a bucket is a PIPE bucket
 * @param e The bucket to inspect
 * @return true or false
 * @deffunc int APR_BUCKET_IS_PIPE(apr_bucket *e)
 */
#define APR_BUCKET_IS_PIPE(e)        (e->type == &apr_bucket_type_pipe)
/**
 * Determine if a bucket is a SOCKET bucket
 * @param e The bucket to inspect
 * @return true or false
 * @deffunc int APR_BUCKET_IS_SOCKET(apr_bucket *e)
 */
#define APR_BUCKET_IS_SOCKET(e)      (e->type == &apr_bucket_type_socket)
/**
 * Determine if a bucket is a HEAP bucket
 * @param e The bucket to inspect
 * @return true or false
 * @deffunc int APR_BUCKET_IS_HEAP(apr_bucket *e)
 */
#define APR_BUCKET_IS_HEAP(e)        (e->type == &apr_bucket_type_heap)
/**
 * Determine if a bucket is a TRANSIENT bucket
 * @param e The bucket to inspect
 * @return true or false
 * @deffunc int APR_BUCKET_IS_TRANSIENT(apr_bucket *e)
 */
#define APR_BUCKET_IS_TRANSIENT(e)   (e->type == &apr_bucket_type_transient)
/**
 * Determine if a bucket is a IMMORTAL bucket
 * @param e The bucket to inspect
 * @return true or false
 * @deffunc int APR_BUCKET_IS_IMMORTAL(apr_bucket *e)
 */
#define APR_BUCKET_IS_IMMORTAL(e)    (e->type == &apr_bucket_type_immortal)
#if APR_HAS_MMAP
/**
 * Determine if a bucket is a MMAP bucket
 * @param e The bucket to inspect
 * @return true or false
 * @deffunc int APR_BUCKET_IS_MMAP(apr_bucket *e)
 */
#define APR_BUCKET_IS_MMAP(e)        (e->type == &apr_bucket_type_mmap)
#endif
/**
 * Determine if a bucket is a POOL bucket
 * @param e The bucket to inspect
 * @return true or false
 * @deffunc int APR_BUCKET_IS_POOL(apr_bucket *e)
 */
#define APR_BUCKET_IS_POOL(e)        (e->type == &apr_bucket_type_pool)

/**
 * Remove all zero length buckets from the brigade.
 * @param b The bucket brigade
 */
#define APR_BRIGADE_NORMALIZE(b)       \
do { \
    apr_bucket *e = APR_BRIGADE_FIRST(b); \
    do {  \
        if (e->length == 0) { \
            apr_bucket *d; \
            d = APR_BUCKET_NEXT(e); \
            apr_bucket_delete(e); \
            e = d; \
        } \
        e = APR_BUCKET_NEXT(e); \
    } while (!APR_BRIGADE_EMPTY(b) && (e != APR_BRIGADE_SENTINEL(b))); \
} while (0)

/*
 * General-purpose reference counting for the various bucket types.
 *
 * Any bucket type that keeps track of the resources it uses (i.e.
 * most of them except for IMMORTAL, TRANSIENT, and EOS) needs to
 * attach a reference count to the resource so that it can be freed
 * when the last bucket that uses it goes away. Resource-sharing may
 * occur because of bucket splits or buckets that refer to globally
 * cached data. */

typedef struct apr_bucket_refcount apr_bucket_refcount;
/**
 * The structure used to manage the shared resource must start with an
 * apr_bucket_refcount which is updated by the general-purpose refcount
 * code. A pointer to the bucket-type-dependent private data structure
 * can be cast to a pointer to an apr_bucket_refcount and vice versa.
 */
struct apr_bucket_refcount {
    /** The number of references to this bucket */
    int          refcount;
};

/*  *****  Reference-counted bucket types  *****  */


typedef struct apr_bucket_heap apr_bucket_heap;
/**
 * A bucket referring to data allocated off the heap.
 */
struct apr_bucket_heap {
    /** Number of buckets using this memory */
    apr_bucket_refcount  refcount;
    /** The start of the data actually allocated.  This should never be
     * modified, it is only used to free the bucket.
     */
    char    *base;
    /** how much memory was allocated */
    apr_size_t  alloc_len;
    /** The SMS from which this structure was allocated */
    apr_sms_t *sms;
};

typedef struct apr_bucket_pool apr_bucket_pool;
/**
 * A bucket referring to data allocated from a pool
 */
struct apr_bucket_pool {
    /** The pool bucket must be able to be easily morphed to a heap
     * bucket if the pool gets cleaned up before all references are
     * destroyed.  This apr_bucket_heap structure is populated automatically
     * when the pool gets cleaned up, and subsequent calls to pool_read()
     * will result in the apr_bucket in question being morphed into a
     * regular heap bucket.  (To avoid having to do many extra refcount
     * manipulations and b->data manipulations, the apr_bucket_pool
     * struct actually *contains* the apr_bucket_heap struct that it
     * will become as its first element; the two share their
     * apr_bucket_refcount members.)
     */
    apr_bucket_heap  heap;
    /** The block of data actually allocated from the pool.
     * Segments of this block are referenced by adjusting
     * the start and length of the apr_bucket accordingly.
     * This will be NULL after the pool gets cleaned up.
     */
    const char *base;
    /** The pool the data was allocated from.  When the pool
     * is cleaned up, this gets set to NULL as an indicator
     * to pool_read() that the data is now on the heap and
     * so it should morph the bucket into a regular heap
     * bucket before continuing.
     */
    apr_pool_t *pool;
};

#if APR_HAS_MMAP
typedef struct apr_bucket_mmap apr_bucket_mmap;
/**
 * A bucket referring to an mmap()ed file
 */
struct apr_bucket_mmap {
    /** Number of buckets using this memory */
    apr_bucket_refcount  refcount;
    /** The mmap this sub_bucket refers to */
    apr_mmap_t *mmap;
    /** The SMS from which this structure was allocated */
    apr_sms_t *sms;
};
#endif

typedef struct apr_bucket_file apr_bucket_file;
/**
 * A bucket referring to an file
 */
struct apr_bucket_file {
    /** Number of buckets using this memory */
    apr_bucket_refcount  refcount;
    /** The file this bucket refers to */
    apr_file_t *fd;
    /** The pool into which any needed structures should
     *  be created while reading from this file bucket */
    apr_pool_t *readpool;
    /** The SMS from which this structure was allocated */
    apr_sms_t *sms;
};

/*  *****  Bucket Brigade Functions  *****  */
/**
 * Create a new bucket brigade.  The bucket brigade is originally empty.
 * @param The pool to associate with the brigade.  Data is not allocated out
 *        of the pool, but a cleanup is registered.
 * @return The empty bucket brigade
 * @deffunc apr_bucket_brigade *apr_brigade_create(apr_pool_t *p)
 */
APU_DECLARE(apr_bucket_brigade *) apr_brigade_create(apr_pool_t *p);

/**
 * destroy an entire bucket brigade.  This includes destroying all of the
 * buckets within the bucket brigade's bucket list. 
 * @param b The bucket brigade to destroy
 * @deffunc apr_status_t apr_brigade_destroy(apr_bucket_brigade *b)
 */
APU_DECLARE(apr_status_t) apr_brigade_destroy(apr_bucket_brigade *b);

/**
 * empty out an entire bucket brigade.  This includes destroying all of the
 * buckets within the bucket brigade's bucket list.  This is similar to
 * apr_brigade_destroy(), except that it does not deregister the brigade's
 * pool cleanup function.
 * @tip Generally, you should use apr_brigade_destroy().  This function
 *      can be useful in situations where you have a single brigade that
 *      you wish to reuse many times by destroying all of the buckets in
 *      the brigade and putting new buckets into it later.
 * @param b The bucket brigade to clean up
 * @deffunc apr_status_t apr_brigade_cleanup(apr_bucket_brigade *b)
 */
APU_DECLARE(apr_status_t) apr_brigade_cleanup(void *data);

/**
 * Split a bucket brigade into two, such that the given bucket is the
 * first in the new bucket brigade. This function is useful when a
 * filter wants to pass only the initial part of a brigade to the next
 * filter.
 * @param b The brigade to split
 * @param e The first element of the new brigade
 * @return The new brigade
 * @deffunc apr_bucket_brigade *apr_brigade_split(apr_bucket_brigade *b, apr_bucket *e)
 */
APU_DECLARE(apr_bucket_brigade *) apr_brigade_split(apr_bucket_brigade *b,
                                                    apr_bucket *e);

/**
 * Partition a bucket brigade at a given offset (in bytes from the start of
 * the brigade).  This is useful whenever a filter wants to use known ranges
 * of bytes from the brigade; the ranges can even overlap.
 * @param b The brigade to partition
 * @param point The offset at which to partition the brigade
 * @param after_point Returns a pointer to the first bucket after the partition
 * @deffunc apr_status_t apr_brigade_partition(apr_bucket_brigade *b, apr_off_t point, apr_bucket **after_point)
 */
APU_DECLARE(apr_status_t) apr_brigade_partition(apr_bucket_brigade *b,
                                                apr_off_t point,
                                                apr_bucket **after_point);

#if APR_NOT_DONE_YET
/**
 * consume nbytes from beginning of b -- call apr_bucket_destroy as
 * appropriate, and/or modify start on last element 
 * @param b The brigade to consume data from
 * @param nbytes The number of bytes to consume
 * @deffunc void apr_brigade_consume(apr_bucket_brigade *b, apr_off_t nbytes)
 */
APU_DECLARE(void) apr_brigade_consume(apr_bucket_brigade *b,
                                      apr_off_t nbytes);
#endif

/**
 * Return the total length of the brigade.
 * @param bb The brigade to compute the length of
 * @param read_all Read unknown-length buckets to force a size
 @ @param length Set to length of the brigade, or -1 if it has unknown-length buckets
 * @deffunc apr_status_t apr_brigade_length(apr_bucket_brigade *bb, int read_all, apr_off_t *length)
 */
APU_DECLARE(apr_status_t) apr_brigade_length(apr_bucket_brigade *bb,
                                             int read_all,
                                             apr_off_t *length);

/**
 * create an iovec of the elements in a bucket_brigade... return number 
 * of elements used.  This is useful for writing to a file or to the
 * network efficiently.
 * @param b The bucket brigade to create the iovec from
 * @param vec The iovec to create
 * @param nvec The number of elements in the iovec. On return, it is the
 *             number of iovec elements actually filled out.
 * @deffunc apr_status_t apr_brigade_to_iovec(apr_bucket_brigade *b, struct iovec *vec, int *nvec);
 */
APU_DECLARE(apr_status_t) apr_brigade_to_iovec(apr_bucket_brigade *b, 
                                               struct iovec *vec, int *nvec);

/**
 * This function writes a list of strings into a bucket brigade. 
 * @param b The bucket brigade to add to
 * @param va A list of strings to add
 * @return The number of bytes added to the brigade
 * @deffunc apr_status_t apr_brigade_vputstrs(apr_bucket_brigade *b, apr_brigade_flush flush, void *ctx, va_list va)
 */
APU_DECLARE(apr_status_t) apr_brigade_vputstrs(apr_bucket_brigade *b,
                                               apr_brigade_flush flush,
                                               void *ctx,
                                               va_list va);

/**
 * This function writes an string into a bucket brigade.
 * @param b The bucket brigade to add to
 * @param str The string to add
 * @return The number of bytes added to the brigade
 * @deffunc apr_status_t apr_brigade_write(ap_bucket_brigade *b, apr_brigade_flush flush, void *ctx, const char *str, apr_size_t nbyte)
 */
APU_DECLARE(apr_status_t) apr_brigade_write(apr_bucket_brigade *b,
                                            apr_brigade_flush flush, void *ctx,
                                            const char *str, apr_size_t nbyte);

/**
 * This function writes an string into a bucket brigade.
 * @param b The bucket brigade to add to
 * @param str The string to add
 * @return The number of bytes added to the brigade
 * @deffunc apr_status_t apr_brigade_puts(ap_bucket_brigade *b, apr_brigade_flush flush, void *ctx, const char *str)
 */
APU_DECLARE(apr_status_t) apr_brigade_puts(apr_bucket_brigade *b,
                                           apr_brigade_flush flush, void *ctx,
                                           const char *str);

/**
 * This function writes a character into a bucket brigade.
 * @param b The bucket brigade to add to
 * @param c The character to add
 * @return The number of bytes added to the brigade
 * @deffunc apr_status_t apr_brigade_putc(apr_bucket_brigade *b, apr_brigade_flush flush, void *ctx, const char c)
 */
APU_DECLARE(apr_status_t) apr_brigade_putc(apr_bucket_brigade *b,
                                           apr_brigade_flush flush, void *ctx,
                                           const char c);

/**
 * This function writes an unspecified number of strings into a bucket brigade.
 * @param b The bucket brigade to add to
 * @param ... The strings to add
 * @return The number of bytes added to the brigade
 * @deffunc apr_status_t apr_brigade_putstrs(apr_bucket_brigade *b, apr_brigade_flush flush, void *ctx, ...)
 */
APU_DECLARE_NONSTD(apr_status_t) apr_brigade_putstrs(apr_bucket_brigade *b,
                                                     apr_brigade_flush flush,
                                                     void *ctx, ...);

/**
 * Evaluate a printf and put the resulting string at the end 
 * of the bucket brigade.
 * @param b The brigade to write to
 * @param fmt The format of the string to write
 * @param ... The arguments to fill out the format
 * @return The number of bytes added to the brigade
 * @deffunc apr_status_t apr_brigade_printf(apr_bucket_brigade *b, apr_brigade_flush flush, void *ctx, const char *fmt, ...) 
 */
APU_DECLARE_NONSTD(apr_status_t) apr_brigade_printf(apr_bucket_brigade *b, 
                                                    apr_brigade_flush flush,
                                                    void *ctx,
                                                    const char *fmt, ...)
        __attribute__((format(printf,4,5)));

/**
 * Evaluate a printf and put the resulting string at the end 
 * of the bucket brigade.
 * @param b The brigade to write to
 * @param fmt The format of the string to write
 * @param va The arguments to fill out the format
 * @return The number of bytes added to the brigade
 * @deffunc apr_status_t apr_brigade_vprintf(apr_bucket_brigade *b, apr_brigade_flush flush, void *ctx, const char *fmt, va_list va) 
 */
APU_DECLARE(apr_status_t) apr_brigade_vprintf(apr_bucket_brigade *b, 
                                              apr_brigade_flush flush,
                                              void *ctx,
                                              const char *fmt, va_list va);


/*  *****  Bucket Functions  *****  */
/**
 * Free the resources used by a bucket. If multiple buckets refer to
 * the same resource it is freed when the last one goes away.
 * @see apr_bucket_delete()
 * @param e The bucket to destroy
 * @deffunc void apr_bucket_destroy(apr_bucket *e)
 */
#define apr_bucket_destroy(e) do {					\
        (e)->type->destroy((e)->data);					\
        apr_sms_free((e)->sms, (e));					\
    } while (0)

/**
 * Delete a bucket by removing it from its brigade (if any) and then
 * destroying it.
 * @tip This mainly acts as an aid in avoiding code verbosity.  It is
 * the preferred exact equivalent to:
 * <pre>
 *      APR_BUCKET_REMOVE(e);
 *      apr_bucket_destroy(e);
 * </pre>
 * @param e The bucket to delete
 * @deffunc void apr_bucket_delete(apr_bucket *e)
 */
#define apr_bucket_delete(e) do {					\
        APR_BUCKET_REMOVE(e);						\
        apr_bucket_destroy(e);						\
    } while (0)

/**
 * read the data from the bucket
 * @param e The bucket to read from
 * @param str The location to store the data in
 * @param len The amount of data read
 * @param block Whether the read function blocks
 * @deffunc apr_status_t apr_bucket_read(apr_bucket *e, const char **str, apr_size_t *len, apr_read_type_e block)
 */
#define apr_bucket_read(e,str,len,block) (e)->type->read(e, str, len, block)

/**
 * Setaside data so that stack data is not destroyed on returning from
 * the function
 * @param e The bucket to setaside
 * @deffunc apr_status_t apr_bucket_setaside(apr_bucket *e)
 */
#define apr_bucket_setaside(e,p) (e)->type->setaside(e,p)

/**
 * Split one bucket in two.
 * @param e The bucket to split
 * @param point The offset to split the bucket at
 * @deffunc apr_status_t apr_bucket_split(apr_bucket *e, apr_size_t point)
 */
#define apr_bucket_split(e,point) (e)->type->split(e, point)

/**
 * Copy a bucket.
 * @param e The bucket to copy
 * @param c Returns a pointer to the new bucket
 * @deffunc apr_status_t apr_bucket_copy(apr_bucket *e, apr_bucket **c)
 */
#define apr_bucket_copy(e,c) (e)->type->copy(e, c)

/* Bucket type handling */

/**
 * This function simply returns APR_SUCCESS to denote that the bucket does
 * not require anything to happen for its setaside() function. This is
 * appropriate for buckets that have "immortal" data -- the data will live
 * at least as long as the bucket.
 * @param data The bucket to setaside
 * @param pool The pool defining the desired lifetime of the bucket data
 * @return APR_SUCCESS
 * @deffunc apr_status_t apr_bucket_setaside_notimpl(apr_bucket *data, apr_pool_t *pool)
 */ 
APU_DECLARE_NONSTD(apr_status_t) apr_bucket_setaside_noop(apr_bucket *data,
                                                          apr_pool_t *pool);

/**
 * A place holder function that signifies that the setaside function was not
 * implemented for this bucket
 * @param data The bucket to setaside
 * @param pool The pool defining the desired lifetime of the bucket data
 * @return APR_ENOTIMPL
 * @deffunc apr_status_t apr_bucket_setaside_notimpl(apr_bucket *data, apr_pool_t *pool)
 */ 
APU_DECLARE_NONSTD(apr_status_t) apr_bucket_setaside_notimpl(apr_bucket *data,
                                                             apr_pool_t *pool);

/**
 * A place holder function that signifies that the split function was not
 * implemented for this bucket
 * @param data The bucket to split
 * @param point The location to split the bucket
 * @return APR_ENOTIMPL
 * @deffunc apr_status_t apr_bucket_split_notimpl(apr_bucket *data, apr_size_t point)
 */ 
APU_DECLARE_NONSTD(apr_status_t) apr_bucket_split_notimpl(apr_bucket *data,
                                                          apr_size_t point);

/**
 * A place holder function that signifies that the copy function was not
 * implemented for this bucket
 * @param e The bucket to copy
 * @param c Returns a pointer to the new bucket
 * @return APR_ENOTIMPL
 * @deffunc apr_status_t apr_bucket_copy_notimpl(apr_bucket *e, apr_bucket **c)
 */
APU_DECLARE_NONSTD(apr_status_t) apr_bucket_copy_notimpl(apr_bucket *e,
                                                         apr_bucket **c);

/**
 * A place holder function that signifies that this bucket does not need
 * to do anything special to be destroyed.  That's only the case for buckets
 * that either have no data (metadata buckets) or buckets whose data pointer
 * points to something that's not a bucket-type-specific structure, as with
 * simple buckets where data points to a string and pipe buckets where data
 * points directly to the apr_file_t.
 * @param data The bucket data to destroy
 * @deffunc void apr_bucket_destroy_noop(void *data)
 */ 
APU_DECLARE_NONSTD(void) apr_bucket_destroy_noop(void *data);

/**
 * There is no apr_bucket_destroy_notimpl, because destruction is required
 * to be implemented (it could be a noop, but only if that makes sense for
 * the bucket type)
 */

/* There is no apr_bucket_read_notimpl, because it is a required function
 */


/* All of the bucket types implemented by the core */
/**
 * The flush bucket type.  This signifies that all data should be flushed to
 * the next filter.  The flush bucket should be sent with the other buckets.
 */
APU_DECLARE_DATA extern const apr_bucket_type_t apr_bucket_type_flush;
/**
 * The EOS bucket type.  This signifies that there will be no more data, ever.
 * All filters MUST send all data to the next filter when they receive a
 * bucket of this type
 */
APU_DECLARE_DATA extern const apr_bucket_type_t apr_bucket_type_eos;
/**
 * The FILE bucket type.  This bucket represents a file on disk
 */
APU_DECLARE_DATA extern const apr_bucket_type_t apr_bucket_type_file;
/**
 * The HEAP bucket type.  This bucket represents a data allocated from the
 * heap.
 */
APU_DECLARE_DATA extern const apr_bucket_type_t apr_bucket_type_heap;
#if APR_HAS_MMAP
/**
 * The MMAP bucket type.  This bucket represents an MMAP'ed file
 */
APU_DECLARE_DATA extern const apr_bucket_type_t apr_bucket_type_mmap;
#endif
/**
 * The POOL bucket type.  This bucket represents a data that was allocated
 * from a pool.  IF this bucket is still available when the pool is cleared,
 * the data is copied on to the heap.
 */
APU_DECLARE_DATA extern const apr_bucket_type_t apr_bucket_type_pool;
/**
 * The PIPE bucket type.  This bucket represents a pipe to another program.
 */
APU_DECLARE_DATA extern const apr_bucket_type_t apr_bucket_type_pipe;
/**
 * The IMMORTAL bucket type.  This bucket represents a segment of data that
 * the creator is willing to take responsability for.  The core will do
 * nothing with the data in an immortal bucket
 */
APU_DECLARE_DATA extern const apr_bucket_type_t apr_bucket_type_immortal;
/**
 * The TRANSIENT bucket type.  This bucket represents a data allocated off
 * the stack.  When the setaside function is called, this data is copied on
 * to the heap
 */
APU_DECLARE_DATA extern const apr_bucket_type_t apr_bucket_type_transient;
/**
 * The SOCKET bucket type.  This bucket represents a socket to another machine
 */
APU_DECLARE_DATA extern const apr_bucket_type_t apr_bucket_type_socket;


/*  *****  Simple buckets  *****  */

/**
 * Split a simple bucket into two at the given point.  Most non-reference
 * counting buckets that allow multiple references to the same block of
 * data (eg transient and immortal) will use this as their split function
 * without any additional type-specific handling.
 * @param b The bucket to be split
 * @param point The offset of the first byte in the new bucket
 * @return APR_EINVAL if the point is not within the bucket;
 *         APR_ENOMEM if allocation failed;
 *         or APR_SUCCESS
 * @deffunc apr_status_t apr_bucket_simple_split(apr_bucket *b, apr_size_t point)
 */
APU_DECLARE_NONSTD(apr_status_t) apr_bucket_simple_split(apr_bucket *b,
                                                         apr_size_t point);

/**
 * Copy a simple bucket.  Most non-reference-counting buckets that allow
 * multiple references to the same block of data (eg transient and immortal)
 * will use this as their copy function without any additional type-specific
 * handling.
 * @param a The bucket to copy
 * @param b Returns a pointer to the new bucket
 * @return APR_ENOMEM if allocation failed;
 *         or APR_SUCCESS
 * @deffunc apr_status_t apr_bucket_simple_copy(apr_bucket *a, apr_bucket **b)
 */
APU_DECLARE_NONSTD(apr_status_t) apr_bucket_simple_copy(apr_bucket *a,
                                                        apr_bucket **b);


/*  *****  Shared, reference-counted buckets  *****  */

/**
 * Initialize a bucket containing reference-counted data that may be
 * shared. The caller must allocate the bucket if necessary and
 * initialize its type-dependent fields, and allocate and initialize
 * its own private data structure. This function should only be called
 * by type-specific bucket creation functions.
 * @param b The bucket to initialize
 * @param data A pointer to the private data structure
 *             with the reference count at the start
 * @param start The start of the data in the bucket
 *              relative to the private base pointer
 * @param length The length of the data in the bucket
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_shared_make(apr_bucket_refcount *r, void *data, apr_off_t start, apr_size_t length) 
 */
APU_DECLARE(apr_bucket *) apr_bucket_shared_make(apr_bucket *b, void *data,
				                 apr_off_t start, 
                                                 apr_size_t length);

/**
 * Decrement the refcount of the data in the bucket. This function
 * should only be called by type-specific bucket destruction functions.
 * @param data The private data pointer from the bucket to be destroyed
 * @return TRUE or FALSE; TRUE if the reference count is now
 *         zero, indicating that the shared resource itself can
 *         be destroyed by the caller.
 * @deffunc int apr_bucket_shared_destroy(void *data)
 */
APU_DECLARE(int) apr_bucket_shared_destroy(void *data);

/**
 * Split a bucket into two at the given point, and adjust the refcount
 * to the underlying data. Most reference-counting bucket types will
 * be able to use this function as their split function without any
 * additional type-specific handling.
 * @param b The bucket to be split
 * @param point The offset of the first byte in the new bucket
 * @return APR_EINVAL if the point is not within the bucket;
 *         APR_ENOMEM if allocation failed;
 *         or APR_SUCCESS
 * @deffunc apr_status_t apr_bucket_shared_split(apr_bucket *b, apr_size_t point)
 */
APU_DECLARE_NONSTD(apr_status_t) apr_bucket_shared_split(apr_bucket *b,
                                                         apr_size_t point);

/**
 * Copy a refcounted bucket, incrementing the reference count. Most
 * reference-counting bucket types will be able to use this function
 * as their copy function without any additional type-specific handling.
 * @param a The bucket to copy
 * @param b Returns a pointer to the new bucket
 * @return APR_ENOMEM if allocation failed;
           or APR_SUCCESS
 * @deffunc apr_status_t apr_bucket_shared_copy(apr_bucket *a, apr_bucket **b)
 */
APU_DECLARE_NONSTD(apr_status_t) apr_bucket_shared_copy(apr_bucket *a,
                                                        apr_bucket **b);


/*  *****  Functions to Create Buckets of varying types  *****  */
/*
 * Each bucket type foo has two initialization functions:
 * apr_bucket_foo_make which sets up some already-allocated memory as a
 * bucket of type foo; and apr_bucket_foo_create which allocates memory
 * for the bucket, calls apr_bucket_make_foo, and initializes the
 * bucket's list pointers. The apr_bucket_foo_make functions are used
 * inside the bucket code to change the type of buckets in place;
 * other code should call apr_bucket_foo_create. All the initialization
 * functions change nothing if they fail.
 */

/**
 * Create an End of Stream bucket.  This indicates that there is no more data
 * coming from down the filter stack.  All filters should flush at this point.
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_eos_create(void)
 */
APU_DECLARE(apr_bucket *) apr_bucket_eos_create(void);

/**
 * Make the bucket passed in an EOS bucket.  This indicates that there is no 
 * more data coming from down the filter stack.  All filters should flush at 
 * this point.
 * @param b The bucket to make into an EOS bucket
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_eos_make(apr_bucket *b)
 */
APU_DECLARE(apr_bucket *) apr_bucket_eos_make(apr_bucket *b);

/**
 * Create a flush  bucket.  This indicates that filters should flush their
 * data.  There is no guarantee that they will flush it, but this is the
 * best we can do.
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_flush_create(void)
 */
APU_DECLARE(apr_bucket *) apr_bucket_flush_create(void);

/**
 * Make the bucket passed in a FLUSH  bucket.  This indicates that filters 
 * should flush their data.  There is no guarantee that they will flush it, 
 * but this is the best we can do.
 * @param b The bucket to make into a FLUSH bucket
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_flush_make(apr_bucket *b)
 */
APU_DECLARE(apr_bucket *) apr_bucket_flush_make(apr_bucket *b);

/**
 * Create a bucket referring to long-lived data.
 * @param buf The data to insert into the bucket
 * @param nbyte The size of the data to insert.
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_immortal_create(const char *buf, apr_size_t nbyte)
 */
APU_DECLARE(apr_bucket *) apr_bucket_immortal_create(const char *buf, 
                                                     apr_size_t nbyte);

/**
 * Make the bucket passed in a bucket refer to long-lived data
 * @param b The bucket to make into a IMMORTAL bucket
 * @param buf The data to insert into the bucket
 * @param nbyte The size of the data to insert.
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_immortal_make(apr_bucket *b, const char *buf, apr_size_t nbyte)
 */
APU_DECLARE(apr_bucket *) apr_bucket_immortal_make(apr_bucket *b, 
                                                   const char *buf, 
                                                   apr_size_t nbyte);

/**
 * Create a bucket referring to data on the stack.
 * @param buf The data to insert into the bucket
 * @param nbyte The size of the data to insert.
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_transient_create(const char *buf, apr_size_t nbyte)
 */
APU_DECLARE(apr_bucket *) apr_bucket_transient_create(const char *buf, 
                                                      apr_size_t nbyte);

/**
 * Make the bucket passed in a bucket refer to stack data
 * @param b The bucket to make into a TRANSIENT bucket
 * @param buf The data to insert into the bucket
 * @param nbyte The size of the data to insert.
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_transient_make(apr_bucket *b, const char *buf, apr_size_t nbyte)
 */
APU_DECLARE(apr_bucket *) apr_bucket_transient_make(apr_bucket *b, 
                                                    const char *buf,
                                                    apr_size_t nbyte);

/**
 * Create a bucket referring to memory on the heap. If the caller asks
 * for the data to be copied, this function always allocates 4K of
 * memory so that more data can be added to the bucket without
 * requiring another allocation. Therefore not all the data may be put
 * into the bucket. If copying is not requested then the bucket takes
 * over responsibility for free()ing the memory.
 * @param buf The buffer to insert into the bucket
 * @param nbyte The size of the buffer to insert.
 * @param copy Whether to copy the data into newly-allocated memory or not
 * @param w The number of bytes actually copied into the bucket.
 *          If copy is zero then this return value can be ignored by passing a NULL pointer.
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_heap_create(const char *buf, apr_size_t nbyte, int copy, apr_size_t *w)
 */
APU_DECLARE(apr_bucket *) apr_bucket_heap_create(const char *buf, 
                                                 apr_size_t nbyte, int copy, 
                                                 apr_size_t *w);
/**
 * Make the bucket passed in a bucket refer to heap data
 * @param b The bucket to make into a HEAP bucket
 * @param buf The buffer to insert into the bucket
 * @param nbyte The size of the buffer to insert.
 * @param copy Whether to copy the data into newly-allocated memory or not
 * @param w The number of bytes actually copied into the bucket.
 *          If copy is zero then this return value can be ignored by passing a NULL pointer.
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_heap_make(apr_bucket *b, const char *buf, apr_size_t nbyte, int copy, apr_size_t *w)
 */
APU_DECLARE(apr_bucket *) apr_bucket_heap_make(apr_bucket *b, const char *buf,
                                               apr_size_t nbyte, int copy, 
                                               apr_size_t *w);

/**
 * Create a bucket referring to memory allocated from a pool.
 *
 * @param buf The buffer to insert into the bucket
 * @param length The number of bytes referred to by this bucket
 * @param pool The pool the memory was allocated from
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_pool_create(const char *buf, apr_size_t *length, apr_pool_t *pool)
 */
APU_DECLARE(apr_bucket *) apr_bucket_pool_create(const char *buf, 
                                                 apr_size_t length,
                                                 apr_pool_t *pool);

/**
 * Make the bucket passed in a bucket refer to pool data
 * @param b The bucket to make into a pool bucket
 * @param buf The buffer to insert into the bucket
 * @param length The number of bytes referred to by this bucket
 * @param pool The pool the memory was allocated from
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_pool_make(apr_bucket *b, const char *buf, apr_size_t *length, apr_pool_t *pool)
 */
APU_DECLARE(apr_bucket *) apr_bucket_pool_make(apr_bucket *b, const char *buf,
                                               apr_size_t length, 
                                               apr_pool_t *pool);

#if APR_HAS_MMAP
/**
 * Create a bucket referring to mmap()ed memory.
 * @param mmap The mmap to insert into the bucket
 * @param start The offset of the first byte in the mmap
 *              that this bucket refers to
 * @param length The number of bytes referred to by this bucket
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_mmap_create(const apr_mmap_t *mm, apr_size_t start, apr_size_t length)
 */
APU_DECLARE(apr_bucket *) apr_bucket_mmap_create(apr_mmap_t *mm, 
                                                 apr_off_t start,
                                                 apr_size_t length);

/**
 * Make the bucket passed in a bucket refer to an MMAP'ed file
 * @param b The bucket to make into a MMAP bucket
 * @param mmap The mmap to insert into the bucket
 * @param start The offset of the first byte in the mmap
 *              that this bucket refers to
 * @param length The number of bytes referred to by this bucket
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_mmap_make(apr_bucket *b, const apr_mmap_t *mm, apr_size_t start, apr_size_t length)
 */
APU_DECLARE(apr_bucket *) apr_bucket_mmap_make(apr_bucket *b, apr_mmap_t *mm,
                                               apr_off_t start, 
                                               apr_size_t length);
#endif

/**
 * Create a bucket referring to a socket.
 * @param thissocket The socket to put in the bucket
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_socket_create(apr_socket_t *thissocket)
 */
APU_DECLARE(apr_bucket *) apr_bucket_socket_create(apr_socket_t *thissock);
/**
 * Make the bucket passed in a bucket refer to a socket
 * @param b The bucket to make into a SOCKET bucket
 * @param thissocket The socket to put in the bucket
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_socket_make(apr_bucket *b, apr_socket_t *thissocket)
 */
APU_DECLARE(apr_bucket *) apr_bucket_socket_make(apr_bucket *b, 
                                                 apr_socket_t *thissock);

/**
 * Create a bucket referring to a pipe.
 * @param thispipe The pipe to put in the bucket
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_pipe_create(apr_file_t *thispipe)
 */
APU_DECLARE(apr_bucket *) apr_bucket_pipe_create(apr_file_t *thispipe);

/**
 * Make the bucket passed in a bucket refer to a pipe
 * @param b The bucket to make into a PIPE bucket
 * @param thispipe The pipe to put in the bucket
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_pipe_make(apr_bucket *b, apr_file_t *thispipe)
 */
APU_DECLARE(apr_bucket *) apr_bucket_pipe_make(apr_bucket *b, 
                                               apr_file_t *thispipe);

/**
 * Create a bucket referring to a file.
 * @param fd The file to put in the bucket
 * @param offset The offset where the data of interest begins in the file
 * @param len The amount of data in the file we are interested in
 * @param p The pool into which any needed structures should be created
 *          while reading from this file bucket
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_file_create(apr_file_t *fd, apr_off_t offset, apr_size_t len, apr_pool_t *p)
 */
APU_DECLARE(apr_bucket *) apr_bucket_file_create(apr_file_t *fd,
                                                 apr_off_t offset,
                                                 apr_size_t len, 
                                                 apr_pool_t *p);

/**
 * Make the bucket passed in a bucket refer to a file
 * @param b The bucket to make into a FILE bucket
 * @param fd The file to put in the bucket
 * @param offset The offset where the data of interest begins in the file
 * @param len The amount of data in the file we are interested in
 * @param p The pool into which any needed structures should be created
 *          while reading from this file bucket
 * @return The new bucket, or NULL if allocation failed
 * @deffunc apr_bucket *apr_bucket_file_make(apr_bucket *b, apr_file_t *fd, apr_off_t offset, apr_size_t len, apr_pool_t *p)
 */
APU_DECLARE(apr_bucket *) apr_bucket_file_make(apr_bucket *b, apr_file_t *fd,
                                               apr_off_t offset,
                                               apr_size_t len, apr_pool_t *p);

#ifdef __cplusplus
}
#endif

#endif /* !APR_BUCKETS_H */
