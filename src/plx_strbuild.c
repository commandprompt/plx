/*
 * plx_strbuild.c - an expanded-object string builder for plx.
 *
 * Concatenating onto a text variable in a loop (s := s || 'x') is quadratic in
 * plpgsql, because text is an immutable varlena and each step copies the whole
 * string. This file provides a `plx_strbuild` type whose in-memory (expanded)
 * form is an over-allocated growable buffer, so appends are amortized O(1). It
 * mirrors how PostgreSQL made array append fast with expanded arrays.
 *
 * The flattened form is byte-for-byte a text varlena, so casts to and from text
 * are binary-coercible (declared WITHOUT FUNCTION in the SQL).
 *
 * `plx_sb_append(sb, text)` mutates the buffer in place when it is handed a
 * read-write expanded object, and its planner support function tells the
 * executor to pass the accumulator argument read-write. plpgsql commandeers the
 * returned read-write object on assignment (via the generic datumTransfer path),
 * so across a loop the same buffer is grown in place.
 */
#include "postgres.h"

#include "fmgr.h"
#include "nodes/primnodes.h"
#include "nodes/supportnodes.h"
#include "utils/builtins.h"
#include "utils/expandeddatum.h"
#include "utils/memutils.h"
#include "varatt.h"

#define SB_MAGIC 0x53425544		/* 'SBUD' */

typedef struct ExpandedSBHeader
{
	ExpandedObjectHeader hdr;
	int32		sb_magic;		/* SB_MAGIC, sanity check */
	char	   *buf;			/* data bytes, in hdr.eoh_context */
	int32		len;			/* used bytes */
	int32		capacity;		/* allocated bytes in buf */
} ExpandedSBHeader;

static Size sb_get_flat_size(ExpandedObjectHeader *eohptr);
static void sb_flatten_into(ExpandedObjectHeader *eohptr, void *result, Size allocated_size);

static const ExpandedObjectMethods SB_methods = {
	sb_get_flat_size,
	sb_flatten_into
};

/* flattened form is a plain 4-byte-header varlena (text-compatible) */
static Size
sb_get_flat_size(ExpandedObjectHeader *eohptr)
{
	ExpandedSBHeader *sb = (ExpandedSBHeader *) eohptr;

	Assert(sb->sb_magic == SB_MAGIC);
	return VARHDRSZ + sb->len;
}

static void
sb_flatten_into(ExpandedObjectHeader *eohptr, void *result, Size allocated_size)
{
	ExpandedSBHeader *sb = (ExpandedSBHeader *) eohptr;

	Assert(sb->sb_magic == SB_MAGIC);
	Assert(allocated_size == (Size) (VARHDRSZ + sb->len));
	SET_VARSIZE(result, allocated_size);
	if (sb->len > 0)
		memcpy(VARDATA(result), sb->buf, sb->len);
}

/* build a fresh expanded string builder from raw bytes, with spare capacity */
static ExpandedSBHeader *
make_expanded_sb(const char *data, int32 len, MemoryContext parent, int32 mincap)
{
	MemoryContext objcxt;
	ExpandedSBHeader *sb;
	int32		cap;

	objcxt = AllocSetContextCreate(parent, "plx string builder",
								   ALLOCSET_START_SMALL_SIZES);
	sb = (ExpandedSBHeader *) MemoryContextAlloc(objcxt, sizeof(ExpandedSBHeader));
	EOH_init_header(&sb->hdr, &SB_methods, objcxt);
	sb->sb_magic = SB_MAGIC;
	cap = Max(mincap, Max(len, 64));
	sb->buf = (char *) MemoryContextAlloc(objcxt, cap);
	sb->capacity = cap;
	sb->len = len;
	if (len > 0)
		memcpy(sb->buf, data, len);
	return sb;
}

