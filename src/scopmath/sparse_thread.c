#include <../../nrnconf.h>

/******************************************************************************
 *
 * File: sparse.c
 *
 * Copyright (c) 1989, 1990
 *   Duke University
 *
 ******************************************************************************/

#ifndef LINT
static char RCSid[] = "sparse.c,v 1.7 1998/03/12 13:17:17 hines Exp";
#endif

#include <stdlib.h>
#include "errcodes.h"

/* Jan 2008 thread safe */
/* 4/23/93 converted to object so many models can use it */
/*-----------------------------------------------------------------------------
 *
 *  sparse()
 *
 *  Abstract: 
 *  This is an experimental numerical method for SCoP-3 which integrates kinetic
 *  rate equations.  It is intended to be used only by models generated by MODL,
 *  and its identity is meant to be concealed from the user.
 *
 *
 *  Calling sequence:
 *	sparse(n, s, d, t, dt, fun, prhs, linflag)
 *
 *  Arguments:
 * 	n		number of state variables
 * 	s		array of pointers to the state variables
 * 	d		array of pointers to the derivatives of states
 * 	t		pointer to the independent variable
 * 	dt		the time step
 * 	fun		pointer to the function corresponding to the
 *			kinetic block equations
 * 	prhs		pointer to right hand side vector (answer on return)
 *			does not have to be allocated by caller.
 * 	linflag		solve as linear equations
 *			when nonlinear, all states are forced >= 0
 * 
 *		
 *  Returns:	nothing
 *
 *  Functions called: IGNORE(), printf(), create_coef_list(), fabs()
 *
 *  Files accessed:  none
 *
*/

#if LINT
#define IGNORE(arg)	{if (arg);}
#else
#define IGNORE(arg)	arg
#endif

#if __TURBOC__ || VMS
#define Free(arg)	myfree((void *)arg)
#else
#define Free(arg)	myfree((char *)arg)
#endif
extern void* nrn_pool_create(long count, int itemsize);
extern void nrn_pool_delete(void* pool);
extern void nrn_pool_freeall(void* pool);
extern void* nrn_pool_alloc(void* pool);

#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <stdlib.h>

typedef	int (*FUN)(void*, double*, double*, void*, void*, void*);

typedef struct Elm {
	unsigned row;		/* Row location */
	unsigned col;		/* Column location */
	double value;		/* The value */
	struct Elm *r_up;	/* Link to element in same column */
	struct Elm *r_down;	/* 	in solution order */
	struct Elm *c_left;	/* Link to left element in same row */
	struct Elm *c_right;	/*	in solution order (see getelm) */
} Elm;
#define ELM0	(Elm *)0

typedef struct Item {
	Elm 		*elm;
	unsigned	norder; /* order of a row */
	struct Item	*next;
	struct Item	*prev;
} Item;
#define ITEM0	(Item *)0

typedef Item List;	/* list of mixed items */

typedef struct SparseObj { 	/* all the state information */
	Elm**	rowst; 		/* link to first element in row (solution order)*/
	Elm**	diag; 		/* link to pivot element in row (solution order)*/
	void* elmpool;		/* no interthread cache line sharing for elements */
	unsigned neqn; 		/* number of equations */
	unsigned* varord; 	/* row and column order for pivots */
	double* rhs; 		/* initially- right hand side	finally - answer */
	FUN oldfun;
	unsigned ngetcall; 	/* counter for number of calls to _getelm */
	int phase; 		/* 0-solution phase; 1-count phase; 2-build list phase */
	int numop;
	double** coef_list; 	/* pointer to value in _getelm order */
	/* don't really need the rest */
	int nroworder;  	/* just for freeing */
	Item** roworder; 	/* roworder[i] is pointer to order item for row i.
					Does not have to be in orderlist */
	List* orderlist; 	/* list of rows sorted by norder
					that haven't been used */
	int do_flag;
} SparseObj;

/* note: solution order refers to the following
	diag[varord[row]]->row = row = diag[varord[row]]->col
	rowst[varord[row]]->row = row
	varord[el->row] < varord[el->c_right->row]
	varord[el->col] < varord[el->r_down->col]
*/
	
