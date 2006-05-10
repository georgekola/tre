/*
  tre-parse.c - Regexp parser

  Copyright (c) 2001-2006 Ville Laurikari <vl@iki.fi>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

/*
  This parser is just a simple recursive descent parser for POSIX.2
  regexps.  The parser supports both the obsolete default syntax and
  the "extended" syntax, and some nonstandard extensions.
*/


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */
#include <string.h>
#include <assert.h>
#include <limits.h>

#include "xmalloc.h"
#include "tre-mem.h"
#include "tre-ast.h"
#include "tre-stack.h"
#include "tre-parse.h"


/* Characters with special meanings in regexp syntax. */
#define CHAR_PIPE	   L'|'
#define CHAR_LPAREN	   L'('
#define CHAR_RPAREN	   L')'
#define CHAR_LBRACE	   L'{'
#define CHAR_RBRACE	   L'}'
#define CHAR_LBRACKET	   L'['
#define CHAR_RBRACKET	   L']'
#define CHAR_MINUS	   L'-'
#define CHAR_STAR	   L'*'
#define CHAR_QUESTIONMARK  L'?'
#define CHAR_PLUS	   L'+'
#define CHAR_PERIOD	   L'.'
#define CHAR_COLON	   L':'
#define CHAR_EQUAL	   L'='
#define CHAR_COMMA	   L','
#define CHAR_CARET	   L'^'
#define CHAR_DOLLAR	   L'$'
#define CHAR_BACKSLASH	   L'\\'
#define CHAR_HASH	   L'#'
#define CHAR_TILDE	   L'~'


/* Some macros for expanding \w, \s, etc. */
static const char *tre_macros[] =
  { "t", "\t",		   "n", "\n",		  "r", "\r",
    "f", "\f",		   "a", "\a",		  "e", "\033",
    "w", "[[:alnum:]_]",   "W", "[^[:alnum:]_]",  "s", "[[:space:]]",
    "S", "[^[:space:]]",   "d", "[[:digit:]]",	  "D", "[^[:digit:]]",
   NULL };


/* Expands a macro delimited by `regex' and `regex_end' to `buf', which
   must have at least `len' items.  Sets buf[0] to zero if the there
   is no match in `tre_macros'. */
static void
tre_expand_macro(const tre_char_t *regex, const tre_char_t *regex_end,
		 tre_char_t *buf, size_t buf_len)
{
  int i;
  size_t len = regex_end - regex;

  buf[0] = 0;
  for (i = 0; tre_macros[i] != NULL; i += 2)
    {
      int match = 0;
      if (strlen(tre_macros[i]) > len)
	continue;
#ifdef TRE_WCHAR
      {
	tre_char_t tmp_wcs[64];
	unsigned int j;
	for (j = 0; j < strlen(tre_macros[i]) && j < elementsof(tmp_wcs); j++)
	  tmp_wcs[j] = btowc(tre_macros[i][j]);
	tmp_wcs[j] = 0;
	match = wcsncmp(tmp_wcs, regex, strlen(tre_macros[i]));
      }
#else /* !TRE_WCHAR */
      match = strncmp(tre_macros[i], regex, strlen(tre_macros[i]));
#endif /* !TRE_WCHAR */
      if (match == 0)
	{
	  unsigned int j;
	  DPRINT(("Expanding macro '%s' => '%s'\n",
		  tre_macros[i], tre_macros[i + 1]));
	  for (j = 0; tre_macros[i + 1][j] != 0 && j < buf_len; j++)
	    {
#ifdef TRE_WCHAR
	      buf[j] = btowc(tre_macros[i + 1][j]);
#else /* !TRE_WCHAR */
	      buf[j] = tre_macros[i + 1][j];
#endif /* !TRE_WCHAR */
	    }
	  buf[j] = 0;
	  break;
	}
    }
}

static reg_errcode_t
tre_new_item(tre_mem_t mem, int min, int max, int *i, int *max_i,
	 tre_ast_node_t ***items)
{
  reg_errcode_t status;
  tre_ast_node_t **array = *items;
  /* Allocate more space if necessary. */
  if (*i >= *max_i)
    {
      tre_ast_node_t **new_items;
      DPRINT(("out of array space, i = %d\n", *i));
      /* If the array is already 1024 items large, give up -- there's
	 probably an error in the regexp (e.g. not a '\0' terminated
	 string and missing ']') */
      if (*max_i > 1024)
	return REG_ESPACE;
      *max_i *= 2;
      new_items = xrealloc(array, sizeof(*items) * *max_i);
      if (new_items == NULL)
	return REG_ESPACE;
      *items = array = new_items;
    }
  array[*i] = tre_ast_new_literal(mem, min, max, -1);
  status = array[*i] == NULL ? REG_ESPACE : REG_OK;
  (*i)++;
  return status;
}


/* Expands a character class to character ranges. */
static reg_errcode_t
tre_expand_ctype(tre_mem_t mem, tre_ctype_t class, tre_ast_node_t ***items,
		 int *i, int *max_i, int cflags)
{
  reg_errcode_t status = REG_OK;
  tre_cint_t c;
  int j, min = -1, max = 0;
  assert(TRE_MB_CUR_MAX == 1);

  DPRINT(("  expanding class to character ranges\n"));
  for (j = 0; (j < 256) && (status == REG_OK); j++)
    {
      c = j;
      if (tre_isctype(c, class)
	  || ((cflags & REG_ICASE)
	      && (tre_isctype(tre_tolower(c), class)
		  || tre_isctype(tre_toupper(c), class))))
{
	  if (min < 0)
	    min = c;
	  max = c;
	}
      else if (min >= 0)
	{
	  DPRINT(("  range %c (%d) to %c (%d)\n", min, min, max, max));
	  status = tre_new_item(mem, min, max, i, max_i, items);
	  min = -1;
	}
    }
  if (min >= 0 && status == REG_OK)
    status = tre_new_item(mem, min, max, i, max_i, items);
  return status;
}


static int
tre_compare_items(const void *a, const void *b)
{
  tre_ast_node_t *node_a = *(tre_ast_node_t **)a;
  tre_ast_node_t *node_b = *(tre_ast_node_t **)b;
  tre_literal_t *l_a = node_a->obj, *l_b = node_b->obj;
  int a_min = l_a->code_min, b_min = l_b->code_min;

  if (a_min < b_min)
    return -1;
  else if (a_min > b_min)
    return 1;
  else
    return 0;
}

#ifndef TRE_USE_SYSTEM_WCTYPE

/* isalnum() and the rest may be macros, so wrap them to functions. */
int tre_isalnum_func(tre_cint_t c) { return tre_isalnum(c); }
int tre_isalpha_func(tre_cint_t c) { return tre_isalpha(c); }

#ifdef tre_isascii
int tre_isascii_func(tre_cint_t c) { return tre_isascii(c); }
#else /* !tre_isascii */
int tre_isascii_func(tre_cint_t c) { return !(c >> 7); }
#endif /* !tre_isascii */

#ifdef tre_isblank
int tre_isblank_func(tre_cint_t c) { return tre_isblank(c); }
#else /* !tre_isblank */
int tre_isblank_func(tre_cint_t c) { return ((c == ' ') || (c == '\t')); }
#endif /* !tre_isblank */

