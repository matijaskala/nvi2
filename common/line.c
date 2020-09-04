/*-
 * Copyright (c) 1992, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1992, 1993, 1994, 1995, 1996
 *	Keith Bostic.  All rights reserved.
 *
 * See the LICENSE file for redistribution information.
 */

#include "config.h"

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/time.h>

#include <bitstring.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "../vi/vi.h"

static int scr_update(SCR *, recno_t, lnop_t, int);

/*
 * db_eget --
 *	Front-end to db_get, special case handling for empty files.
 *
 * PUBLIC: int db_eget(SCR *, recno_t, CHAR_T **, size_t *, int *);
 */
int
db_eget(SCR *sp,
	recno_t lno,				/* Line number. */
	CHAR_T **pp,				/* Pointer store. */
	size_t *lenp,				/* Length store. */
	int *isemptyp)
{
	recno_t l1;

	if (isemptyp != NULL)
		*isemptyp = 0;

	/* If the line exists, simply return it. */
	if (!db_get(sp, lno, 0, pp, lenp))
		return (0);

	/*
	 * If the user asked for line 0 or line 1, i.e. the only possible
	 * line in an empty file, find the last line of the file; db_last
	 * fails loudly.
	 */
	if ((lno == 0 || lno == 1) && db_last(sp, &l1))
		return (1);

	/* If the file isn't empty, fail loudly. */
	if ((lno != 0 && lno != 1) || l1 != 0) {
		db_err(sp, lno);
		return (1);
	}

	if (isemptyp != NULL)
		*isemptyp = 1;

	return (1);
}

/*
 * db_get --
 *	Look in the text buffers for a line, followed by the cache, followed
 *	by the database.
 *
 * PUBLIC: int db_get(SCR *, recno_t, u_int32_t, CHAR_T **, size_t *);
 */