static int matsol(SparseObj* so);
static void subrow(SparseObj* so,Elm* pivot, Elm* rowsub);
static void bksub(SparseObj* so);
static void prmat(SparseObj* so);
static void initeqn(SparseObj* so, unsigned maxeqn);
static void free_elm(SparseObj* so);
static Elm* getelm(SparseObj* so,unsigned row, unsigned col, Elm* new);
static void create_coef_list(SparseObj* so, int n, FUN fun, double* p, void* ppvar, void* thread, void* nt);
static void init_coef_list(SparseObj* so);
static void init_minorder(SparseObj* so);
static void increase_order(SparseObj* so, unsigned row);
static void reduce_order(SparseObj* so, unsigned row);
static void spar_minorder(SparseObj* so);
static void get_next_pivot(SparseObj* so, unsigned i);
static Item* newitem();
static List* newlist();
static void freelist(List* list);
static void linkitem(Item* item, Item* i);
static void insert(SparseObj* so, Item* item);
static void delete(Item* item);
static void *emalloc(unsigned n);
static void myfree(void*);
static void check_assert();
static void re_link(SparseObj* so, unsigned i);
static SparseObj* create_sparseobj();
void _nrn_destroy_sparseobj_thread(SparseObj* so);

/* sparse matrix dynamic allocation:
create_coef_list makes a list for fast setup, does minimum ordering and
ensures all elements needed are present */
/* this could easily be made recursive but it isn't right now */

#define s_(arg) *p[s[arg]]
#define d_(arg) *p[d[arg]]
int sparse_thread(void** v, int n, int* s, int* d, double** p, double* t, double dt, FUN fun, int linflag, void* ppvar, void* thread, void *nt) {
	int i, j, ierr;
	double err;
	SparseObj* so;
	
	so = (SparseObj*)(*v);
	if (!so) {
		so = create_sparseobj();
		*v = (void*)so;
	}
	if (so->oldfun != fun) {
		so->oldfun = fun;
		create_coef_list(so, n, fun, p, ppvar, thread, nt); /* calls fun twice */
	}
	for (i=0; i<n; i++) { /*save old state*/
		d_(i) = s_(i);
	}
	for (err=1, j=0; err > CONVERGE; j++) {
		init_coef_list(so);
		(*fun)(so, so->rhs, p, ppvar, thread, nt);
		if((ierr = matsol(so))) {
			return ierr;
		}
		for (err=0.,i=1; i<=n; i++) {/* why oh why did I write it from 1 */
			s_(i-1) += so->rhs[i];
#if 1 /* stability of nonlinear kinetic schemes sometimes requires this */
if (!linflag && s_(i-1) < 0.) { s_(i-1) = 0.; }
#endif
			err += fabs(so->rhs[i]);
		}
		if (j > MAXSTEPS) {
			return EXCEED_ITERS;
		}
		if (linflag) break;
	}
	init_coef_list(so);
	(*fun)(so, so->rhs, p, ppvar, thread, nt);
	for (i=0; i<n; i++) { /*restore Dstate at t+dt*/
		d_(i) = (s_(i) - d_(i))/dt;
	}
	return SUCCESS;
}

/* for solving ax=b */
int _cvode_sparse_thread(v, n, x, p, fun, ppvar, thread, nt)
	void** v;
	int n;
	FUN fun;
	double **p;
	int *x;
	void* ppvar; void* thread; void* nt;
#define x_(arg) *p[x[arg]]
{
	int i, j, ierr;
	SparseObj* so;
	
	so = (SparseObj*)(*v);
	if (!so) {
		so = create_sparseobj();
		*v = (void*)so;
	}
	if (so->oldfun != fun) {
		so->oldfun = fun;
		create_coef_list(so, n, fun, p, ppvar, thread, nt); /* calls fun twice */
	}
		init_coef_list(so);
		(*fun)(so, so->rhs, p, ppvar, thread, nt);
		if((ierr = matsol(so))) {
			return ierr;
		}
		for (i=1; i<=n; i++) {/* why oh why did I write it from 1 */
			x_(i-1) = so->rhs[i];
		}
	return SUCCESS;
}

static int matsol(SparseObj* so) {
	register Elm *pivot, *el;
	unsigned i;

	/* Upper triangularization */
	so->numop = 0;
	for (i=1 ; i <= so->neqn ; i++)
	{
		if (fabs((pivot = so->diag[i])->value) <= ROUNDOFF)
		{
			return SINGULAR;
		}
		/* Eliminate all elements in pivot column */
		for (el = pivot->r_down ; el ; el = el->r_down)
		{
			subrow(so, pivot, el);
		}
	}
	bksub(so);
	return(SUCCESS);
}