int tre_iscntrl_func(tre_cint_t c) { return tre_iscntrl(c); }
int tre_isdigit_func(tre_cint_t c) { return tre_isdigit(c); }
int tre_isgraph_func(tre_cint_t c) { return tre_isgraph(c); }
int tre_islower_func(tre_cint_t c) { return tre_islower(c); }
int tre_isprint_func(tre_cint_t c) { return tre_isprint(c); }
int tre_ispunct_func(tre_cint_t c) { return tre_ispunct(c); }
int tre_isspace_func(tre_cint_t c) { return tre_isspace(c); }
int tre_isupper_func(tre_cint_t c) { return tre_isupper(c); }
int tre_isxdigit_func(tre_cint_t c) { return tre_isxdigit(c); }

struct {
  char *name;
  int (*func)(tre_cint_t);
} tre_ctype_map[] = {
  { "alnum", &tre_isalnum_func },
  { "alpha", &tre_isalpha_func },
#ifdef tre_isascii
  { "ascii", &tre_isascii_func },
#endif /* tre_isascii */
#ifdef tre_isblank
  { "blank", &tre_isblank_func },
#endif /* tre_isblank */
  { "cntrl", &tre_iscntrl_func },
  { "digit", &tre_isdigit_func },
  { "graph", &tre_isgraph_func },
  { "lower", &tre_islower_func },
  { "print", &tre_isprint_func },
  { "punct", &tre_ispunct_func },
  { "space", &tre_isspace_func },
  { "upper", &tre_isupper_func },
  { "xdigit", &tre_isxdigit_func },
  { NULL, NULL}
};

tre_ctype_t tre_ctype(const char *name)
{
  int i;
  for (i = 0; tre_ctype_map[i].name != NULL; i++)
    {
      if (strcmp(name, tre_ctype_map[i].name) == 0)
	return tre_ctype_map[i].func;
    }
  return (tre_ctype_t)0;
}
#endif /* !TRE_USE_SYSTEM_WCTYPE */

/* Maximum number of character classes that can occur in a negated bracket
   expression.	*/
#define MAX_NEG_CLASSES 64

/* Maximum length of character class names. */
#define MAX_CLASS_NAME

static reg_errcode_t
tre_parse_bracket_items(tre_parse_ctx_t *ctx, int negate,
			tre_ctype_t neg_classes[], int *num_neg_classes,
			tre_ast_node_t ***items, int *num_items,
			int *items_size)
{
  const tre_char_t *re = ctx->re;
  reg_errcode_t status = REG_OK;
  tre_ctype_t class = (tre_ctype_t)0;
  int i = *num_items;
  int max_i = *items_size;
  int skip;

  /* Build an array of the items in the bracket expression. */
  while (status == REG_OK)
    {
      skip = 0;
      if (re == ctx->re_end)
	{
	  status = REG_EBRACK;
	}
      else if (*re == CHAR_RBRACKET && re > ctx->re)
	{
	  DPRINT(("tre_parse_bracket:	done: '%.*" STRF "'\n",
		  ctx->re_end - re, re));
	  re++;
	  break;
	}
      else
	{
	  tre_cint_t min = 0, max = 0;

	  class = (tre_ctype_t)0;
	  if (re + 2 < ctx->re_end
	      && *(re + 1) == CHAR_MINUS && *(re + 2) != CHAR_RBRACKET)
	    {
	      DPRINT(("tre_parse_bracket:  range: '%.*" STRF "'\n",
		      ctx->re_end - re, re));
	      min = *re;
	      max = *(re + 2);
	      re += 3;
	      /* XXX - Should use collation order instead of encoding values
		 in character ranges. */
	      if (min > max)
		status = REG_ERANGE;
	    }
	  else if (re + 1 < ctx->re_end
		   && *re == CHAR_LBRACKET && *(re + 1) == CHAR_PERIOD)
	    status = REG_ECOLLATE;
	  else if (re + 1 < ctx->re_end
		   && *re == CHAR_LBRACKET && *(re + 1) == CHAR_EQUAL)
	    status = REG_ECOLLATE;
	  else if (re + 1 < ctx->re_end
		   && *re == CHAR_LBRACKET && *(re + 1) == CHAR_COLON)
	    {
	      char tmp_str[64];
	      const tre_char_t *endptr = re + 2;
	      int len;
	      DPRINT(("tre_parse_bracket:  class: '%.*" STRF "'\n",
		      ctx->re_end - re, re));
	      while (endptr < ctx->re_end && *endptr != CHAR_COLON)
		endptr++;
	      if (endptr != ctx->re_end)
		{
		  len = MIN(endptr - re - 2, 63);
#ifdef TRE_WCHAR
		  {
		    tre_char_t tmp_wcs[64];
		    wcsncpy(tmp_wcs, re + 2, len);
		    tmp_wcs[len] = L'\0';
#if defined HAVE_WCSRTOMBS
		    {
		      mbstate_t state;
		      const tre_char_t *src = tmp_wcs;
		      memset(&state, '\0', sizeof(state));
		      len = wcsrtombs(tmp_str, &src, sizeof(tmp_str), &state);
		    }
#elif defined HAVE_WCSTOMBS
		    len = wcstombs(tmp_str, tmp_wcs, 63);
#endif /* defined HAVE_WCSTOMBS */
		  }
#else /* !TRE_WCHAR */
		  strncpy(tmp_str, re + 2, len);
#endif /* !TRE_WCHAR */
		  tmp_str[len] = '\0';
		  DPRINT(("  class name: %s\n", tmp_str));
		  class = tre_ctype(tmp_str);
		  if (!class)
		    status = REG_ECTYPE;
		  /* Optimize character classes for 8 bit character sets. */
		  if (status == REG_OK && TRE_MB_CUR_MAX == 1)
		    {
		      status = tre_expand_ctype(ctx->mem, class, items,
						&i, &max_i, ctx->cflags);
		      class = (tre_ctype_t)0;
		      skip = 1;
		    }
		  re = endptr + 2;
		}
	      else
		status = REG_ECTYPE;
	      min = 0;
	      max = TRE_CHAR_MAX;
	    }
	  else
	    {
	      DPRINT(("tre_parse_bracket:   char: '%.*" STRF "'\n",
		      ctx->re_end - re, re));
	      if (*re == CHAR_MINUS && *(re + 1) != CHAR_RBRACKET
		  && ctx->re != re)
		/* Two ranges are not allowed to share and endpoint. */
		status = REG_ERANGE;
	      min = max = *re++;
	    }

	  if (status != REG_OK)
	    break;

	  if (class && negate)
	    if (*num_neg_classes >= MAX_NEG_CLASSES)
	      status = REG_ESPACE;
	    else
	      neg_classes[(*num_neg_classes)++] = class;
	  else if (!skip)
	    {
	      status = tre_new_item(ctx->mem, min, max, &i, &max_i, items);
	      if (status != REG_OK)
		break;
	      ((tre_literal_t*)((*items)[i-1])->obj)->u.class = class;
	    }

	  /* Add opposite-case counterpoints if REG_ICASE is present.
	     This is broken if there are more than two "same" characters. */
	  if (ctx->cflags & REG_ICASE && !class && status == REG_OK && !skip)
	    {
	      int cmin, ccurr;

	      DPRINT(("adding opposite-case counterpoints\n"));
	      while (min <= max)
		{
		  if (tre_islower(min))
		    {
		      cmin = ccurr = tre_toupper(min++);
		      while (tre_islower(min) && tre_toupper(min) == ccurr + 1
			     && min <= max)
			ccurr = tre_toupper(min++);
		      status = tre_new_item(ctx->mem, cmin, ccurr,
					    &i, &max_i, items);
		    }
		  else if (tre_isupper(min))
		    {
		      cmin = ccurr = tre_tolower(min++);
		      while (tre_isupper(min) && tre_tolower(min) == ccurr + 1
			     && min <= max)
			ccurr = tre_tolower(min++);
		      status = tre_new_item(ctx->mem, cmin, ccurr,
					    &i, &max_i, items);
		    }
		  else min++;
		  if (status != REG_OK)
		    break;
		}
	      if (status != REG_OK)
		break;
	    }
	}
    }
  *num_items = i;
  *items_size = max_i;
  ctx->re = re;
  return status;
}