int
db_get(SCR *sp,
	recno_t lno,				/* Line number. */
	u_int32_t flags,
	CHAR_T **pp,				/* Pointer store. */
	size_t *lenp)				/* Length store. */
{
	DBT data, key;
	EXF *ep;
	TEXT *tp;
	recno_t l1, l2;
	CHAR_T *wp;
	size_t wlen;

	/*
	 * The underlying recno stuff handles zero by returning NULL, but
	 * have to have an OOB condition for the look-aside into the input
	 * buffer anyway.
	 */
	if (lno == 0)
		goto err1;

	/* Check for no underlying file. */
	if ((ep = sp->ep) == NULL) {
		ex_emsg(sp, NULL, EXM_NOFILEYET);
		goto err3;
	}

	if (LF_ISSET(DBG_NOCACHE))
		goto nocache;

	/*
	 * Look-aside into the TEXT buffers and see if the line we want
	 * is there.
	 */
	if (F_ISSET(sp, SC_TINPUT)) {
		l1 = ((TEXT *)TAILQ_FIRST(sp->tiq))->lno;
		l2 = ((TEXT *)TAILQ_LAST(sp->tiq, _texth))->lno;
		if (l1 <= lno && l2 >= lno) {
#if defined(DEBUG) && 0
	TRACE(sp, "retrieve TEXT buffer line %lu\n", (u_long)lno);
#endif
			for (tp = TAILQ_FIRST(sp->tiq);
			    tp->lno != lno; tp = TAILQ_NEXT(tp, q));
			if (lenp != NULL)
				*lenp = tp->len;
			if (pp != NULL)
				*pp = tp->lb;
			return (0);
		}
		/*
		 * Adjust the line number for the number of lines used
		 * by the text input buffers.
		 */
		if (lno > l2)
			lno -= l2 - l1;
	}

	/* Look-aside into the cache, and see if the line we want is there. */
	if (lno == ep->c_lno) {
#if defined(DEBUG) && 0
	TRACE(sp, "retrieve cached line %lu\n", (u_long)lno);
#endif
		if (lenp != NULL)
			*lenp = ep->c_len;
		if (pp != NULL)
			*pp = ep->c_lp;
		return (0);
	}
	ep->c_lno = OOBLNO;

nocache:
	/* Get the line from the underlying database. */
	memset(&key, 0, sizeof(key));
	key.data = &lno;
	key.size = sizeof(lno);
	memset(&data, 0, sizeof(data));
#ifdef DB_VERSION_MAJOR
	switch (ep->db->get(ep->db, NULL, &key, &data, 0)) {
	case 0:
		break;
	default:
		goto err2;
	case DB_NOTFOUND:
#else
	switch (ep->db->get(ep->db, &key, &data, 0)) {
	case -1:
		goto err2;
	case 1:
#endif
err1:		if (LF_ISSET(DBG_FATAL))
err2:			db_err(sp, lno);
alloc_err:
err3:		if (lenp != NULL)
			*lenp = 0;
		if (pp != NULL)
			*pp = NULL;
		return (1);
	}

	if (FILE2INT(sp, data.data, data.size, wp, wlen)) {
		if (!F_ISSET(sp, SC_CONV_ERROR)) {
			F_SET(sp, SC_CONV_ERROR);
			msgq(sp, M_ERR, "324|Conversion error on line %d", lno);
		}
		goto err3;
	}

	/* Reset the cache. */
	if (wp != data.data) {
		BINC_GOTOW(sp, ep->c_lp, ep->c_blen, wlen);
		MEMCPY(ep->c_lp, wp, wlen);
	} else
		ep->c_lp = data.data;
	ep->c_lno = lno;
	ep->c_len = wlen;

#if defined(DEBUG) && 0
	TRACE(sp, "retrieve DB line %lu\n", (u_long)lno);
#endif
	if (lenp != NULL)
		*lenp = wlen;
	if (pp != NULL)
		*pp = ep->c_lp;
	return (0);
}

/*
 * db_delete --
 *	Delete a line from the file.
 *
 * PUBLIC: int db_delete(SCR *, recno_t);
 */
int
db_delete(SCR *sp, recno_t lno)
{
	DBT key;
	EXF *ep;

#if defined(DEBUG) && 0
	TRACE(sp, "delete line %lu\n", (u_long)lno);
#endif
	/* Check for no underlying file. */
	if ((ep = sp->ep) == NULL) {
		ex_emsg(sp, NULL, EXM_NOFILEYET);
		return (1);
	}
		
	/* Update marks, @ and global commands. */
	if (mark_insdel(sp, LINE_DELETE, lno))
		return (1);
	if (ex_g_insdel(sp, LINE_DELETE, lno))
		return (1);

	/* Log change. */
	log_line(sp, lno, LOG_LINE_DELETE);

	/* Update file. */
	memset(&key, 0, sizeof(key));
	key.data = &lno;
	key.size = sizeof(lno);
#ifdef DB_VERSION_MAJOR
	if (ep->db->del(ep->db, NULL, &key, 0) != 0) {
#else
	if (ep->db->del(ep->db, &key, 0) == 1) {
#endif
		msgq(sp, M_SYSERR,
		    "003|unable to delete line %lu", (u_long)lno);
		return (1);
	}

	/* Flush the cache, update line count, before screen update. */
	if (lno <= ep->c_lno)
		ep->c_lno = OOBLNO;
	if (ep->c_nlines != OOBLNO)
		--ep->c_nlines;

	/* File now modified. */
	if (F_ISSET(ep, F_FIRSTMODIFY))
		(void)rcv_init(sp);
	F_SET(ep, F_MODIFIED);

	/* Update screen. */
	return (scr_update(sp, lno, LINE_DELETE, 1));
}

/*
 * db_append --
 *	Append a line into the file.
 *
 * PUBLIC: int db_append(SCR *, int, recno_t, CHAR_T *, size_t);
 */
int
db_append(SCR *sp, int update, recno_t lno, CHAR_T *p, size_t len)
{
	DBT data, key;
#ifdef DB_VERSION_MAJOR
	DBC *dbcp_put;
#endif
	EXF *ep;
	char *fp;
	size_t flen;
	int rval;

#if defined(DEBUG) && 0
	TRACE(sp, "append to %lu: len %u {%.*s}\n", lno, len, MIN(len, 20), p);
#endif
	/* Check for no underlying file. */
	if ((ep = sp->ep) == NULL) {
		ex_emsg(sp, NULL, EXM_NOFILEYET);
		return (1);
	}
		
	INT2FILE(sp, p, len, fp, flen);

	/* Update file. */
	memset(&key, 0, sizeof(key));
	key.data = &lno;
	key.size = sizeof(lno);
	memset(&data, 0, sizeof(data));
	data.data = fp;
	data.size = flen;
#ifdef DB_VERSION_MAJOR
	if ((ep->db->cursor(ep->db, NULL, &dbcp_put, 0)) != 0)
	    return 1;
	if (lno != 0) {
		if (dbcp_put->c_get(dbcp_put, &key, &data, DB_SET) != 0) {
			(void)dbcp_put->c_close(dbcp_put);
			msgq(sp, M_SYSERR,
			    "004|unable to append to line %lu", (u_long)lno);
			return (1);
		}

		memset(&data, 0, sizeof(data));
		data.data = fp;
		data.size = flen;
		if (dbcp_put->c_put(dbcp_put, &key, &data, DB_AFTER) != 0) {
			(void)dbcp_put->c_close(dbcp_put);
			msgq(sp, M_SYSERR,
			    "004|unable to append to line %lu", (u_long)lno);
			return (1);
		}
	} else {
		int db_error;
		if ((db_error = dbcp_put->c_get(dbcp_put, &key, &data, DB_FIRST)) != 0) {
			if (db_error != DB_NOTFOUND) {
				(void)dbcp_put->c_close(dbcp_put);
				msgq(sp, M_SYSERR,
				    "004|unable to append to line %lu", (u_long)lno);
				return (1);
			}
			memset(&data, 0, sizeof(data));
			data.data = fp;
			data.size = flen;
			if (ep->db->put(ep->db, NULL, &key, &data, DB_APPEND) != 0) {
				(void)dbcp_put->c_close(dbcp_put);
				msgq(sp, M_SYSERR,
				    "004|unable to append to line %lu", (u_long)lno);
				return (1);
			}
		} else {
			memset(&key, 0, sizeof(key));
			key.data = &lno;
			key.size = sizeof(lno);
			memset(&data, 0, sizeof(data));
			data.data = fp;
			data.size = flen;
			if (dbcp_put->c_put(dbcp_put, &key, &data, DB_BEFORE) != 0) {
				(void)dbcp_put->c_close(dbcp_put);
				msgq(sp, M_SYSERR,
				    "004|unable to append to line %lu", (u_long)lno);
				return (1);
			}
		}
	}
	(void)dbcp_put->c_close(dbcp_put);
#else
	if (ep->db->put(ep->db, &key, &data, R_IAFTER) == -1) {
		msgq(sp, M_SYSERR,
		    "004|unable to append to line %lu", (u_long)lno);
		return (1);
	}
#endif

	/* Flush the cache, update line count, before screen update. */
	if (lno < ep->c_lno)
		ep->c_lno = OOBLNO;
	if (ep->c_nlines != OOBLNO)
		++ep->c_nlines;

	/* File now dirty. */
	if (F_ISSET(ep, F_FIRSTMODIFY))
		(void)rcv_init(sp);
	F_SET(ep, F_MODIFIED);

	/* Log change. */
	log_line(sp, lno + 1, LOG_LINE_APPEND);

	/* Update marks, @ and global commands. */
	rval = 0;
	if (mark_insdel(sp, LINE_INSERT, lno + 1))
		rval = 1;
	if (ex_g_insdel(sp, LINE_INSERT, lno + 1))
		rval = 1;

	/*
	 * Update screen.
	 *
	 * XXX
	 * Nasty hack.  If multiple lines are input by the user, they aren't
	 * committed until an <ESC> is entered.  The problem is the screen was
	 * updated/scrolled as each line was entered.  So, when this routine
	 * is called to copy the new lines from the cut buffer into the file,
	 * it has to know not to update the screen again.
	 */
	return (scr_update(sp, lno, LINE_APPEND, update) || rval);
}

/*
 * db_insert --
 *	Insert a line into the file.
 *
 * PUBLIC: int db_insert(SCR *, recno_t, CHAR_T *, size_t);
 */
int
db_insert(SCR *sp, recno_t lno, CHAR_T *p, size_t len)
{
	DBT data, key;
#ifdef DB_VERSION_MAJOR
	DBC *dbcp_put;
#endif
	EXF *ep;
	char *fp;
	size_t flen;
	int rval;

#if defined(DEBUG) && 0
	TRACE(sp, "insert before %lu: len %lu {%.*s}\n",
	    (u_long)lno, (u_long)len, MIN(len, 20), p);
#endif
	/* Check for no underlying file. */
	if ((ep = sp->ep) == NULL) {
		ex_emsg(sp, NULL, EXM_NOFILEYET);
		return (1);
	}
		
	INT2FILE(sp, p, len, fp, flen);
		
	/* Update file. */
#ifdef DB_VERSION_MAJOR
	recno_t lno1 = lno - 1;
	memset(&key, 0, sizeof(key));
	key.data = &lno1;
	key.size = sizeof(lno1);
	memset(&data, 0, sizeof(data));
	data.data = fp;
	data.size = flen;
	if ((ep->db->cursor(ep->db, NULL, &dbcp_put, 0)) != 0)
	    return 1;
	if (lno1 != 0) {
		if (dbcp_put->c_get(dbcp_put, &key, &data, DB_SET) != 0) {
			(void)dbcp_put->c_close(dbcp_put);
			msgq(sp, M_SYSERR,
			    "005|unable to insert at line %lu", (u_long)lno);
			return (1);
		}
		memset(&data, 0, sizeof(data));
		data.data = fp;
		data.size = flen;
		if (dbcp_put->c_put(dbcp_put, &key, &data, DB_AFTER) != 0) {
			(void)dbcp_put->c_close(dbcp_put);
			msgq(sp, M_SYSERR,
			    "005|unable to insert at line %lu", (u_long)lno);
			return (1);
		}
	} else {
		int db_error;
		if ((db_error = dbcp_put->c_get(dbcp_put, &key, &data, DB_FIRST)) != 0) {
			if (db_error != DB_NOTFOUND) {
				(void)dbcp_put->c_close(dbcp_put);
				msgq(sp, M_SYSERR,
				    "005|unable to insert at line %lu", (u_long)lno);
				return (1);
			}
			memset(&data, 0, sizeof(data));
			data.data = fp;
			data.size = flen;
			if (ep->db->put(ep->db, NULL, &key, &data, DB_APPEND) != 0) {
				(void)dbcp_put->c_close(dbcp_put);
				msgq(sp, M_SYSERR,
				    "005|unable to insert at line %lu", (u_long)lno);
				return (1);
			}
		} else {
			memset(&key, 0, sizeof(key));
			key.data = &lno1;
			key.size = sizeof(lno1);
			memset(&data, 0, sizeof(data));
			data.data = fp;
			data.size = flen;
			if (dbcp_put->c_put(dbcp_put, &key, &data, DB_BEFORE) != 0) {
				(void)dbcp_put->c_close(dbcp_put);
				msgq(sp, M_SYSERR,
				    "005|unable to insert at line %lu", (u_long)lno);
				return (1);
			}
		}
	}
	(void)dbcp_put->c_close(dbcp_put);
#else
	key.data = &lno;
	key.size = sizeof(lno);
	data.data = fp;
	data.size = flen;
	if (ep->db->put(ep->db, &key, &data, R_IBEFORE) == -1) {
		msgq(sp, M_SYSERR,
		    "005|unable to insert at line %lu", (u_long)lno);
		return (1);
	}
#endif

	/* Flush the cache, update line count, before screen update. */
	if (lno >= ep->c_lno)
		ep->c_lno = OOBLNO;
	if (ep->c_nlines != OOBLNO)
		++ep->c_nlines;

	/* File now dirty. */
	if (F_ISSET(ep, F_FIRSTMODIFY))
		(void)rcv_init(sp);
	F_SET(ep, F_MODIFIED);

	/* Log change. */
	log_line(sp, lno, LOG_LINE_INSERT);

	/* Update marks, @ and global commands. */
	rval = 0;
	if (mark_insdel(sp, LINE_INSERT, lno))
		rval = 1;
	if (ex_g_insdel(sp, LINE_INSERT, lno))
		rval = 1;

	/* Update screen. */
	return (scr_update(sp, lno, LINE_INSERT, 1) || rval);
}

/*
 * db_set --
 *	Store a line in the file.
 *
 * PUBLIC: int db_set(SCR *, recno_t, CHAR_T *, size_t);
 */
int
db_set(SCR *sp, recno_t lno, CHAR_T *p, size_t len)
{
	DBT data, key;
	EXF *ep;
	char *fp;
	size_t flen;

#if defined(DEBUG) && 0
	TRACE(sp, "replace line %lu: len %lu {%.*s}\n",
	    (u_long)lno, (u_long)len, MIN(len, 20), p);
#endif
	/* Check for no underlying file. */
	if ((ep = sp->ep) == NULL) {
		ex_emsg(sp, NULL, EXM_NOFILEYET);
		return (1);
	}
		
	/* Log before change. */
	log_line(sp, lno, LOG_LINE_RESET_B);

	INT2FILE(sp, p, len, fp, flen);

	/* Update file. */
	memset(&key, 0, sizeof(key));
	key.data = &lno;
	key.size = sizeof(lno);
	memset(&data, 0, sizeof(data));
	data.data = fp;
	data.size = flen;
#ifdef DB_VERSION_MAJOR
	if (ep->db->put(ep->db, NULL, &key, &data, 0) != 0) {
#else
	if (ep->db->put(ep->db, &key, &data, 0) == -1) {
#endif
		msgq(sp, M_SYSERR,
		    "006|unable to store line %lu", (u_long)lno);
		return (1);
	}

	/* Flush the cache, before logging or screen update. */
	if (lno == ep->c_lno)
		ep->c_lno = OOBLNO;

	/* File now dirty. */
	if (F_ISSET(ep, F_FIRSTMODIFY))
		(void)rcv_init(sp);
	F_SET(ep, F_MODIFIED);

	/* Log after change. */
	log_line(sp, lno, LOG_LINE_RESET_F);

	/* Update screen. */
	return (scr_update(sp, lno, LINE_RESET, 1));
}

/*
 * db_exist --
 *	Return if a line exists.
 *
 * PUBLIC: int db_exist(SCR *, recno_t);
 */
int
db_exist(SCR *sp, recno_t lno)
{
	EXF *ep;

	/* Check for no underlying file. */
	if ((ep = sp->ep) == NULL) {
		ex_emsg(sp, NULL, EXM_NOFILEYET);
		return (1);
	}

	if (lno == OOBLNO)
		return (0);
		
	/*
	 * Check the last-line number cache.  Adjust the cached line
	 * number for the lines used by the text input buffers.
	 */
	if (ep->c_nlines != OOBLNO)
		return (lno <= (F_ISSET(sp, SC_TINPUT) ?
		    ep->c_nlines + (((TEXT *)TAILQ_LAST(sp->tiq, _texth))->lno -
		    ((TEXT *)TAILQ_FIRST(sp->tiq))->lno) : ep->c_nlines));

	/* Go get the line. */
	return (!db_get(sp, lno, 0, NULL, NULL));
}

/*
 * db_last --
 *	Return the number of lines in the file.
 *
 * PUBLIC: int db_last(SCR *, recno_t *);
 */
int
db_last(SCR *sp, recno_t *lnop)
{
	DBT data, key;
#ifdef DB_VERSION_MAJOR
	DBC *dbcp;
#endif
	EXF *ep;
	recno_t lno;
	CHAR_T *wp;
	size_t wlen;

	/* Check for no underlying file. */
	if ((ep = sp->ep) == NULL) {
		ex_emsg(sp, NULL, EXM_NOFILEYET);
		return (1);
	}
		
	/*
	 * Check the last-line number cache.  Adjust the cached line
	 * number for the lines used by the text input buffers.
	 */
	if (ep->c_nlines != OOBLNO) {
		*lnop = ep->c_nlines;
		if (F_ISSET(sp, SC_TINPUT))
			*lnop += ((TEXT *)TAILQ_LAST(sp->tiq, _texth))->lno -
			    ((TEXT *)TAILQ_FIRST(sp->tiq))->lno;
		return (0);
	}

	memset(&key, 0, sizeof(key));
	key.data = &lno;
	key.size = sizeof(lno);
	memset(&data, 0, sizeof(data));

#ifdef DB_VERSION_MAJOR
	if (ep->db->cursor(ep->db, NULL, &dbcp, 0) != 0)
	    goto alloc_err;
	switch (dbcp->c_get(dbcp, &key, &data, DB_LAST)) {
	case 0:
		break;
	default:
		(void)dbcp->c_close(dbcp);
#else
	switch (ep->db->seq(ep->db, &key, &data, R_LAST)) {
	case -1:
#endif
alloc_err:
		msgq(sp, M_SYSERR, "007|unable to get last line");
		*lnop = 0;
		return (1);
#ifdef DB_VERSION_MAJOR
	case DB_NOTFOUND:
#else
	case 1:
#endif
		*lnop = 0;
		return (0);
	}

	memcpy(&lno, key.data, sizeof(lno));

	if (lno != ep->c_lno) {
		FILE2INT(sp, data.data, data.size, wp, wlen);

		/* Fill the cache. */
		if (wp != data.data) {
			BINC_GOTOW(sp, ep->c_lp, ep->c_blen, wlen);
			MEMCPY(ep->c_lp, wp, wlen);
		} else
			ep->c_lp = data.data;
		ep->c_lno = lno;
		ep->c_len = wlen;
	}
	ep->c_nlines = lno;

#ifdef DB_VERSION_MAJOR
	(void)dbcp->c_close(dbcp);
#endif

	/* Return the value. */
	*lnop = (F_ISSET(sp, SC_TINPUT) &&
	    ((TEXT *)TAILQ_LAST(sp->tiq, _texth))->lno > lno ?
	    ((TEXT *)TAILQ_LAST(sp->tiq, _texth))->lno : lno);
	return (0);
}

/*
 * db_rget --
 *	Retrieve a raw line from the database.
 *
 * PUBLIC: int db_rget(SCR *, recno_t, char **, size_t *);
 */
int
db_rget(SCR *sp,
	recno_t lno,				/* Line number. */
	char **pp,				/* Pointer store. */
	size_t *lenp)				/* Length store. */
{
	DBT data, key;
	EXF *ep = sp->ep;
	int rval;

	/* Get the line from the underlying database. */
	memset(&key, 0, sizeof(key));
	key.data = &lno;
	key.size = sizeof(lno);
	memset(&data, 0, sizeof(data));
#ifdef DB_VERSION_MAJOR
	if ((rval = ep->db->get(ep->db, NULL, &key, &data, 0)) == 0)
#else
	if ((rval = ep->db->get(ep->db, &key, &data, 0)) == 0)
#endif
	{
		*lenp = data.size;
		*pp = data.data;
	}

	return (rval);
}

/*
 * db_rset --
 *	Store a raw line into the database.
 *
 * PUBLIC: int db_rset(SCR *, recno_t, char *, size_t);
 */
int
db_rset(SCR *sp, recno_t lno, char *p, size_t len)
{
	DBT data, key;
	EXF *ep = sp->ep;

	/* Update file. */
	memset(&key, 0, sizeof(key));
	key.data = &lno;
	key.size = sizeof(lno);
	memset(&data, 0, sizeof(data));
	data.data = p;
	data.size = len;
#ifdef DB_VERSION_MAJOR
	return ep->db->put(ep->db, NULL, &key, &data, 0);
#else
	return ep->db->put(ep->db, &key, &data, 0);
#endif
}

/*
 * db_err --
 *	Report a line error.
 *
 * PUBLIC: void db_err(SCR *, recno_t);
 */
void
db_err(SCR *sp, recno_t lno)
{
	msgq(sp, M_ERR,
	    "008|Error: unable to retrieve line %lu", (u_long)lno);
}

/*
 * scr_update --
 *	Update all of the screens that are backed by the file that
 *	just changed.
 */
static int
scr_update(SCR *sp, recno_t lno, lnop_t op, int current)
{
	EXF *ep;
	SCR *tsp;

	if (F_ISSET(sp, SC_EX))
		return (0);

	ep = sp->ep;
	if (ep->refcnt != 1)
		TAILQ_FOREACH(tsp, sp->gp->dq, q)
			if (sp != tsp && tsp->ep == ep)
				if (vs_change(tsp, lno, op))
					return (1);
	return (current ? vs_change(sp, lno, op) : 0);
}

/* Round up v to the nearest power of 2 */
static size_t round_up(size_t v)
{
	ssize_t old_v = v;

	while (v) {
		old_v = v;
		v ^= v & -v;
	}
	return old_v << 1;
}

/*
 * PUBLIC: int db_init (SCR *, EXF *, char *, char *, size_t, int *);
 */
int
db_init(SCR *sp, EXF *ep, char *rcv_name, char *oname, size_t psize, int *open_err) {
#ifdef DB_VERSION_MAJOR
	char path[PATH_MAX];
	int fd;
	DB_ENV	*env;

	(void)snprintf(path, sizeof(path), "%s/vi.XXXXXX", O_STR(sp, O_RECDIR));
	if ((fd = mkstemp(path)) == -1) {
		msgq(sp, M_SYSERR, "%s", path);
		return 1;
	}
	(void)close(fd);
	(void)unlink(path);
	if (mkdir(path, S_IRWXU)) {
		msgq(sp, M_SYSERR, "%s", path);
		return 1;
	}
	if (db_env_create(&env, 0)) {
		msgq(sp, M_ERR, "env_create");
		return 1;
	}
	if (env->open(env, path, 
	    DB_PRIVATE | DB_CREATE | DB_INIT_MPOOL, 0) != 0) {
		msgq(sp, M_SYSERR, "env->open");
		return 1;
	}

	if ((ep->env_path = strdup(path)) == NULL) {
		msgq(sp, M_SYSERR, NULL);
		(void)rmdir(path);
		return 1;
	}
	ep->env = env;

	/* Open a db structure. */
	if (db_create(&ep->db, 0, 0) != 0) {
		msgq(sp, M_SYSERR, "db_create");
		return 1;
	}

	ep->db->set_re_delim(ep->db, '\n');		/* Always set. */
	ep->db->set_pagesize(ep->db, round_up(psize));
	ep->db->set_flags(ep->db, DB_RENUMBER | DB_SNAPSHOT);
	if (rcv_name == NULL)
		ep->db->set_re_source(ep->db, oname);

#define _DB_OPEN_MODE	S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH

	if (ep->db->open(ep->db, NULL, ep->rcv_path, NULL, DB_RECNO,
	    ((rcv_name == 0) ? DB_TRUNCATE : 0) | DB_NOMMAP | DB_CREATE,
	    _DB_OPEN_MODE) != 0) {
#else

	/* Set up recovery. */
	RECNOINFO oinfo = { 0 };
	oinfo.bval = '\n';			/* Always set. */
	oinfo.psize = psize;
	oinfo.flags = F_ISSET(sp->gp, G_SNAPSHOT) ? R_SNAPSHOT : 0;
	oinfo.bfname = ep->rcv_path;

	/* Open a db structure. */
	if ((ep->db = dbopen(rcv_name == NULL ? oname : NULL,
	    O_NONBLOCK | O_RDONLY,
	    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH,
	    DB_RECNO, &oinfo)) == NULL) {
#endif
		msgq_str(sp,
		    M_SYSERR, rcv_name == NULL ? oname : rcv_name, "%s");
		/*
		 * !!!
		 * Historically, vi permitted users to edit files that couldn't
		 * be read.  This isn't useful for single files from a command
		 * line, but it's quite useful for "vi *.c", since you can skip
		 * past files that you can't read.
		 */ 
		*open_err = 1;
		return 1;
	}

#ifdef DB_VERSION_MAJOR
	/* re_source is loaded into the database.
	 * Close it and reopen it in the environment. 
	 */
	if (ep->db->close(ep->db, 0)) {
		msgq(sp, M_SYSERR, "close");
		return 1;
	}
	if (db_create(&ep->db, ep->env, 0) != 0) {
		msgq(sp, M_SYSERR, "db_create 2");
		return 1;
	}
	if (ep->db->open(ep->db, NULL, ep->rcv_path, NULL, DB_RECNO,
	    DB_NOMMAP | DB_CREATE, _DB_OPEN_MODE) != 0) {
		msgq_str(sp,
		    M_SYSERR, ep->rcv_path, "%s");
		return 1;
	}
#endif

	return 0;
}