static void subrow(SparseObj* so, Elm* pivot, Elm* rowsub) {
	double r;
	register Elm *el;

	r = rowsub->value / pivot->value;
	so->rhs[rowsub->row] -= so->rhs[pivot->row] * r;
	so->numop++;
	for (el = pivot->c_right ; el ; el = el->c_right) {
		for (rowsub = rowsub->c_right; rowsub->col != el->col;
		  rowsub = rowsub->c_right) {
		  	;
		}
		rowsub->value -= el->value * r;
		so->numop++;
	}
}

static void bksub(SparseObj* so) {
	unsigned i;
	Elm *el;

	for (i = so->neqn ; i >= 1 ; i--)
	{
		for (el = so->diag[i]->c_right ; el ; el = el->c_right)
		{
			so->rhs[el->row] -= el->value * so->rhs[el->col];
			so->numop++;
		}
		so->rhs[so->diag[i]->row] /= so->diag[i]->value;
		so->numop++;
	}
}


static void prmat(SparseObj* so) {
	unsigned i, j;
	Elm *el;

	IGNORE(printf("\n        "));
	for (i=10 ; i <= so->neqn ; i += 10)
		IGNORE(printf("         %1d", (i%100)/10));
	IGNORE(printf("\n        "));
	for (i=1 ; i <= so->neqn; i++)
		IGNORE(printf("%1d", i%10));
	IGNORE(printf("\n\n"));
	for (i=1 ; i <= so->neqn ; i++)
	{
		IGNORE(printf("%3d %3d ", so->diag[i]->row, i));
		j = 0;
		for (el = so->rowst[i] ;el ; el = el->c_right)
		{
			for ( j++ ; j < so->varord[el->col] ; j++)
				IGNORE(printf(" "));
			IGNORE(printf("*"));
		}
		IGNORE(printf("\n"));
	}
	IGNORE(fflush(stdin));
}

static void initeqn(SparseObj* so, unsigned maxeqn)	/* reallocate space for matrix */
{
	register unsigned i;

	if (maxeqn == so->neqn) return;
	free_elm(so);
	if (so->rowst)
		Free(so->rowst);
	if (so->diag)
		Free(so->diag);
	if (so->varord)
		Free(so->varord);
	if (so->rhs)
		Free(so->rhs);
	so->rowst = so->diag = (Elm **)0;
	so->varord = (unsigned *)0;
	so->rowst = (Elm **)emalloc((maxeqn + 1)*sizeof(Elm *));
	so->diag = (Elm **)emalloc((maxeqn + 1)*sizeof(Elm *));
	so->varord = (unsigned *)emalloc((maxeqn + 1)*sizeof(unsigned));
	so->rhs = (double *)emalloc((maxeqn + 1)*sizeof(double));
	for (i=1 ; i<= maxeqn ; i++)
	{
		so->varord[i] = i;
		so->diag[i] = (Elm *)nrn_pool_alloc(so->elmpool);
		so->rowst[i] = so->diag[i];
		so->diag[i]->row = i;
		so->diag[i]->col = i;
		so->diag[i]->r_down = so->diag[i]->r_up = ELM0;
		so->diag[i]->c_right = so->diag[i]->c_left = ELM0;
		so->diag[i]->value = 0.;
		so->rhs[i] = 0.;
	}
	so->neqn = maxeqn;
}

static void free_elm(SparseObj* so) {
	unsigned i;
	Elm *el, *elnext;
	
	/* free all elements */
	nrn_pool_freeall(so->elmpool);
	for (i=1; i <= so->neqn; i++)
	{
		so->rowst[i] = ELM0;
		so->diag[i] = ELM0;
	}
}


/* see check_assert in minorder for info about how this matrix is supposed
to look.  In new is nonzero and an element would otherwise be created, new
is used instead. This is because linking an element is highly nontrivial
The biggest difference is that elements are no longer removed and this
saves much time allocating and freeing during the solve phase
*/