static reg_errcode_t
tre_parse_bracket(tre_parse_ctx_t *ctx, tre_ast_node_t **result)
{
  tre_ast_node_t *node = NULL;
  int negate = 0;
  reg_errcode_t status = REG_OK;
  tre_ast_node_t **items, *u, *n;
  int i = 0, j, max_i = 32, curr_max, curr_min;
  tre_ctype_t neg_classes[MAX_NEG_CLASSES];
  int num_neg_classes = 0;

  /* Start off with an array of `max_i' elements. */
  items = xmalloc(sizeof(*items) * max_i);
  if (items == NULL)
    return REG_ESPACE;

  if (*ctx->re == CHAR_CARET)
    {
      DPRINT(("tre_parse_bracket: negate: '%.*" STRF "'\n",
	      ctx->re_end - ctx->re, ctx->re));
      negate = 1;
      ctx->re++;
    }

  status = tre_parse_bracket_items(ctx, negate, neg_classes, &num_neg_classes,
				   &items, &i, &max_i);

  if (status != REG_OK)
    goto parse_bracket_done;

  /* Sort the array if we need to negate it. */
  if (negate)
    qsort(items, i, sizeof(*items), tre_compare_items);

  curr_max = curr_min = 0;
  /* Build a union of the items in the array, negated if necessary. */
  for (j = 0; j < i && status == REG_OK; j++)
    {
      int min, max;
      tre_literal_t *l = items[j]->obj;
      min = l->code_min;
      max = l->code_max;

      DPRINT(("item: %d - %d, class %ld, curr_max = %d\n",
	      (int)l->code_min, (int)l->code_max, (long)l->u.class, curr_max));

      if (negate)
	{
	  if (min < curr_max)
	    {
	      /* Overlap. */
	      curr_max = MAX(max + 1, curr_max);
	      DPRINT(("overlap, curr_max = %d\n", curr_max));
	      l = NULL;
	    }
	  else
	    {
	      /* No overlap. */
	      curr_max = min - 1;
	      if (curr_max >= curr_min)
		{
		  DPRINT(("no overlap\n"));
		  l->code_min = curr_min;
		  l->code_max = curr_max;
		}
	      else
		{
		  DPRINT(("no overlap, zero room\n"));
		  l = NULL;
		}
	      curr_min = curr_max = max + 1;
	    }
	}

      if (l != NULL)
	{
	  int k;
	  DPRINT(("creating %d - %d\n", (int)l->code_min, (int)l->code_max));
	  l->position = ctx->position;
	  if (num_neg_classes > 0)
	    {
	      l->neg_classes = tre_mem_alloc(ctx->mem,
					     (sizeof(l->neg_classes)
					      * (num_neg_classes + 1)));
	      if (l->neg_classes == NULL)
		{
		  status = REG_ESPACE;
		  break;
		}
	      for (k = 0; k < num_neg_classes; k++)
		l->neg_classes[k] = neg_classes[k];
	      l->neg_classes[k] = (tre_ctype_t)0;
	    }
	  else
	    l->neg_classes = NULL;
	  if (node == NULL)
	    node = items[j];
	  else
	    {
	      u = tre_ast_new_union(ctx->mem, node, items[j]);
	      if (u == NULL)
		status = REG_ESPACE;
	      node = u;
	    }
	}
    }

  if (status != REG_OK)
    goto parse_bracket_done;

  if (negate)
    {
      int k;
      DPRINT(("final: creating %d - %d\n", curr_min, (int)TRE_CHAR_MAX));
      n = tre_ast_new_literal(ctx->mem, curr_min, TRE_CHAR_MAX, ctx->position);
      if (n == NULL)
	status = REG_ESPACE;
      else
	{
	  tre_literal_t *l = n->obj;
	  if (num_neg_classes > 0)
	    {
	      l->neg_classes = tre_mem_alloc(ctx->mem,
					     (sizeof(l->neg_classes)
					      * (num_neg_classes + 1)));
	      if (l->neg_classes == NULL)
		{
		  status = REG_ESPACE;
		  goto parse_bracket_done;
		}
	      for (k = 0; k < num_neg_classes; k++)
		l->neg_classes[k] = neg_classes[k];
	      l->neg_classes[k] = (tre_ctype_t)0;
	    }
	  else
	    l->neg_classes = NULL;
	  if (node == NULL)
	    node = n;
	  else
	    {
	      u = tre_ast_new_union(ctx->mem, node, n);
	      if (u == NULL)
		status = REG_ESPACE;
	      node = u;
	    }
	}
    }

  if (status != REG_OK)
    goto parse_bracket_done;

#ifdef TRE_DEBUG
  tre_ast_print(node);
#endif /* TRE_DEBUG */

 parse_bracket_done:
  xfree(items);
  ctx->position++;
  *result = node;
  return status;
}


/* Parses a positive decimal integer.  Returns -1 if the string does not
   contain a valid number. */
static int
tre_parse_int(const tre_char_t **regex, const tre_char_t *regex_end)
{
  int num = -1;
  const tre_char_t *r = *regex;
  while (r < regex_end && *r >= L'0' && *r <= L'9')
    {
      if (num < 0)
	num = 0;
      num = num * 10 + *r - L'0';
      r++;
    }
  *regex = r;
  return num;
}