/* grow the buffer so at least `need` more bytes fit */
static void
sb_ensure(ExpandedSBHeader *sb, int32 need)
{
	if (sb->len + need <= sb->capacity)
		return;
	{
		int32		newcap = sb->capacity * 2;

		while (newcap < sb->len + need)
			newcap *= 2;
		sb->buf = (char *) repalloc(sb->buf, newcap);
		sb->capacity = newcap;
	}
}

PG_FUNCTION_INFO_V1(plx_sb_in);
PG_FUNCTION_INFO_V1(plx_sb_out);
PG_FUNCTION_INFO_V1(plx_sb_append);
PG_FUNCTION_INFO_V1(plx_sb_append_support);

/* input: a cstring becomes the flat (text-compatible) form */
Datum
plx_sb_in(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);

	PG_RETURN_TEXT_P(cstring_to_text(s));
}

/* output: flatten (PG_GETARG_TEXT_PP detoasts an expanded value) and render */
Datum
plx_sb_out(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_PP(0);

	PG_RETURN_CSTRING(text_to_cstring(t));
}

/*
 * plx_sb_append(sb, suffix): append suffix's bytes to sb. If sb is a read-write
 * expanded object, mutate in place and return the same object; otherwise build
 * a new expanded object from the flat value. Not strict: a NULL builder starts
 * empty and a NULL suffix appends nothing.
 */
Datum
plx_sb_append(PG_FUNCTION_ARGS)
{
	ExpandedSBHeader *sb;
	text	   *suffix;
	int32		slen;

	/* the accumulator argument */
	if (!PG_ARGISNULL(0) &&
		VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(PG_GETARG_DATUM(0))))
	{
		sb = (ExpandedSBHeader *) DatumGetEOHP(PG_GETARG_DATUM(0));
		Assert(sb->sb_magic == SB_MAGIC);
	}
	else if (PG_ARGISNULL(0))
	{
		sb = make_expanded_sb(NULL, 0, CurrentMemoryContext, 0);
	}
	else
	{
		text	   *cur = PG_GETARG_TEXT_PP(0);
		int32		clen = VARSIZE_ANY_EXHDR(cur);

		sb = make_expanded_sb(VARDATA_ANY(cur), clen, CurrentMemoryContext, clen * 2);
	}

	if (PG_ARGISNULL(1))
		PG_RETURN_DATUM(EOHPGetRWDatum(&sb->hdr));

	suffix = PG_GETARG_TEXT_PP(1);
	slen = VARSIZE_ANY_EXHDR(suffix);
	if (slen > 0)
	{
		sb_ensure(sb, slen);
		memcpy(sb->buf + sb->len, VARDATA_ANY(suffix), slen);
		sb->len += slen;
	}
	PG_RETURN_DATUM(EOHPGetRWDatum(&sb->hdr));
}

/*
 * Planner support: tell the executor that plx_sb_append can modify its first
 * argument in place, provided that argument is exactly the variable being
 * assigned to. Without this the executor passes the argument read-only and the
 * in-place fast path never fires.
 *
 * SupportRequestModifyInPlace was introduced in PostgreSQL 18. On earlier
 * versions this support function does nothing (returns NULL), so plx_sb_append
 * still produces correct results but without the amortized-O(1) fast path: on
 * PostgreSQL 13 to 17, only the built-in array functions can take a read-write
 * expanded argument, and a third-party function cannot opt in.
 */
Datum
plx_sb_append_support(PG_FUNCTION_ARGS)
{
	Node	   *ret = NULL;

#if PG_VERSION_NUM >= 180000
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestModifyInPlace))
	{
		SupportRequestModifyInPlace *req = (SupportRequestModifyInPlace *) rawreq;
		Param	   *arg = (Param *) linitial(req->args);

		if (arg && IsA(arg, Param) &&
			arg->paramkind == PARAM_EXTERN &&
			arg->paramid == req->paramid)
			ret = (Node *) arg;
	}
#endif
	PG_RETURN_POINTER(ret);
}