static Elm* getelm(SparseObj* so, unsigned row, unsigned col, Elm* new)
   /* return pointer to row col element maintaining order in rows */
{
	register Elm *el, *elnext;
	unsigned vrow, vcol;
	
	vrow = so->varord[row];
	vcol = so->varord[col];
	
	if (vrow == vcol) {
		return so->diag[vrow]; /* a common case */
	}
	if (vrow > vcol) { /* in the lower triangle */
		/* search downward from diag[vcol] */
		for (el=so->diag[vcol]; ; el = elnext) {
			elnext = el->r_down;
			if (!elnext) {
				break;
			}else if (elnext->row == row) { /* found it */
				return elnext;
			}else if (so->varord[elnext->row] > vrow) {
				break;
			}
		}
		/* insert below el */
		if (!new) {
			new = (Elm *)nrn_pool_alloc(so->elmpool);
			new->value = 0.;
			increase_order(so, row);
		}
		new->r_down = el->r_down;
		el->r_down = new;
		new->r_up = el;
		if (new->r_down) {
			new->r_down->r_up = new;
		}
		/* search leftward from diag[vrow] */
		for (el=so->diag[vrow]; ; el = elnext) {
			elnext = el->c_left;
			if (!elnext) {
				break;
			} else if (so->varord[elnext->col] < vcol) {
				break;
			}
		}
		/* insert to left of el */
		new->c_left = el->c_left;
		el->c_left = new;
		new->c_right = el;
		if (new->c_left) {
			new->c_left->c_right = new;
		}else{
			so->rowst[vrow] = new;
		}
	} else { /* in the upper triangle */
		/* search upward from diag[vcol] */
		for (el=so->diag[vcol]; ; el = elnext) {
			elnext = el->r_up;
			if (!elnext) {
				break;
			}else if (elnext->row == row) { /* found it */
				return elnext;
			}else if (so->varord[elnext->row] < vrow) {
				break;
			}
		}
		/* insert above el */
		if (!new) {
			new = (Elm *)nrn_pool_alloc(so->elmpool);
			new->value = 0.;
			increase_order(so, row);
		}
		new->r_up = el->r_up;
		el->r_up = new;
		new->r_down = el;
		if (new->r_up) {
			new->r_up->r_down = new;
		}
		/* search right from diag[vrow] */
		for (el=so->diag[vrow]; ; el = elnext) {
			elnext = el->c_right;
			if (!elnext) {
				break;
			}else if (so->varord[elnext->col] > vcol) {
				break;
			}
		}
		/* insert to right of el */
		new->c_right = el->c_right;
		el->c_right = new;
		new->c_left = el;
		if (new->c_right) {
			new->c_right->c_left = new;
		}
	}
	new->row = row;
	new->col = col;
	return new;
}

double* _nrn_thread_getelm(SparseObj* so, int row, int col) {
	Elm *el;
	if (!so->phase) {
		return 	so->coef_list[so->ngetcall++];
	}
	el = getelm(so, (unsigned)row, (unsigned)col, ELM0);
	if (so->phase ==1) {
		so->ngetcall++;
	}else{
		so->coef_list[so->ngetcall++] = &el->value;
	}
	return &el->value;
}

static void create_coef_list(SparseObj* so, int n, FUN fun, double* p, void* ppvar, void* thread, void* nt)
{
	initeqn(so, (unsigned)n);
	so->phase = 1;
	so->ngetcall = 0;
	(*fun)(so, so->rhs, p, ppvar, thread, nt);
	if (so->coef_list) {
		free(so->coef_list);
	}
	so->coef_list = (double **)emalloc(so->ngetcall * sizeof(double *));
	spar_minorder(so);
	so->phase = 2;
	so->ngetcall = 0;
	(*fun)(so, so->rhs, p, ppvar, thread, nt);
	so->phase = 0;
}

static void init_coef_list(SparseObj* so) {
	unsigned i;
	Elm *el;
	
	so->ngetcall = 0;
	for (i=1; i<=so->neqn; i++) {
		for (el = so->rowst[i]; el; el = el->c_right) {
			el->value = 0.;
		}
	}
}


static void init_minorder(SparseObj* so) {
	/* matrix has been set up. Construct the orderlist and orderfind
	   vector.
	*/
	unsigned i, j;
	Elm *el;
	
	so->do_flag = 1;
	if (so->roworder) {
		for (i=1; i <= so->nroworder; ++i) {
			Free(so->roworder[i]);
		}
		Free(so->roworder);
	}
	so->roworder = (Item **)emalloc((so->neqn+1)*sizeof(Item *));
	so->nroworder = so->neqn;
	if (so->orderlist) freelist(so->orderlist);
	so->orderlist = newlist();
	for (i=1; i<=so->neqn; i++) {
		so->roworder[i] = newitem();
	}
	for (i=1; i<=so->neqn; i++) {
		for (j=0, el = so->rowst[i]; el; el = el->c_right) {
			j++;
		}
		so->roworder[so->diag[i]->row]->elm = so->diag[i];
		so->roworder[so->diag[i]->row]->norder = j;
		insert(so, so->roworder[so->diag[i]->row]);
	}
}