static reg_errcode_t
tre_parse_bound(tre_parse_ctx_t *ctx, tre_ast_node_t **result)
{
  int min, max, i;
  int cost_ins, cost_del, cost_subst, cost_max;
  int limit_ins, limit_del, limit_subst, limit_err;
  const tre_char_t *r = ctx->re;
  const tre_char_t *start;
  int minimal = (ctx->cflags & REG_UNGREEDY) ? 1 : 0;
  int approx = 0;
  int costs_set = 0;
  int counts_set = 0;

  cost_ins = cost_del = cost_subst = cost_max = TRE_PARAM_UNSET;
  limit_ins = limit_del = limit_subst = limit_err = TRE_PARAM_UNSET;

  /* Parse number (minimum repetition count). */
  min = -1;
  if (r < ctx->re_end && *r >= L'0' && *r <= L'9') {
    DPRINT(("tre_parse:	  min count: '%.*" STRF "'\n", ctx->re_end - r, r));
    min = tre_parse_int(&r, ctx->re_end);
  }

  /* Parse comma and second number (maximum repetition count). */
  max = min;
  if (r < ctx->re_end && *r == CHAR_COMMA)
    {
      r++;
      DPRINT(("tre_parse:   max count: '%.*" STRF "'\n", ctx->re_end - r, r));
      max = tre_parse_int(&r, ctx->re_end);
    }

  /* Check that the repeat counts are sane. */
  if ((max >= 0 && min > max) || max > RE_DUP_MAX)
    return REG_BADBR;


  /*
   '{'
     optionally followed immediately by a number == minimum repcount
     optionally followed by , then a number == maximum repcount
      + then a number == maximum insertion count
      - then a number == maximum deletion count
      # then a number == maximum substitution count
      ~ then a number == maximum number of errors
      Any of +, -, # or ~ without followed by a number means that
      the maximum count/number of errors is infinite.

      An equation of the form
	Xi + Yd + Zs < C
      can be specified to set costs and the cost limit to a value
      different from the default value:
	- X is the cost of an insertion
	- Y is the cost of a deletion
	- Z is the cost of a substitution
	- C is the maximum cost

      If no count limit or cost is set for an operation, the operation
      is not allowed at all.
  */


  do {
    int done;
    start = r;

    /* Parse count limit settings */
    done = 0;
    if (!counts_set)
      while (r + 1 < ctx->re_end && !done)
	{
	  switch (*r)
	    {
	    case CHAR_PLUS:  /* Insert limit */
	      DPRINT(("tre_parse:   ins limit: '%.*" STRF "'\n", ctx->re_end - r, r));
	      r++;
	      limit_ins = tre_parse_int(&r, ctx->re_end);
	      if (limit_ins < 0)
		limit_ins = INT_MAX;
	      counts_set = 1;
	      break;
	    case CHAR_MINUS: /* Delete limit */
	      DPRINT(("tre_parse:   del limit: '%.*" STRF "'\n", ctx->re_end - r, r));
	      r++;
	      limit_del = tre_parse_int(&r, ctx->re_end);
	      if (limit_del < 0)
		limit_del = INT_MAX;
	      counts_set = 1;
	      break;
	    case CHAR_HASH:  /* Substitute limit */
	      DPRINT(("tre_parse: subst limit: '%.*" STRF "'\n", ctx->re_end - r, r));
	      r++;
	      limit_subst = tre_parse_int(&r, ctx->re_end);
	      if (limit_subst < 0)
		limit_subst = INT_MAX;
	      counts_set = 1;
	      break;
	    case CHAR_TILDE: /* Maximum number of changes */
	      DPRINT(("tre_parse: count limit: '%.*" STRF "'\n", ctx->re_end - r, r));
	      r++;
	      limit_err = tre_parse_int(&r, ctx->re_end);
	      if (limit_err < 0)
		limit_err = INT_MAX;
	      approx = 1;
	      break;
	    case CHAR_COMMA:
	      r++;
	      break;
	    case L' ':
	      r++;
	      break;
	    case L'}':
	      done = 1;
	      break;
	    default:
	      done = 1;
	      break;
	    }
	}

    /* Parse cost restriction equation. */
    done = 0;
    if (!costs_set)
      while (r + 1 < ctx->re_end && !done)
	{
	  switch (*r)
	    {
	    case CHAR_PLUS:
	    case L' ':
	      r++;
	      break;
	    case L'<':
	      DPRINT(("tre_parse:    max cost: '%.*" STRF "'\n", ctx->re_end - r, r));
	      r++;
	      while (*r == L' ')
		r++;
	      cost_max = tre_parse_int(&r, ctx->re_end);
	      if (cost_max < 0)
		cost_max = INT_MAX;
	      else
		cost_max--;
	      approx = 1;
	      break;
	    case CHAR_COMMA:
	      r++;
	      done = 1;
	      break;
	    default:
	      if (*r >= L'0' && *r <= L'9')
		{
#ifdef TRE_DEBUG
		  const tre_char_t *sr = r;
#endif /* TRE_DEBUG */
		  int cost = tre_parse_int(&r, ctx->re_end);
		  /* XXX - make sure r is not past end. */
		  switch (*r)
		    {
		    case L'i':	/* Insert cost */
		      DPRINT(("tre_parse:    ins cost: '%.*" STRF "'\n",
			      ctx->re_end - sr, sr));
		      r++;
		      cost_ins = cost;
		      costs_set = 1;
		      break;
		    case L'd':	/* Delete cost */
		      DPRINT(("tre_parse:    del cost: '%.*" STRF "'\n",
			      ctx->re_end - sr, sr));
		      r++;
		      cost_del = cost;
		      costs_set = 1;
		      break;
		    case L's':	/* Substitute cost */
		      DPRINT(("tre_parse:  subst cost: '%.*" STRF "'\n",
			      ctx->re_end - sr, sr));
		      r++;
		      cost_subst = cost;
		      costs_set = 1;
		      break;
		    default:
		      return REG_BADBR;
		    }
		}
	      else
		{
		  done = 1;
		  break;
		}
	    }
	}
  } while (start != r);

  /* Missing }. */
  if (r >= ctx->re_end)
    return REG_EBRACE;

  /* Empty contents of {}. */
  if (r == ctx->re)
    return REG_BADBR;

  /* Parse the ending '}' or '\}'.*/
  if (ctx->cflags & REG_EXTENDED)
    {
      if (r >= ctx->re_end || *r != CHAR_RBRACE)
	return REG_BADBR;
      r++;
    }
  else
    {
      if (r + 1 >= ctx->re_end
	  || *r != CHAR_BACKSLASH
	  || *(r + 1) != CHAR_RBRACE)
	return REG_BADBR;
      r += 2;
    }


  /* Parse trailing '?' marking minimal repetition. */
  if (r < ctx->re_end && *r == CHAR_QUESTIONMARK)
    {
      minimal = !(ctx->cflags & REG_UNGREEDY);
      r++;
    }

  /* Create the AST node(s). */
  if (min == 0 && max == 0)
    {
      *result = tre_ast_new_literal(ctx->mem, EMPTY, -1, -1);
      if (*result == NULL)
	return REG_ESPACE;
    }
  else
    {
      if (min < 0 && max < 0)
	/* Only approximate parameters set, no repetitions. */
	min = max = 1;

      *result = tre_ast_new_iter(ctx->mem, *result, min, max, minimal);
      if (!*result)
	return REG_ESPACE;

      /* If approximate matching parameters are set, add them to the
	 iteration node. */
      if (approx || costs_set || counts_set)
	{
	  unsigned int *params;
	  tre_iteration_t *iter = (*result)->obj;

	  if (costs_set || counts_set)
	    {
	      if (limit_ins == TRE_PARAM_UNSET)
		{
		  if (cost_ins == TRE_PARAM_UNSET)
		    limit_ins = 0;
		  else
		    limit_ins = INT_MAX;
		}

	      if (limit_del == TRE_PARAM_UNSET)
		{
		  if (cost_del == TRE_PARAM_UNSET)
		    limit_del = 0;
		  else
		    limit_del = INT_MAX;
		}

	      if (limit_subst == TRE_PARAM_UNSET)
		{
		  if (cost_subst == TRE_PARAM_UNSET)
		    limit_subst = 0;
		  else
		    limit_subst = INT_MAX;
		}
	    }

	  if (cost_max == TRE_PARAM_UNSET)
	    cost_max = INT_MAX;
	  if (limit_err == TRE_PARAM_UNSET)
	    limit_err = INT_MAX;

	  ctx->have_approx = 1;
	  params = tre_mem_alloc(ctx->mem, sizeof(*params) * TRE_PARAM_LAST);
	  if (!params)
	    return REG_ESPACE;
	  for (i = 0; i < TRE_PARAM_LAST; i++)
	    params[i] = TRE_PARAM_UNSET;
	  params[TRE_PARAM_COST_INS] = cost_ins;
	  params[TRE_PARAM_COST_DEL] = cost_del;
	  params[TRE_PARAM_COST_SUBST] = cost_subst;
	  params[TRE_PARAM_COST_MAX] = cost_max;
	  params[TRE_PARAM_MAX_INS] = limit_ins;
	  params[TRE_PARAM_MAX_DEL] = limit_del;
	  params[TRE_PARAM_MAX_SUBST] = limit_subst;
	  params[TRE_PARAM_MAX_ERR] = limit_err;
	  iter->params = params;
	}
    }

  DPRINT(("tre_parse_bound: min %d, max %d, costs [%d,%d,%d, total %d], "
	  "limits [%d,%d,%d, total %d]\n",
	  min, max, cost_ins, cost_del, cost_subst, cost_max,
	  limit_ins, limit_del, limit_subst, limit_err));


  ctx->re = r;
  return REG_OK;
}

