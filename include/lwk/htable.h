/* Copyright (c) 2008, Sandia National Laboratories */

#ifndef _LWK_HTABLE_H
#define _LWK_HTABLE_H

#include <lwk/idspace.h>

/** Opaque hash table type */
struct htable;

/** Opaque hash table chaining type */
struct hlist_node;


/**
 * \file Hash table API.
 *
 * Hash tables are implemented in a generic way to allow them to
 * contain arbitrary structures with chaining.  To create a structure
 * that is compatible with the hash table it must contain an id_t id
 * and a struct hlist_node for chaining, and pass the offsets of these
 * values to the htable_create() function as shown in this example:
 *
 * \code
 * #include <lwk/htable.h>
 * #include <lwk/list.h>
 *
 * struct foo {
 *	id_t			id;
 *	struct hlist_node	ht_link;
 *	// Other stuff ...
 * };
 *
 * struct htable * foo_table;
 *
 * void foo_init( void )
 * {
 *	foo_table = htable_create(
 *		7,
 *		offsetof( struct foo, id ),
 *		offsetof( struct foo, ht_link )
 *	);
 *
 *	if( !foo_table )
 *		panic( "Unable to create foo_table!" );
 * }
 * \endcode
 */

/** Create a hash table.
 *
 * \returns Pointer to table if successful, NULL if allocation failed.
 */
extern struct htable *
htable_create(
	size_t			tbl_order,	//!< 2^@tbl_order elements
	size_t			obj_key_offset,
	size_t			obj_link_offset
);


extern int
htable_destroy(
	struct htable *		ht
);


extern int
htable_add(
	struct htable *		tbl,
	void *			obj
);


extern int
htable_del(
	struct htable *		tbl,
	void *			obj
);


extern void *
htable_lookup(
	struct htable *		tbl,
	id_t			key
);


#endif