static void increase_order(SparseObj* so, unsigned row) {
	/* order of row increases by 1. Maintain the orderlist. */
	Item *order;

	if(!so->do_flag) return;
	order = so->roworder[row];
	delete(order);
	order->norder++;
	insert(so, order);
}

static void reduce_order(SparseObj* so, unsigned row) {
	/* order of row decreases by 1. Maintain the orderlist. */
	Item *order;

	if(!so->do_flag) return;
	order = so->roworder[row];
	delete(order);
	order->norder--;
	insert(so, order);
}

static void spar_minorder(SparseObj* so) { /* Minimum ordering algorithm to determine the order
			that the matrix should be solved. Also make sure
			all needed elements are present.
			This does not mess up the matrix
		*/
	unsigned i;

	check_assert(so);
	init_minorder(so);
	for (i=1; i<=so->neqn; i++) {
		get_next_pivot(so, i);
	}
	so->do_flag = 0;
	check_assert(so);
}

static void get_next_pivot(SparseObj* so, unsigned i) {
	/* get varord[i], etc. from the head of the orderlist. */
	Item *order;
	Elm *pivot, *el;
	unsigned j;

	order = so->orderlist->next;
	assert(order != so->orderlist);
	
	if ((j=so->varord[order->elm->row]) != i) {
		/* push order lists down by 1 and put new diag in empty slot */
		assert(j > i);
		el = so->rowst[j];
		for (; j > i; j--) {
			so->diag[j] = so->diag[j-1];
			so->rowst[j] = so->rowst[j-1];
			so->varord[so->diag[j]->row] = j;
		}
		so->diag[i] = order->elm;
		so->rowst[i] = el;
		so->varord[so->diag[i]->row] = i;
		/* at this point row links are out of order for diag[i]->col
		   and col links are out of order for diag[i]->row */
		re_link(so, i);
	}


	/* now make sure all needed elements exist */
	for (el = so->diag[i]->r_down; el; el = el->r_down) {
		for (pivot = so->diag[i]->c_right; pivot; pivot = pivot->c_right) {
			IGNORE(getelm(so, el->row, pivot->col, ELM0));
		}
		reduce_order(so, el->row);
	}

#if 0
{int j; Item *or;
	printf("%d  ", i);
	for (or = so->orderlist->next, j=0; j<5 && or != so->orderlist; j++, or=or->next) {
		printf("(%d, %d)  ", or->elm->row, or->norder);
	}
	printf("\n");
}
#endif
	delete(order);
}

/* The following routines support the concept of a list.
modified from modl
*/

/* Implementation
  The list is a doubly linked list. A special item with element 0 is
  always at the tail of the list and is denoted as the List pointer itself.
  list->next point to the first item in the list and
  list->prev points to the last item in the list.
	i.e. the list is circular
  Note that in an empty list next and prev points to itself.

It is intended that this implementation be hidden from the user via the
following function calls.
*/

static Item* newitem() {
	Item *i;
	i = (Item *)emalloc(sizeof(Item));
	i->prev = ITEM0;
	i->next = ITEM0;
	i->norder = 0;
	i->elm = (Elm *)0;
	return i;
}

static List* newlist() {
	Item *i;
	i = newitem();
	i->prev = i;
	i->next = i;
	return (List *)i;
}

static void freelist(List* list) /*free the list but not the elements*/
{
	Item *i1, *i2;
	for (i1 = list->next; i1 != list; i1 = i2) {
		i2 = i1->next;
		Free(i1);
	}
	Free(list);
}

static void linkitem(Item* item, Item* i)	/*link i before item*/
{
	i->prev = item->prev;
	i->next = item;
	item->prev = i;
	i->prev->next = i;
}


static void insert(SparseObj* so, Item* item) {
	Item *i;

	for (i = so->orderlist->next; i != so->orderlist; i = i->next) {
		if (i->norder >= item->norder) {
			break;
		}
	}
	linkitem(i, item);
}

static void delete(Item* item) {
	item->next->prev = item->prev;
	item->prev->next = item->next;
	item->prev = ITEM0;
	item->next = ITEM0;
}

static void *emalloc(unsigned n) { /* check return from malloc */
	void *p;
	p = malloc(n);
	if (p == (void *)0) {
		abort_run(LOWMEM);
	}
	return (void *)p;
}

void myfree(void* ptr) {
	free(ptr);
}