typedef enum {
  PARSE_RE = 0,
  PARSE_ATOM,
  PARSE_MARK_FOR_SUBMATCH,
  PARSE_BRANCH,
  PARSE_PIECE,
  PARSE_CATENATION,
  PARSE_POST_CATENATION,
  PARSE_UNION,
  PARSE_POST_UNION,
  PARSE_POSTFIX,
  PARSE_RESTORE_CFLAGS
} tre_parse_re_stack_symbol_t;


reg_errcode_t
tre_parse(tre_parse_ctx_t *ctx)
{
  tre_ast_node_t *result = NULL;
  tre_parse_re_stack_symbol_t symbol;
  reg_errcode_t status = REG_OK;
  tre_stack_t *stack = ctx->stack;
  int bottom = tre_stack_num_objects(stack);
  int depth = 0;
  int temporary_cflags = 0;

  DPRINT(("tre_parse: parsing '%.*" STRF "', len = %d\n",
	  ctx->len, ctx->re, ctx->len));

  if (!ctx->nofirstsub)
    {
      STACK_PUSH(stack, voidptr, (void*)ctx->re);
      STACK_PUSH(stack, int, ctx->submatch_id);
      STACK_PUSH(stack, int, PARSE_MARK_FOR_SUBMATCH);
      ctx->submatch_id++;
    }
  STACK_PUSH(stack, int, PARSE_RE);
  ctx->re_start = ctx->re;
  ctx->re_end = ctx->re + ctx->len;


  /* The following is basically just a recursive descent parser.  I use
     an explicit stack instead of recursive functions mostly because of
     two reasons: compatibility with systems which have an overflowable
     call stack, and efficiency (both in lines of code and speed).  */
  while (tre_stack_num_objects(stack) > bottom && status == REG_OK)
    {
      if (status != REG_OK)
	break;
      symbol = tre_stack_pop_int(stack);
      switch (symbol)
	{
	case PARSE_RE:
	  /* Parse a full regexp.  A regexp is one or more branches,
	     separated by the union operator `|'. */
#ifdef REG_LITERAL
	  if (!(ctx->cflags & REG_LITERAL)
	      && ctx->cflags & REG_EXTENDED)
#endif /* REG_LITERAL */
	    STACK_PUSHX(stack, int, PARSE_UNION);
	  STACK_PUSHX(stack, int, PARSE_BRANCH);
	  break;

	case PARSE_BRANCH:
	  /* Parse a branch.  A branch is one or more pieces, concatenated.
	     A piece is an atom possibly followed by a postfix operator. */
	  STACK_PUSHX(stack, int, PARSE_CATENATION);
	  STACK_PUSHX(stack, int, PARSE_PIECE);
	  break;

	case PARSE_PIECE:
	  /* Parse a piece.  A piece is an atom possibly followed by one
	     or more postfix operators. */
#ifdef REG_LITERAL
	  if (!(ctx->cflags & REG_LITERAL))
#endif /* REG_LITERAL */
	    STACK_PUSHX(stack, int, PARSE_POSTFIX);
	  STACK_PUSHX(stack, int, PARSE_ATOM);
	  break;

	case PARSE_CATENATION:
	  /* If the expression has not ended, parse another piece. */
	  {
	    tre_char_t c;
	    if (ctx->re >= ctx->re_end)
	      break;
	    c = *ctx->re;
#ifdef REG_LITERAL
	    if (!(ctx->cflags & REG_LITERAL))
	      {
#endif /* REG_LITERAL */
		if (ctx->cflags & REG_EXTENDED && c == CHAR_PIPE)
		  break;
		if ((ctx->cflags & REG_EXTENDED
		     && c == CHAR_RPAREN && depth > 0)
		    || (!(ctx->cflags & REG_EXTENDED)
			&& (c == CHAR_BACKSLASH
			    && *(ctx->re + 1) == CHAR_RPAREN)))
		  {
		    if (!(ctx->cflags & REG_EXTENDED) && depth == 0)
		      status = REG_EPAREN;
		    DPRINT(("tre_parse:	  group end: '%.*" STRF "'\n",
			    ctx->re_end - ctx->re, ctx->re));
		    depth--;
		    if (!(ctx->cflags & REG_EXTENDED))
		      ctx->re += 2;
		    break;
		  }
#ifdef REG_LITERAL
	      }
#endif /* REG_LITERAL */

#ifdef REG_RIGHT_ASSOC
	    if (ctx->cflags & REG_RIGHT_ASSOC)
	      {
		/* Right associative concatenation. */
		STACK_PUSHX(stack, voidptr, result);
		STACK_PUSHX(stack, int, PARSE_POST_CATENATION);
		STACK_PUSHX(stack, int, PARSE_CATENATION);
		STACK_PUSHX(stack, int, PARSE_PIECE);
	      }
	    else
#endif /* REG_RIGHT_ASSOC */
	      {
		/* Default case, left associative concatenation. */
		STACK_PUSHX(stack, int, PARSE_CATENATION);
		STACK_PUSHX(stack, voidptr, result);
		STACK_PUSHX(stack, int, PARSE_POST_CATENATION);
		STACK_PUSHX(stack, int, PARSE_PIECE);
	      }
	    break;
	  }

	case PARSE_POST_CATENATION:
	  {
	    tre_ast_node_t *tree = tre_stack_pop_voidptr(stack);
	    tre_ast_node_t *tmp_node;
	    tmp_node = tre_ast_new_catenation(ctx->mem, tree, result);
	    if (!tmp_node)
	      return REG_ESPACE;
	    result = tmp_node;
	    break;
	  }

	case PARSE_UNION:
	  if (ctx->re >= ctx->re_end)
	    break;
#ifdef REG_LITERAL
	  if (ctx->cflags & REG_LITERAL)
	    break;
#endif /* REG_LITERAL */
	  switch (*ctx->re)
	    {
	    case CHAR_PIPE:
	      DPRINT(("tre_parse:	union: '%.*" STRF "'\n",
		      ctx->re_end - ctx->re, ctx->re));
	      STACK_PUSHX(stack, int, PARSE_UNION);
	      STACK_PUSHX(stack, voidptr, result);
	      STACK_PUSHX(stack, int, PARSE_POST_UNION);
	      STACK_PUSHX(stack, int, PARSE_BRANCH);
	      ctx->re++;
	      break;

	    case CHAR_RPAREN:
	      ctx->re++;
	      break;

	    default:
	      break;
	    }
	  break;

	case PARSE_POST_UNION:
	  {
	    tre_ast_node_t *tmp_node;
	    tre_ast_node_t *tree = tre_stack_pop_voidptr(stack);
	    tmp_node = tre_ast_new_union(ctx->mem, tree, result);
	    if (!tmp_node)
	      return REG_ESPACE;
	    result = tmp_node;
	    break;
	  }

	case PARSE_POSTFIX:
	  /* Parse postfix operators. */
	  if (ctx->re >= ctx->re_end)
	    break;
#ifdef REG_LITERAL
	  if (ctx->cflags & REG_LITERAL)
	    break;
#endif /* REG_LITERAL */
	  switch (*ctx->re)
	    {
	    case CHAR_STAR:
	    case CHAR_PLUS:
	    case CHAR_QUESTIONMARK:
	      {
		tre_ast_node_t *tmp_node;
		int minimal = (ctx->cflags & REG_UNGREEDY) ? 1 : 0;
		int rep_min = 0;
		int rep_max = -1;
		const tre_char_t *tmp_re;

		if (*ctx->re == CHAR_PLUS)
		  rep_min = 1;
		if (*ctx->re == CHAR_QUESTIONMARK)
		  rep_max = 1;
		tmp_re = ctx->re;
		
		if (ctx->re + 1 < ctx->re_end
		    && *(ctx->re + 1) == CHAR_QUESTIONMARK)
		  { 
		    minimal = !(ctx->cflags & REG_UNGREEDY);
		    ctx->re++;
		  }

		DPRINT(("tre_parse: %s star: '%.*" STRF "'\n",
			minimal ? "  minimal" : "greedy",
			ctx->re_end - tmp_re, tmp_re));
		ctx->re++;
		tmp_node = tre_ast_new_iter(ctx->mem, result, rep_min, rep_max,
					    minimal);
		if (tmp_node == NULL)
		  return REG_ESPACE;
		result = tmp_node;
		STACK_PUSHX(stack, int, PARSE_POSTFIX);
		break;
	      }

	    case CHAR_BACKSLASH:
	      /* "\{" is special without REG_EXTENDED */
	      if (!(ctx->cflags & REG_EXTENDED)
		  && ctx->re + 1 < ctx->re_end
		  && *(ctx->re + 1) == CHAR_LBRACE)
		{
		  ctx->re++;
		  goto parse_brace;
		}
	      else
		break;

	    case CHAR_LBRACE:
	      /* "{" is literal without REG_EXTENDED */
	      if (!(ctx->cflags & REG_EXTENDED))
		break;

	    parse_brace:
	      DPRINT(("tre_parse:	bound: '%.*" STRF "'\n",
		      ctx->re_end - ctx->re, ctx->re));
	      ctx->re++;

	      status = tre_parse_bound(ctx, &result);
	      if (status != REG_OK)
		return status;
	      STACK_PUSHX(stack, int, PARSE_POSTFIX);
	      break;
	    }
	  break;

	case PARSE_ATOM:
	  /* Parse an atom.  An atom is a regular expression enclosed in `()',
	     an empty set of `()', a bracket expression, `.', `^', `$',
	     a `\' followed by a character, or a single character. */

	  /* End of regexp? (empty string). */
	  if (ctx->re >= ctx->re_end)
	    goto parse_literal;

#ifdef REG_LITERAL
	  if (ctx->cflags & REG_LITERAL)
	    goto parse_literal;
#endif /* REG_LITERAL */

	  switch (*ctx->re)
	    {
	    case CHAR_LPAREN:  /* parenthesized subexpression */

	      /* Handle "(?...)" extensions.  They work in a way similar
		 to Perls corresponding extensions. */
	      if (ctx->cflags & REG_EXTENDED
		  && *(ctx->re + 1) == CHAR_QUESTIONMARK)
		{
		  int new_cflags = ctx->cflags;
		  int bit = 1;
		  DPRINT(("tre_parse:	extension: '%.*" STRF "\n",
			  ctx->re_end - ctx->re, ctx->re));
		  ctx->re += 2;
		  while (1)
		    {
		      if (*ctx->re == L'i')
			{
			  DPRINT(("tre_parse:	    icase: '%.*" STRF "\n",
				  ctx->re_end - ctx->re, ctx->re));
			  if (bit)
			    new_cflags |= REG_ICASE;
			  else
			    new_cflags &= ~REG_ICASE;
			  ctx->re++;
			}
		      else if (*ctx->re == L'n')
			{
			  DPRINT(("tre_parse:	  newline: '%.*" STRF "\n",
				  ctx->re_end - ctx->re, ctx->re));
			  if (bit)
			    new_cflags |= REG_NEWLINE;
			  else
			    new_cflags &= ~REG_NEWLINE;
			  ctx->re++;
			}
#ifdef REG_RIGHT_ASSOC
		      else if (*ctx->re == L'r')
			{
			  DPRINT(("tre_parse: right assoc: '%.*" STRF "\n",
				  ctx->re_end - ctx->re, ctx->re));
			  if (bit)
			    new_cflags |= REG_RIGHT_ASSOC;
			  else
			    new_cflags &= ~REG_RIGHT_ASSOC;
			  ctx->re++;
			}
#endif /* REG_RIGHT_ASSOC */
#ifdef REG_UNGREEDY
		      else if (*ctx->re == L'U')
			{
			  DPRINT(("tre_parse:    ungreedy: '%.*" STRF "\n",
				  ctx->re_end - ctx->re, ctx->re));
			  if (bit)
			    new_cflags |= REG_UNGREEDY;
			  else
			    new_cflags &= ~REG_UNGREEDY;
			  ctx->re++;
			}
#endif /* REG_UNGREEDY */
		      else if (*ctx->re == CHAR_MINUS)
			{
			  DPRINT(("tre_parse:	 turn off: '%.*" STRF "\n",
				  ctx->re_end - ctx->re, ctx->re));
			  ctx->re++;
			  bit = 0;
			}
		      else if (*ctx->re == CHAR_COLON)
			{
			  DPRINT(("tre_parse:	 no group: '%.*" STRF "\n",
				  ctx->re_end - ctx->re, ctx->re));
			  ctx->re++;
			  depth++;
			  break;
			}
		      else if (*ctx->re == CHAR_HASH)
			{
			  DPRINT(("tre_parse:    comment: '%.*" STRF "\n",
				  ctx->re_end - ctx->re, ctx->re));
			  /* A comment can contain any character except a 
			     right parenthesis */
			  while (*ctx->re != CHAR_RPAREN
				 && ctx->re < ctx->re_end)
			    ctx->re++;
			  if (*ctx->re == CHAR_RPAREN && ctx->re < ctx->re_end)
			    {
			      ctx->re++;
			      break;
			    }
			  else
			    return REG_BADPAT;
			}
		      else if (*ctx->re == CHAR_RPAREN)
			{
			  ctx->re++;
			  break;
			}
		      else
			return REG_BADPAT;
		    }

		  /* Turn on the cflags changes for the rest of the
		     enclosing group. */
		  STACK_PUSHX(stack, int, ctx->cflags);
		  STACK_PUSHX(stack, int, PARSE_RESTORE_CFLAGS);
		  STACK_PUSHX(stack, int, PARSE_RE);
		  ctx->cflags = new_cflags;
		  break;
		}

	      if (ctx->cflags & REG_EXTENDED
		  || (ctx->re > ctx->re_start
		      && *(ctx->re - 1) == CHAR_BACKSLASH))
		{
		  depth++;
		  if (ctx->re + 2 < ctx->re_end
		      && *(ctx->re + 1) == CHAR_QUESTIONMARK
		      && *(ctx->re + 2) == CHAR_COLON)
		    {
		      DPRINT(("tre_parse: group begin: '%.*" STRF
			      "', no submatch\n",
			      ctx->re_end - ctx->re, ctx->re));
		      /* Don't mark for submatching. */
		      ctx->re += 3;
		      STACK_PUSHX(stack, int, PARSE_RE);
		    }
		  else
		    {
		      DPRINT(("tre_parse: group begin: '%.*" STRF
			      "', submatch %d\n",
			      ctx->re_end - ctx->re, ctx->re,
			      ctx->submatch_id));
		      ctx->re++;
		      /* First parse a whole RE, then mark the resulting tree
			 for submatching. */
		      STACK_PUSHX(stack, int, ctx->submatch_id);
		      STACK_PUSHX(stack, int, PARSE_MARK_FOR_SUBMATCH);
		      STACK_PUSHX(stack, int, PARSE_RE);
		      ctx->submatch_id++;
		    }
		}
	      else
		goto parse_literal;
	      break;

	    case CHAR_RPAREN:  /* end of current subexpression */
	      if ((ctx->cflags & REG_EXTENDED && depth > 0)
		  || (ctx->re > ctx->re_start
		      && *(ctx->re - 1) == CHAR_BACKSLASH))
		{
		  DPRINT(("tre_parse:	    empty: '%.*" STRF "'\n",
			  ctx->re_end - ctx->re, ctx->re));
		  /* We were expecting an atom, but instead the current
		     subexpression was closed.	POSIX leaves the meaning of
		     this to be implementation-defined.	 We interpret this as
		     an empty expression (which matches an empty string).  */
		  result = tre_ast_new_literal(ctx->mem, EMPTY, -1, -1);
		  if (result == NULL)
		    return REG_ESPACE;
		  if (!(ctx->cflags & REG_EXTENDED))
		    ctx->re--;
		}
	      else
		goto parse_literal;
	      break;

	    case CHAR_LBRACKET: /* bracket expression */
	      DPRINT(("tre_parse:     bracket: '%.*" STRF "'\n",
		      ctx->re_end - ctx->re, ctx->re));
	      ctx->re++;
	      status = tre_parse_bracket(ctx, &result);
	      if (status != REG_OK)
		return status;
	      break;

	    case CHAR_BACKSLASH:
	      /* If this is "\(" or "\)" chew off the backslash and
		 try again. */
	      if (!(ctx->cflags & REG_EXTENDED)
		  && ctx->re + 1 < ctx->re_end
		  && (*(ctx->re + 1) == CHAR_LPAREN
		      || *(ctx->re + 1) == CHAR_RPAREN))
		{
		  ctx->re++;
		  STACK_PUSHX(stack, int, PARSE_ATOM);
		  break;
		}

	      /* If a macro is used, parse the expanded macro recursively. */
	      {
		tre_char_t buf[64];
		tre_expand_macro(ctx->re + 1, ctx->re_end,
				 buf, elementsof(buf));
		if (buf[0] != 0)
		  {
		    tre_parse_ctx_t subctx;
		    memcpy(&subctx, ctx, sizeof(subctx));
		    subctx.re = buf;
		    subctx.len = tre_strlen(buf);
		    subctx.nofirstsub = 1;
		    status = tre_parse(&subctx);
		    if (status != REG_OK)
		      return status;
		    ctx->re += 2;
		    ctx->position = subctx.position;
		    result = subctx.result;
		    break;
		  }
	      }

	      if (ctx->re + 1 >= ctx->re_end)
		/* Trailing backslash. */
		return REG_EESCAPE;

#ifdef REG_LITERAL
	      if (*(ctx->re + 1) == L'Q')
		{
		  DPRINT(("tre_parse: tmp literal: '%.*" STRF "'\n",
			  ctx->re_end - ctx->re, ctx->re));
		  ctx->cflags |= REG_LITERAL;
		  temporary_cflags |= REG_LITERAL;
		  ctx->re += 2;
		  STACK_PUSHX(stack, int, PARSE_ATOM);
		  break;
		}
#endif /* REG_LITERAL */

	      DPRINT(("tre_parse:  bleep: '%.*" STRF "'\n",
		      ctx->re_end - ctx->re, ctx->re));
	      ctx->re++;
	      switch (*ctx->re)
		{
		case L'b':
		  result = tre_ast_new_literal(ctx->mem, ASSERTION,
					       ASSERT_AT_WB, -1);
		  ctx->re++;
		  break;
		case L'B':
		  result = tre_ast_new_literal(ctx->mem, ASSERTION,
					       ASSERT_AT_WB_NEG, -1);
		  ctx->re++;
		  break;
		case L'<':
		  result = tre_ast_new_literal(ctx->mem, ASSERTION,
					       ASSERT_AT_BOW, -1);
		  ctx->re++;
		  break;
		case L'>':
		  result = tre_ast_new_literal(ctx->mem, ASSERTION,
					       ASSERT_AT_EOW, -1);
		  ctx->re++;
		  break;
		case L'x':
		  ctx->re++;
		  if (ctx->re[0] != CHAR_LBRACE && ctx->re < ctx->re_end)
		    {
		      /* 8 bit hex char. */
		      char tmp[3] = {0, 0, 0};
		      long val;
		      DPRINT(("tre_parse:  8 bit hex: '%.*" STRF "'\n",
			      ctx->re_end - ctx->re + 2, ctx->re - 2));

		      if (tre_isxdigit(ctx->re[0]) && ctx->re < ctx->re_end)
			{
			  tmp[0] = (char)ctx->re[0];
			  ctx->re++;
			}
		      if (tre_isxdigit(ctx->re[0]) && ctx->re < ctx->re_end)
			{
			  tmp[1] = (char)ctx->re[0];
			  ctx->re++;
			}
		      val = strtol(tmp, NULL, 16);
		      result = tre_ast_new_literal(ctx->mem, val, val,
						   ctx->position);
		      ctx->position++;
		      break;
		    }
		  else if (ctx->re < ctx->re_end)
		    {
		      /* Wide char. */
		      char tmp[32];
		      long val;
		      int i = 0;
		      ctx->re++;
		      while (ctx->re_end - ctx->re >= 0)
			{
			  if (ctx->re[0] == CHAR_RBRACE)
			    break;
			  if (tre_isxdigit(ctx->re[0]))
			    {
			      tmp[i] = (char)ctx->re[0];
			      i++;
			      ctx->re++;
			      continue;
			    }
			  return REG_EBRACE;
			}
		      ctx->re++;
		      tmp[i] = 0;
		      val = strtol(tmp, NULL, 16);
		      result = tre_ast_new_literal(ctx->mem, val, val,
						   ctx->position);
		      ctx->position++;
		      break;
		    }

		default:
		  if (tre_isdigit(*ctx->re))
		    {
		      /* Back reference. */
		      int val = *ctx->re - L'0';
		      DPRINT(("tre_parse:     backref: '%.*" STRF "'\n",
			      ctx->re_end - ctx->re + 1, ctx->re - 1));
		      result = tre_ast_new_literal(ctx->mem, BACKREF, val,
						   ctx->position);
		      if (result == NULL)
			return REG_ESPACE;
		      ctx->position++;
		      ctx->max_backref = MAX(val, ctx->max_backref);
		      ctx->re++;
		    }
		  else
		    {
		      /* Escaped character. */
		      DPRINT(("tre_parse:     escaped: '%.*" STRF "'\n",
			      ctx->re_end - ctx->re + 1, ctx->re - 1));
		      result = tre_ast_new_literal(ctx->mem, *ctx->re, *ctx->re,
						   ctx->position);
		      ctx->position++;
		      ctx->re++;
		    }
		  break;
		}
	      if (result == NULL)
		return REG_ESPACE;
	      break;

	    case CHAR_PERIOD:	 /* the any-symbol */
	      DPRINT(("tre_parse:	  any: '%.*" STRF "'\n",
		      ctx->re_end - ctx->re, ctx->re));
	      if (ctx->cflags & REG_NEWLINE)
		{
		  tre_ast_node_t *tmp1;
		  tre_ast_node_t *tmp2;
		  tmp1 = tre_ast_new_literal(ctx->mem, 0, L'\n' - 1,
					     ctx->position);
		  if (!tmp1)
		    return REG_ESPACE;
		  tmp2 = tre_ast_new_literal(ctx->mem, L'\n' + 1, TRE_CHAR_MAX,
					     ctx->position + 1);
		  if (!tmp2)
		    return REG_ESPACE;
		  result = tre_ast_new_union(ctx->mem, tmp1, tmp2);
		  if (!result)
		    return REG_ESPACE;
		  ctx->position += 2;
		}
	      else
		{
		  result = tre_ast_new_literal(ctx->mem, 0, TRE_CHAR_MAX,
					       ctx->position);
		  if (!result)
		    return REG_ESPACE;
		  ctx->position++;
		}
	      ctx->re++;
	      break;

	    case CHAR_CARET:	 /* beginning of line assertion */
	      /* '^' has a special meaning everywhere in EREs, and in the
		 beginning of the RE and after \( is BREs. */
	      if (ctx->cflags & REG_EXTENDED
		  || (ctx->re - 2 >= ctx->re_start
		      && *(ctx->re - 2) == CHAR_BACKSLASH
		      && *(ctx->re - 1) == CHAR_LPAREN)
		  || ctx->re == ctx->re_start)
		{
		  DPRINT(("tre_parse:	      BOL: '%.*" STRF "'\n",
			  ctx->re_end - ctx->re, ctx->re));
		  result = tre_ast_new_literal(ctx->mem, ASSERTION,
					       ASSERT_AT_BOL, -1);
		  if (result == NULL)
		    return REG_ESPACE;
		  ctx->re++;
		}
	      else
		goto parse_literal;
	      break;

	    case CHAR_DOLLAR:	 /* end of line assertion. */
	      /* '$' is special everywhere in EREs, and in the end of the
		 string and before \) is BREs. */
	      if (ctx->cflags & REG_EXTENDED
		  || (ctx->re + 2 < ctx->re_end
		      && *(ctx->re + 1) == CHAR_BACKSLASH
		      && *(ctx->re + 2) == CHAR_RPAREN)
		  || ctx->re + 1 == ctx->re_end)
		{
		  DPRINT(("tre_parse:	      EOL: '%.*" STRF "'\n",
			  ctx->re_end - ctx->re, ctx->re));
		  result = tre_ast_new_literal(ctx->mem, ASSERTION,
					       ASSERT_AT_EOL, -1);
		  if (result == NULL)
		    return REG_ESPACE;
		  ctx->re++;
		}
	      else
		goto parse_literal;
	      break;

	    default:
	    parse_literal:

	      if (temporary_cflags && ctx->re + 1 < ctx->re_end
		  && *ctx->re == CHAR_BACKSLASH && *(ctx->re + 1) == L'E')
		{
		  DPRINT(("tre_parse:	 end tmps: '%.*" STRF "'\n",
			  ctx->re_end - ctx->re, ctx->re));
		  ctx->cflags &= ~temporary_cflags;
		  temporary_cflags = 0;
		  ctx->re += 2;
		  STACK_PUSHX(stack, int, PARSE_PIECE);
		  break;
		}


	      /* We are expecting an atom.  If the subexpression (or the whole
		 regexp ends here, we interpret it as an empty expression
		 (which matches an empty string).  */
	      if (
#ifdef REG_LITERAL
		  !(ctx->cflags & REG_LITERAL) &&
#endif /* REG_LITERAL */
		  (ctx->re >= ctx->re_end
		   || *ctx->re == CHAR_STAR
		   || (ctx->cflags & REG_EXTENDED
		       && (*ctx->re == CHAR_PIPE
			   || *ctx->re == CHAR_LBRACE
			   || *ctx->re == CHAR_PLUS
			   || *ctx->re == CHAR_QUESTIONMARK))
		   /* Test for "\)" in BRE mode. */
		   || (!(ctx->cflags & REG_EXTENDED)
		       && ctx->re + 1 < ctx->re_end
		       && *ctx->re == CHAR_BACKSLASH
		       && *(ctx->re + 1) == CHAR_LBRACE)))
		{
		  DPRINT(("tre_parse:	    empty: '%.*" STRF "'\n",
			  ctx->re_end - ctx->re, ctx->re));
		  result = tre_ast_new_literal(ctx->mem, EMPTY, -1, -1);
		  if (!result)
		    return REG_ESPACE;
		  break;
		}

	      DPRINT(("tre_parse:     literal: '%.*" STRF "'\n",
		      ctx->re_end - ctx->re, ctx->re));
	      /* Note that we can't use an tre_isalpha() test here, since there
		 may be characters which are alphabetic but neither upper or
		 lower case. */
	      if (ctx->cflags & REG_ICASE
		  && (tre_isupper(*ctx->re) || tre_islower(*ctx->re)))
		{
		  tre_ast_node_t *tmp1;
		  tre_ast_node_t *tmp2;

		  /* XXX - Can there be more than one opposite-case
		     counterpoints for some character in some locale?  Or
		     more than two characters which all should be regarded
		     the same character if case is ignored?  If yes, there
		     does not seem to be a portable way to detect it.  I guess
		     that at least for multi-character collating elements there
		     could be several opposite-case counterpoints, but they
		     cannot be supported portably anyway. */
		  tmp1 = tre_ast_new_literal(ctx->mem, tre_toupper(*ctx->re),
					     tre_toupper(*ctx->re),
					     ctx->position);
		  if (!tmp1)
		    return REG_ESPACE;
		  tmp2 = tre_ast_new_literal(ctx->mem, tre_tolower(*ctx->re),
					     tre_tolower(*ctx->re),
					     ctx->position);
		  if (!tmp2)
		    return REG_ESPACE;
		  result = tre_ast_new_union(ctx->mem, tmp1, tmp2);
		  if (!result)
		    return REG_ESPACE;
		}
	      else
		{
		  result = tre_ast_new_literal(ctx->mem, *ctx->re, *ctx->re,
					       ctx->position);
		  if (!result)
		    return REG_ESPACE;
		}
	      ctx->position++;
	      ctx->re++;
	      break;
	    }
	  break;

	case PARSE_MARK_FOR_SUBMATCH:
	  {
	    int submatch_id = tre_stack_pop_int(stack);

	    if (result->submatch_id >= 0)
	      {
		tre_ast_node_t *n, *tmp_node;
		n = tre_ast_new_literal(ctx->mem, EMPTY, -1, -1);
		if (n == NULL)
		  return REG_ESPACE;
		tmp_node = tre_ast_new_catenation(ctx->mem, n, result);
		if (tmp_node == NULL)
		  return REG_ESPACE;
		tmp_node->num_submatches = result->num_submatches;
		result = tmp_node;
	      }
	    result->submatch_id = submatch_id;
	    result->num_submatches++;
	    break;
	  }

	case PARSE_RESTORE_CFLAGS:
	  ctx->cflags = tre_stack_pop_int(stack);
	  break;
	}
    }

  /* Check for missing closing parentheses. */
  if (depth > 0)
    return REG_EPAREN;

  if (status == REG_OK)
    ctx->result = result;

  return status;
}

/* EOF */
