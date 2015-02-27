/*------------------------------------------------------------------------*/
/* Copyright (c) 2005 - 2015 Armin Biere, Johannes Kepler University.     */
/*------------------------------------------------------------------------*/

/*-----------------------------------------------------------------------*\
 * Documentation and terminology used in the implementation can be found in
 * the seperate documentation file 'README.tracecheck'.
 *
 * Armin Biere, JKU Linz, 2005.
\*-----------------------------------------------------------------------*/

/*-----------------------------------------------------------------------*\
 * TODO:
 *
 *   1. allow multiple occurrences of the same literal and the same
 *   antecedent by making the error that currently is reported into a
 *   warning
 *
 *   2. interleave collection of literals and resolution checking to be able
 *   to delete collected literals early
 *------------------------------------------------------------------------*/

#include "config.h"

/*------------------------------------------------------------------------*/

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <time.h>

/*------------------------------------------------------------------------*/

#include "bftime.h"
#include "bfio.h"
#include "bfmem.h"

/*------------------------------------------------------------------------*/

#include "simpaig.h"
#include "aiger.h"


#ifndef NDEBUG
#define SANITY_AIG "/tmp/sanity.aig"
#define SANITY_CNF "/tmp/sanity.cnf"
#define AIGTOCNF_PATH "~/tools/aiger-1.9.4/aigtocnf"
#define PICOSAT_PATH "~/tools/picosat-959/picosat"
#endif

/*------------------------------------------------------------------------*/

#define VPFX "c "		/* verbose output prefix string */
#define EPFX "*** tracecheck: "	/* error message prefix string */

/*------------------------------------------------------------------------*/

#define DONE INT_MIN		/* scanner done token */
#define ERROR INT_MAX		/* scanner error token */

/*------------------------------------------------------------------------*/
/* The scanner returns integer tokens, which either denote an error,
 * represented by 'ERROR', the end of the token stream, represented by
 * 'DONE' or a legal integer.  Thus the maximal index, for both clauses and
 * literals is given by the remaining range of integers.  In essence, we
 * only waste one legal index, e.g. 2^32-1 on a 32-bit machine.
 */
#define MAX_IDX (INT_MAX - 1)

/*------------------------------------------------------------------------*/
/* Lisp-like list operations.  See also the constructor 'cons' below.
 */
#define car(l) ((l)->head)
#define cdr(l) ((l)->tail)

/*------------------------------------------------------------------------*/
/* Macro for declaring an integer stack.  The corresponding operations on
 * the stack are defined by the macro 'DefineIntStackFunctions'.
 */
#define DeclareIntStack(name) \
static int * name; \
static int size_ ## name; \
static int count_ ## name

/*------------------------------------------------------------------------*/
/* Macro for declaring an aig component storage. The corresponding operations on
 * the stack are defined by the macro 'DefineAIGStoreFunctions'.
 */
#define DeclareAIGComponentStorage(name) \
static simpaig ** name; \
static int count_ ## name
/*------------------------------------------------------------------------*/

typedef struct Clause Clause;
typedef struct Cell Cell;	/* list node */
typedef struct Literal Literal;	/* signed */

/*------------------------------------------------------------------------*/

enum Boole {
    FALSE = -1,
    UNKNOWN = 0,
    TRUE = 1,
};

/*------------------------------------------------------------------------*/

enum Format {
    ASCII_FORMAT,
    BINARY_FORMAT,
    COMPRESSED_FORMAT
};

typedef enum Format Format;

/*------------------------------------------------------------------------*/

struct Clause {
    Clause *next_in_order;	/* links relevant clauses */
    Clause *dfs_tree_parent;	/* graph parent in DFS tree */
    int *labels; /* zero terminated list */
    int *literals;		/* zero terminated list */
    int *antecedents;		/* zero terminated list */
    int idx;			/* "<0"=original, ">0"=derived */
    int newidx;			/* "<0"=original, ">0"=derived */
    int mark;			/* graph traversal */
    simpaig *itp; /* partial interpolant */
    unsigned lineno: 28;			/* where the definition started */
    unsigned partition : 3;
    unsigned split: 1;
#ifndef NDEBUG
    unsigned resolved:1;		/* 1 iff already resolved */
#endif
};

/*------------------------------------------------------------------------*/
/* List node for occurrence lists of literals.
 */
struct Cell {
    void *head;
    Cell *tail;
};

/*------------------------------------------------------------------------*/

// Note: bitfields provide no memory savings
//       sizeof(Literal) = 16 (64-bit) best case
// Data structure alignment to multiples of 16 bytes according to Wiki:
//  "x86 CPUs do require the data to be 128-bit (16-byte) aligned"
// A pointer uses 8 bytes, so we need to use a second chunk of 8 bytes in any
// case. Whether that is through padding or data doesn't make a difference, so
// we use mark (4) + label (2) + partition (2) = 8 bytes.
struct Literal {
    Cell *clauses;		/* occurrence lists in chain */

    // local and reset
    int mark;
    short label;

    simpaig *aig;
    unsigned idx;
    // global for whole proof
    short partition;
};

/*------------------------------------------------------------------------*/
/**** START OF STATIC VARIABLES *******************************************/
/*------------------------------------------------------------------------*/

static int static_variables_are_dirty;

/*------------------------------------------------------------------------*/

/* literals[-max_lit_idx,max_lit_idx]
 */
static Literal *literals;
static int max_lit_idx;
static int first_defined_lit_idx;

/*------------------------------------------------------------------------*/

static Cell *free_cells;	/* list of recycled cells */
static Cell *cells;
static int size_cells;

/*------------------------------------------------------------------------*/
#ifndef NDEBUG
static int debug;
#endif
/*------------------------------------------------------------------------*/

static FILE *input;
static const char *input_name;
static int close_input;
static int current_lineno;
static int last_token_lineno;	/* fix for more specific diagnosis */

/*------------------------------------------------------------------------*/

static Format format;

/*------------------------------------------------------------------------*/

static BooleforceInputBuffer buffer;
static int previous_char;	/* implements ungetc */

/*------------------------------------------------------------------------*/

static FILE *output;
static int close_output;
static int verbose;		/* verbose level */
static int itp_system_strength;

/*------------------------------------------------------------------------*/

static FILE * bintrace;
static FILE * ebintrace;
static FILE * restrace;
static FILE * rpttrace;
static FILE * etrace;
static FILE * stats_file;
static FILE * interpolant;
static FILE * dotgraph;

/*------------------------------------------------------------------------*/

static char * original_cnf_file_name;
static int original_variables;
static int original_clauses;

/*------------------------------------------------------------------------*/
/* If this flag is set, for instance through the command line option
 * '--linear', then the linearization of the antecedents in the individual
 * chains is skipped.  It is assumed that the antecedents of all chains in
 * the trace can already be resolved in the given order with regular input
 * resolution (trivial resolution in [1]).  Since linearization is currently
 * the most expensive phase, enabling this flag can speed up the checker
 * quite a bit.  Experimentally we observerd speedups of 4.
 */
static int assume_already_linearized;

/*------------------------------------------------------------------------*/

static int lax;

/*------------------------------------------------------------------------*/
/* head of list (Clause.next_in_order)
 */
static Clause *first_in_order;

/* clauses[1,size_clauses[
 */
static Clause **clauses;
static int size_clauses;
static int max_cls_idx;
static int empty_cls_idx;

static int min_derived_idx;
static int max_original_idx;

static int num_original_clauses;
static int num_original_literals;

static int num_derived_init;
static int num_derived_clauses;
static int num_derived_literals;

static int num_splits;
static int num_merge_literals;
static int num_merge_literals_after;

static int partition_split;

static simpaigmgr *mgr;
static simpaig *final_itp;
static simpaig **aigs;
static aiger *output_aig;

static int gates;
static int ands;


#ifndef NDEBUG
static simpaig *partition_a;
static simpaig *partition_b;
#endif


static char *outbuffer;
static unsigned size_outbuffer;

enum Label {
    UNDEF = -1,
    BOT = 0,
    A = 1,
    B = 2,
    AB = 3,
};

/*------------------------------------------------------------------------*/

DeclareIntStack (resolvent);
DeclareIntStack (stack);
DeclareIntStack (roots);
DeclareIntStack (trail);
DeclareIntStack (labels);

/*------------------------------------------------------------------------*/

DeclareAIGComponentStorage (circuit_components_m);
DeclareAIGComponentStorage (circuit_components_itp);

/*------------------------------------------------------------------------*/

static int conflict_occurred;

/*------------------------------------------------------------------------*/
/* statistics
 */
static int num_resolutions;	/* number resolution steps */
static int num_antecedents;
static int max_antecedents;
static int num_empty_clauses;

static int init_max_cls_idx;
static int num_split_clauses;
static int num_derived_clauses_before_split;
static int num_antecedents_before_split;
static int num_derived_clauses_after_split;
static int num_antecedents_after;

/*------------------------------------------------------------------------*/

static double entered_time;