static void check_assert(SparseObj* so) {
	/* check that all links are consistent */
	unsigned i;
	Elm *el;
	
	for (i=1; i<=so->neqn; i++) {
		assert(so->diag[i]);
		assert(so->diag[i]->row == so->diag[i]->col);
		assert(so->varord[so->diag[i]->row] == i);
		assert(so->rowst[i]->row == so->diag[i]->row);
		for (el = so->rowst[i]; el; el = el->c_right) {
			if (el == so->rowst[i]) {
				assert(el->c_left == ELM0);
			}else{
			   assert(el->c_left->c_right == el);
			   assert(so->varord[el->c_left->col] < so->varord[el->col]);
			}
		}
		for (el = so->diag[i]->r_down; el; el = el->r_down) {
			assert(el->r_up->r_down == el);
			assert(so->varord[el->r_up->row] < so->varord[el->row]);
		}
		for (el = so->diag[i]->r_up; el; el = el->r_up) {
			assert(el->r_down->r_up == el);
			assert(so->varord[el->r_down->row] > so->varord[el->row]);
		}
	}
}

	/* at this point row links are out of order for diag[i]->col
	   and col links are out of order for diag[i]->row */
static void re_link(SparseObj* so, unsigned i) {

	Elm *el, *dright, *dleft, *dup, *ddown, *elnext;
	
	for (el=so->rowst[i]; el; el = el->c_right) {
		/* repair hole */
		if (el->r_up) el->r_up->r_down = el->r_down;
		if (el->r_down) el->r_down->r_up = el->r_up;
	}

	for (el=so->diag[i]->r_down; el; el = el->r_down) {
		/* repair hole */
		if (el->c_right) el->c_right->c_left = el->c_left;
		if (el->c_left) el->c_left->c_right = el->c_right;
		else so->rowst[so->varord[el->row]] = el->c_right;
	}

	for (el=so->diag[i]->r_up; el; el = el->r_up) {
		/* repair hole */
		if (el->c_right) el->c_right->c_left = el->c_left;
		if (el->c_left) el->c_left->c_right = el->c_right;
		else so->rowst[so->varord[el->row]] = el->c_right;
	}

   /* matrix is consistent except that diagonal row elements are unlinked from
   their columns and the diagonal column elements are unlinked from their
   rows.
   For simplicity discard all knowledge of links and use getelm to relink
   */
	so->rowst[i] = so->diag[i];
	dright = so->diag[i]->c_right;
	dleft = so->diag[i]->c_left;
	dup = so->diag[i]->r_up;
	ddown = so->diag[i]->r_down;
	so->diag[i]->c_right = so->diag[i]->c_left = ELM0;
	so->diag[i]->r_up = so->diag[i]->r_down = ELM0;
	for (el=dright; el; el = elnext) {
		elnext = el->c_right;
		IGNORE(getelm(so, el->row, el->col, el));
	}
	for (el=dleft; el; el = elnext) {
		elnext = el->c_left;
		IGNORE(getelm(so, el->row, el->col, el));
	}
	for (el=dup; el; el = elnext) {
		elnext = el->r_up;
		IGNORE(getelm(so, el->row, el->col, el));
	}
	for (el=ddown; el; el = elnext){
		elnext = el->r_down;
		IGNORE(getelm(so, el->row, el->col, el));
	}
}

static SparseObj* create_sparseobj() {

	SparseObj* so;

	so = emalloc(sizeof(SparseObj));
	so->elmpool = nrn_pool_create(100, sizeof(Elm));
	so->rowst = 0;
	so->diag = 0;
	so->neqn = 0;
	so->varord = 0;
	so->rhs = 0;
	so->oldfun = 0;
	so->ngetcall = 0;
	so->phase = 0;
	so->coef_list = 0;
	so->roworder = 0;
	so->nroworder = 0;
	so->orderlist = 0;
	so->do_flag = 0;

	return so;
}

void _nrn_destroy_sparseobj_thread(SparseObj* so) {
	int i;
	if (!so) { return; }
	nrn_pool_delete(so->elmpool);
	if (so->rowst)
		Free(so->rowst);
	if (so->diag)
		Free(so->diag);
	if (so->varord)
		Free(so->varord);
	if (so->rhs)
		Free(so->rhs);
	if (so->coef_list)
		Free(so->coef_list);
	if (so->roworder) {
		for (i=1; i <= so->nroworder; ++i) {
			Free(so->roworder[i]);
		}
		Free(so->roworder);
	}
	if (so->orderlist) freelist(so->orderlist);
	Free(so);
}