/*------------------------------------------------------------------------*/
/***** END OF STATIC VARIABLES ********************************************/
/*------------------------------------------------------------------------*/
#define DefineIntStackFunctionsNoPop(name) \
static void \
enlarge_ ## name (void) \
{ \
  int new_size_ ## name = size_ ## name ? 2 * size_ ## name : 1; \
  BOOLEFORCE_ENLARGE_ARRAY (name, size_ ## name, new_size_ ## name); \
  size_ ## name = new_size_ ## name; \
} \
\
inline static void \
push_ ## name (int n) \
{ \
  if (size_ ## name == count_ ## name) \
    enlarge_ ## name (); \
 \
   name[count_ ## name++] = n; \
} \
\
static void \
release_ ## name (void) \
{ \
  BOOLEFORCE_DELETE_ARRAY (name, size_ ## name); \
  name = 0; \
  size_ ## name = 0; \
  count_ ## name = 0; \
}

#define DefineIntStackFunctions(name) \
\
DefineIntStackFunctionsNoPop(name) \
\
inline static int \
pop_ ## name (void) \
{ \
  int res; \
  assert (count_ ## name > 0); \
  res =  name[--count_ ## name]; \
  return res; \
}

/*------------------------------------------------------------------------*/
/* *INDENT-OFF* */
DefineIntStackFunctions(stack)
DefineIntStackFunctions(resolvent)
DefineIntStackFunctionsNoPop(roots)
DefineIntStackFunctions(trail);
DefineIntStackFunctionsNoPop(labels);
/* *INDENT-ON* */
/*------------------------------------------------------------------------*/

/*------------------------------------------------------------------------*/
/* Build circuit like a tree, so that it has less levels (maybe better for AIG
 * reductions)
 */
#define DefineAIGStoreFunctions(name) \
    static simpaig* \
build_circuit_rec_ ## name (int level, \
  simpaig* (*aig_connective) (simpaigmgr*, simpaig*, simpaig*), \
  simpaig* base) \
{ \
  simpaig *c1, *c2; \
  if (level == 1) \
  { \
    if (count_circuit_components_ ## name > 0) \
      return circuit_components_ ## name[--count_circuit_components_ ## name]; \
    else \
      return base; \
  } \
  c1 = build_circuit_rec_ ## name (level >> 1, aig_connective, base); \
  c2 = build_circuit_rec_ ## name (level >> 1, aig_connective, base); \
  return aig_connective (mgr, c1, c2); \
}

/*------------------------------------------------------------------------*/
/* *INDENT-OFF* */
DefineAIGStoreFunctions(m);
DefineAIGStoreFunctions(itp);
/* *INDENT-ON* */
/*------------------------------------------------------------------------*/


static void
LOG (const char *fmt, ...)
{
    va_list ap;

    fputs (VPFX, output);
    va_start (ap, fmt);
    vfprintf (output, fmt, ap);
    va_end (ap);
    fputc ('\n', output);
}

/*------------------------------------------------------------------------*/
/* Lisp's 'cons' operator for generated a linked list node.
 */
inline static Cell *
cons (Cell * tail, void *head)
{
    Cell *res;

    assert (free_cells);
    res = free_cells;
    free_cells = cdr (res);
    res->head = head;
    res->tail = tail;

    return res;
}

/*------------------------------------------------------------------------*/

#define CONS(tail,head) \
  do { (tail) = cons ((tail), (head)); } while (0)

/*------------------------------------------------------------------------*/
/* Recycle all cells reachable from 'root'.
 */
static void
recycle_cells (Cell * root)
{
    Cell *p, *tail;

    assert (root);

    for (p = root; (tail = cdr (p)); p = tail)
        ;

    p->tail = free_cells;
    free_cells = root;
}

/*------------------------------------------------------------------------*/

#define RECYCLE_CELLS(root) \
do { \
  if (!(root)) \
    break; \
  recycle_cells (root); \
  (root) = 0; \
} while(0)

/*------------------------------------------------------------------------*/

static void
recycle_cell (Cell * cell)
{
    cell->tail = free_cells;
    free_cells = cell;
}

/*------------------------------------------------------------------------*/
#if 0

inline static int
length_gt_than (Cell * root, int n)
{
    Cell *p;
    int i;

    assert (n >= 0);

    i = 0;
    for (p = root; p; p = cdr (p))
        if (++i > n)
            return 1;

    return 0;
}

#endif
/*------------------------------------------------------------------------*/

static size_t
length_ints (int *a)
{
    size_t res;
    int *p;

    assert (a);

    res = 0;
    for (p = a; *p; p++)
        res++;

    return res;
}

/*------------------------------------------------------------------------*/

// TODO: num_derived not correct as it is used with splits as well
inline static Clause *
add_clause (int idx, int *literals, int *antecedents, int lineno)
{
    int new_size, llen, alen;
    Clause *res, **p;
    size_t res_bytes;

    assert (idx);
    assert (idx > 0);

    if(idx > max_cls_idx)
        max_cls_idx = idx;

    if (literals && !literals[0])
        num_empty_clauses++;

    llen = literals ? length_ints (literals) : 0;
    alen = antecedents ? length_ints (antecedents) : 0;

    res_bytes = sizeof (*res);
    res = booleforce_new (res_bytes);

    res->idx = idx;
    res->lineno = lineno;
    assert (!res->mark);

    res->literals = literals;
    res->antecedents = antecedents;

    while (idx >= size_clauses) {
        if (!(new_size = 2 * size_clauses))
            new_size = 1;

        BOOLEFORCE_ENLARGE_ARRAY (clauses, size_clauses, new_size);
        size_clauses = new_size;
    }

    p = clauses + idx;

    if (antecedents) {
        num_derived_clauses++;
        num_antecedents += alen;

        if (literals)
            num_derived_literals += llen;

        if (!min_derived_idx || idx < min_derived_idx)
            min_derived_idx = idx;
    } else {
        num_original_clauses++;

        if (idx > max_original_idx)
            max_original_idx = idx;

        if (literals)
            num_original_literals += llen;
    }

    assert (!*p);
    *p = res;

    return res;
}

/*------------------------------------------------------------------------*/

static void
delete_clause (Clause * clause)
{
    assert (clause);

    if (clause->literals)
        booleforce_delete_ints (clause->literals);

    if (clause->antecedents)
        booleforce_delete_ints (clause->antecedents);

    if (clause->labels)
        booleforce_delete_ints (clause->labels);

    booleforce_delete (clause, sizeof (*clause));
}

/*------------------------------------------------------------------------*/

static void
print_zero_terminated_array (int *a, FILE * file)
{
    int *p, i;

    for (p = a; (i = *p); p++)
        fprintf (file, "%d ", i);

    fputc ('0', file);
}

/*------------------------------------------------------------------------*/

static void
print_clause (Clause * clause, int print_literals, FILE * file)
{
    assert (clause);
    fprintf (file, "%d ", clause->idx);

    if ((print_literals || !clause->antecedents) && clause->literals)
        print_zero_terminated_array (clause->literals, file);
    else
        fputc ('*', file);

    if (clause->antecedents) {
        fputc (' ', file);
        print_zero_terminated_array (clause->antecedents, file);
    } else
        fputs (" 0", file);

    fputc ('\n', file);
}

/*------------------------------------------------------------------------*/

static void
print (FILE * file)
{
    Clause *clause;
    int i;

    for (i = 1; i < size_clauses; i++)
        if ((clause = clauses[i]))
            print_clause (clause, 1, file);
}

/*------------------------------------------------------------------------*/

inline static int
next_char (void)
{
    int ch;

    if (previous_char != EOF) {
        ch = previous_char;
        previous_char = EOF;
    } else
        ch = next_char_from_buffer (buffer);

    if (ch == '\n')
        current_lineno++;

    return ch;
}

/*------------------------------------------------------------------------*/

inline static void
put_back_char (int ch)
{
    assert (previous_char == EOF);
    previous_char = ch;
    if (ch == '\n') {
        assert (current_lineno > 1);
        current_lineno--;
    }
}

/*------------------------------------------------------------------------*/
/* Unfortunately we have three kind of error codes.  In one case, my
 * preferred version, the error code of a function is actually a success
 * code:  the function returns zero iff it was not successful.  This is the
 * convention used for 'parse_error' and 'check_error'.  However, the OS
 * view is different and the main function has to return zero iff it was
 * successful.  Accordingly we use this convention for 'option_error'.  A
 * parse error is signalled by using a specific non zero integer, that can
 * not represent a legal number in the input.
 */
#define check_error !option_error
#define parse_error !scan_error

/*------------------------------------------------------------------------*/

static int
scan_error (const char *fmt, ...)
{
    va_list ap;

    fprintf (output, "%s:%d: ", input_name, last_token_lineno);
    va_start (ap, fmt);
    vfprintf (output, fmt, ap);
    va_end (ap);
    fputc ('\n', output);

    return ERROR;
}

/*------------------------------------------------------------------------*/

static int
option_error (const char *fmt, ...)
{
    va_list ap;

    fputs (EPFX, output);
    va_start (ap, fmt);
    vfprintf (output, fmt, ap);
    va_end (ap);
    fputc ('\n', output);

    return 1;
}

/*------------------------------------------------------------------------*/

inline static int
next_non_white_space_char_ignoring_comments (void)
{
    int ch;

SKIP_WHITE_SPACE_AND_COMMENTS:

    ch = next_char ();
    if (isspace (ch))
        goto SKIP_WHITE_SPACE_AND_COMMENTS;

    if (ch == 'c') {
        while ((ch = next_char ()) != '\n' && ch != EOF)
            ;

        if (ch == '\n')
            goto SKIP_WHITE_SPACE_AND_COMMENTS;
    }

    return ch;
}

/*------------------------------------------------------------------------*/

inline static int
next_int (void)
{
    int ch, res, sign, digit;

    ch = next_non_white_space_char_ignoring_comments ();

    if (ch == EOF)
        return DONE;

    last_token_lineno = current_lineno;

    sign = 1;
    if (ch == '-') {
        sign = -1;
        ch = next_char ();
    }

    if (!isdigit (ch)) {
        if (isprint (ch))
            return scan_error ("expected digit or '-' but got '%c'", ch);
        else
            return scan_error ("expected digit or '-' but got 0x%x", ch);
    }

    res = ch - '0';

    ch = next_char ();
    while (isdigit (ch)) {
        if (res > (MAX_IDX / 10))
NUMBER_TOO_LARGE:
            return scan_error ("number too large");

        res *= 10;
        digit = ch - '0';

        if (res > MAX_IDX - digit)
            goto NUMBER_TOO_LARGE;

        res += digit;
        ch = next_char ();
    }

    if (ch != EOF && !isspace (ch))
        return scan_error ("expected EOF or white space after number");

    res *= sign;

    return res;
}

/*------------------------------------------------------------------------*/

inline static int
is_valid_clause_idx (int idx)
{
    if (idx <= 0)
        return 0;

    return idx < size_clauses;
}

/*------------------------------------------------------------------------*/

inline static Clause *
idx2clause (int idx)
{
    if (!is_valid_clause_idx (idx))
        return 0;

    return clauses[idx];
}

/*------------------------------------------------------------------------*/

static int
parse_zero_terminated_list_of_numbers (int only_positive_indices)
{
    int n;

    assert (!count_stack);

NEXT_NUMBER:
    n = next_int ();

    if (n == DONE)
        return parse_error ("no zero before EOF");

    if (n == ERROR)
        return 0;

    if (!n)
        return 1;

    if (only_positive_indices && n < 0)
        return parse_error ("expected positive number");

    push_stack (n);

    goto NEXT_NUMBER;
}

/*------------------------------------------------------------------------*/

static int *
copy_ints (int * start, int count)
{
    int *res, i, tmp;

    BOOLEFORCE_NEW_ARRAY (res, count + 1);
    for (i = 0; i < count; i++) {
        tmp = start[i];
        assert (tmp);
        res[i] = tmp;
    }

    res[count] = 0;

    return res;
}

/*------------------------------------------------------------------------*/

static int *
copy_stack (void)
{
    int * res;
    res = copy_ints (stack, count_stack);
    count_stack = 0;
    return res;
}

/*------------------------------------------------------------------------*/

static int *
copy_labels (void)
{
    int * res;
    res = copy_ints (labels, count_labels);
    count_labels = 0;
    return res;
}

/*------------------------------------------------------------------------*/

static int
parse_ascii (void)
{
    int ch, idx, idx_lineno, *literals, *antecedents, *p, lit;
    Clause *other;

NEXT_CLAUSE:
    idx = next_int ();

    if (idx == ERROR)
        return 0;

    if (idx == DONE)
        return 1;

    if (idx < 0)
        return parse_error ("negative clause index");

    if (!idx)
        return parse_error ("zero clause index");

    if ((other = idx2clause (idx)))
        return parse_error ("clause %d already defined at line %d",
                            idx, other->lineno);

    idx_lineno = last_token_lineno;

    ch = next_non_white_space_char_ignoring_comments ();

    if (ch != '*') {
        put_back_char (ch);
        if (!parse_zero_terminated_list_of_numbers (0))
            return 0;

        literals = copy_stack ();
    } else
        literals = 0;

    if (!parse_zero_terminated_list_of_numbers (1)) {
        BOOLEFORCE_DELETE_ARRAY (literals, length_ints (literals) + 1);
        return 0;
    }

    // Note: delete parsed literals if there are antecedents
    // We have to enter collect_literals for labelling, so we only support '*'
    // literal lists for internal vertices
    // TODO Maybe allow labelling through input file, then this could be changed
    if (count_stack) {
        if (literals)
            BOOLEFORCE_DELETE_ARRAY (literals, length_ints (literals) + 1);
        literals = 0;
        antecedents = copy_stack ();
    } else
        antecedents = 0;

    if (!literals && !antecedents)
        return parse_error ("original clause without literals");

    if (original_cnf_file_name) {
        if (antecedents && idx <= original_clauses)
            return parse_error ("derived clause index %d too small", idx);

        if (!antecedents && idx > original_clauses)
            return parse_error ("original clause index %d too large", idx);

        if (literals) {
            for (p = literals; (lit = *p); p++)
                if (abs (lit) > original_variables)
                    return parse_error ("literal %d too large", lit);
        }
    }

    (void) add_clause (idx, literals, antecedents, idx_lineno);

    goto NEXT_CLAUSE;
}

/*------------------------------------------------------------------------*/

static const char *
format2str (Format f)
{
    switch (f) {
    case ASCII_FORMAT:
        return "ascii";
    case BINARY_FORMAT:
        return "binary";
    default:
        break;
    }

    assert (f == COMPRESSED_FORMAT);

    return "compressed";
}

/*------------------------------------------------------------------------*/

static int
parse_header (void)
{
    int ch;

    ch = next_non_white_space_char_ignoring_comments ();

    if (ch == EOF) {
        if (verbose)
            LOG ("empty trace file");

        format = ASCII_FORMAT;
        return 1;
    } else if (ch == 'p') {
        ch = next_char ();
        if (!isspace (ch))
EXPECTED_WHITE_SPACE:
            return parse_error ("expected white space");

        while ((ch = next_char ()) == ' ')
            ;

        switch (ch) {
        case 'a':
            if (next_char () != 's' ||
                    next_char () != 'c' ||
                    next_char () != 'i' || next_char () != 'i')
                goto INVALID_FORMAT_ERROR;
            format = ASCII_FORMAT;
            break;
        case 'b':
            if (next_char () != 'i' ||
                    next_char () != 'n' ||
                    next_char () != 'a' ||
                    next_char () != 'r' || next_char () != 'y')
                goto INVALID_FORMAT_ERROR;
            format = BINARY_FORMAT;
            break;
        case 'c':
            if (next_char () != 'i' ||
                    next_char () != 'n' ||
                    next_char () != 'a' ||
                    next_char () != 'r' || next_char () != 'y')
                goto INVALID_FORMAT_ERROR;
            format = COMPRESSED_FORMAT;
            break;
        default:
INVALID_FORMAT_ERROR:
            return parse_error ("expected format: ascii, binary, compressed");
        }

        if ((ch = next_char ()) != ' ')
            goto EXPECTED_WHITE_SPACE;

        while ((ch = next_char ()) == ' ')
            ;

        if (ch != 't' ||
                next_char () != 'r' ||
                next_char () != 'a' || next_char () != 'c' || next_char () != 'e')
            return parse_error ("expected 'trace'");

        while ((ch = next_char ()) == ' ')
            ;

        if (ch != EOF && ch != '\n')
            return parse_error ("expected new line or EOF after 'trace'");

        return 1;
    } else if (ch != '-' && !isdigit (ch))
        return parse_error ("expected 'p' or digit");
    else {
        if (verbose)
            LOG ("format header missing");

        format = ASCII_FORMAT;
        put_back_char (ch);
        return 1;
    }
}

/*------------------------------------------------------------------------*/

#define AVG(a,b) \
  (((b) != 0) ? (a) / (double) (b) : 0.0)

/*------------------------------------------------------------------------*/

static int
parse (void)
{
    double start_time = booleforce_time_stamp ();
    int num_clauses, num_literals;
    int res;

    if (verbose)
        LOG ("parsing %s", input_name);

    current_lineno = 1;
    previous_char = EOF;

    if (!parse_header ())
        return 0;

    if (verbose)
        LOG ("%s trace", format2str (format));

    res = 0;

    if (format == BINARY_FORMAT)
        return parse_error ("parsing of binary traces not implemented yet");
    else if (format == COMPRESSED_FORMAT)
        return parse_error ("parsing of compressed traces not implemented yet");
    else {
        assert (format == ASCII_FORMAT);
        res = parse_ascii ();
    }

    if (res && verbose) {
        num_clauses = num_original_clauses + num_derived_clauses;
        num_literals = num_original_literals + num_derived_literals;

        LOG ("   original: %9d clauses %10d literals    %7.1f/clause",
             num_original_clauses,
             num_original_literals,
             AVG (num_original_literals, num_original_clauses));
        LOG ("    derived: %9d clauses %10d literals    %7.1f/clause",
             num_derived_clauses,
             num_derived_literals,
             AVG (num_derived_literals, num_derived_clauses));
        LOG ("        all: %9d clauses %10d literals    %7.1f/clause",
             num_clauses, num_literals, AVG (num_literals, num_clauses));
        LOG ("antecedents:             %16d antecedents %7.1f/chain",
             num_antecedents, AVG (num_antecedents, num_derived_clauses));

        LOG ("found %d empty clause%s",
             num_empty_clauses, num_empty_clauses == 1 ? "" : "s");

        booleforce_report (start_time, output, VPFX "parsed %s", input_name);
    }

    return res;
}

/*------------------------------------------------------------------------*/

static int
link_clause (Clause * clause)
{
    int *p, idx;

    assert (clause);
    assert (clause->antecedents);

    for (p = clause->antecedents; (idx = *p); p++)
        if (!idx2clause (idx))
            return check_error ("clause %d used in clause %d is undefined",
                                idx, clause->idx);

    return 1;
}

/*------------------------------------------------------------------------*/
/* Check that all mentionend antecedents are indeed defined.
 */
static int
link_derived_clauses (void)
{
    double start_time = booleforce_time_stamp ();
    Clause **p, **end, *clause;
    int count;

    count = 0;
    end = clauses + size_clauses;
    for (p = clauses + 1; p < end; p++) {
        if ((clause = *p)) {
            if (!clause->antecedents)
                continue;

            count++;
            if (!link_clause (clause))
                return 0;
        }
    }

    if (verbose)
        booleforce_report (start_time, output, VPFX "linked %d clauses", count);

    return 1;
}

/*------------------------------------------------------------------------*/

static int
color_clause (Clause * clause)
{
    int * p, lit;
    if (clause->antecedents)
        return check_error ("clause has antecedents");

    for (p = clause->literals; (lit = *p); p++) {
        short label = clause->partition;
        push_labels(label);
    }
    clause->labels = copy_labels();

    return 1;
}

/*------------------------------------------------------------------------*/

static void
find_roots (void)
{
    double start_time = booleforce_time_stamp ();
    Clause *clause, *other;
    Clause **p, **end;
    int *q, idx;

    /* First mark all clauses that are used in an antecedent.
     */
    end = clauses + size_clauses;
    for (p = clauses + 1; p < end; p++) {
        clause = *p;

        if (!clause)
            continue;

        if (!clause->antecedents)
            continue;

        for (q = clause->antecedents; (idx = *q); q++) {
            other = idx2clause (idx);
            assert (other);

            if (other->antecedents)
                other->mark = 1;
        }
    }

    /* Collect unmarked derived clauses as roots.
     */
    for (p = clauses + 1; p < end; p++) {
        clause = *p;

        if (!clause)
            continue;

        if (!clause->antecedents)
            continue;

        if (clause->mark) {
            clause->mark = 0;
            continue;
        }

        push_roots (clause->idx);
#ifdef BOOLEFORCE_LOG
        if (clause->literals && verbose > 1)
            LOG ("found root%s %d",
                 clause->literals[0] ? "" : " empty clause", clause->idx);
#endif
    }

    if (verbose)
        booleforce_report (start_time, output,
                           VPFX "found %d derived root clause%s",
                           count_roots, count_roots == 1 ? "" : "s");
}

/*------------------------------------------------------------------------*/

static void
copy_roots (void)
{
    int *p;
    assert (!count_stack);
    for (p = roots; p < roots + count_roots; p++)
        push_stack (*p);
}

/*------------------------------------------------------------------------*/
/* This phase collects all relevant clauses for deriving the empty clause in
 * the list that uses the 'next_in_order' field of clauses and starts with
 * 'first_in_order'.  Missing clauses are reported as error.  The order of
 * the clauses is a prefix order which does not necessarily respect the
 * global resolution proof order and will be fixed by 'order ()'.  If there
 * are derived clauses left that are not reachable from roots, then the
 * dependency between these clauses necessarily is cyclic and is reported as
 * an error.
 */
static int
collect (void)
{
    int *p, mark_original_clauses, mark_derived_clauses, idx;
    double start_time = booleforce_time_stamp ();
    Clause *clause;

    assert (!first_in_order);
    mark_derived_clauses = mark_original_clauses = 0;

    copy_roots ();

    while (count_stack) {
        idx = pop_stack ();
        clause = idx2clause (idx);
        assert (clause);

        if (clause->mark)
            continue;

        clause->next_in_order = first_in_order;
        first_in_order = clause;

        clause->mark = 1;

        if (clause->antecedents) {
            for (p = clause->antecedents; *p; p++)
                push_stack (*p);

            mark_derived_clauses++;
        } else
            mark_original_clauses++;
    }

    for (idx = 1; idx < size_clauses; idx++) {
        clause = clauses[idx];

        if (!clause)
            continue;

        if (!clause->antecedents)
            continue;

        if (!clause->mark)
            return check_error ("clause %d has a cyclic dependency", idx);
    }

    for (clause = first_in_order; clause; clause = clause->next_in_order)
        clause->mark = 0;

    if (verbose)
        booleforce_report (start_time, output,
                           VPFX "collected %d original and %d derived clauses",
                           mark_original_clauses, mark_derived_clauses);

    return 1;
}

/*------------------------------------------------------------------------*/

static void
unmark_clauses (void)
{
    Clause *p;

    for (p = first_in_order; p; p = p->next_in_order)
        p->mark = 0;
}

/*------------------------------------------------------------------------*/

static void
reverse_clauses (void)
{
    Clause *prev, *this, *next_in_order;

    prev = 0;
    this = first_in_order;
    while (this) {
        next_in_order = this->next_in_order;
        this->next_in_order = prev;
        prev = this;
        this = next_in_order;
    }
    first_in_order = prev;
}

/*------------------------------------------------------------------------*/
#ifndef NDEBUG
/*------------------------------------------------------------------------*/

static void
check_topologically_sorted (void)
{
    Clause *p, *antecedent;
    int *q, idx, order;

    order = 0;
    for (p = first_in_order; p; p = p->next_in_order)
        p->mark = order++;

    for (p = first_in_order; p; p = p->next_in_order) {
        if (!p->antecedents)
            continue;

        for (q = p->antecedents; (idx = *q); q++) {
            antecedent = idx2clause (idx);
            assert (antecedent->mark < p->mark);
        }
    }

    unmark_clauses ();
}

/*------------------------------------------------------------------------*/
#endif
/*------------------------------------------------------------------------*/
/* Check if the resolution proof contains a cycle.  As example consider the
 * 'tccycle0' test case with trace 'LOG/tccycle0.in':
 *
 *   -1 1 -2 0
 *   -2 -1 2 0
 *   1 1 0 -1 2 0
 *   2 2 0 -2 1 0
 *   3 -1 0 -2 4 0
 *   4 -2 0 -1 3 0
 *   5 0 1 3 0
 *
 * If ordering is removed, then the checker reports no error, though the
 * original formula is satisfiable.
 *
 * Finally the list of clauses connected through the 'next_in_order' fields
 * and with head 'first_in_order' is reordered in postfix order to match the
 * global resolution proof order.
 *
 * Since the number of clauses can be really large we implemented a non
 * recursive depth first search (DFS) with three colors stored in the mark
 * flag.  The color 'white' represented by the integer '0' means unvisited,
 * color 'grey' represented by '1' means visited, but not all children
 * visited yet, and finally color 'black' means visited and all children
 * visited.  The grey clauses are those to which a back edge may still
 * exist.  They are stored before their children on the search stack,
 * seperated by '0'.  If all the children of a node have been visited, then
 * a '0' is found on top of the stack, which indicates that the children of
 * the next clause on the stack have been visited, and this clause can be
 * colored 'black'.
 *
 * A description of the three color scheme for DFS can be found in standard
 * text books on algorithms, such as 'Introduction to Algorithms' by Cormen
 * et.al. though I have not found a description of the non-recursive
 * version implemented here anywhere.
 */
static int
order (void)
{
    int *p, clause_idx, antecedent_idx;
    double start_time = booleforce_time_stamp ();
    Clause *clause, *antecedent;

    first_in_order = 0;

    copy_roots ();

    while (count_stack) {
        clause_idx = pop_stack ();

        if (clause_idx) {
            clause = idx2clause (clause_idx);

            if (clause->mark == 2)
                continue;

            if (clause->mark == 1) {
                /* This is the only place where we need the 'dfs_tree_parent'
                 * file, which can be optimized away if this error message is
                 * not too cumbersome otherwise.
                 */
                assert (clause->dfs_tree_parent);
                return
                    check_error ("clause %d depends recursively on clause %d",
                                 clause->dfs_tree_parent->idx, clause->idx);
            }

            clause->mark = 1;

            /* Clauses without antecedents (leaves in the DFS) can be post
             * processed immediately.
             */
            if (!clause->antecedents)
                goto POSTFIX_PROCESSING;

            push_stack (clause_idx);
            push_stack (0);

            assert (clause->antecedents);
            for (p = clause->antecedents; (antecedent_idx = *p); p++) {
                antecedent = idx2clause (antecedent_idx);
                antecedent->dfs_tree_parent = clause;
                push_stack (antecedent_idx);
            }
        } else {
            assert (count_stack);
            clause_idx = pop_stack ();
            clause = idx2clause (clause_idx);

POSTFIX_PROCESSING:
            clause->next_in_order = first_in_order;
            first_in_order = clause;

            clause->mark = 2;
        }
    }

    unmark_clauses ();
    reverse_clauses ();
#ifndef NDEBUG
    check_topologically_sorted ();
#endif
    if (verbose)
        booleforce_report (start_time, output,
                           VPFX "topologically sorted chains");

    return 1;
}

/*------------------------------------------------------------------------*/

static void
init_aigs (void)
{
    int lit;
    for (lit = 1; lit <= max_lit_idx; lit++) {
        literals[lit].aig = simpaig_var (mgr, literals + lit, 0);
        literals[lit].idx = lit;
        literals[-lit].aig = simpaig_not (literals[lit].aig);
        literals[-lit].idx = lit;
    }
}

/*------------------------------------------------------------------------*/

static void
init_literals (void)
{
    double start_time = booleforce_time_stamp ();
    Clause *clause;
    int *p, idx;

    max_lit_idx = 0;

    for (clause = first_in_order; clause; clause = clause->next_in_order) {
        if (!clause->literals)
            continue;

        if (!clause->antecedents) {
            if (clause->idx <= partition_split) {
                clause->partition = A;
                clause->itp = simpaig_false (mgr);
            } else {
                clause->partition = B;
                clause->itp = simpaig_true (mgr);
            }

            color_clause(clause);
        }

        for (p = clause->literals; (idx = *p); p++) {
            if (idx < 0)
                idx = -idx;

            if (idx > max_lit_idx)
                max_lit_idx = idx;
        }
    }

    BOOLEFORCE_NEW_ARRAY (literals, 2 * max_lit_idx + 1);
    // Note: set pointer to the middle of the range
    literals += max_lit_idx;

#ifndef NDEBUG
    /* A valid position denotes a valid index in a stack.  Therefore a
     * negative position means undefined.
     */
    for (idx = -max_lit_idx; idx <= max_lit_idx; idx++) {
        Literal * lit = literals + idx;
        assert (lit->mark == 0);
    }
#endif

    if (verbose)
        booleforce_report (start_time, output,
                           VPFX "initialized literals with maximal index %d",
                           max_lit_idx);
}

/*------------------------------------------------------------------------*/

static void
init_cells (void)
{
    double start_time = booleforce_time_stamp ();
    Clause *clause;
    int tmp, i;

    max_antecedents = 0;

    for (clause = first_in_order; clause; clause = clause->next_in_order) {
        if (!clause->antecedents)
            continue;

        tmp = length_ints (clause->antecedents);
        if (tmp > max_antecedents)
            max_antecedents = tmp;
    }

    /* We watch at most '2 * max_antecedents' literals.  Since recycling of a
     * cell is done after moving the watch and thus allocating a new cell, we
     * need to preallocate one more cell than the maximum number of watched
     * literals.
     */
    size_cells = 2 * max_antecedents + 1;
    BOOLEFORCE_NEW_ARRAY (cells, size_cells);

    for (i = 0; i < size_cells; i++)
        recycle_cell (cells + i);

    if (verbose)
        booleforce_report (start_time, output,
                           VPFX "maximal %d antecedents", max_antecedents);
}

/*------------------------------------------------------------------------*/

static void
mark_literals (int *zero_terminated_array, int mark)
{
    int *p, idx;

    for (p = zero_terminated_array; (idx = *p); p++)
        literals[idx].mark = mark;
}


/*------------------------------------------------------------------------*/

static int
normalize_literals (Clause * clause)
{
    int idx, *p;

    assert (clause->literals);
#ifndef NDEBUG
    for (p = clause->literals; (idx = *p); p++)
        assert (!literals[idx].mark);
#endif

    for (p = clause->literals; (idx = *p); p++) {
        if (literals[idx].mark)
            return
                check_error
                ("multiple occurrences of literal %d in clause %d at line %d", idx,
                 clause->idx, clause->lineno);

        if (literals[-idx].mark)
            return check_error ("clause %d at line %d is trivial "
                                "since it contains %d and %d",
                                clause->idx, clause->lineno, -idx, idx);

        literals[idx].mark = 1;
    }

    mark_literals (clause->literals, 0);

    return 1;
}

/*------------------------------------------------------------------------*/

static int
normalize_antecedents (Clause * clause)
{
    Clause *antecedent;
    int idx, *p;

    assert (clause->antecedents);
#ifndef NDEBUG
    for (p = clause->antecedents; (idx = *p); p++)
        assert (!idx2clause (idx)->mark);
#endif

    for (p = clause->antecedents; (idx = *p); p++) {
        antecedent = idx2clause (idx);
        assert (antecedent);
        if (antecedent->mark)
            return
                check_error
                ("multiple occurrene of antecedent %d in chain %d at line %d", idx,
                 clause->idx, clause->lineno);

        antecedent->mark = 1;
    }

    for (p = clause->antecedents; (idx = *p); p++)
        idx2clause (idx)->mark = 0;

    return 1;
}

/*------------------------------------------------------------------------*/
/* Including these normalization checks restricts legal traces to those that
 * do not include trivial clauses or clauses with multiple occurrences of the
 * same literal nor the same antecedent.  Both restrictions are not strictly
 * necessary but simplify the implementation.
 *
 * TODO:
 *
 *  rather then aborting try to fix the problem.
 */
static int
normalize (Clause * clause)
{
    if (!normalize_literals (clause))
        return 0;

    if (clause->antecedents && !normalize_antecedents (clause))
        return 0;

    return 1;
}

/*------------------------------------------------------------------------*/
/* Computing final clause of a chain. It is the remainder of possible
 * resolutions.
 */
static int
collect_literals (Clause * clause)
{
    int idx, * p, * q, lit;
    Clause * antecedent;

    // Note: Collect literals for all internal vertices, no matter if literal
    // list supplied or not
    if (!clause->antecedents/*clause->literals*/)
        return 1;

    assert (!count_stack);
    assert (clause->antecedents);		/* should be checked by parser */

    for (p = clause->antecedents; (idx = *p); p++) {
        antecedent = idx2clause (idx);

        assert (antecedent);
        assert (antecedent->literals);

        for (q = antecedent->literals; (lit = *q); q++) {
            short label = UNDEF;
            if (!antecedent->antecedents) {
                // TODO visit always or just once?
                label = antecedent->partition;

                // partition(var(l))
                literals[-lit].partition = literals[lit].partition =
                                               (literals[-lit].partition | literals[lit].partition | label);

            } else
                label = antecedent->labels[q - antecedent->literals];


            // Note: label for pivots doesn't matter at this point
            // It is determined in resolve_and_split later.
            // We only compute labels for literals in resolvent
            // marks encoded in LSBs: xyz:
            //   x ... merge literal
            //   y ... appears in both phases
            //   z ... appears in one phase
            // y and z are mutually exclusive
            // x is only set if y or z are set
            if (literals[lit].mark & 1) {
                literals[lit].mark |= 4;
                assert (literals[lit].label != UNDEF);
                literals[lit].label |= label;
            } else if (literals[lit].mark == 0) {
                literals[lit].mark = 1;
                literals[lit].label = label;
            } else if (literals[lit].mark & 2) {
                assert (literals[lit].label != UNDEF);
                literals[lit].mark |= 4;
            }

            if (literals[-lit].mark) {
                literals[lit].mark = (literals[lit].mark & 4) | 2;
                literals[-lit].mark = (literals[-lit].mark & 4) | 2;
            }

            if (original_cnf_file_name && abs (lit) > original_variables)
                return check_error ("literal %d too large", lit);

            push_stack (lit);
        }
    }

    /* Shrink stack by literals in both phases, keep those occurring only in
     * one phase.
     */

    q = stack;
    for (p = stack; p < stack + count_stack; p++) {
        lit = *p;
        if (literals[lit].mark & 1) {
            *q++ = lit;
            push_labels(literals[lit].label);
        }
        if (literals[lit].mark & 4)
            num_merge_literals++;

        // if lit appears more than once with mark == 1 in stack before this
        // loop: as mark is reset, rest is ignored

        literals[lit].mark = 0;
        literals[lit].label = UNDEF;
    }

    count_stack = q - stack;

    if (!count_stack)
        num_empty_clauses++;

    clause->literals = copy_stack ();
    clause->labels = copy_labels ();

    return 1;
}

static int
split_clauses_count (Clause * clause)
{
    if (!clause->antecedents)
        return 1;

    if (clause->split && clause->idx <= max_cls_idx)
        num_split_clauses++;

    return 1;
}

static int
merge_literals_count (Clause * clause)
{
    int idx, * p, * q, lit;
    Clause * antecedent;

    // Note: Collect literals for all internal vertices, no matter if literal list
    // supplied or not
    if (!clause->antecedents/*clause->literals*/)
        return 1;

    assert (clause->antecedents);		/* should be checked by parser */

    for (p = clause->antecedents; (idx = *p); p++) {
        antecedent = idx2clause (idx);

        assert (antecedent);
        assert (antecedent->literals);

        for (q = antecedent->literals; (lit = *q); q++) {
            // Note: label for pivots doesn't matter at this point
            // It is determined in resolve_and_split later.
            // We only compute labels for literals in resolvent
            // marks encoded in LSBs: xyz:
            //   x ... merge literal
            //   y ... appears in both phases
            //   z ... appears in one phase
            // y and z are mutually exclusive
            // x is only set if y or z are set
            if (literals[lit].mark & 1)
                literals[lit].mark |= 4;
            else if (literals[lit].mark == 0)
                literals[lit].mark = 1;
            else if (literals[lit].mark & 2)
                literals[lit].mark |= 4;

            if (literals[-lit].mark) {
                literals[lit].mark = (literals[lit].mark & 4) | 2;
                literals[-lit].mark = (literals[-lit].mark & 4) | 2;
            }

            push_stack (lit);
        }
    }

    /* Shrink stack by literals in both phases, keep those occurring only in
     * one phase.
     */
    for (p = stack; p < stack + count_stack; p++) {
        lit = *p;
        if (literals[lit].mark & 4)
            num_merge_literals_after++;

        // if lit appears more than once with mark == 1 in stack before this loop:
        // as mark is reset, rest is ignored

        literals[lit].mark = 0;
        literals[lit].label = UNDEF;
    }
    count_stack = 0;

    return 1;
}


/*------------------------------------------------------------------------*/
/* Return the first literal that occurs already negated in the resolvent.
 * Also set the label of the pivot.
 */
static int
pivot (Clause * clause, Clause * context)
{
    int *p, idx, pos;
    int merge;

    for (p = clause->literals; (idx = *p); p++) {
        pos = literals[-idx].mark;
        if (pos > 0) {
            // Compute merge of literal labels for the pivot
            merge = clause->labels[p - clause->literals] | literals[-idx].label;
            literals[-idx].label = merge;
            return idx;
        }
    }

    return check_error (
               "clause %d has no pivot in deriviation of clause %d",
               clause->idx, context->idx);
}

/*------------------------------------------------------------------------*/

static void
remove_literal_from_resolvent (int idx)
{
    int pos, last;

    pos = literals[idx].mark;

    assert (pos > 0);
    assert (pos <= count_resolvent);
    assert (resolvent[pos - 1] == idx);

    literals[idx].mark = 0;
    last = pop_resolvent ();

    if (last == idx)
        return;

    assert (count_resolvent >= 1);

    resolvent[pos - 1] = last;
    assert (literals[last].mark == count_resolvent + 1);
    literals[last].mark = pos;
}

/*------------------------------------------------------------------------*/

static void
add_literal_to_resolvent (int idx, short label)
{
    Literal *lit;

    lit = literals + idx;
    assert (!lit->mark);
    // intermediate clause index
    // used in remove_literal_from_resolvent
    lit->mark = count_resolvent + 1;
    lit->label = label;
    push_resolvent (idx);
    assert (resolvent[lit->mark - 1] == idx);
}

/*------------------------------------------------------------------------*/

static void
add_to_resolvent_except (Clause * clause, int idx)
{
    int *p, other;

    for (p = clause->literals; (other = *p); p++) {
        /* Skip resolved literal
         */
        if (other == idx)
            continue;

        /* Note: skip literals already part of resolvent
                 or add them
           merge of lit here
         */
        if (literals[other].mark) {
            assert (literals[other].label != UNDEF);
            literals[other].label |= clause->labels[p - clause->literals];
        } else
            add_literal_to_resolvent (other, 
              clause->labels[p - clause->literals]);
    }
}

/*------------------------------------------------------------------------*/
#ifdef BOOLEFORCE_LOG
/*------------------------------------------------------------------------*/

static void
printnl_ints (int *a)
{
    int *p, idx;

    for (p = a; (idx = *p); p++)
        fprintf (output, "%d ", idx);

    fputs ("0\n", output);
}

/*------------------------------------------------------------------------*/

static void
print_resolvent (const char *type)
{
    fprintf (output, VPFX "%s resolvent: ", type);
    push_resolvent (0);
    printnl_ints (resolvent);
    (void) pop_resolvent ();
}

/*------------------------------------------------------------------------*/
#endif
/*------------------------------------------------------------------------*/

inline static int
resolve_clause (Clause * clause, Clause * context, short pivot_label)
{
    int idx;

    num_resolutions++;

    if (!(idx = pivot (clause, context)))
        return 0;

#ifdef BOOLEFORCE_LOG
    if (verbose > 1)
        LOG ("resolving clause %d on literal %d", clause->idx, idx);
#endif

    // check whether chain is contiguously labeled.
    if (pivot_label != UNDEF && pivot_label != literals[-idx].label)
        return idx;

    remove_literal_from_resolvent (-idx);
    add_to_resolvent_except (clause, idx);

    return idx;
}

/*------------------------------------------------------------------------*/

static int
subsumes (int *a, int *b)
{
    int *p, idx, count;

    for (p = a; (idx = *p); p++)
        literals[idx].mark = 1;

    count = p - a;

    for (p = b; (idx = *p); p++) {
        if (!literals[idx].mark)
            continue;

        assert (count > 0);
        count--;
    }

    assert (count >= 0);

    mark_literals (a, 0);

    return !count;
}

/*------------------------------------------------------------------------*/

static void
write_res_header (void)
{
    char line[40];
    assert (restrace);
    assert (original_cnf_file_name);
    sprintf (line, "%%RESL32 %d %d", original_variables, original_clauses);
    fprintf (restrace, "%-255s\n", line);
    fflush (restrace);
}

/*------------------------------------------------------------------------*/

static void
write_res_line (int count, int literal,
                int op1, int op2, int len, int * literals)
{
    int buffer[5];

    assert (restrace);

    assert (op1 > 0);
    assert (op2 > 0);
    assert (count > op1);
    assert (count > op2);

    buffer[0] = count;
    buffer[1] = literal;
    buffer[2] = op1;
    buffer[3] = op2;
    buffer[4] = len;

    fwrite (buffer, sizeof (int), 5, restrace);
    fwrite (literals, sizeof (int), len, restrace);
    fwrite (buffer + 4, sizeof (int), 1, restrace);
}

/*------------------------------------------------------------------------*/

static void
write_rpt_header (void)
{
    char line[40];
    assert (rpttrace);
    assert (original_cnf_file_name);
    sprintf (line, "%%RPTL32 %d %d", original_variables, original_clauses);
    fprintf (rpttrace, "%-255s\n", line);
    fflush (rpttrace);
}

/*------------------------------------------------------------------------*/

static void
write_rpt_line (int count, int literal, int op1, int op2)
{
    int buffer[4];

    assert (rpttrace);

    assert (op1 > 0);
    assert (op2 > 0);
    assert (count > op1);
    assert (count > op2);

    buffer[0] = count;
    buffer[1] = literal;
    buffer[2] = op1;
    buffer[3] = op2;

    fwrite (buffer, sizeof (int), 4, rpttrace);
}

static void
write_dotgraph_header (void)
{
    // TODO sanitize input_name and use it
    fprintf (dotgraph, "digraph resproof {\n");
}

static void
write_dotgraph_footer (void)
{
    fprintf (dotgraph, "}");
}

/*------------------------------------------------------------------------*/
#ifndef NDEBUG
/*------------------------------------------------------------------------*/

static void
check_prestine_clause (Clause * clause)
{
    Literal *lit;
    int *p, idx;

    if (!clause->literals)
        return;

    assert (clause->mark == 0);

    for (p = clause->literals; (idx = *p); p++) {
        lit = literals + idx;

        assert (!lit->mark);
        assert (!lit->clauses);
    }
}

/*------------------------------------------------------------------------*/

static void
check_prestine_chain (Clause * clause)
{
    int *p, idx;

    if (!debug)
        return;

    check_prestine_clause (clause);
    if (!clause->antecedents)
        return;

    for (p = clause->antecedents; (idx = *p); p++)
        check_prestine_clause (idx2clause (idx));
}

/*------------------------------------------------------------------------*/
#endif
/*------------------------------------------------------------------------*/
/* This function assumes that the antecedents of a clauses can be resolved
 * with input resolution in increasing order to obtain the clause.  This
 * restricts checkable traces to piecewise regular input resolutions, e.g.
 * every derived clause is obtained by input resolution. This is also called
 * 'trivial resolution' in [1].  Note, that 'linerarization' should generate
 * the right order.
 *
 * TODO: if requested by the user, the resolutions are recorded as new
 * clauses, such they can be dumped afterwards.
 */
static int
resolve_and_split (Clause ** clause)
{
    int *p, idx, iterations, len, count, prev, i, lit;
    short pivot_label;
    Clause *antecedent;
#ifndef NDEBUG
    int *q, idx2;
#endif

    pivot_label = UNDEF;
    if (!(*clause)->antecedents)
        goto POST_PROCESS_RESOLVED_CHAIN;

#ifdef BOOLEFORCE_LOG
    if (verbose > 1)
        LOG ("resolving antecedents of clause %d", (*clause)->idx);
#endif

    assert (!count_resolvent);
    assert (!count_stack);

    assert ((*clause)->antecedents);
    p = (*clause)->antecedents;
    idx = *p++;

    if (!idx)
        return check_error ("clause %d at line %d has no antecedents",
                            (*clause)->idx, (*clause)->lineno);

#if 0
    if (!*p)
        return check_error ("clause %d at line %d has only one antecedent",
                            clause->idx, clause->lineno);
#endif

    antecedent = idx2clause (idx);
    push_stack (idx);
    assert (antecedent);
    assert (antecedent->resolved);	/* check topological order */

    /* Copy all literals of the first clause to the resolvent.  This will be
     * the 'initial resolvent'.  The second argument '0' is a non valid
     * literal.  Thus all literals are added.
     */
    add_to_resolvent_except (antecedent, 0);

    len = length_ints ((*clause)->antecedents);
    count = (*clause)->newidx + 2 - len;
    prev = antecedent->newidx;

#ifdef BOOLEFORCE_LOG
    if (verbose > 1)
        LOG ("copying antecedent clause %d as initial resolvent",
             antecedent->idx);
#endif
    if (dotgraph)
        fprintf (dotgraph, "n_%d -> n_%d;\n", antecedent->idx, (*clause)->idx);

    /* Further try to resolve in the given order the remaining clauses with
     * the current resolvent to obtain the next intermediate resolvent.
     */
    iterations = 0;
    while ((idx = *p++)) {
#ifdef BOOLEFORCE_LOG
        if (verbose > 1)
            print_resolvent (iterations ? "next" : "initial");
#endif

        iterations++;

        antecedent = idx2clause (idx);
        assert (antecedent);
        assert (antecedent->resolved);

        if (!(lit = resolve_clause (antecedent, (*clause), pivot_label)))
            return 0;

        // check whether chain is contiguously labeled.
        // Note: careful with lit/idx
        // literals[-lit].label is the merged label of the pivot literals
        if (pivot_label != UNDEF && pivot_label != literals[-lit].label) {
            // NEW SPLIT
            // TODO refactor into new function
            num_splits++;
            (*clause)->split = 1;

            for (i = 0; i < count_resolvent; i++)
                push_labels(literals[resolvent[i]].label);

            // create intermediate clause
            int *tmp_res = copy_ints(resolvent, count_resolvent);
            int *tmp_ants = copy_stack();
            int new_idx = max_cls_idx + 1;
            Clause *intermediate = add_clause(new_idx, tmp_res, tmp_ants, 0);
            intermediate->labels = copy_labels();

            // UPDATE
            // remove antecedents (indices from 0 to (iterations-1))
            // add new resolvent as antecedent at index (iterations-1)
            // in order not to leak memory
            // copy relevant memory region and free previously used memory
            (*clause)->antecedents[iterations - 1] = new_idx;
            int *tmp = copy_ints((*clause)->antecedents + (iterations - 1),
                                 length_ints((*clause)->antecedents) - (iterations - 1));
            booleforce_delete_ints ((*clause)->antecedents);
            (*clause)->antecedents = tmp;

            intermediate->next_in_order = *clause;
            if(*clause == first_in_order)
                first_in_order = intermediate;
#ifndef NDEBUG
            (*clause)->resolved = 1;
#endif
            *clause = intermediate;
            // Note: Reset not necessary because label is set from clause label anyhow
            // before merge
            literals[-lit].label = UNDEF;
            break;
        }
        // else

        // Note: lagging one iteration behind so that antecedent causing split is
        // not included
        push_stack(idx);

        pivot_label = literals[-lit].label;

#ifndef NDEBUG
        for (q = (*clause)->literals; (idx2 = *q); q++) {
            assert (idx2 != lit && idx2 != -lit);
        }
#endif

        if (bintrace)
            fprintf (bintrace, "%d * %d %d 0\n", count, prev, antecedent->newidx);

        if (ebintrace) {
            fprintf (ebintrace, "%d ", count);
            for (i = 0; i < count_resolvent; i++)
                fprintf (ebintrace, "%d ", resolvent[i]);
            fprintf (ebintrace, "0 %d %d 0\n", prev, antecedent->newidx);
        }

        if (rpttrace)
            write_rpt_line (count, lit, prev, antecedent->newidx);

        if (restrace)
            write_res_line (count, lit,
                            prev, antecedent->newidx,
                            count_resolvent, resolvent);

        if (dotgraph)
            fprintf (dotgraph, "n_%d -> n_%d;\n", antecedent->idx, (*clause)->idx);

        prev = count++;
    }

#ifdef BOOLEFORCE_LOG
    if (verbose > 1)
        print_resolvent ("last");
#endif

    if (count_resolvent == 0)
        empty_cls_idx = (*clause)->idx;

    /* The final resolvent should be equal to the clause.  We check it by two
     * subsume tests, which, if one of them fails, gives a little bit more
     * information to the user.
     */
    push_resolvent (0);		/* sentinel */

    for (p = resolvent; (idx = *p); p++) {
        literals[idx].mark = 0;
        // Note: Reset not necessary because label is set from clause label anyhow
        // before merge
        literals[idx].label = UNDEF;
    }

    if (!subsumes (resolvent, (*clause)->literals))
        return check_error ("resolvent does not subsume clause %d at line %d",
                            (*clause)->idx, (*clause)->lineno);

    if (!subsumes ((*clause)->literals, resolvent))
        return check_error ("clause %d at line %d does not subsume resolvent",
                            (*clause)->idx, (*clause)->lineno);

    count_resolvent = 0;

    // Note: delete the stack if we haven't used it to copy the antecedents from
    // it
    // TODO: enough to free memory of stack or leaking here?
    count_stack = 0;

    // Note: would jump over this otherwise anyhow
    // if((*clause)->antecedents)
    (*clause)->partition = pivot_label;
    assert ((*clause)->partition != UNDEF);

POST_PROCESS_RESOLVED_CHAIN:

#ifdef BOOLEFORCE_LOG
    if (verbose > 1) {
        fprintf (output, VPFX "checked clause %d: ", (*clause)->idx);
        printnl_ints ((*clause)->literals);
    }
#endif

    if (!(*clause)->antecedents) {
        if (bintrace)
            print_clause (*clause, 0, bintrace);

        if (ebintrace)
            print_clause (*clause, 1, ebintrace);
    }

    if (dotgraph)
        fprintf (dotgraph, "n_%d [label=\"%d\"];\n", (*clause)->idx,
                 (*clause)->partition);

#ifndef NDEBUG
    (*clause)->resolved = 1;
#endif

    return 1;
}

static void
mark_resolvent (Clause * clause)
{
    int *p, idx;

    for (p = clause->literals; (idx = *p); p++) {
        assert (!literals[idx].mark);
        literals[idx].mark = 1;
    }
}

static void
unmark_resolvent (Clause * clause)
{
    int *p, idx;

    for (p = clause->literals; (idx = *p); p++) {
        assert (literals[idx].mark);
        literals[idx].mark = 0;
    }
}

static int
rnd_up_pow2 (int v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}


static simpaig *
compute_m (Clause * clause)
{
    int *p, idx, len, input_count, iterations;
    simpaig **tmp, *ret_val;

    len = length_ints (clause->literals);
    // TODO use realloc and maybe booleforce mem managment
    tmp = malloc(len * sizeof (circuit_components_m[0]));
    if(tmp != NULL) {
        circuit_components_m = tmp;
        // TODO len isn't the actual number of components
        //      iterations after loops is
        //    count_circuit_components_m = len;
    }

    iterations = 0;
    for (p = clause->literals; (idx = *p); p++) {
        if (!literals[idx].mark) {
            circuit_components_m[iterations++] = literals[idx].aig;

            // Set mark to 2, in order not to add lits twice
            literals[idx].mark = 2;
        }
    }
    // Reset the literals just marked, so not to bleed into the context
    for (p = clause->literals; (idx = *p); p++) {
        if(literals[idx].mark == 2)
            literals[idx].mark = 0;
    }

    input_count = rnd_up_pow2 (iterations);
    count_circuit_components_m = iterations;
    gates += (iterations - 1);
    ret_val = build_circuit_rec_m (input_count, simpaig_or, simpaig_false (mgr));
    assert (count_circuit_components_m == 0);
    free (circuit_components_m);
    return ret_val;
}

/*------------------------------------------------------------------------*/
/* expand, copyaig, next_symbol taken from aigunroll.c to copy simpaig data
 * structure into aiger data structure, which allows to print the AIG.
 */
static const char *
next_symbol (unsigned idx)
{
    unsigned len;

    len = 50;

    if (size_outbuffer < len) {
        if (size_outbuffer) {
            while (size_outbuffer < len)
                size_outbuffer *= 2;

            outbuffer = realloc (outbuffer, size_outbuffer);
        } else
            outbuffer = malloc (size_outbuffer = len);
    }

    sprintf (outbuffer, "x%u", idx);

    return outbuffer;
}

static void
copyaig (simpaig * aig)
{
    Literal *aig_input;
    simpaig *c0, *c1;
    const char *name;
    unsigned idx;

    assert (aig);

    aig = simpaig_strip (aig);
    idx = simpaig_index (aig);
    if (!idx || aigs[idx])
        return;

    aigs[idx] = aig;
    if (simpaig_isand (aig)) {
        ands++;
        c0 = simpaig_child (aig, 0);
        c1 = simpaig_child (aig, 1);
        copyaig (c0);
        copyaig (c1);
        aiger_add_and (output_aig,
                       2 * idx, // lhs
                       simpaig_unsigned_index (c0), // rhs1
                       simpaig_unsigned_index (c1)); // rhs2
    } else {
        name = 0;
        aig_input = simpaig_isvar (aig);
        name = next_symbol (aig_input->idx);
        aiger_add_input (output_aig, 2 * idx, name);
    }
}

static void
expand (simpaig * aig)
{
    unsigned maxvar;
    simpaig_assign_indices (mgr, aig);
    maxvar = simpaig_max_index (mgr);
    aigs = calloc (maxvar + 1, sizeof aigs[0]);
    copyaig (aig);
    aiger_add_output (output_aig, simpaig_unsigned_index (aig), 0);
    free (aigs);
    simpaig_reset_indices (mgr);
    free (outbuffer);
    size_outbuffer = 0;
}


#ifndef NDEBUG

static simpaig *
compute_cl (Clause * clause)
{
    int *p, idx;
    simpaig * cl, * tmp;

    cl = simpaig_false (mgr);

    for (p = clause->literals; (idx = *p); p++) {
        tmp = simpaig_or (mgr, cl, literals[idx].aig);
        cl = tmp;
    }

    return cl;
}

static int
compute_partition_aigs (Clause * clause)
{
    simpaig * cl, * tmp;

    if (clause->antecedents)
        return 1;

    cl = compute_cl (clause);
    if (clause->partition == A) {
        tmp = simpaig_and (mgr, partition_a, cl);
        partition_a = tmp;
    } else if (clause->partition == B) {
        tmp = simpaig_and (mgr, partition_b, cl);
        partition_b = tmp;
    } else
        return check_error("Initial vertices either in A or B");

    return 1;
}

static simpaig*
compute_upward_projection (Clause * clause, short label)
{
    int *p, idx;
    simpaig * cl, * tmp;

    cl = simpaig_false (mgr);

    for (p = clause->literals; (idx = *p); p++) {
        if ((label & literals[idx].partition) != 0) {
            tmp = simpaig_or (mgr, cl, literals[idx].aig);
            cl = tmp;
        }
    }
    return cl;
}

static int
check_interpolation_invariant (simpaig *invariant)
{
    int sat_result;
    FILE *file;

    output_aig = aiger_init ();
    expand (invariant);
    file = booleforce_open_file_for_writing (SANITY_AIG);
    aiger_write_to_file (output_aig, aiger_binary_mode, file);
    booleforce_close_file (file);
    aiger_reset (output_aig);
    // TODO paths / system()
    system(AIGTOCNF_PATH " " SANITY_AIG " > " SANITY_CNF);
    sat_result = system(PICOSAT_PATH " -n " SANITY_CNF " > /dev/null");
    // Note: return code 10 == sat, 20 == unsat
    if(sat_result >> 8 != 20)
        return 0;
    return 1;
}

#endif


static int
compute_itp (Clause * clause)
{
    int *p, idx, iterations, len, input_count;
    Clause *antecedent;
    simpaig *m, **tmp;

    if (!clause->antecedents)
        return 1;

    len = length_ints(clause->antecedents);

    // TODO use realloc and maybe booleforce mem managment
    tmp = malloc(len * sizeof (circuit_components_itp[0]));
    if(tmp != NULL) {
        circuit_components_itp = tmp;
        count_circuit_components_itp = len;
    }

    assert (clause->antecedents);
    p = clause->antecedents;

    iterations = 0;
    while ((idx = *p++)) {
        antecedent = idx2clause (idx);
        assert (antecedent);
        assert (antecedent->itp);
        if (clause->partition == AB) {
            mark_resolvent (clause);
            m = compute_m (antecedent);
            // conjunction of I_i \vee M_i
            if(itp_system_strength == 1) {
                circuit_components_itp[iterations] = simpaig_or (mgr, antecedent->itp, m);
                gates++;
            } else {
                circuit_components_itp[iterations] = simpaig_and(mgr, antecedent->itp, simpaig_not(m));
                gates++;
            }
            unmark_resolvent (clause);
        } else if (clause->partition == A)
            circuit_components_itp[iterations] = antecedent->itp;
        else if (clause->partition == B)
            circuit_components_itp[iterations] = antecedent->itp;
        else
            return check_error ("chain pivots have to be labelled A, B, or AB");

        iterations++;
    }
    input_count = rnd_up_pow2 (len);

    if (clause->partition == B ||
            (clause->partition == AB && itp_system_strength == 1))
        clause->itp = build_circuit_rec_itp (input_count, simpaig_and, simpaig_true (mgr));
    else if (clause->partition == A ||
             (clause->partition == AB && itp_system_strength == 2))
        clause->itp = build_circuit_rec_itp (input_count, simpaig_or, simpaig_false (mgr));
    else
        return check_error ("that itp system does not exist.");

    num_derived_clauses_after_split++;
    num_antecedents_after += iterations;
    gates += (len - 1);

    assert (count_circuit_components_itp == 0);
    free(circuit_components_itp);

#ifndef NDEBUG
    // Note: remove to check invariant for all chains,
    if(clause->idx != empty_cls_idx)
        return 1;

    simpaig *sanity_tmp, *invariant;

    sanity_tmp = simpaig_and(mgr, partition_a, simpaig_not(clause->itp));
    invariant = simpaig_and(mgr, sanity_tmp, simpaig_not(compute_upward_projection(clause, A)));
    if (!check_interpolation_invariant(invariant))
        return check_error("violated itp invariant in clause %d\n", clause->idx);
    simpaig_dec (mgr, sanity_tmp);
    simpaig_dec (mgr, invariant);

    sanity_tmp = simpaig_and(mgr, partition_b, clause->itp);
    invariant = simpaig_and(mgr, sanity_tmp, simpaig_not(compute_upward_projection(clause, B)));
    if (!check_interpolation_invariant(invariant))
        return check_error("violated itp invariant in clause %d\n", clause->idx);
    simpaig_dec (mgr, sanity_tmp);
    simpaig_dec (mgr, invariant);

    printf("OKAY: partial interpolant @ clause %d\n", clause->idx);

#endif


    return 1;
}

/*------------------------------------------------------------------------*/

static int
copy_resolvent_in_reverse_order_as_antecedents (Clause * clause)
{
    int *p, l;

    l = length_ints (clause->antecedents);
    assert (count_resolvent <= l);

    if (count_resolvent < l) {
        if (lax) {
            booleforce_delete_ints (clause->antecedents);
            BOOLEFORCE_NEW_ARRAY (clause->antecedents, count_resolvent + 1);
            clause->antecedents[count_resolvent] = 0;
        } else
            return check_error ("%d antecedents in clause %d can not be resolved",
                                l - count_resolvent, clause->idx);
    }

    p = clause->antecedents;
    while (count_resolvent)
        *p++ = pop_resolvent ();

    return 1;
}

/*------------------------------------------------------------------------*/

inline static int
deref (int idx)
{
    return literals[idx].mark;
}

/*------------------------------------------------------------------------*/

inline static void
enqueue (int idx, int reason)
{
    assert (deref (idx) == UNKNOWN);
    push_stack (idx);
    push_stack (reason);

#ifdef BOOLEFORCE_LOG
    if (verbose > 1)
        LOG ("enqueue %d with reason %d", idx, reason);
#endif
}

/*------------------------------------------------------------------------*/

inline static void
assign (int idx, int reason)
{
    (void) reason;

    assert (deref (idx) == UNKNOWN);

    literals[idx].mark = TRUE;
    literals[-idx].mark = FALSE;

    push_trail (idx);

#ifdef BOOLEFORCE_LOG
    if (verbose > 1)
        LOG ("assigned %d with reason %d", idx, reason);
#endif
}

/*------------------------------------------------------------------------*/

static void
untrail (void)
{
    int idx;

    while (count_trail) {
        idx = pop_trail ();
        literals[idx].mark = UNKNOWN;
        literals[-idx].mark = UNKNOWN;
    }
}

/*------------------------------------------------------------------------*/
/* Visit a clause in which one watched literal became false.  Return a non
 * zero value iff the watched literal should not be watched anymore.
 */
inline static int
visit (Clause * clause)
{
    int *p, *q, false_literal_pos, idx0, idx1, tmp0, tmp1, idx, label, tmp;

    p = clause->literals;

    idx0 = p[0];
    assert (idx0);

    idx1 = p[1];
    assert (idx1);

    tmp0 = deref (idx0);
    tmp1 = deref (idx1);

    if (tmp0 == TRUE || tmp1 == TRUE)	/* ignore satisfied clauses */
        return 0;			/* keep old watch */

    if (tmp0 == FALSE && tmp1 == FALSE)	/* ignore conflicts */
        return 0;			/* keep old watch */

    /* exactly one watched literal is unknown and the other false
     */
    assert (tmp0 == UNKNOWN || tmp1 == UNKNOWN);
    assert (tmp0 == FALSE || tmp1 == FALSE);

    q = p + 2;

    false_literal_pos = (tmp1 == FALSE);

    while ((idx = *q)) {
        tmp = deref (idx);
        if (tmp != FALSE) {
            label = clause->labels[q - clause->literals];

            *q = p[false_literal_pos];
            p[false_literal_pos] = idx;

            clause->labels[q - clause->literals] = clause->labels[false_literal_pos];
            clause->labels[false_literal_pos] = label;

            CONS (literals[idx].clauses, clause);

            return 1;		/* remove old watch */
        }

        q++;
    }

    enqueue (p[!false_literal_pos], clause->idx);

    return 0;			/* keep old watch */
}

/*------------------------------------------------------------------------*/

static void
bcp (int idx)
{
    Cell *this, **prev, *next;
    Clause *clause;
    Literal *lit;

    lit = literals + idx;
    prev = &lit->clauses;

    for (this = *prev; this; this = next) {
        next = cdr (this);
        clause = car (this);

        if (visit (clause)) {
            *prev = next;		/* dequeue this watch */
            recycle_cell (this);
        } else
            prev = &this->tail;	/* keep old watch */
    }
}

/*------------------------------------------------------------------------*/
/* We need to reorder the antecedents in such a way that they allow a
 * trivial resolution, which is a regular input resolution with the
 * antecedents of the chain resolution.  We call this 'linearization',
 * though a linear resolution proof is a slight generalization of an input
 * resolution proof (one of the antecedents is a input clause as in input
 * resolution but the other can be any previously derived resolvent and not
 * just the last resolvent).  If you set 'assume_already_linearized', then
 * this reordering is skipped.  Therefore the checker needs to assume that
 * the antecedents are already 'linearized'.
 *
 * Originally we implemented a complicated reverse resolution scheme, which
 * starts with the resolvent of the chain and 'unresolves' candidate
 * antecedents with distance one, producing the intermediate resolvents in
 * reverse order and at the same time reorders the antecedents.  It turns
 * out that this took 3/4 of the total running time and was rather complex
 * to implement.
 *
 * The new implementation is based on boolean constraint propagation as in a
 * SAT solver itself using a two literal watched scheme as described by van
 * Gelder and implemented in 'compsat', 'minisat', and 'booleforce', where
 * the first two literals of a clause are the watched literals.  As soon a
 * literal is assigned the reason, which is one of the antecedents, for the
 * assignment, is ordered next in the new antecedent order (actually in
 * reverse order).
 */
static int
linearize (Clause * clause)
{
    int *p, antecedent_idx, idx, idx0, idx1, reason, tmp;
    Clause *antecedent;

    if (!clause->antecedents)
        return 1;

#ifdef BOOLEFORCE_LOG
    if (verbose > 1)
        LOG ("linearizing antecedents of clause %d", clause->idx);
#endif

    assert (clause->antecedents);
    if (!clause->antecedents[0] ||
            !clause->antecedents[1] || !clause->antecedents[2]) {
#ifdef BOOLEFORCE_LOG
        if (verbose > 1)
            LOG ("clause %d has not more than two antecedents", clause->idx);
#endif

        return 1;
    }

    assert (!count_stack);

    /* Watch the first two literals in all antecedents and enqueue units.
     */
    for (p = clause->antecedents; (antecedent_idx = *p); p++) {
        antecedent = idx2clause (antecedent_idx);

        idx0 = antecedent->literals[0];
        assert (idx0);

        idx1 = antecedent->literals[1];
        if (idx1) {
            CONS (literals[idx0].clauses, antecedent);
            CONS (literals[idx1].clauses, antecedent);
        } else
            enqueue (idx0, antecedent_idx);	/* unit antecedent */
    }

    /* Enqueue the negation of the resolvent of this chain.
     */
    for (p = clause->literals; (idx = *p); p++)
        enqueue (-idx, 0);

    /* bcp until conflict is found
     */
    conflict_occurred = 0;
    assert (!count_resolvent);
    while (count_stack && !conflict_occurred) {
        reason = pop_stack ();	/* DFS BCP */
        idx = pop_stack ();
        tmp = deref (idx);
        if (tmp == FALSE) {
            count_stack = 0;
        } else if (tmp == UNKNOWN) {
            assign (idx, reason);
            bcp (-idx);
        } else
            reason = 0;

        if (!reason)
            continue;

        push_resolvent (reason);
#ifdef BOOLEFORCE_LOG
        if (verbose > 1)
            LOG ("new antecedent %d", reason);
#endif
    }

    untrail ();
    count_stack = 0;

    /* Recycle Cells.
     */
    for (p = clause->antecedents; (antecedent_idx = *p); p++) {
        antecedent = idx2clause (antecedent_idx);

        idx0 = antecedent->literals[0];
        assert (idx0);

        idx1 = antecedent->literals[1];
        if (!idx1)
            continue;

        RECYCLE_CELLS (literals[idx0].clauses);
        RECYCLE_CELLS (literals[idx1].clauses);
    }

    if (!copy_resolvent_in_reverse_order_as_antecedents (clause))
        return 0;

    return 1;
}

/*------------------------------------------------------------------------*/
/* Traverse all collected clauses and apply the 'checker' to theses clauses.
 * If one clause does not pass the check abort the traversal assuming that
 * the checker itself already reported an error message.  On success the
 * verbose message is printed
 */
static int
forall_clauses (int (*checker) (Clause *), const char *verbose_msg)
{
    double start_time = booleforce_time_stamp ();
    Clause *clause;

    for (clause = first_in_order; clause; clause = clause->next_in_order) {
#ifndef NDEBUG
        check_prestine_chain (clause);
#endif

        if (!checker (clause))
            return 0;
#ifndef NDEBUG
        check_prestine_chain (clause);
#endif
    }

    if (verbose)
        booleforce_report (start_time, output, VPFX "%s", verbose_msg);

    return 1;
}

/*------------------------------------------------------------------------*/
/* Traverse all collected clauses and apply the 'checker' to theses clauses.
 * If one clause does not pass the check abort the traversal assuming that
 * the checker itself already reported an error message.  On success the
 * verbose message is printed
 */
static int
forall_clauses_mutable (int (*checker) (Clause **), const char *verbose_msg)
{
    double start_time = booleforce_time_stamp ();
    Clause **clause;

    for (clause = &first_in_order; *clause; clause = &(*clause)->next_in_order) {
#ifndef NDEBUG
        check_prestine_chain (*clause);
#endif

        if (!checker (clause))
            return 0;
#ifndef NDEBUG
        check_prestine_chain (*clause);
#endif
    }

    if (verbose)
        booleforce_report (start_time, output, VPFX "%s", verbose_msg);

    return 1;
}

/*------------------------------------------------------------------------*/

static void
generate_new_indices (void)
{
    double start_time = booleforce_time_stamp ();
    int newidx, count;
    Clause * clause;

    newidx = min_derived_idx;

    if (max_original_idx > newidx)
        newidx = max_original_idx + 1;

    for (clause = first_in_order; clause; clause = clause->next_in_order) {
        if (clause->antecedents) {
            count = length_ints (clause->antecedents);
            assert (count > 0);
            newidx += count - 1;
            clause->newidx = newidx - 1;
        } else
            clause->newidx = clause->idx;
    }

    if (verbose)
        booleforce_report (start_time, output,
                           VPFX "mapped to %d new clause indices", newidx);
}

/*------------------------------------------------------------------------*/

static int
check (void)
{
    if (!link_derived_clauses ())
        return 0;

    find_roots ();

    if (!collect ())
        return 0;

    if (!order ())
        return 0;

    // TODO use booleforce memory wrapper for aigs, to track mem usage
    mgr = simpaig_init ();

    init_literals ();

    forall_clauses (collect_literals, "literal collection");

    init_aigs ();

    if (!forall_clauses (normalize, "normalization"))
        return 0;

    init_cells ();

    if (assume_already_linearized) {
        if (verbose)
            LOG ("skipping linearization");
    } else if (!forall_clauses (linearize, "linearization"))
        return 0;

    if (bintrace || ebintrace || restrace || rpttrace)
        generate_new_indices ();

    if (restrace)
        write_res_header ();

    if (rpttrace)
        write_rpt_header ();

    if (dotgraph)
        write_dotgraph_header ();

    num_derived_clauses_before_split = num_derived_clauses;
    num_antecedents_before_split = num_antecedents;
    init_max_cls_idx = max_cls_idx;
    if (!forall_clauses_mutable (resolve_and_split, "resolution"))
        return 0;

#ifndef NDEBUG
    partition_a = simpaig_true (mgr);
    partition_b = simpaig_true (mgr);
    forall_clauses (compute_partition_aigs, "partition aigs");
#endif

    if (interpolant) {
        if (!forall_clauses (compute_itp, "interpolation"))
            return 0;
    }

    if (!forall_clauses (merge_literals_count, "merge_literals_count"))
        return 0;

    if (!forall_clauses (split_clauses_count, "split_clauses_count"))
        return 0;



    return 1;
}

/*------------------------------------------------------------------------*/

static void
print_stats (void)
{
    float mem_usage;
    double avg, seconds;
    avg = 0.0f;
    fprintf (stats_file, "%s;%d;%d", input_name, partition_split, itp_system_strength);

    avg = AVG(num_antecedents_before_split, num_derived_clauses_before_split);
    fprintf (stats_file, ";%d;%.2f;%d", num_derived_clauses_before_split, avg, num_merge_literals);

    fprintf (stats_file, ";%d;%d", num_splits, num_split_clauses);

    avg = AVG(num_antecedents_after, num_derived_clauses_after_split);
    fprintf (stats_file, ";%d;%.2f;%d;%d;%d", num_derived_clauses_after_split, avg, num_merge_literals_after, gates, ands);

    seconds = booleforce_time_stamp () - entered_time;
    mem_usage = (float) booleforce_max_bytes() / (double) (1 << 20);
    fprintf (stats_file, ";%.2f;%.2f\n", seconds, mem_usage);
}

/*------------------------------------------------------------------------*/

static void
release_literals (void)
{
    int i;

    for (i = -max_lit_idx; i <= max_lit_idx; i++) {
        // TODO simpaig_dec final_itp should take care of this
        //if (i > 0)
        //simpaig_dec (mgr, literals[i].aig); // dec variables

        RECYCLE_CELLS (literals[i].clauses);
    }

    literals -= max_lit_idx;
    BOOLEFORCE_DELETE_ARRAY (literals, 2 * max_lit_idx + 1);
}

/*------------------------------------------------------------------------*/

static void
release_clauses (void)
{
    Clause *clause;
    int i;

    for (i = 1; i < size_clauses; i++)
        if ((clause = clauses[i]))
            delete_clause (clause);

    BOOLEFORCE_DELETE_ARRAY (clauses, size_clauses);
}

/*------------------------------------------------------------------------*/
/* release all heap allocated memory
 */
static void
release (void)
{
    release_trail ();
    release_stack ();
    release_roots ();
    release_resolvent ();
    release_labels ();

    if (cells)
        BOOLEFORCE_DELETE_ARRAY (cells, size_cells);

    release_clauses ();
}

/*------------------------------------------------------------------------*/
/* reset all static variables
 */
static void
reset (void)
{
    if (close_input) {
        booleforce_close_file (input);
        close_input = 0;
    }

    if (etrace) {
        booleforce_close_file (etrace);
        etrace = 0;
    }

    if (bintrace) {
        booleforce_close_file (bintrace);
        bintrace = 0;
    }

    if (ebintrace) {
        booleforce_close_file (ebintrace);
        ebintrace = 0;
    }

    if (rpttrace) {
        booleforce_close_file (rpttrace);
        rpttrace = 0;
    }

    if (restrace) {
        booleforce_close_file (restrace);
        restrace = 0;
    }

    if (dotgraph) {
        booleforce_close_file (dotgraph);
        dotgraph = 0;
    }

    if (interpolant) {
        booleforce_close_file (interpolant);
        interpolant = 0;
    }

    if (stats_file) {
        booleforce_close_file (stats_file);
        stats_file = 0;
    }

    booleforce_reset_mem ();

    if (close_output) {
        fclose (output);
        close_output = 0;
    }
}

/*------------------------------------------------------------------------*/

#define STATEMACRO(state) \
  do { memset (&state, 0, sizeof(state)); } while (0)

static void
clear_static_variables (void)
{
    assert (static_variables_are_dirty);
#include "tracecheck.states"
    assert (!static_variables_are_dirty);
}

/*------------------------------------------------------------------------*/

static void
init (void)
{
    double tmp = booleforce_time_stamp ();

    if (static_variables_are_dirty)
        clear_static_variables ();

    entered_time = tmp;

    output = stdout;
    assert (!close_output);

    static_variables_are_dirty = 1;
}

/*------------------------------------------------------------------------*/

static int
parse_header_of_original_dimacs_file (void)
{
    double start_time = booleforce_time_stamp ();
    FILE * file;
    int ch;

    assert (original_cnf_file_name);
    file = booleforce_open_file_for_reading (original_cnf_file_name);

    if (!file)
        return parse_error ("can not read '%s'", original_cnf_file_name);

SKIP:
    ch = getc (file);
    if (isspace (ch))
        goto SKIP;

    if (ch == 'c') {
        while ((ch = getc (file)) != '\n' && ch != EOF)
            ;

        goto SKIP;
    }

    if (ch != 'p' ||
            fscanf (file,
                    " cnf %d %d",
                    &original_variables,
                    &original_clauses) != 2 ||
            original_variables < 0 ||
            original_clauses < 0) {
        booleforce_close_file (file);
        return parse_error ("invalid header in '%s'", original_cnf_file_name);
    }

    booleforce_close_file (file);

    if (verbose)
        booleforce_report (start_time, output,
                           VPFX "found 'p cnf %d %d' in '%s'",
                           original_variables,
                           original_clauses,
                           original_cnf_file_name);
    return 1;
}

/*------------------------------------------------------------------------*/

#define USAGE \
  "usage: tracecheck [<option> ...][<input>]\n" \
"\n" \
"where <option> is one of the following:\n" \
"\n" \
"  -h           print this command line option summary\n" \
"  --version    print this command line option summary\n" \
"  --config     print configuration options\n" \
"  --id         print CVS/RCS id\n" \
"  -v[<inc>]    increase verbose level by <inc> (default 1)\n" \
"  --linear     assume that chains are already linearized\n" \
"  --lax        ignore multiple occurrences and left over antecedents\n" \
"  --print      parse input file, print trace and exit\n" \
"  --debug      enable internal consistency checking\n" \
"  -e<idx>      first defined variable index in extended resolution trace\n" \
"  -E <proof>   generate extended clausal trace\n" \
"  -b <proof>   generate compact binary resolution trace\n" \
"  -B <proof>   generate extended binary resolution trace\n" \
"  -r <proof>   generate compact binary resolution trace in RPT format\n" \
"  -R <proof>   generate extended binary resolution proof in RES format\n" \
"  -i <interpolant> <split> <strength>  generate Craig interpolant from hyper resolution proof\n" \
"  -s <stats>   generate statistics\n" \
"  -c <cnf>     specify original CNF for '-r' and '-R'\n" \
"  -o <output>  set output file (for verbose and error output)\n" \
"\n" \
"Verbose level of 1 just prints statistics, while verbose level of two\n" \
"and larger allows debugging of the input trace (and 'tracecheck').\n"

/*------------------------------------------------------------------------*/

static const char *
tracecheck_configuration (void)
{
    return
        "VERSION=\"" BOOLEFORCE_VERSION "\"\n"
        "OS=\"" BOOLEFORCE_OS "\"\n"
        "ID=\"$Id: tracecheck.c,v 1.117 2010-09-03 08:29:23 biere Exp $\"\n"
        "CC=\"" BOOLEFORCE_CC "\"\n"
        "CCVERSION=\"" BOOLEFORCE_CCVERSION "\"\n"
        "CFLAGS=\"" BOOLEFORCE_CFLAGS "\"\n"
#ifdef NDEBUG
        "NDEBUG=1\n"
#else
        "NDEBUG=0\n"
#endif
#ifdef BOOLEFORCE_LOG
        "LOG=1\n"
#else
        "LOG=0\n"
#endif
        ;
}

/*------------------------------------------------------------------------*/

static int
is_valid_number_str (const char * str)
{
    const char * p;
    char ch;

    for (p = str; (ch = *p); p++)
        if (!isdigit (ch))
            return 0;

    return 1;
}

/*------------------------------------------------------------------------*/

int
tracecheck_main (int argc, char **argv)
{
    int res, done, i, print_only;
    const char * arg;
    double seconds;
    FILE *file;

    init ();

    print_only = 0;
    done = 0;
    res = 0;

    for (i = 1; !done && !res && i < argc; i++) {
        if (!strcmp (argv[i], "-h")) {
            fprintf (output, USAGE);
            done = 1;
        } else if (!strcmp (argv[i], "--version")) {
            fprintf (output, "%s\n", BOOLEFORCE_VERSION);
            done = 1;
        } else if (!strcmp (argv[i], "--id")) {
            /**INDENT-OFF**/
            fprintf (output,
                     "$Id: tracecheck.c,v 1.117 2010-09-03 08:29:23 biere Exp $\n");
            done = 1;
            /**INDENT-ON**/
        } else if (!strcmp (argv[i], "--config")) {
            fprintf (output, "%s", tracecheck_configuration ());
            done = 1;
        } else if (argv[i][0] == '-' && argv[i][1] == 'v') {
            if (argv[i][2])
                verbose += atoi (argv[i] + 2);
            else
                verbose++;
        } else if (!strcmp (argv[i], "-i")) {
            if (++i == argc)
                res = option_error ("argument to '-i' missing");
            else if (interpolant)
                res = option_error ("multiple '-i' options");
            else {
                file = booleforce_open_file_for_writing (argv[i]);
                if (file)
                    interpolant = file;
                else
                    res = option_error ("can not write to '%s'", argv[i]);

                partition_split = atoi (argv[++i]);
                if(partition_split < 0)
                    res = option_error ("invalid (A,B) split");

                itp_system_strength = atoi (argv[++i]);
                if(itp_system_strength < 1 || itp_system_strength > 2)
                    res = option_error ("itp system strength must be 1 or 2");
            }
        } else if (!strcmp (argv[i], "-s")) {
            if (++i == argc)
                res = option_error ("argument to '-s' missing");
            else if (stats_file)
                res = option_error ("multiple '-s' options");
            else {
                file = booleforce_open_file_for_writing (argv[i]);
                if (file)
                    stats_file = file;
                else
                    res = option_error ("can not write to '%s'", argv[i]);
            }
        } else if (!strcmp (argv[i], "-E")) {
            if (++i == argc)
                res = option_error ("argument to '-E' missing");
            else if (etrace)
                res = option_error ("multiple '-E' options");
            else {
                file = booleforce_open_file_for_writing (argv[i]);
                if (file)
                    etrace = file;
                else
                    res = option_error ("can not write to '%s'", argv[i]);
            }
        } else if (!strcmp (argv[i], "-b")) {
            if (++i == argc)
                res = option_error ("argument to '-b' missing");
            else if (bintrace)
                res = option_error ("multiple '-b' options");
            else {
                file = booleforce_open_file_for_writing (argv[i]);
                if (file)
                    bintrace = file;
                else
                    res = option_error ("can not write to '%s'", argv[i]);
            }
        } else if (!strcmp (argv[i], "-B")) {
            if (++i == argc)
                res = option_error ("argument to '-B' missing");
            else if (ebintrace)
                res = option_error ("multiple '-B' options");
            else {
                file = booleforce_open_file_for_writing (argv[i]);
                if (file)
                    ebintrace = file;
                else
                    res = option_error ("can not write to '%s'", argv[i]);
            }
        } else if (!strcmp (argv[i], "-r")) {
            if (++i == argc)
                res = option_error ("argument to '-r' missing");
            else if (rpttrace)
                res = option_error ("multiple '-r' options");
            else {
                file = booleforce_open_file_for_writing (argv[i]);
                if (file)
                    rpttrace = file;
                else
                    res = option_error ("can not write to '%s'", argv[i]);
            }
        } else if (!strcmp (argv[i], "-R")) {
            if (++i == argc)
                res = option_error ("argument to '-R' missing");
            else if (restrace)
                res = option_error ("multiple '-R' options");
            else {
                file = booleforce_open_file_for_writing (argv[i]);
                if (file)
                    restrace = file;
                else
                    res = option_error ("can not write to '%s'", argv[i]);
            }
        } else if (!strcmp (argv[i], "-c")) {
            if (++i == argc)
                res = option_error ("argument to '-c' missing");
            else if (original_cnf_file_name)
                res = option_error ("multiple '-c' options");
            else
                original_cnf_file_name = argv[i];
        } else if (argv[i][0] == '-' && argv[i][1] == 'e') {
            arg = 0;

            if (argv[i][2])
                arg = argv[i] + 2;
            else if (++i == argc)
                res = option_error ("argument to '-e' missing");
            else
                arg = argv[i];

            if (arg) {
                if (is_valid_number_str (arg))
                    first_defined_lit_idx = atoi (arg);
                else
                    res = option_error (
                              "expected number as argument to '-e' but got '%s'", arg);
            }
        } else if (!strcmp (argv[i], "-d")) {
            if (++i == argc)
                res = option_error ("argument to '-d' missing");
            else if (dotgraph)
                res = option_error ("multiple '-d' options");
            else {
                file = booleforce_open_file_for_writing (argv[i]);
                if (file)
                    dotgraph = file;
                else
                    res = option_error ("can not write to '%s'", argv[i]);
            }
        } else if (!strcmp (argv[i], "--print"))
            print_only = 1;
        else if (!strcmp (argv[i], "--linear"))
            assume_already_linearized = 1;
        else if (!strcmp (argv[i], "--lax"))
            lax = 1;
        else if (!strcmp (argv[i], "--debug")) {
#ifndef NDEBUG
            debug = 1;
#endif
        } else if (!strcmp (argv[i], "-o")) {
            if (++i <= argc) {
                if ((file = fopen (argv[i], "w"))) {
                    output = file;
                    close_output = 1;
                } else
                    res = option_error ("can not write to '%s'", argv[i]);
            } else
                res = option_error ("argument to '-o' missing");
        } else if (argv[i][0] == '-')
            res = option_error ("invalid option '%s'", argv[i]);
        else if (input)
            res = option_error ("multiple input files");
        else if (!(file = booleforce_open_file_for_reading (argv[i])))
            res = option_error ("can not read '%s'", argv[i]);
        else {
            input = file;
            input_name = argv[i];
            close_input = 1;
        }
    }

    if (!res && !done && !input) {
        input = stdin;
        input_name = "<stdin>";
        assert (!close_input);
    }

    if (!res && !done && !original_cnf_file_name) {
        if (rpttrace)
            res = option_error ("option '-r' requires '-c'");
        else if (restrace)
            res = option_error ("option '-R' requires '-c'");
    }

    if (!res && !done && original_cnf_file_name)
        res = !parse_header_of_original_dimacs_file ();

    if (!res && !done)
        init_buffer (buffer, input);

    if (!res && !done) {
        if (parse ()) {
            if (print_only)
                print (output);
            else {
                if (check ()) {
                    if (interpolant) {
                        output_aig = aiger_init ();
                        final_itp = clauses[empty_cls_idx]->itp;
                        expand (final_itp);
                        aiger_write_to_file (output_aig, aiger_binary_mode, interpolant);
                        aiger_reset (output_aig);
                        simpaig_dec (mgr, final_itp);
                    }

                    if (stats_file)
                        print_stats ();

                    if (dotgraph)
                        write_dotgraph_footer ();

                    fprintf (output,
                             "resolved %d root%s and %d empty clause%s\n",
                             count_roots,
                             count_roots == 1 ? "" : "s",
                             num_empty_clauses,
                             num_empty_clauses == 1 ? "" : "s");
 
                    if (etrace)
                        print (etrace);
                } else
                    res = 1;

                // simpaig clean-up
                if (literals)
                    release_literals ();
                simpaig_reset (mgr);
            }
        } else
            res = 1;

        // Booleforce clean-up
        release ();
    }

    seconds = booleforce_time_stamp () - entered_time;
    seconds = (seconds < 0) ? 0 : seconds;

    if (verbose) {
        LOG ("%d resolutions", num_resolutions);
        LOG ("time spent %.2f seconds", seconds);
        LOG ("memory usage %.1f MB",
             booleforce_max_bytes () / (double) (1 << 20));
    }

    reset ();			/* no code between 'reset' and 'return' */

    return res;
}

