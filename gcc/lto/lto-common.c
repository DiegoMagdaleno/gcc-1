/* Top-level LTO routines.
   Copyright (C) 2009-2022 Free Software Foundation, Inc.
   Contributed by CodeSourcery, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "function.h"
#include "bitmap.h"
#include "basic-block.h"
#include "tree.h"
#include "gimple.h"
#include "cfghooks.h"
#include "alloc-pool.h"
#include "tree-pass.h"
#include "tree-streamer.h"
#include "cgraph.h"
#include "opts.h"
#include "toplev.h"
#include "stor-layout.h"
#include "symbol-summary.h"
#include "tree-vrp.h"
#include "ipa-prop.h"
#include "common.h"
#include "debug.h"
#include "lto.h"
#include "lto-section-names.h"
#include "splay-tree.h"
#include "lto-partition.h"
#include "context.h"
#include "pass_manager.h"
#include "ipa-fnsummary.h"
#include "ipa-utils.h"
#include "gomp-constants.h"
#include "lto-symtab.h"
#include "stringpool.h"
#include "fold-const.h"
#include "attribs.h"
#include "builtins.h"
#include "lto-common.h"
#include "tree-pretty-print.h"
#include "print-tree.h"

/* True when no new types are going to be streamd from the global stream.  */

static bool type_streaming_finished = false;

GTY(()) tree first_personality_decl;

GTY(()) const unsigned char *lto_mode_identity_table;

/* Returns a hash code for P.  */

static hashval_t
hash_name (const void *p)
{
  const struct lto_section_slot *ds = (const struct lto_section_slot *) p;
  return (hashval_t) htab_hash_string (ds->name);
}


/* Returns nonzero if P1 and P2 are equal.  */

static int
eq_name (const void *p1, const void *p2)
{
  const struct lto_section_slot *s1
    = (const struct lto_section_slot *) p1;
  const struct lto_section_slot *s2
    = (const struct lto_section_slot *) p2;

  return strcmp (s1->name, s2->name) == 0;
}

/* Free lto_section_slot.  */

static void
free_with_string (void *arg)
{
  struct lto_section_slot *s = (struct lto_section_slot *)arg;

  free (CONST_CAST (char *, s->name));
  free (arg);
}

/* Create section hash table.  */

htab_t
lto_obj_create_section_hash_table (void)
{
  return htab_create (37, hash_name, eq_name, free_with_string);
}

/* Delete an allocated integer KEY in the splay tree.  */

static void
lto_splay_tree_delete_id (splay_tree_key key)
{
  free ((void *) key);
}

/* Compare splay tree node ids A and B.  */

static int
lto_splay_tree_compare_ids (splay_tree_key a, splay_tree_key b)
{
  unsigned HOST_WIDE_INT ai;
  unsigned HOST_WIDE_INT bi;

  ai = *(unsigned HOST_WIDE_INT *) a;
  bi = *(unsigned HOST_WIDE_INT *) b;

  if (ai < bi)
    return -1;
  else if (ai > bi)
    return 1;
  return 0;
}

/* Look up splay tree node by ID in splay tree T.  */

static splay_tree_node
lto_splay_tree_lookup (splay_tree t, unsigned HOST_WIDE_INT id)
{
  return splay_tree_lookup (t, (splay_tree_key) &id);
}

/* Check if KEY has ID.  */

static bool
lto_splay_tree_id_equal_p (splay_tree_key key, unsigned HOST_WIDE_INT id)
{
  return *(unsigned HOST_WIDE_INT *) key == id;
}

/* Insert a splay tree node into tree T with ID as key and FILE_DATA as value.
   The ID is allocated separately because we need HOST_WIDE_INTs which may
   be wider than a splay_tree_key.  */

static void
lto_splay_tree_insert (splay_tree t, unsigned HOST_WIDE_INT id,
		       struct lto_file_decl_data *file_data)
{
  unsigned HOST_WIDE_INT *idp = XCNEW (unsigned HOST_WIDE_INT);
  *idp = id;
  splay_tree_insert (t, (splay_tree_key) idp, (splay_tree_value) file_data);
}

/* Create a splay tree.  */

static splay_tree
lto_splay_tree_new (void)
{
  return splay_tree_new (lto_splay_tree_compare_ids,
			 lto_splay_tree_delete_id,
			 NULL);
}

/* Decode the content of memory pointed to by DATA in the in decl
   state object STATE.  DATA_IN points to a data_in structure for
   decoding.  Return the address after the decoded object in the
   input.  */

static const uint32_t *
lto_read_in_decl_state (class data_in *data_in, const uint32_t *data,
			struct lto_in_decl_state *state)
{
  uint32_t ix;
  tree decl;
  uint32_t i, j;

  ix = *data++;
  state->compressed = ix & 1;
  ix /= 2;
  decl = streamer_tree_cache_get_tree (data_in->reader_cache, ix);
  if (!VAR_OR_FUNCTION_DECL_P (decl))
    {
      gcc_assert (decl == void_type_node);
      decl = NULL_TREE;
    }
  state->fn_decl = decl;

  for (i = 0; i < LTO_N_DECL_STREAMS; i++)
    {
      uint32_t size = *data++;
      vec<tree, va_gc> *decls = NULL;
      vec_alloc (decls, size);

      for (j = 0; j < size; j++)
	vec_safe_push (decls,
		       streamer_tree_cache_get_tree (data_in->reader_cache,
						     data[j]));

      state->streams[i] = decls;
      data += size;
    }

  return data;
}


/* Global canonical type table.  */
static htab_t gimple_canonical_types;
static hash_map<const_tree, hashval_t> *canonical_type_hash_cache;
static unsigned long num_canonical_type_hash_entries;
static unsigned long num_canonical_type_hash_queries;

/* Types postponed for registration to the canonical type table.
   During streaming we postpone all TYPE_CXX_ODR_P types so we can alter
   decide whether there is conflict with non-ODR type or not.  */
static GTY(()) vec<tree, va_gc> *types_to_register = NULL;

static void iterative_hash_canonical_type (tree type, inchash::hash &hstate);
static hashval_t gimple_canonical_type_hash (const void *p);
static hashval_t gimple_register_canonical_type_1 (tree t, hashval_t hash);

/* Returning a hash value for gimple type TYPE.

   The hash value returned is equal for types considered compatible
   by gimple_canonical_types_compatible_p.  */

static hashval_t
hash_canonical_type (tree type)
{
  inchash::hash hstate;
  enum tree_code code;

  /* We compute alias sets only for types that needs them.
     Be sure we do not recurse to something else as we cannot hash incomplete
     types in a way they would have same hash value as compatible complete
     types.  */
  gcc_checking_assert (type_with_alias_set_p (type));

  /* Combine a few common features of types so that types are grouped into
     smaller sets; when searching for existing matching types to merge,
     only existing types having the same features as the new type will be
     checked.  */
  code = tree_code_for_canonical_type_merging (TREE_CODE (type));
  hstate.add_int (code);
  hstate.add_int (TYPE_MODE (type));

  /* Incorporate common features of numerical types.  */
  if (INTEGRAL_TYPE_P (type)
      || SCALAR_FLOAT_TYPE_P (type)
      || FIXED_POINT_TYPE_P (type)
      || TREE_CODE (type) == OFFSET_TYPE
      || POINTER_TYPE_P (type))
    {
      hstate.add_int (TYPE_PRECISION (type));
      if (!type_with_interoperable_signedness (type))
	hstate.add_int (TYPE_UNSIGNED (type));
    }

  if (VECTOR_TYPE_P (type))
    {
      hstate.add_poly_int (TYPE_VECTOR_SUBPARTS (type));
      hstate.add_int (TYPE_UNSIGNED (type));
    }

  if (TREE_CODE (type) == COMPLEX_TYPE)
    hstate.add_int (TYPE_UNSIGNED (type));

  /* Fortran's C_SIGNED_CHAR is !TYPE_STRING_FLAG but needs to be
     interoperable with "signed char".  Unless all frontends are revisited to
     agree on these types, we must ignore the flag completely.  */

  /* Fortran standard define C_PTR type that is compatible with every
     C pointer.  For this reason we need to glob all pointers into one.
     Still pointers in different address spaces are not compatible.  */
  if (POINTER_TYPE_P (type))
    hstate.add_int (TYPE_ADDR_SPACE (TREE_TYPE (type)));

  /* For array types hash the domain bounds and the string flag.  */
  if (TREE_CODE (type) == ARRAY_TYPE && TYPE_DOMAIN (type))
    {
      hstate.add_int (TYPE_STRING_FLAG (type));
      /* OMP lowering can introduce error_mark_node in place of
	 random local decls in types.  */
      if (TYPE_MIN_VALUE (TYPE_DOMAIN (type)) != error_mark_node)
	inchash::add_expr (TYPE_MIN_VALUE (TYPE_DOMAIN (type)), hstate);
      if (TYPE_MAX_VALUE (TYPE_DOMAIN (type)) != error_mark_node)
	inchash::add_expr (TYPE_MAX_VALUE (TYPE_DOMAIN (type)), hstate);
    }

  /* Recurse for aggregates with a single element type.  */
  if (TREE_CODE (type) == ARRAY_TYPE
      || TREE_CODE (type) == COMPLEX_TYPE
      || TREE_CODE (type) == VECTOR_TYPE)
    iterative_hash_canonical_type (TREE_TYPE (type), hstate);

  /* Incorporate function return and argument types.  */
  if (TREE_CODE (type) == FUNCTION_TYPE || TREE_CODE (type) == METHOD_TYPE)
    {
      unsigned na;
      tree p;

      iterative_hash_canonical_type (TREE_TYPE (type), hstate);

      for (p = TYPE_ARG_TYPES (type), na = 0; p; p = TREE_CHAIN (p))
	{
	  iterative_hash_canonical_type (TREE_VALUE (p), hstate);
	  na++;
	}

      hstate.add_int (na);
    }

  if (RECORD_OR_UNION_TYPE_P (type))
    {
      unsigned nf;
      tree f;

      for (f = TYPE_FIELDS (type), nf = 0; f; f = TREE_CHAIN (f))
	if (TREE_CODE (f) == FIELD_DECL
	    && (! DECL_SIZE (f)
		|| ! integer_zerop (DECL_SIZE (f))))
	  {
	    iterative_hash_canonical_type (TREE_TYPE (f), hstate);
	    nf++;
	  }

      hstate.add_int (nf);
    }

  return hstate.end();
}

/* Returning a hash value for gimple type TYPE combined with VAL.  */

static void
iterative_hash_canonical_type (tree type, inchash::hash &hstate)
{
  hashval_t v;

  /* All type variants have same TYPE_CANONICAL.  */
  type = TYPE_MAIN_VARIANT (type);

  if (!canonical_type_used_p (type))
    v = hash_canonical_type (type);
  /* An already processed type.  */
  else if (TYPE_CANONICAL (type))
    {
      type = TYPE_CANONICAL (type);
      v = gimple_canonical_type_hash (type);
    }
  else
    {
      /* Canonical types should not be able to form SCCs by design, this
	 recursion is just because we do not register canonical types in
	 optimal order.  To avoid quadratic behavior also register the
	 type here.  */
      v = hash_canonical_type (type);
      v = gimple_register_canonical_type_1 (type, v);
    }
  hstate.merge_hash (v);
}

/* Returns the hash for a canonical type P.  */

static hashval_t
gimple_canonical_type_hash (const void *p)
{
  num_canonical_type_hash_queries++;
  hashval_t *slot = canonical_type_hash_cache->get ((const_tree) p);
  gcc_assert (slot != NULL);
  return *slot;
}



/* Returns nonzero if P1 and P2 are equal.  */

static int
gimple_canonical_type_eq (const void *p1, const void *p2)
{
  const_tree t1 = (const_tree) p1;
  const_tree t2 = (const_tree) p2;
  return gimple_canonical_types_compatible_p (CONST_CAST_TREE (t1),
					      CONST_CAST_TREE (t2));
}

/* Main worker for gimple_register_canonical_type.  */

static hashval_t
gimple_register_canonical_type_1 (tree t, hashval_t hash)
{
  void **slot;

  gcc_checking_assert (TYPE_P (t) && !TYPE_CANONICAL (t)
		       && type_with_alias_set_p (t)
		       && canonical_type_used_p (t));

  /* ODR types for which there is no ODR violation and we did not record
     structurally equivalent non-ODR type can be treated as unique by their
     name.

     hash passed to gimple_register_canonical_type_1 is a structural hash
     that we can use to lookup structurally equivalent non-ODR type.
     In case we decide to treat type as unique ODR type we recompute hash based
     on name and let TBAA machinery know about our decision.  */
  if (RECORD_OR_UNION_TYPE_P (t) && odr_type_p (t)
      && TYPE_CXX_ODR_P (t) && !odr_type_violation_reported_p (t))
    {
      /* Anonymous namespace types never conflict with non-C++ types.  */
      if (type_with_linkage_p (t) && type_in_anonymous_namespace_p (t))
	slot = NULL;
      else
	{
	  /* Here we rely on fact that all non-ODR types was inserted into
	     canonical type hash and thus we can safely detect conflicts between
	     ODR types and interoperable non-ODR types.  */
	  gcc_checking_assert (type_streaming_finished
			       && TYPE_MAIN_VARIANT (t) == t);
	  slot = htab_find_slot_with_hash (gimple_canonical_types, t, hash,
					   NO_INSERT);
	}
      if (slot && !TYPE_CXX_ODR_P (*(tree *)slot))
	{
	  tree nonodr = *(tree *)slot;
	  gcc_checking_assert (!flag_ltrans);
	  if (symtab->dump_file)
	    {
	      fprintf (symtab->dump_file,
		       "ODR and non-ODR type conflict: ");
	      print_generic_expr (symtab->dump_file, t);
	      fprintf (symtab->dump_file, " and ");
	      print_generic_expr (symtab->dump_file, nonodr);
	      fprintf (symtab->dump_file, " mangled:%s\n",
			 IDENTIFIER_POINTER
			   (DECL_ASSEMBLER_NAME (TYPE_NAME (t))));
	    }
	  /* Set canonical for T and all other ODR equivalent duplicates
	     including incomplete structures.  */
	  set_type_canonical_for_odr_type (t, nonodr);
	}
      else
	{
	  tree prevail = prevailing_odr_type (t);

	  if (symtab->dump_file)
	    {
	      fprintf (symtab->dump_file,
		       "New canonical ODR type: ");
	      print_generic_expr (symtab->dump_file, t);
	      fprintf (symtab->dump_file, " mangled:%s\n",
			 IDENTIFIER_POINTER
			   (DECL_ASSEMBLER_NAME (TYPE_NAME (t))));
	    }
	  /* Set canonical for T and all other ODR equivalent duplicates
	     including incomplete structures.  */
	  set_type_canonical_for_odr_type (t, prevail);
	  enable_odr_based_tbaa (t);
	  if (!type_in_anonymous_namespace_p (t))
	    hash = htab_hash_string (IDENTIFIER_POINTER
					   (DECL_ASSEMBLER_NAME
						   (TYPE_NAME (t))));
	  else
	    hash = TYPE_UID (t);

	  /* All variants of t now have TYPE_CANONICAL set to prevail.
	     Update canonical type hash cache accordingly.  */
	  num_canonical_type_hash_entries++;
	  bool existed_p = canonical_type_hash_cache->put (prevail, hash);
	  gcc_checking_assert (!existed_p);
	}
      return hash;
    }

  slot = htab_find_slot_with_hash (gimple_canonical_types, t, hash, INSERT);
  if (*slot)
    {
      tree new_type = (tree)(*slot);
      gcc_checking_assert (new_type != t);
      TYPE_CANONICAL (t) = new_type;
    }
  else
    {
      TYPE_CANONICAL (t) = t;
      *slot = (void *) t;
      /* Cache the just computed hash value.  */
      num_canonical_type_hash_entries++;
      bool existed_p = canonical_type_hash_cache->put (t, hash);
      gcc_assert (!existed_p);
    }
  return hash;
}

/* Register type T in the global type table gimple_types and set
   TYPE_CANONICAL of T accordingly.
   This is used by LTO to merge structurally equivalent types for
   type-based aliasing purposes across different TUs and languages.

   ???  This merging does not exactly match how the tree.c middle-end
   functions will assign TYPE_CANONICAL when new types are created
   during optimization (which at least happens for pointer and array
   types).  */

static void
gimple_register_canonical_type (tree t)
{
  if (TYPE_CANONICAL (t) || !type_with_alias_set_p (t)
      || !canonical_type_used_p (t))
    return;

  /* Canonical types are same among all complete variants.  */
  if (TYPE_CANONICAL (TYPE_MAIN_VARIANT (t)))
    TYPE_CANONICAL (t) = TYPE_CANONICAL (TYPE_MAIN_VARIANT (t));
  else
    {
      hashval_t h = hash_canonical_type (TYPE_MAIN_VARIANT (t));
      gimple_register_canonical_type_1 (TYPE_MAIN_VARIANT (t), h);
      TYPE_CANONICAL (t) = TYPE_CANONICAL (TYPE_MAIN_VARIANT (t));
    }
}

/* Re-compute TYPE_CANONICAL for NODE and related types.  */

static void
lto_register_canonical_types (tree node, bool first_p)
{
  if (!node
      || !TYPE_P (node))
    return;

  if (first_p)
    TYPE_CANONICAL (node) = NULL_TREE;

  if (POINTER_TYPE_P (node)
      || TREE_CODE (node) == COMPLEX_TYPE
      || TREE_CODE (node) == ARRAY_TYPE)
    lto_register_canonical_types (TREE_TYPE (node), first_p);

 if (!first_p)
    gimple_register_canonical_type (node);
}

/* Finish canonical type calculation: after all units has been streamed in we
   can check if given ODR type structurally conflicts with a non-ODR type.  In
   the first case we set type canonical according to the canonical type hash.
   In the second case we use type names.  */

static void
lto_register_canonical_types_for_odr_types ()
{
  tree t;
  unsigned int i;

  if (!types_to_register)
    return;

  type_streaming_finished = true;

  /* Be sure that no types derived from ODR types was
     not inserted into the hash table.  */
  if (flag_checking)
    FOR_EACH_VEC_ELT (*types_to_register, i, t)
      gcc_assert (!TYPE_CANONICAL (t));

  /* Register all remaining types.  */
  FOR_EACH_VEC_ELT (*types_to_register, i, t)
    {
      /* For pre-streamed types like va-arg it is possible that main variant
	 is !CXX_ODR_P while the variant (which is streamed) is.
	 Copy CXX_ODR_P to make type verifier happy.  This is safe because
	 in canonical type calculation we only consider main variants.
	 However we can not change this flag before streaming is finished
	 to not affect tree merging.  */
      TYPE_CXX_ODR_P (t) = TYPE_CXX_ODR_P (TYPE_MAIN_VARIANT (t));
      if (!TYPE_CANONICAL (t))
        gimple_register_canonical_type (t);
    }
}


/* Remember trees that contains references to declarations.  */
vec <tree, va_gc> *tree_with_vars;

#define CHECK_VAR(tt) \
  do \
    { \
      if ((tt) && VAR_OR_FUNCTION_DECL_P (tt) \
	  && (TREE_PUBLIC (tt) || DECL_EXTERNAL (tt))) \
	return true; \
    } while (0)

#define CHECK_NO_VAR(tt) \
  gcc_checking_assert (!(tt) || !VAR_OR_FUNCTION_DECL_P (tt))

/* Check presence of pointers to decls in fields of a tree_typed T.  */

static inline bool
mentions_vars_p_typed (tree t)
{
  CHECK_NO_VAR (TREE_TYPE (t));
  return false;
}

/* Check presence of pointers to decls in fields of a tree_common T.  */

static inline bool
mentions_vars_p_common (tree t)
{
  if (mentions_vars_p_typed (t))
    return true;
  CHECK_NO_VAR (TREE_CHAIN (t));
  return false;
}

/* Check presence of pointers to decls in fields of a decl_minimal T.  */

static inline bool
mentions_vars_p_decl_minimal (tree t)
{
  if (mentions_vars_p_common (t))
    return true;
  CHECK_NO_VAR (DECL_NAME (t));
  CHECK_VAR (DECL_CONTEXT (t));
  return false;
}

/* Check presence of pointers to decls in fields of a decl_common T.  */

static inline bool
mentions_vars_p_decl_common (tree t)
{
  if (mentions_vars_p_decl_minimal (t))
    return true;
  CHECK_VAR (DECL_SIZE (t));
  CHECK_VAR (DECL_SIZE_UNIT (t));
  CHECK_VAR (DECL_INITIAL (t));
  CHECK_NO_VAR (DECL_ATTRIBUTES (t));
  CHECK_VAR (DECL_ABSTRACT_ORIGIN (t));
  return false;
}

/* Check presence of pointers to decls in fields of a decl_with_vis T.  */

static inline bool
mentions_vars_p_decl_with_vis (tree t)
{
  if (mentions_vars_p_decl_common (t))
    return true;

  /* Accessor macro has side-effects, use field-name here.  */
  CHECK_NO_VAR (DECL_ASSEMBLER_NAME_RAW (t));
  return false;
}

/* Check presence of pointers to decls in fields of a decl_non_common T.  */

static inline bool
mentions_vars_p_decl_non_common (tree t)
{
  if (mentions_vars_p_decl_with_vis (t))
    return true;
  CHECK_NO_VAR (DECL_RESULT_FLD (t));
  return false;
}

/* Check presence of pointers to decls in fields of a decl_non_common T.  */

static bool
mentions_vars_p_function (tree t)
{
  if (mentions_vars_p_decl_non_common (t))
    return true;
  CHECK_NO_VAR (DECL_ARGUMENTS (t));
  CHECK_NO_VAR (DECL_VINDEX (t));
  CHECK_VAR (DECL_FUNCTION_PERSONALITY (t));
  return false;
}

/* Check presence of pointers to decls in fields of a field_decl T.  */

static bool
mentions_vars_p_field_decl (tree t)
{
  if (mentions_vars_p_decl_common (t))
    return true;
  CHECK_VAR (DECL_FIELD_OFFSET (t));
  CHECK_NO_VAR (DECL_BIT_FIELD_TYPE (t));
  CHECK_NO_VAR (DECL_QUALIFIER (t));
  CHECK_NO_VAR (DECL_FIELD_BIT_OFFSET (t));
  CHECK_NO_VAR (DECL_FCONTEXT (t));
  return false;
}

/* Check presence of pointers to decls in fields of a type T.  */

static bool
mentions_vars_p_type (tree t)
{
  if (mentions_vars_p_common (t))
    return true;
  CHECK_NO_VAR (TYPE_CACHED_VALUES (t));
  CHECK_VAR (TYPE_SIZE (t));
  CHECK_VAR (TYPE_SIZE_UNIT (t));
  CHECK_NO_VAR (TYPE_ATTRIBUTES (t));
  CHECK_NO_VAR (TYPE_NAME (t));

  CHECK_VAR (TYPE_MIN_VALUE_RAW (t));
  CHECK_VAR (TYPE_MAX_VALUE_RAW (t));

  /* Accessor is for derived node types only.  */
  CHECK_NO_VAR (TYPE_LANG_SLOT_1 (t));

  CHECK_VAR (TYPE_CONTEXT (t));
  CHECK_NO_VAR (TYPE_CANONICAL (t));
  CHECK_NO_VAR (TYPE_MAIN_VARIANT (t));
  CHECK_NO_VAR (TYPE_NEXT_VARIANT (t));
  return false;
}

/* Check presence of pointers to decls in fields of a BINFO T.  */

static bool
mentions_vars_p_binfo (tree t)
{
  unsigned HOST_WIDE_INT i, n;

  if (mentions_vars_p_common (t))
    return true;
  CHECK_VAR (BINFO_VTABLE (t));
  CHECK_NO_VAR (BINFO_OFFSET (t));
  CHECK_NO_VAR (BINFO_VIRTUALS (t));
  CHECK_NO_VAR (BINFO_VPTR_FIELD (t));
  n = vec_safe_length (BINFO_BASE_ACCESSES (t));
  for (i = 0; i < n; i++)
    CHECK_NO_VAR (BINFO_BASE_ACCESS (t, i));
  /* Do not walk BINFO_INHERITANCE_CHAIN, BINFO_SUBVTT_INDEX
     and BINFO_VPTR_INDEX; these are used by C++ FE only.  */
  n = BINFO_N_BASE_BINFOS (t);
  for (i = 0; i < n; i++)
    CHECK_NO_VAR (BINFO_BASE_BINFO (t, i));
  return false;
}

/* Check presence of pointers to decls in fields of a CONSTRUCTOR T.  */

static bool
mentions_vars_p_constructor (tree t)
{
  unsigned HOST_WIDE_INT idx;
  constructor_elt *ce;

  if (mentions_vars_p_typed (t))
    return true;

  for (idx = 0; vec_safe_iterate (CONSTRUCTOR_ELTS (t), idx, &ce); idx++)
    {
      CHECK_NO_VAR (ce->index);
      CHECK_VAR (ce->value);
    }
  return false;
}

/* Check presence of pointers to decls in fields of an expression tree T.  */

static bool
mentions_vars_p_expr (tree t)
{
  int i;
  if (mentions_vars_p_typed (t))
    return true;
  for (i = TREE_OPERAND_LENGTH (t) - 1; i >= 0; --i)
    CHECK_VAR (TREE_OPERAND (t, i));
  return false;
}

/* Check presence of pointers to decls in fields of an OMP_CLAUSE T.  */

static bool
mentions_vars_p_omp_clause (tree t)
{
  int i;
  if (mentions_vars_p_common (t))
    return true;
  for (i = omp_clause_num_ops[OMP_CLAUSE_CODE (t)] - 1; i >= 0; --i)
    CHECK_VAR (OMP_CLAUSE_OPERAND (t, i));
  return false;
}

/* Check presence of pointers to decls that needs later fixup in T.  */

static bool
mentions_vars_p (tree t)
{
  switch (TREE_CODE (t))
    {
    case IDENTIFIER_NODE:
      break;

    case TREE_LIST:
      CHECK_VAR (TREE_VALUE (t));
      CHECK_VAR (TREE_PURPOSE (t));
      CHECK_NO_VAR (TREE_CHAIN (t));
      break;

    case FIELD_DECL:
      return mentions_vars_p_field_decl (t);

    case LABEL_DECL:
    case CONST_DECL:
    case PARM_DECL:
    case RESULT_DECL:
    case IMPORTED_DECL:
    case NAMESPACE_DECL:
    case NAMELIST_DECL:
      return mentions_vars_p_decl_common (t);

    case VAR_DECL:
      return mentions_vars_p_decl_with_vis (t);

    case TYPE_DECL:
      return mentions_vars_p_decl_non_common (t);

    case FUNCTION_DECL:
      return mentions_vars_p_function (t);

    case TREE_BINFO:
      return mentions_vars_p_binfo (t);

    case PLACEHOLDER_EXPR:
      return mentions_vars_p_common (t);

    case BLOCK:
    case TRANSLATION_UNIT_DECL:
    case OPTIMIZATION_NODE:
    case TARGET_OPTION_NODE:
      break;

    case CONSTRUCTOR:
      return mentions_vars_p_constructor (t);

    case OMP_CLAUSE:
      return mentions_vars_p_omp_clause (t);

    default:
      if (TYPE_P (t))
	{
	  if (mentions_vars_p_type (t))
	    return true;
	}
      else if (EXPR_P (t))
	{
	  if (mentions_vars_p_expr (t))
	    return true;
	}
      else if (CONSTANT_CLASS_P (t))
	CHECK_NO_VAR (TREE_TYPE (t));
      else
	gcc_unreachable ();
    }
  return false;
}


/* Return the resolution for the decl with index INDEX from DATA_IN.  */

static enum ld_plugin_symbol_resolution
get_resolution (class data_in *data_in, unsigned index)
{
  if (data_in->globals_resolution.exists ())
    {
      ld_plugin_symbol_resolution_t ret;
      /* We can have references to not emitted functions in
	 DECL_FUNCTION_PERSONALITY at least.  So we can and have
	 to indeed return LDPR_UNKNOWN in some cases.  */
      if (data_in->globals_resolution.length () <= index)
	return LDPR_UNKNOWN;
      ret = data_in->globals_resolution[index];
      return ret;
    }
  else
    /* Delay resolution finding until decl merging.  */
    return LDPR_UNKNOWN;
}

/* We need to record resolutions until symbol table is read.  */
static void
register_resolution (struct lto_file_decl_data *file_data, tree decl,
		     enum ld_plugin_symbol_resolution resolution)
{
  bool existed;
  if (resolution == LDPR_UNKNOWN)
    return;
  if (!file_data->resolution_map)
    file_data->resolution_map
      = new hash_map<tree, ld_plugin_symbol_resolution>;
  ld_plugin_symbol_resolution_t &res
     = file_data->resolution_map->get_or_insert (decl, &existed);
  if (!existed
      || resolution == LDPR_PREVAILING_DEF_IRONLY
      || resolution == LDPR_PREVAILING_DEF
      || resolution == LDPR_PREVAILING_DEF_IRONLY_EXP)
    res = resolution;
}

/* Register DECL with the global symbol table and change its
   name if necessary to avoid name clashes for static globals across
   different files.  */

static void
lto_register_var_decl_in_symtab (class data_in *data_in, tree decl,
				 unsigned ix)
{
  tree context;

  /* Variable has file scope, not local.  */
  if (!TREE_PUBLIC (decl)
      && !((context = decl_function_context (decl))
	   && auto_var_in_fn_p (decl, context)))
    rest_of_decl_compilation (decl, 1, 0);

  /* If this variable has already been declared, queue the
     declaration for merging.  */
  if (TREE_PUBLIC (decl))
    register_resolution (data_in->file_data,
			 decl, get_resolution (data_in, ix));
}


/* Register DECL with the global symbol table and change its
   name if necessary to avoid name clashes for static globals across
   different files.  DATA_IN contains descriptors and tables for the
   file being read.  */

static void
lto_register_function_decl_in_symtab (class data_in *data_in, tree decl,
				      unsigned ix)
{
  /* If this variable has already been declared, queue the
     declaration for merging.  */
  if (TREE_PUBLIC (decl) && !DECL_ABSTRACT_P (decl))
    register_resolution (data_in->file_data,
			 decl, get_resolution (data_in, ix));
}

/* Check if T is a decl and needs register its resolution info.  */

static void
lto_maybe_register_decl (class data_in *data_in, tree t, unsigned ix)
{
  if (TREE_CODE (t) == VAR_DECL)
    lto_register_var_decl_in_symtab (data_in, t, ix);
  else if (TREE_CODE (t) == FUNCTION_DECL
	   && !fndecl_built_in_p (t))
    lto_register_function_decl_in_symtab (data_in, t, ix);
}


/* For the type T re-materialize it in the type variant list and
   the pointer/reference-to chains.  */

static void
lto_fixup_prevailing_type (tree t)
{
  /* The following re-creates proper variant lists while fixing up
     the variant leaders.  We do not stream TYPE_NEXT_VARIANT so the
     variant list state before fixup is broken.  */

  /* If we are not our own variant leader link us into our new leaders
     variant list.  */
  if (TYPE_MAIN_VARIANT (t) != t)
    {
      tree mv = TYPE_MAIN_VARIANT (t);
      TYPE_NEXT_VARIANT (t) = TYPE_NEXT_VARIANT (mv);
      TYPE_NEXT_VARIANT (mv) = t;
    }

  /* The following reconstructs the pointer chains
     of the new pointed-to type if we are a main variant.  We do
     not stream those so they are broken before fixup.  */
  if (TREE_CODE (t) == POINTER_TYPE
      && TYPE_MAIN_VARIANT (t) == t)
    {
      TYPE_NEXT_PTR_TO (t) = TYPE_POINTER_TO (TREE_TYPE (t));
      TYPE_POINTER_TO (TREE_TYPE (t)) = t;
    }
  else if (TREE_CODE (t) == REFERENCE_TYPE
	   && TYPE_MAIN_VARIANT (t) == t)
    {
      TYPE_NEXT_REF_TO (t) = TYPE_REFERENCE_TO (TREE_TYPE (t));
      TYPE_REFERENCE_TO (TREE_TYPE (t)) = t;
    }
}


/* We keep prevailing tree SCCs in a hashtable with manual collision
   handling (in case all hashes compare the same) and keep the colliding
   entries in the tree_scc->next chain.  */

struct tree_scc
{
  tree_scc *next;
  /* Hash of the whole SCC.  */
  hashval_t hash;
  /* Number of trees in the SCC.  */
  unsigned len;
  /* Number of possible entries into the SCC (tree nodes [0..entry_len-1]
     which share the same individual tree hash).  */
  unsigned entry_len;
  /* The members of the SCC.
     We only need to remember the first entry node candidate for prevailing
     SCCs (but of course have access to all entries for SCCs we are
     processing).
     ???  For prevailing SCCs we really only need hash and the first
     entry candidate, but that's too awkward to implement.  */
  tree entries[1];
};

struct tree_scc_hasher : nofree_ptr_hash <tree_scc>
{
  static inline hashval_t hash (const tree_scc *);
  static inline bool equal (const tree_scc *, const tree_scc *);
};

hashval_t
tree_scc_hasher::hash (const tree_scc *scc)
{
  return scc->hash;
}

bool
tree_scc_hasher::equal (const tree_scc *scc1, const tree_scc *scc2)
{
  if (scc1->hash != scc2->hash
      || scc1->len != scc2->len
      || scc1->entry_len != scc2->entry_len)
    return false;
  return true;
}

static hash_table<tree_scc_hasher> *tree_scc_hash;
static struct obstack tree_scc_hash_obstack;

static unsigned long num_merged_types;
static unsigned long num_prevailing_types;
static unsigned long num_type_scc_trees;
static unsigned long total_scc_size;
static unsigned long num_sccs_read;
static unsigned long num_unshared_trees_read;
static unsigned long total_scc_size_merged;
static unsigned long num_sccs_merged;
static unsigned long num_scc_compares;
static unsigned long num_scc_compare_collisions;


/* Compare the two entries T1 and T2 of two SCCs that are possibly equal,
   recursing through in-SCC tree edges.  Returns true if the SCCs entered
   through T1 and T2 are equal and fills in *MAP with the pairs of
   SCC entries we visited, starting with (*MAP)[0] = T1 and (*MAP)[1] = T2.  */

static bool
compare_tree_sccs_1 (tree t1, tree t2, tree **map)
{
  enum tree_code code;

  /* Mark already visited nodes.  */
  TREE_ASM_WRITTEN (t2) = 1;

  /* Push the pair onto map.  */
  (*map)[0] = t1;
  (*map)[1] = t2;
  *map = *map + 2;

  /* Compare value-fields.  */
#define compare_values(X) \
  do { \
    if (X(t1) != X(t2)) \
      return false; \
  } while (0)

  compare_values (TREE_CODE);
  code = TREE_CODE (t1);

  /* If we end up comparing translation unit decls we either forgot to mark
     some SCC as local or we compare too much.  */
  gcc_checking_assert (code != TRANSLATION_UNIT_DECL);

  if (!TYPE_P (t1))
    {
      compare_values (TREE_SIDE_EFFECTS);
      compare_values (TREE_CONSTANT);
      compare_values (TREE_READONLY);
      compare_values (TREE_PUBLIC);
    }
  compare_values (TREE_ADDRESSABLE);
  compare_values (TREE_THIS_VOLATILE);
  if (DECL_P (t1))
    compare_values (DECL_UNSIGNED);
  else if (TYPE_P (t1))
    compare_values (TYPE_UNSIGNED);
  if (TYPE_P (t1))
    compare_values (TYPE_ARTIFICIAL);
  else
    compare_values (TREE_NO_WARNING);
  compare_values (TREE_NOTHROW);
  compare_values (TREE_STATIC);
  if (code != TREE_BINFO)
    compare_values (TREE_PRIVATE);
  compare_values (TREE_PROTECTED);
  compare_values (TREE_DEPRECATED);
  if (TYPE_P (t1))
    {
      if (AGGREGATE_TYPE_P (t1))
	compare_values (TYPE_REVERSE_STORAGE_ORDER);
      else
	compare_values (TYPE_SATURATING);
      compare_values (TYPE_ADDR_SPACE);
    }
  else if (code == SSA_NAME)
    compare_values (SSA_NAME_IS_DEFAULT_DEF);

  if (CODE_CONTAINS_STRUCT (code, TS_INT_CST))
    {
      if (wi::to_wide (t1) != wi::to_wide (t2))
	return false;
    }

  if (CODE_CONTAINS_STRUCT (code, TS_REAL_CST))
    {
      /* ???  No suitable compare routine available.  */
      REAL_VALUE_TYPE r1 = TREE_REAL_CST (t1);
      REAL_VALUE_TYPE r2 = TREE_REAL_CST (t2);
      if (r1.cl != r2.cl
	  || r1.decimal != r2.decimal
	  || r1.sign != r2.sign
	  || r1.signalling != r2.signalling
	  || r1.canonical != r2.canonical
	  || r1.uexp != r2.uexp)
	return false;
      for (unsigned i = 0; i < SIGSZ; ++i)
	if (r1.sig[i] != r2.sig[i])
	  return false;
    }

  if (CODE_CONTAINS_STRUCT (code, TS_FIXED_CST))
    if (!fixed_compare (EQ_EXPR,
			TREE_FIXED_CST_PTR (t1), TREE_FIXED_CST_PTR (t2)))
      return false;

  if (CODE_CONTAINS_STRUCT (code, TS_VECTOR))
    {
      compare_values (VECTOR_CST_LOG2_NPATTERNS);
      compare_values (VECTOR_CST_NELTS_PER_PATTERN);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_COMMON))
    {
      compare_values (DECL_MODE);
      compare_values (DECL_NONLOCAL);
      compare_values (DECL_VIRTUAL_P);
      compare_values (DECL_IGNORED_P);
      compare_values (DECL_ABSTRACT_P);
      compare_values (DECL_ARTIFICIAL);
      compare_values (DECL_USER_ALIGN);
      compare_values (DECL_PRESERVE_P);
      compare_values (DECL_EXTERNAL);
      compare_values (DECL_NOT_GIMPLE_REG_P);
      compare_values (DECL_ALIGN);
      if (code == LABEL_DECL)
	{
	  compare_values (EH_LANDING_PAD_NR);
	  compare_values (LABEL_DECL_UID);
	}
      else if (code == FIELD_DECL)
	{
	  compare_values (DECL_PACKED);
	  compare_values (DECL_NONADDRESSABLE_P);
	  compare_values (DECL_PADDING_P);
	  compare_values (DECL_FIELD_ABI_IGNORED);
	  compare_values (DECL_FIELD_CXX_ZERO_WIDTH_BIT_FIELD);
	  compare_values (DECL_OFFSET_ALIGN);
	}
      else if (code == VAR_DECL)
	{
	  compare_values (DECL_HAS_DEBUG_EXPR_P);
	  compare_values (DECL_NONLOCAL_FRAME);
	}
      if (code == RESULT_DECL
	  || code == PARM_DECL
	  || code == VAR_DECL)
	{
	  compare_values (DECL_BY_REFERENCE);
	  if (code == VAR_DECL
	      || code == PARM_DECL)
	    compare_values (DECL_HAS_VALUE_EXPR_P);
	}
    }

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_WRTL))
    compare_values (DECL_REGISTER);

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_WITH_VIS))
    {
      compare_values (DECL_COMMON);
      compare_values (DECL_DLLIMPORT_P);
      compare_values (DECL_WEAK);
      compare_values (DECL_SEEN_IN_BIND_EXPR_P);
      compare_values (DECL_COMDAT);
      compare_values (DECL_VISIBILITY);
      compare_values (DECL_VISIBILITY_SPECIFIED);
      if (code == VAR_DECL)
	{
	  compare_values (DECL_HARD_REGISTER);
	  /* DECL_IN_TEXT_SECTION is set during final asm output only.  */
	  compare_values (DECL_IN_CONSTANT_POOL);
	}
    }

  if (CODE_CONTAINS_STRUCT (code, TS_FUNCTION_DECL))
    {
      compare_values (DECL_BUILT_IN_CLASS);
      compare_values (DECL_STATIC_CONSTRUCTOR);
      compare_values (DECL_STATIC_DESTRUCTOR);
      compare_values (DECL_UNINLINABLE);
      compare_values (DECL_POSSIBLY_INLINED);
      compare_values (DECL_IS_NOVOPS);
      compare_values (DECL_IS_RETURNS_TWICE);
      compare_values (DECL_IS_MALLOC);
      compare_values (FUNCTION_DECL_DECL_TYPE);
      compare_values (DECL_DECLARED_INLINE_P);
      compare_values (DECL_STATIC_CHAIN);
      compare_values (DECL_NO_INLINE_WARNING_P);
      compare_values (DECL_NO_INSTRUMENT_FUNCTION_ENTRY_EXIT);
      compare_values (DECL_NO_LIMIT_STACK);
      compare_values (DECL_DISREGARD_INLINE_LIMITS);
      compare_values (DECL_PURE_P);
      compare_values (DECL_LOOPING_CONST_OR_PURE_P);
      compare_values (DECL_IS_REPLACEABLE_OPERATOR);
      compare_values (DECL_FINAL_P);
      compare_values (DECL_CXX_CONSTRUCTOR_P);
      compare_values (DECL_CXX_DESTRUCTOR_P);
      if (DECL_BUILT_IN_CLASS (t1) != NOT_BUILT_IN)
	compare_values (DECL_UNCHECKED_FUNCTION_CODE);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_TYPE_COMMON))
    {
      compare_values (TYPE_MODE);
      compare_values (TYPE_NEEDS_CONSTRUCTING);
      if (RECORD_OR_UNION_TYPE_P (t1))
	{
	  compare_values (TYPE_TRANSPARENT_AGGR);
	  compare_values (TYPE_FINAL_P);
          compare_values (TYPE_CXX_ODR_P);
	}
      else if (code == ARRAY_TYPE)
	compare_values (TYPE_NONALIASED_COMPONENT);
      if (code == ARRAY_TYPE || code == INTEGER_TYPE)
        compare_values (TYPE_STRING_FLAG);
      if (AGGREGATE_TYPE_P (t1))
	compare_values (TYPE_TYPELESS_STORAGE);
      compare_values (TYPE_EMPTY_P);
      compare_values (TYPE_PACKED);
      compare_values (TYPE_RESTRICT);
      compare_values (TYPE_USER_ALIGN);
      compare_values (TYPE_READONLY);
      compare_values (TYPE_PRECISION);
      compare_values (TYPE_ALIGN);
      /* Do not compare TYPE_ALIAS_SET.  Doing so introduce ordering issues
	 with calls to get_alias_set which may initialize it for streamed
	 in types.  */
    }

  /* We don't want to compare locations, so there is nothing do compare
     for TS_EXP.  */

  /* BLOCKs are function local and we don't merge anything there, so
     simply refuse to merge.  */
  if (CODE_CONTAINS_STRUCT (code, TS_BLOCK))
    return false;

  if (CODE_CONTAINS_STRUCT (code, TS_TRANSLATION_UNIT_DECL))
    if (strcmp (TRANSLATION_UNIT_LANGUAGE (t1),
		TRANSLATION_UNIT_LANGUAGE (t2)) != 0)
      return false;

  if (CODE_CONTAINS_STRUCT (code, TS_TARGET_OPTION))
    if (!cl_target_option_eq (TREE_TARGET_OPTION (t1), TREE_TARGET_OPTION (t2)))
      return false;

  if (CODE_CONTAINS_STRUCT (code, TS_OPTIMIZATION))
    if (!cl_optimization_option_eq (TREE_OPTIMIZATION (t1),
				    TREE_OPTIMIZATION (t2)))
      return false;

  if (CODE_CONTAINS_STRUCT (code, TS_BINFO))
    if (vec_safe_length (BINFO_BASE_ACCESSES (t1))
	!= vec_safe_length (BINFO_BASE_ACCESSES (t2)))
      return false;

  if (CODE_CONTAINS_STRUCT (code, TS_CONSTRUCTOR))
    compare_values (CONSTRUCTOR_NELTS);

  if (CODE_CONTAINS_STRUCT (code, TS_IDENTIFIER))
    if (IDENTIFIER_LENGTH (t1) != IDENTIFIER_LENGTH (t2)
	|| memcmp (IDENTIFIER_POINTER (t1), IDENTIFIER_POINTER (t2),
		   IDENTIFIER_LENGTH (t1)) != 0)
      return false;

  if (CODE_CONTAINS_STRUCT (code, TS_STRING))
    if (TREE_STRING_LENGTH (t1) != TREE_STRING_LENGTH (t2)
	|| memcmp (TREE_STRING_POINTER (t1), TREE_STRING_POINTER (t2),
		   TREE_STRING_LENGTH (t1)) != 0)
      return false;

  if (code == OMP_CLAUSE)
    {
      compare_values (OMP_CLAUSE_CODE);
      switch (OMP_CLAUSE_CODE (t1))
	{
	case OMP_CLAUSE_DEFAULT:
	  compare_values (OMP_CLAUSE_DEFAULT_KIND);
	  break;
	case OMP_CLAUSE_SCHEDULE:
	  compare_values (OMP_CLAUSE_SCHEDULE_KIND);
	  break;
	case OMP_CLAUSE_DEPEND:
	  compare_values (OMP_CLAUSE_DEPEND_KIND);
	  break;
	case OMP_CLAUSE_MAP:
	  compare_values (OMP_CLAUSE_MAP_KIND);
	  break;
	case OMP_CLAUSE_PROC_BIND:
	  compare_values (OMP_CLAUSE_PROC_BIND_KIND);
	  break;
	case OMP_CLAUSE_REDUCTION:
	  compare_values (OMP_CLAUSE_REDUCTION_CODE);
	  compare_values (OMP_CLAUSE_REDUCTION_GIMPLE_INIT);
	  compare_values (OMP_CLAUSE_REDUCTION_GIMPLE_MERGE);
	  break;
	default:
	  break;
	}
    }

#undef compare_values


  /* Compare pointer fields.  */

  /* Recurse.  Search & Replaced from DFS_write_tree_body.
     Folding the early checks into the compare_tree_edges recursion
     macro makes debugging way quicker as you are able to break on
     compare_tree_sccs_1 and simply finish until a call returns false
     to spot the SCC members with the difference.  */
#define compare_tree_edges(E1, E2) \
  do { \
    tree t1_ = (E1), t2_ = (E2); \
    if (t1_ != t2_ \
	&& (!t1_ || !t2_ \
	    || !TREE_VISITED (t2_) \
	    || (!TREE_ASM_WRITTEN (t2_) \
		&& !compare_tree_sccs_1 (t1_, t2_, map)))) \
      return false; \
    /* Only non-NULL trees outside of the SCC may compare equal.  */ \
    gcc_checking_assert (t1_ != t2_ || (!t2_ || !TREE_VISITED (t2_))); \
  } while (0)

  if (CODE_CONTAINS_STRUCT (code, TS_TYPED))
    {
      if (code != IDENTIFIER_NODE)
	compare_tree_edges (TREE_TYPE (t1), TREE_TYPE (t2));
    }

  if (CODE_CONTAINS_STRUCT (code, TS_VECTOR))
    {
      /* Note that the number of elements for EXPR has already been emitted
	 in EXPR's header (see streamer_write_tree_header).  */
      unsigned int count = vector_cst_encoded_nelts (t1);
      for (unsigned int i = 0; i < count; ++i)
	compare_tree_edges (VECTOR_CST_ENCODED_ELT (t1, i),
			    VECTOR_CST_ENCODED_ELT (t2, i));
    }

  if (CODE_CONTAINS_STRUCT (code, TS_COMPLEX))
    {
      compare_tree_edges (TREE_REALPART (t1), TREE_REALPART (t2));
      compare_tree_edges (TREE_IMAGPART (t1), TREE_IMAGPART (t2));
    }

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_MINIMAL))
    {
      compare_tree_edges (DECL_NAME (t1), DECL_NAME (t2));
      /* ???  Global decls from different TUs have non-matching
	 TRANSLATION_UNIT_DECLs.  Only consider a small set of
	 decls equivalent, we should not end up merging others.  */
      if ((code == TYPE_DECL
	   || code == NAMESPACE_DECL
	   || code == IMPORTED_DECL
	   || code == CONST_DECL
	   || (VAR_OR_FUNCTION_DECL_P (t1)
	       && (TREE_PUBLIC (t1) || DECL_EXTERNAL (t1))))
	  && DECL_FILE_SCOPE_P (t1) && DECL_FILE_SCOPE_P (t2))
	;
      else
	compare_tree_edges (DECL_CONTEXT (t1), DECL_CONTEXT (t2));
    }

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_COMMON))
    {
      compare_tree_edges (DECL_SIZE (t1), DECL_SIZE (t2));
      compare_tree_edges (DECL_SIZE_UNIT (t1), DECL_SIZE_UNIT (t2));
      compare_tree_edges (DECL_ATTRIBUTES (t1), DECL_ATTRIBUTES (t2));
      compare_tree_edges (DECL_ABSTRACT_ORIGIN (t1), DECL_ABSTRACT_ORIGIN (t2));
      if ((code == VAR_DECL
	   || code == PARM_DECL)
	  && DECL_HAS_VALUE_EXPR_P (t1))
	compare_tree_edges (DECL_VALUE_EXPR (t1), DECL_VALUE_EXPR (t2));
      if (code == VAR_DECL
	  && DECL_HAS_DEBUG_EXPR_P (t1))
	compare_tree_edges (DECL_DEBUG_EXPR (t1), DECL_DEBUG_EXPR (t2));
      /* LTO specific edges.  */
      if (code != FUNCTION_DECL
	  && code != TRANSLATION_UNIT_DECL)
	compare_tree_edges (DECL_INITIAL (t1), DECL_INITIAL (t2));
    }

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_NON_COMMON))
    {
      if (code == FUNCTION_DECL)
	{
	  tree a1, a2;
	  for (a1 = DECL_ARGUMENTS (t1), a2 = DECL_ARGUMENTS (t2);
	       a1 || a2;
	       a1 = TREE_CHAIN (a1), a2 = TREE_CHAIN (a2))
	    compare_tree_edges (a1, a2);
	  compare_tree_edges (DECL_RESULT (t1), DECL_RESULT (t2));
	}
      else if (code == TYPE_DECL)
	compare_tree_edges (DECL_ORIGINAL_TYPE (t1), DECL_ORIGINAL_TYPE (t2));
    }

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_WITH_VIS))
    {
      /* Make sure we don't inadvertently set the assembler name.  */
      if (DECL_ASSEMBLER_NAME_SET_P (t1))
	compare_tree_edges (DECL_ASSEMBLER_NAME (t1),
			    DECL_ASSEMBLER_NAME (t2));
    }

  if (CODE_CONTAINS_STRUCT (code, TS_FIELD_DECL))
    {
      compare_tree_edges (DECL_FIELD_OFFSET (t1), DECL_FIELD_OFFSET (t2));
      compare_tree_edges (DECL_BIT_FIELD_TYPE (t1), DECL_BIT_FIELD_TYPE (t2));
      compare_tree_edges (DECL_BIT_FIELD_REPRESENTATIVE (t1),
			  DECL_BIT_FIELD_REPRESENTATIVE (t2));
      compare_tree_edges (DECL_FIELD_BIT_OFFSET (t1),
			  DECL_FIELD_BIT_OFFSET (t2));
      compare_tree_edges (DECL_FCONTEXT (t1), DECL_FCONTEXT (t2));
    }

  if (CODE_CONTAINS_STRUCT (code, TS_FUNCTION_DECL))
    {
      compare_tree_edges (DECL_FUNCTION_PERSONALITY (t1),
			  DECL_FUNCTION_PERSONALITY (t2));
      compare_tree_edges (DECL_VINDEX (t1), DECL_VINDEX (t2));
      compare_tree_edges (DECL_FUNCTION_SPECIFIC_TARGET (t1),
			  DECL_FUNCTION_SPECIFIC_TARGET (t2));
      compare_tree_edges (DECL_FUNCTION_SPECIFIC_OPTIMIZATION (t1),
			  DECL_FUNCTION_SPECIFIC_OPTIMIZATION (t2));
    }

  if (CODE_CONTAINS_STRUCT (code, TS_TYPE_COMMON))
    {
      compare_tree_edges (TYPE_SIZE (t1), TYPE_SIZE (t2));
      compare_tree_edges (TYPE_SIZE_UNIT (t1), TYPE_SIZE_UNIT (t2));
      compare_tree_edges (TYPE_ATTRIBUTES (t1), TYPE_ATTRIBUTES (t2));
      compare_tree_edges (TYPE_NAME (t1), TYPE_NAME (t2));
      /* Do not compare TYPE_POINTER_TO or TYPE_REFERENCE_TO.  They will be
	 reconstructed during fixup.  */
      /* Do not compare TYPE_NEXT_VARIANT, we reconstruct the variant lists
	 during fixup.  */
      compare_tree_edges (TYPE_MAIN_VARIANT (t1), TYPE_MAIN_VARIANT (t2));
      /* ???  Global types from different TUs have non-matching
	 TRANSLATION_UNIT_DECLs.  Still merge them if they are otherwise
	 equal.  */
      if (TYPE_FILE_SCOPE_P (t1) && TYPE_FILE_SCOPE_P (t2))
	;
      else
	compare_tree_edges (TYPE_CONTEXT (t1), TYPE_CONTEXT (t2));
      /* TYPE_CANONICAL is re-computed during type merging, so do not
	 compare it here.  */
      compare_tree_edges (TYPE_STUB_DECL (t1), TYPE_STUB_DECL (t2));
    }

  if (CODE_CONTAINS_STRUCT (code, TS_TYPE_NON_COMMON))
    {
      if (code == ARRAY_TYPE)
	compare_tree_edges (TYPE_DOMAIN (t1), TYPE_DOMAIN (t2));
      else if (RECORD_OR_UNION_TYPE_P (t1))
	{
	  tree f1, f2;
	  for (f1 = TYPE_FIELDS (t1), f2 = TYPE_FIELDS (t2);
	       f1 || f2;
	       f1 = TREE_CHAIN (f1), f2 = TREE_CHAIN (f2))
	    compare_tree_edges (f1, f2);
	}
      else if (code == FUNCTION_TYPE
	       || code == METHOD_TYPE)
	compare_tree_edges (TYPE_ARG_TYPES (t1), TYPE_ARG_TYPES (t2));

      if (!POINTER_TYPE_P (t1))
	compare_tree_edges (TYPE_MIN_VALUE_RAW (t1), TYPE_MIN_VALUE_RAW (t2));
      compare_tree_edges (TYPE_MAX_VALUE_RAW (t1), TYPE_MAX_VALUE_RAW (t2));
    }

  if (CODE_CONTAINS_STRUCT (code, TS_LIST))
    {
      compare_tree_edges (TREE_PURPOSE (t1), TREE_PURPOSE (t2));
      compare_tree_edges (TREE_VALUE (t1), TREE_VALUE (t2));
      compare_tree_edges (TREE_CHAIN (t1), TREE_CHAIN (t2));
    }

  if (CODE_CONTAINS_STRUCT (code, TS_VEC))
    for (int i = 0; i < TREE_VEC_LENGTH (t1); i++)
      compare_tree_edges (TREE_VEC_ELT (t1, i), TREE_VEC_ELT (t2, i));

  if (CODE_CONTAINS_STRUCT (code, TS_EXP))
    {
      for (int i = 0; i < TREE_OPERAND_LENGTH (t1); i++)
	compare_tree_edges (TREE_OPERAND (t1, i),
			    TREE_OPERAND (t2, i));

      /* BLOCKs are function local and we don't merge anything there.  */
      if (TREE_BLOCK (t1) || TREE_BLOCK (t2))
	return false;
    }

  if (CODE_CONTAINS_STRUCT (code, TS_BINFO))
    {
      unsigned i;
      tree t;
      /* Lengths have already been compared above.  */
      FOR_EACH_VEC_ELT (*BINFO_BASE_BINFOS (t1), i, t)
	compare_tree_edges (t, BINFO_BASE_BINFO (t2, i));
      FOR_EACH_VEC_SAFE_ELT (BINFO_BASE_ACCESSES (t1), i, t)
	compare_tree_edges (t, BINFO_BASE_ACCESS (t2, i));
      compare_tree_edges (BINFO_OFFSET (t1), BINFO_OFFSET (t2));
      compare_tree_edges (BINFO_VTABLE (t1), BINFO_VTABLE (t2));
      compare_tree_edges (BINFO_VPTR_FIELD (t1), BINFO_VPTR_FIELD (t2));
      /* Do not walk BINFO_INHERITANCE_CHAIN, BINFO_SUBVTT_INDEX
	 and BINFO_VPTR_INDEX; these are used by C++ FE only.  */
    }

  if (CODE_CONTAINS_STRUCT (code, TS_CONSTRUCTOR))
    {
      unsigned i;
      tree index, value;
      /* Lengths have already been compared above.  */
      FOR_EACH_CONSTRUCTOR_ELT (CONSTRUCTOR_ELTS (t1), i, index, value)
	{
	  compare_tree_edges (index, CONSTRUCTOR_ELT (t2, i)->index);
	  compare_tree_edges (value, CONSTRUCTOR_ELT (t2, i)->value);
	}
    }

  if (code == OMP_CLAUSE)
    {
      int i;

      for (i = 0; i < omp_clause_num_ops[OMP_CLAUSE_CODE (t1)]; i++)
	compare_tree_edges (OMP_CLAUSE_OPERAND (t1, i),
			    OMP_CLAUSE_OPERAND (t2, i));
      compare_tree_edges (OMP_CLAUSE_CHAIN (t1), OMP_CLAUSE_CHAIN (t2));
    }

#undef compare_tree_edges

  return true;
}

/* Compare the tree scc SCC to the prevailing candidate PSCC, filling
   out MAP if they are equal.  */

static bool
compare_tree_sccs (tree_scc *pscc, tree_scc *scc,
		   tree *map)
{
  /* Assume SCC entry hashes are sorted after their cardinality.  Which
     means we can simply take the first n-tuple of equal hashes
     (which is recorded as entry_len) and do n SCC entry candidate
     comparisons.  */
  for (unsigned i = 0; i < pscc->entry_len; ++i)
    {
      tree *mapp = map;
      num_scc_compare_collisions++;
      if (compare_tree_sccs_1 (pscc->entries[0], scc->entries[i], &mapp))
	{
	  /* Equal - no need to reset TREE_VISITED or TREE_ASM_WRITTEN
	     on the scc as all trees will be freed.  */
	  return true;
	}
      /* Reset TREE_ASM_WRITTEN on scc for the next compare or in case
	 the SCC prevails.  */
      for (unsigned j = 0; j < scc->len; ++j)
	TREE_ASM_WRITTEN (scc->entries[j]) = 0;
    }

  return false;
}

/* QSort sort function to sort a map of two pointers after the 2nd
   pointer.  */

static int
cmp_tree (const void *p1_, const void *p2_)
{
  tree *p1 = (tree *)(const_cast<void *>(p1_));
  tree *p2 = (tree *)(const_cast<void *>(p2_));
  if (p1[1] == p2[1])
    return 0;
  return ((uintptr_t)p1[1] < (uintptr_t)p2[1]) ? -1 : 1;
}

/* New scc of size 1 containing T was streamed in from DATA_IN and not merged.
   Register it to reader cache at index FROM.  */

static void
process_dref (class data_in *data_in, tree t, unsigned from)
{
  struct streamer_tree_cache_d *cache = data_in->reader_cache;
  /* If we got a debug reference queued, see if the prevailing
     tree has a debug reference and if not, register the one
     for the tree we are about to throw away.  */
  if (dref_queue.length () == 1)
    {
      dref_entry e = dref_queue.pop ();
      gcc_assert (e.decl
		  == streamer_tree_cache_get_tree (cache, from));
      const char *sym;
      unsigned HOST_WIDE_INT off;
      if (!debug_hooks->die_ref_for_decl (t, &sym, &off))
	debug_hooks->register_external_die (t, e.sym, e.off);
    }
}

/* Try to unify the SCC with nodes FROM to FROM + LEN in CACHE and
   hash value SCC_HASH with an already recorded SCC.  Return true if
   that was successful, otherwise return false.  */

static bool
unify_scc (class data_in *data_in, unsigned from,
	   unsigned len, unsigned scc_entry_len, hashval_t scc_hash)
{
  bool unified_p = false;
  struct streamer_tree_cache_d *cache = data_in->reader_cache;
  tree_scc *scc
    = (tree_scc *) alloca (sizeof (tree_scc) + (len - 1) * sizeof (tree));
  scc->next = NULL;
  scc->hash = scc_hash;
  scc->len = len;
  scc->entry_len = scc_entry_len;
  for (unsigned i = 0; i < len; ++i)
    {
      tree t = streamer_tree_cache_get_tree (cache, from + i);
      scc->entries[i] = t;
      /* These types should be streamed as unshared.  */
      gcc_checking_assert
	 (!(TREE_CODE (t) == TRANSLATION_UNIT_DECL
	    || (VAR_OR_FUNCTION_DECL_P (t)
		&& !(TREE_PUBLIC (t) || DECL_EXTERNAL (t)))
	    || TREE_CODE (t) == LABEL_DECL
	    || (TREE_CODE (t) == NAMESPACE_DECL && !DECL_NAME (t))
	    || (TYPE_P (t)
		&& type_with_linkage_p (TYPE_MAIN_VARIANT (t))
		&& type_in_anonymous_namespace_p (TYPE_MAIN_VARIANT (t)))));
    }

  /* Look for the list of candidate SCCs to compare against.  */
  tree_scc **slot;
  slot = tree_scc_hash->find_slot_with_hash (scc, scc_hash, INSERT);
  if (*slot)
    {
      /* Try unifying against each candidate.  */
      num_scc_compares++;

      /* Set TREE_VISITED on the scc so we can easily identify tree nodes
	 outside of the scc when following tree edges.  Make sure
	 that TREE_ASM_WRITTEN is unset so we can use it as 2nd bit
	 to track whether we visited the SCC member during the compare.
	 We cannot use TREE_VISITED on the pscc members as the extended
	 scc and pscc can overlap.  */
      for (unsigned i = 0; i < scc->len; ++i)
	{
	  TREE_VISITED (scc->entries[i]) = 1;
	  gcc_checking_assert (!TREE_ASM_WRITTEN (scc->entries[i]));
	}

      tree *map = XALLOCAVEC (tree, 2 * len);
      for (tree_scc *pscc = *slot; pscc; pscc = pscc->next)
	{
	  if (!compare_tree_sccs (pscc, scc, map))
	    continue;

	  /* Found an equal SCC.  */
	  unified_p = true;
	  num_scc_compare_collisions--;
	  num_sccs_merged++;
	  total_scc_size_merged += len;

	  if (flag_checking)
	    for (unsigned i = 0; i < len; ++i)
	      {
		tree t = map[2*i+1];
		enum tree_code code = TREE_CODE (t);
		/* IDENTIFIER_NODEs should be singletons and are merged by the
		   streamer.  The others should be singletons, too, and we
		   should not merge them in any way.  */
		gcc_assert (code != TRANSLATION_UNIT_DECL
			    && code != IDENTIFIER_NODE);
	      }

	  /* Fixup the streamer cache with the prevailing nodes according
	     to the tree node mapping computed by compare_tree_sccs.  */
	  if (len == 1)
	    {
	      process_dref (data_in, pscc->entries[0], from);
	      lto_maybe_register_decl (data_in, pscc->entries[0], from);
	      streamer_tree_cache_replace_tree (cache, pscc->entries[0], from);
	    }
	  else
	    {
	      tree *map2 = XALLOCAVEC (tree, 2 * len);
	      for (unsigned i = 0; i < len; ++i)
		{
		  map2[i*2] = (tree)(uintptr_t)(from + i);
		  map2[i*2+1] = scc->entries[i];
		}
	      qsort (map2, len, 2 * sizeof (tree), cmp_tree);
	      qsort (map, len, 2 * sizeof (tree), cmp_tree);
	      for (unsigned i = 0; i < len; ++i)
		{
		  lto_maybe_register_decl (data_in, map[2*i],
					   (uintptr_t)map2[2*i]);
		  streamer_tree_cache_replace_tree (cache, map[2*i],
						    (uintptr_t)map2[2*i]);
		}
	    }

	  /* Free the tree nodes from the read SCC.  */
	  data_in->location_cache.revert_location_cache ();
	  for (unsigned i = 0; i < len; ++i)
	    {
	      if (TYPE_P (scc->entries[i]))
		num_merged_types++;
	      free_node (scc->entries[i]);
	    }

	  /* Drop DIE references.
	     ???  Do as in the size-one SCC case which involves sorting
	     the queue.  */
	  dref_queue.truncate (0);

	  break;
	}

      /* Reset TREE_VISITED if we didn't unify the SCC with another.  */
      if (!unified_p)
	for (unsigned i = 0; i < scc->len; ++i)
	  TREE_VISITED (scc->entries[i]) = 0;
    }

  /* If we didn't unify it to any candidate duplicate the relevant
     pieces to permanent storage and link it into the chain.  */
  if (!unified_p)
    {
      tree_scc *pscc
	= XOBNEWVAR (&tree_scc_hash_obstack, tree_scc, sizeof (tree_scc));
      memcpy (pscc, scc, sizeof (tree_scc));
      pscc->next = (*slot);
      *slot = pscc;
    }
  return unified_p;
}

typedef int_hash<unsigned, 0, UINT_MAX> code_id_hash;

/* Do registering necessary once new tree fully streamed in (including all
   trees it reffers to).  */

static void
process_new_tree (tree t, hash_map <code_id_hash, unsigned> *hm,
		  unsigned index, unsigned *total, class data_in *data_in)
{
  /* Reconstruct the type variant and pointer-to/reference-to
     chains.  */
  if (TYPE_P (t))
    {
      /* Map the tree types to their frequencies.  */
      if (flag_lto_dump_type_stats)
	{
	  unsigned key = (unsigned) TREE_CODE (t);
	  unsigned *countp = hm->get (key);
	  hm->put (key, countp ? (*countp) + 1 : 1);
	  (*total)++;
	}

      num_prevailing_types++;
      lto_fixup_prevailing_type (t);

      /* Compute the canonical type of all non-ODR types.
	 Delay ODR types for the end of merging process - the canonical
	 type for those can be computed using the (unique) name however
	 we want to do this only if units in other languages do not
	 contain structurally equivalent type.

	 Because SCC components are streamed in random (hash) order
	 we may have encountered the type before while registering
	 type canonical of a derived type in the same SCC.  */
      if (!TYPE_CANONICAL (t))
	{
	  if (!RECORD_OR_UNION_TYPE_P (t)
	      || !TYPE_CXX_ODR_P (t))
	    gimple_register_canonical_type (t);
	  else if (COMPLETE_TYPE_P (t))
	    vec_safe_push (types_to_register, t);
	}
      if (TYPE_MAIN_VARIANT (t) == t && odr_type_p (t))
	register_odr_type (t);
    }
  /* Link shared INTEGER_CSTs into TYPE_CACHED_VALUEs of its
     type which is also member of this SCC.  */
  if (TREE_CODE (t) == INTEGER_CST
      && !TREE_OVERFLOW (t))
    cache_integer_cst (t);
  if (!flag_ltrans)
    {
      lto_maybe_register_decl (data_in, t, index);
      /* Scan the tree for references to global functions or
	 variables and record those for later fixup.  */
      if (mentions_vars_p (t))
	vec_safe_push (tree_with_vars, t);
    }
}

/* Read all the symbols from buffer DATA, using descriptors in DECL_DATA.
   RESOLUTIONS is the set of symbols picked by the linker (read from the
   resolution file when the linker plugin is being used).  */

static void
lto_read_decls (struct lto_file_decl_data *decl_data, const void *data,
		vec<ld_plugin_symbol_resolution_t> resolutions)
{
  const struct lto_decl_header *header = (const struct lto_decl_header *) data;
  const int decl_offset = sizeof (struct lto_decl_header);
  const int main_offset = decl_offset + header->decl_state_size;
  const int string_offset = main_offset + header->main_size;
  class data_in *data_in;
  unsigned int i;
  const uint32_t *data_ptr, *data_end;
  uint32_t num_decl_states;

  lto_input_block ib_main ((const char *) data + main_offset,
			   header->main_size, decl_data->mode_table);

  data_in = lto_data_in_create (decl_data, (const char *) data + string_offset,
				header->string_size, resolutions);

  /* We do not uniquify the pre-loaded cache entries, those are middle-end
     internal types that should not be merged.  */

  hash_map <code_id_hash, unsigned> hm;
  unsigned total = 0;

  /* Read the global declarations and types.  */
  while (ib_main.p < ib_main.len)
    {
      tree t;
      unsigned from = data_in->reader_cache->nodes.length ();
      /* Read and uniquify SCCs as in the input stream.  */
      enum LTO_tags tag = streamer_read_record_start (&ib_main);
      if (tag == LTO_tree_scc || tag == LTO_trees)
	{
	  unsigned len_;
	  unsigned scc_entry_len;

	  /* Because we stream in SCC order we know that all unshared trees
	     are now fully streamed.  Process them.  */
	  hashval_t scc_hash = lto_input_scc (&ib_main, data_in, &len_,
					      &scc_entry_len,
					      tag == LTO_tree_scc);
	  unsigned len = data_in->reader_cache->nodes.length () - from;
	  gcc_assert (len == len_);

	  if (tag == LTO_tree_scc)
	    {
	      total_scc_size += len;
	      num_sccs_read++;
	    }
	  else
	    num_unshared_trees_read += len;

	  /* We have the special case of size-1 SCCs that are pre-merged
	     by means of identifier and string sharing for example.
	     ???  Maybe we should avoid streaming those as SCCs.  */
	  tree first = streamer_tree_cache_get_tree (data_in->reader_cache,
						     from);
	  /* Identifier and integers are shared specially, they should never
	     go by the tree merging path.  */
	  gcc_checking_assert ((TREE_CODE (first) != IDENTIFIER_NODE
				&& (TREE_CODE (first) != INTEGER_CST
				    || TREE_OVERFLOW (first)))
			       || len != 1);

	  /* Try to unify the SCC with already existing ones.  */
	  if (!flag_ltrans && tag != LTO_trees
	      && unify_scc (data_in, from,
			    len, scc_entry_len, scc_hash))
	    continue;

	  /* Tree merging failed, mark entries in location cache as
	     permanent.  */
	  data_in->location_cache.accept_location_cache ();

	  bool seen_type = false;
	  for (unsigned i = 0; i < len; ++i)
	    {
	      tree t = streamer_tree_cache_get_tree (data_in->reader_cache,
						     from + i);
	      process_new_tree (t, &hm, from + i, &total, data_in);
	      if (TYPE_P (t))
		seen_type = true;
	    }

	  /* Register DECLs with the debuginfo machinery.  */
	  while (!dref_queue.is_empty ())
	    {
	      dref_entry e = dref_queue.pop ();
	      debug_hooks->register_external_die (e.decl, e.sym, e.off);
	    }

	  if (seen_type)
	    num_type_scc_trees += len;
	}
      else
	{
	  t = lto_input_tree_1 (&ib_main, data_in, tag, 0);
	  gcc_assert (data_in->reader_cache->nodes.length () == from + 1);
	  num_unshared_trees_read++;
	  data_in->location_cache.accept_location_cache ();
	  process_dref (data_in, t, from);
	  if (TREE_CODE (t) == IDENTIFIER_NODE
	      || (TREE_CODE (t) == INTEGER_CST
		  && !TREE_OVERFLOW (t)))
	    ;
	  else
	    {
	      lto_maybe_register_decl (data_in, t, from);
	      process_new_tree (t, &hm, from, &total, data_in);
	    }
	}
    }

  /* Dump type statistics.  */
  if (flag_lto_dump_type_stats)
    {
      fprintf (stdout, "       Type     Frequency   Percentage\n\n");
      for (hash_map<code_id_hash, unsigned>::iterator itr = hm.begin ();
	   itr != hm.end ();
	   ++itr)
	{
	  std::pair<unsigned, unsigned> p = *itr;
	  enum tree_code code = (enum tree_code) p.first;
	  fprintf (stdout, "%14s %6d %12.2f\n", get_tree_code_name (code),
		   p.second, float (p.second)/total*100);
	}
    }

  data_in->location_cache.apply_location_cache ();

  /* Read in lto_in_decl_state objects.  */
  data_ptr = (const uint32_t *) ((const char*) data + decl_offset);
  data_end
    = (const uint32_t *) ((const char*) data_ptr + header->decl_state_size);
  num_decl_states = *data_ptr++;

  gcc_assert (num_decl_states > 0);
  decl_data->global_decl_state = lto_new_in_decl_state ();
  data_ptr = lto_read_in_decl_state (data_in, data_ptr,
				     decl_data->global_decl_state);

  /* Read in per-function decl states and enter them in hash table.  */
  decl_data->function_decl_states
    = hash_table<decl_state_hasher>::create_ggc (37);

  for (i = 1; i < num_decl_states; i++)
    {
      struct lto_in_decl_state *state = lto_new_in_decl_state ();

      data_ptr = lto_read_in_decl_state (data_in, data_ptr, state);
      lto_in_decl_state **slot
	= decl_data->function_decl_states->find_slot (state, INSERT);
      gcc_assert (*slot == NULL);
      *slot = state;
    }

  if (data_ptr != data_end)
    internal_error ("bytecode stream: garbage at the end of symbols section");

  /* Set the current decl state to be the global state.  */
  decl_data->current_decl_state = decl_data->global_decl_state;

  lto_data_in_delete (data_in);
}

/* Custom version of strtoll, which is not portable.  */

static int64_t
lto_parse_hex (const char *p)
{
  int64_t ret = 0;

  for (; *p != '\0'; ++p)
    {
      char c = *p;
      unsigned char part;
      ret <<= 4;
      if (c >= '0' && c <= '9')
	part = c - '0';
      else if (c >= 'a' && c <= 'f')
	part = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
	part = c - 'A' + 10;
      else
	internal_error ("could not parse hex number");
      ret |= part;
    }

  return ret;
}

/* Read resolution for file named FILE_NAME.  The resolution is read from
   RESOLUTION.  */

static void
lto_resolution_read (splay_tree file_ids, FILE *resolution, lto_file *file)
{
  /* We require that objects in the resolution file are in the same
     order as the lto1 command line.  */
  unsigned int name_len;
  char *obj_name;
  unsigned int num_symbols;
  unsigned int i;
  struct lto_file_decl_data *file_data;
  splay_tree_node nd = NULL;

  if (!resolution)
    return;

  name_len = strlen (file->filename);
  obj_name = XNEWVEC (char, name_len + 1);
  fscanf (resolution, " ");   /* Read white space.  */

  fread (obj_name, sizeof (char), name_len, resolution);
  obj_name[name_len] = '\0';
  if (filename_cmp (obj_name, file->filename) != 0)
    internal_error ("unexpected file name %s in linker resolution file. "
		    "Expected %s", obj_name, file->filename);
  if (file->offset != 0)
    {
      int t;
      char offset_p[17];
      int64_t offset;
      t = fscanf (resolution, "@0x%16s", offset_p);
      if (t != 1)
	internal_error ("could not parse file offset");
      offset = lto_parse_hex (offset_p);
      if (offset != file->offset)
	internal_error ("unexpected offset");
    }

  free (obj_name);

  fscanf (resolution, "%u", &num_symbols);

  for (i = 0; i < num_symbols; i++)
    {
      int t;
      unsigned index;
      unsigned HOST_WIDE_INT id;
      char r_str[27];
      enum ld_plugin_symbol_resolution r = (enum ld_plugin_symbol_resolution) 0;
      unsigned int j;
      unsigned int lto_resolution_str_len
	= sizeof (lto_resolution_str) / sizeof (char *);
      res_pair rp;

      t = fscanf (resolution, "%u " HOST_WIDE_INT_PRINT_HEX_PURE
		  " %26s %*[^\n]\n", &index, &id, r_str);
      if (t != 3)
	internal_error ("invalid line in the resolution file");

      for (j = 0; j < lto_resolution_str_len; j++)
	{
	  if (strcmp (lto_resolution_str[j], r_str) == 0)
	    {
	      r = (enum ld_plugin_symbol_resolution) j;
	      break;
	    }
	}
      if (j == lto_resolution_str_len)
	internal_error ("invalid resolution in the resolution file");

      if (!(nd && lto_splay_tree_id_equal_p (nd->key, id)))
	{
	  nd = lto_splay_tree_lookup (file_ids, id);
	  if (nd == NULL)
	    internal_error ("resolution sub id %wx not in object file", id);
	}

      file_data = (struct lto_file_decl_data *)nd->value;
      /* The indexes are very sparse.  To save memory save them in a compact
	 format that is only unpacked later when the subfile is processed.  */
      rp.res = r;
      rp.index = index;
      file_data->respairs.safe_push (rp);
      if (file_data->max_index < index)
	file_data->max_index = index;
    }
}

/* List of file_decl_datas.  */
struct file_data_list
{
  struct lto_file_decl_data *first, *last;
};

/* Is the name for a id'ed LTO section? */

static int
lto_section_with_id (const char *name, unsigned HOST_WIDE_INT *id)
{
  const char *s;

  if (strncmp (name, section_name_prefix, strlen (section_name_prefix)))
    return 0;
  s = strrchr (name, '.');
  if (!s)
    return 0;
  /* If the section is not suffixed with an ID return.  */
  if ((size_t)(s - name) == strlen (section_name_prefix))
    return 0;
  return sscanf (s, "." HOST_WIDE_INT_PRINT_HEX_PURE, id) == 1;
}

/* Create file_data of each sub file id.  */

static int
create_subid_section_table (struct lto_section_slot *ls, splay_tree file_ids,
			    struct file_data_list *list)
{
  struct lto_section_slot s_slot, *new_slot;
  unsigned HOST_WIDE_INT id;
  splay_tree_node nd;
  void **hash_slot;
  char *new_name;
  struct lto_file_decl_data *file_data;

  if (!lto_section_with_id (ls->name, &id))
    return 1;

  /* Find hash table of sub module id.  */
  nd = lto_splay_tree_lookup (file_ids, id);
  if (nd != NULL)
    {
      file_data = (struct lto_file_decl_data *)nd->value;
    }
  else
    {
      file_data = ggc_alloc<lto_file_decl_data> ();
      memset(file_data, 0, sizeof (struct lto_file_decl_data));
      file_data->id = id;
      file_data->section_hash_table = lto_obj_create_section_hash_table ();
      lto_splay_tree_insert (file_ids, id, file_data);

      /* Maintain list in linker order.  */
      if (!list->first)
	list->first = file_data;
      if (list->last)
	list->last->next = file_data;

      list->last = file_data;
    }

  /* Copy section into sub module hash table.  */
  new_name = XDUPVEC (char, ls->name, strlen (ls->name) + 1);
  s_slot.name = new_name;
  hash_slot = htab_find_slot (file_data->section_hash_table, &s_slot, INSERT);
  gcc_assert (*hash_slot == NULL);

  new_slot = XDUP (struct lto_section_slot, ls);
  new_slot->name = new_name;
  *hash_slot = new_slot;
  return 1;
}

/* Read declarations and other initializations for a FILE_DATA.  */

static void
lto_file_finalize (struct lto_file_decl_data *file_data, lto_file *file,
		   int order)
{
  const char *data;
  size_t len;
  vec<ld_plugin_symbol_resolution_t>
	resolutions = vNULL;
  int i;
  res_pair *rp;

  /* Create vector for fast access of resolution.  We do this lazily
     to save memory.  */
  resolutions.safe_grow_cleared (file_data->max_index + 1, true);
  for (i = 0; file_data->respairs.iterate (i, &rp); i++)
    resolutions[rp->index] = rp->res;
  file_data->respairs.release ();

  file_data->renaming_hash_table = lto_create_renaming_table ();
  file_data->file_name = file->filename;
  file_data->order = order;

  /* Read and verify LTO section.  */
  data = lto_get_summary_section_data (file_data, LTO_section_lto, &len);
  if (data == NULL)
    {
      fatal_error (input_location, "bytecode stream in file %qs generated "
		   "with GCC compiler older than 10.0", file_data->file_name);
      return;
    }

  memcpy (&file_data->lto_section_header, data, sizeof (lto_section));
  lto_check_version (file_data->lto_section_header.major_version,
		     file_data->lto_section_header.minor_version,
		     file_data->file_name);

#ifdef ACCEL_COMPILER
  lto_input_mode_table (file_data);
#else
  file_data->mode_table = lto_mode_identity_table;
#endif

  data = lto_get_summary_section_data (file_data, LTO_section_decls, &len);
  if (data == NULL)
    {
      internal_error ("cannot read %<LTO_section_decls%> from %s",
		      file_data->file_name);
      return;
    }
  /* Frees resolutions.  */
  lto_read_decls (file_data, data, resolutions);
  lto_free_section_data (file_data, LTO_section_decls, NULL, data, len);
}

/* Finalize FILE_DATA in FILE and increase COUNT.  */

static int
lto_create_files_from_ids (lto_file *file, struct lto_file_decl_data *file_data,
			   int *count, int order)
{
  lto_file_finalize (file_data, file, order);
  if (symtab->dump_file)
    fprintf (symtab->dump_file,
	     "Creating file %s with sub id " HOST_WIDE_INT_PRINT_HEX "\n",
	     file_data->file_name, file_data->id);
  (*count)++;
  return 0;
}

/* Generate a TREE representation for all types and external decls
   entities in FILE.

   Read all of the globals out of the file.  Then read the cgraph
   and process the .o index into the cgraph nodes so that it can open
   the .o file to load the functions and ipa information.  */

static struct lto_file_decl_data *
lto_file_read (lto_file *file, FILE *resolution_file, int *count)
{
  struct lto_file_decl_data *file_data = NULL;
  splay_tree file_ids;
  htab_t section_hash_table;
  struct lto_section_slot *section;
  struct file_data_list file_list;
  struct lto_section_list section_list;

  memset (&section_list, 0, sizeof (struct lto_section_list));
  section_hash_table = lto_obj_build_section_table (file, &section_list);

  /* Dump the details of LTO objects.  */
  if (flag_lto_dump_objects)
  {
    int i=0;
    fprintf (stdout, "\n    LTO Object Name: %s\n", file->filename);
    fprintf (stdout, "\nNo.    Offset    Size       Section Name\n\n");
    for (section = section_list.first; section != NULL; section = section->next)
      fprintf (stdout, "%2d %8" PRId64 " %8" PRIu64 "   %s\n",
	       ++i, (int64_t) section->start, (uint64_t) section->len,
	       section->name);
  }

  /* Find all sub modules in the object and put their sections into new hash
     tables in a splay tree.  */
  file_ids = lto_splay_tree_new ();
  memset (&file_list, 0, sizeof (struct file_data_list));
  for (section = section_list.first; section != NULL; section = section->next)
    create_subid_section_table (section, file_ids, &file_list);

  /* Add resolutions to file ids.  */
  lto_resolution_read (file_ids, resolution_file, file);

  /* Finalize each lto file for each submodule in the merged object.  */
  int order = 0;
  for (file_data = file_list.first; file_data != NULL;
       file_data = file_data->next)
    lto_create_files_from_ids (file, file_data, count, order++);

  splay_tree_delete (file_ids);
  htab_delete (section_hash_table);

  return file_list.first;
}

#if HAVE_MMAP_FILE && HAVE_SYSCONF && defined _SC_PAGE_SIZE
#define LTO_MMAP_IO 1
#endif

#if LTO_MMAP_IO
/* Page size of machine is used for mmap and munmap calls.  */
static size_t page_mask;
#endif

/* Get the section data of length LEN from FILENAME starting at
   OFFSET.  The data segment must be freed by the caller when the
   caller is finished.  Returns NULL if all was not well.  */

static char *
lto_read_section_data (struct lto_file_decl_data *file_data,
		       intptr_t offset, size_t len)
{
  char *result;
  static int fd = -1;
  static char *fd_name;
#if LTO_MMAP_IO
  intptr_t computed_len;
  intptr_t computed_offset;
  intptr_t diff;
#endif

  /* Keep a single-entry file-descriptor cache.  The last file we
     touched will get closed at exit.
     ???  Eventually we want to add a more sophisticated larger cache
     or rather fix function body streaming to not stream them in
     practically random order.  */
  if (fd != -1
      && filename_cmp (fd_name, file_data->file_name) != 0)
    {
      free (fd_name);
      close (fd);
      fd = -1;
    }
  if (fd == -1)
    {
      fd = open (file_data->file_name, O_RDONLY|O_BINARY);
      if (fd == -1)
	{
	  fatal_error (input_location, "Cannot open %s", file_data->file_name);
	  return NULL;
	}
      fd_name = xstrdup (file_data->file_name);
    }

#if LTO_MMAP_IO
  if (!page_mask)
    {
      size_t page_size = sysconf (_SC_PAGE_SIZE);
      page_mask = ~(page_size - 1);
    }

  computed_offset = offset & page_mask;
  diff = offset - computed_offset;
  computed_len = len + diff;

  result = (char *) mmap (NULL, computed_len, PROT_READ, MAP_PRIVATE,
			  fd, computed_offset);
  if (result == MAP_FAILED)
    {
      fatal_error (input_location, "Cannot map %s", file_data->file_name);
      return NULL;
    }

  return result + diff;
#else
  result = (char *) xmalloc (len);
  if (lseek (fd, offset, SEEK_SET) != offset
      || read (fd, result, len) != (ssize_t) len)
    {
      free (result);
      fatal_error (input_location, "Cannot read %s", file_data->file_name);
      result = NULL;
    }
#ifdef __MINGW32__
  /* Native windows doesn't supports delayed unlink on opened file.  So
     we close file here again.  This produces higher I/O load, but at least
     it prevents to have dangling file handles preventing unlink.  */
  free (fd_name);
  fd_name = NULL;
  close (fd);
  fd = -1;
#endif
  return result;
#endif
}


/* Get the section data from FILE_DATA of SECTION_TYPE with NAME.
   NAME will be NULL unless the section type is for a function
   body.  */

static const char *
get_section_data (struct lto_file_decl_data *file_data,
		  enum lto_section_type section_type,
		  const char *name, int order,
		  size_t *len)
{
  htab_t section_hash_table = file_data->section_hash_table;
  struct lto_section_slot *f_slot;
  struct lto_section_slot s_slot;
  const char *section_name = lto_get_section_name (section_type, name,
						   order, file_data);
  char *data = NULL;

  *len = 0;
  s_slot.name = section_name;
  f_slot = (struct lto_section_slot *) htab_find (section_hash_table, &s_slot);
  if (f_slot)
    {
      data = lto_read_section_data (file_data, f_slot->start, f_slot->len);
      *len = f_slot->len;
    }

  free (CONST_CAST (char *, section_name));
  return data;
}


/* Free the section data from FILE_DATA of SECTION_TYPE with NAME that
   starts at OFFSET and has LEN bytes.  */

static void
free_section_data (struct lto_file_decl_data *file_data ATTRIBUTE_UNUSED,
		   enum lto_section_type section_type ATTRIBUTE_UNUSED,
		   const char *name ATTRIBUTE_UNUSED,
		   const char *offset, size_t len ATTRIBUTE_UNUSED)
{
#if LTO_MMAP_IO
  intptr_t computed_len;
  intptr_t computed_offset;
  intptr_t diff;
#endif

#if LTO_MMAP_IO
  computed_offset = ((intptr_t) offset) & page_mask;
  diff = (intptr_t) offset - computed_offset;
  computed_len = len + diff;

  munmap ((caddr_t) computed_offset, computed_len);
#else
  free (CONST_CAST(char *, offset));
#endif
}

static lto_file *current_lto_file;

/* If TT is a variable or function decl replace it with its
   prevailing variant.  */
#define LTO_SET_PREVAIL(tt) \
  do {\
    if ((tt) && VAR_OR_FUNCTION_DECL_P (tt) \
	&& (TREE_PUBLIC (tt) || DECL_EXTERNAL (tt))) \
      { \
	tt = lto_symtab_prevailing_decl (tt); \
	fixed = true; \
      } \
  } while (0)

/* Ensure that TT isn't a replacable var of function decl.  */
#define LTO_NO_PREVAIL(tt) \
  gcc_checking_assert (!(tt) || !VAR_OR_FUNCTION_DECL_P (tt))

/* Given a tree T replace all fields referring to variables or functions
   with their prevailing variant.  */
static void
lto_fixup_prevailing_decls (tree t)
{
  enum tree_code code = TREE_CODE (t);
  bool fixed = false;

  gcc_checking_assert (code != TREE_BINFO);
  LTO_NO_PREVAIL (TREE_TYPE (t));
  if (CODE_CONTAINS_STRUCT (code, TS_COMMON)
      /* lto_symtab_prevail_decl use TREE_CHAIN to link to the prevailing decl.
	 in the case T is a prevailed declaration we would ICE here.  */
      && !VAR_OR_FUNCTION_DECL_P (t))
    LTO_NO_PREVAIL (TREE_CHAIN (t));
  if (DECL_P (t))
    {
      LTO_NO_PREVAIL (DECL_NAME (t));
      LTO_SET_PREVAIL (DECL_CONTEXT (t));
      if (CODE_CONTAINS_STRUCT (code, TS_DECL_COMMON))
	{
	  LTO_SET_PREVAIL (DECL_SIZE (t));
	  LTO_SET_PREVAIL (DECL_SIZE_UNIT (t));
	  LTO_SET_PREVAIL (DECL_INITIAL (t));
	  LTO_NO_PREVAIL (DECL_ATTRIBUTES (t));
	  LTO_SET_PREVAIL (DECL_ABSTRACT_ORIGIN (t));
	}
      if (CODE_CONTAINS_STRUCT (code, TS_DECL_WITH_VIS))
	{
	  LTO_NO_PREVAIL (DECL_ASSEMBLER_NAME_RAW (t));
	}
      if (CODE_CONTAINS_STRUCT (code, TS_DECL_NON_COMMON))
	{
	  LTO_NO_PREVAIL (DECL_RESULT_FLD (t));
	}
      if (CODE_CONTAINS_STRUCT (code, TS_FUNCTION_DECL))
	{
	  LTO_NO_PREVAIL (DECL_ARGUMENTS (t));
	  LTO_SET_PREVAIL (DECL_FUNCTION_PERSONALITY (t));
	  LTO_NO_PREVAIL (DECL_VINDEX (t));
	}
      if (CODE_CONTAINS_STRUCT (code, TS_FIELD_DECL))
	{
	  LTO_SET_PREVAIL (DECL_FIELD_OFFSET (t));
	  LTO_NO_PREVAIL (DECL_BIT_FIELD_TYPE (t));
	  LTO_NO_PREVAIL (DECL_QUALIFIER (t));
	  LTO_NO_PREVAIL (DECL_FIELD_BIT_OFFSET (t));
	  LTO_NO_PREVAIL (DECL_FCONTEXT (t));
	}
    }
  else if (TYPE_P (t))
    {
      LTO_NO_PREVAIL (TYPE_CACHED_VALUES (t));
      LTO_SET_PREVAIL (TYPE_SIZE (t));
      LTO_SET_PREVAIL (TYPE_SIZE_UNIT (t));
      LTO_NO_PREVAIL (TYPE_ATTRIBUTES (t));
      LTO_NO_PREVAIL (TYPE_NAME (t));

      LTO_SET_PREVAIL (TYPE_MIN_VALUE_RAW (t));
      LTO_SET_PREVAIL (TYPE_MAX_VALUE_RAW (t));
      LTO_NO_PREVAIL (TYPE_LANG_SLOT_1 (t));

      LTO_SET_PREVAIL (TYPE_CONTEXT (t));

      LTO_NO_PREVAIL (TYPE_CANONICAL (t));
      LTO_NO_PREVAIL (TYPE_MAIN_VARIANT (t));
      LTO_NO_PREVAIL (TYPE_NEXT_VARIANT (t));
    }
  else if (EXPR_P (t))
    {
      int i;
      for (i = TREE_OPERAND_LENGTH (t) - 1; i >= 0; --i)
	LTO_SET_PREVAIL (TREE_OPERAND (t, i));
    }
  else if (TREE_CODE (t) == CONSTRUCTOR)
    {
      unsigned i;
      tree val;
      FOR_EACH_CONSTRUCTOR_VALUE (CONSTRUCTOR_ELTS (t), i, val)
	LTO_SET_PREVAIL (val);
    }
  else
    {
      switch (code)
	{
	case TREE_LIST:
	  LTO_SET_PREVAIL (TREE_VALUE (t));
	  LTO_SET_PREVAIL (TREE_PURPOSE (t));
	  break;
	default:
	  gcc_unreachable ();
	}
    }
  /* If we fixed nothing, then we missed something seen by
     mentions_vars_p.  */
  gcc_checking_assert (fixed);
}
#undef LTO_SET_PREVAIL
#undef LTO_NO_PREVAIL

/* Helper function of lto_fixup_decls.  Walks the var and fn streams in STATE,
   replaces var and function decls with the corresponding prevailing def.  */

static void
lto_fixup_state (struct lto_in_decl_state *state)
{
  unsigned i, si;

  /* Although we only want to replace FUNCTION_DECLs and VAR_DECLs,
     we still need to walk from all DECLs to find the reachable
     FUNCTION_DECLs and VAR_DECLs.  */
  for (si = 0; si < LTO_N_DECL_STREAMS; si++)
    {
      vec<tree, va_gc> *trees = state->streams[si];
      for (i = 0; i < vec_safe_length (trees); i++)
	{
	  tree t = (*trees)[i];
	  if (flag_checking && TYPE_P (t))
	    verify_type (t);
	  if (VAR_OR_FUNCTION_DECL_P (t)
	      && (TREE_PUBLIC (t) || DECL_EXTERNAL (t)))
	    (*trees)[i] = lto_symtab_prevailing_decl (t);
	}
    }
}

/* Fix the decls from all FILES.  Replaces each decl with the corresponding
   prevailing one.  */

static void
lto_fixup_decls (struct lto_file_decl_data **files)
{
  unsigned int i;
  tree t;

  if (tree_with_vars)
    FOR_EACH_VEC_ELT ((*tree_with_vars), i, t)
      lto_fixup_prevailing_decls (t);

  for (i = 0; files[i]; i++)
    {
      struct lto_file_decl_data *file = files[i];
      struct lto_in_decl_state *state = file->global_decl_state;
      lto_fixup_state (state);

      hash_table<decl_state_hasher>::iterator iter;
      lto_in_decl_state *elt;
      FOR_EACH_HASH_TABLE_ELEMENT (*file->function_decl_states, elt,
				   lto_in_decl_state *, iter)
	lto_fixup_state (elt);
    }
}

static GTY((length ("lto_stats.num_input_files + 1"))) struct lto_file_decl_data **all_file_decl_data;

/* Turn file datas for sub files into a single array, so that they look
   like separate files for further passes.  */

static void
lto_flatten_files (struct lto_file_decl_data **orig, int count,
		   int last_file_ix)
{
  struct lto_file_decl_data *n, *next;
  int i, k;

  lto_stats.num_input_files = count;
  all_file_decl_data
    = ggc_cleared_vec_alloc<lto_file_decl_data_ptr> (count + 1);
  /* Set the hooks so that all of the ipa passes can read in their data.  */
  lto_set_in_hooks (all_file_decl_data, get_section_data, free_section_data);
  for (i = 0, k = 0; i < last_file_ix; i++)
    {
      for (n = orig[i]; n != NULL; n = next)
	{
	  all_file_decl_data[k++] = n;
	  next = n->next;
	  n->next = NULL;
	}
    }
  all_file_decl_data[k] = NULL;
  gcc_assert (k == count);
}

/* Input file data before flattening (i.e. splitting them to subfiles to support
   incremental linking.  */
static int real_file_count;
static GTY((length ("real_file_count + 1"))) struct lto_file_decl_data **real_file_decl_data;

/* Read all the symbols from the input files FNAMES.  NFILES is the
   number of files requested in the command line.  Instantiate a
   global call graph by aggregating all the sub-graphs found in each
   file.  */

void
read_cgraph_and_symbols (unsigned nfiles, const char **fnames)
{
  unsigned int i, last_file_ix;
  FILE *resolution;
  int count = 0;
  struct lto_file_decl_data **decl_data;
  symtab_node *snode;

  symtab->initialize ();

  timevar_push (TV_IPA_LTO_DECL_IN);

#ifdef ACCEL_COMPILER
  section_name_prefix = OFFLOAD_SECTION_NAME_PREFIX;
  lto_stream_offload_p = true;
#endif

  real_file_decl_data
    = decl_data = ggc_cleared_vec_alloc<lto_file_decl_data_ptr> (nfiles + 1);
  real_file_count = nfiles;

  /* Read the resolution file.  */
  resolution = NULL;
  if (resolution_file_name)
    {
      int t;
      unsigned num_objects;

      resolution = fopen (resolution_file_name, "r");
      if (resolution == NULL)
	fatal_error (input_location,
		     "could not open symbol resolution file: %m");

      t = fscanf (resolution, "%u", &num_objects);
      gcc_assert (t == 1);

      /* True, since the plugin splits the archives.  */
      gcc_assert (num_objects == nfiles);
    }
  symtab->state = LTO_STREAMING;

  canonical_type_hash_cache = new hash_map<const_tree, hashval_t> (251);
  gimple_canonical_types = htab_create (16381, gimple_canonical_type_hash,
					gimple_canonical_type_eq, NULL);
  gcc_obstack_init (&tree_scc_hash_obstack);
  tree_scc_hash = new hash_table<tree_scc_hasher> (4096);

  /* Register the common node types with the canonical type machinery so
     we properly share alias-sets across languages and TUs.  Do not
     expose the common nodes as type merge target - those that should be
     are already exposed so by pre-loading the LTO streamer caches.
     Do two passes - first clear TYPE_CANONICAL and then re-compute it.  */
  for (i = 0; i < itk_none; ++i)
    lto_register_canonical_types (integer_types[i], true);
  for (i = 0; i < stk_type_kind_last; ++i)
    lto_register_canonical_types (sizetype_tab[i], true);
  for (i = 0; i < TI_MAX; ++i)
    lto_register_canonical_types (global_trees[i], true);
  for (i = 0; i < itk_none; ++i)
    lto_register_canonical_types (integer_types[i], false);
  for (i = 0; i < stk_type_kind_last; ++i)
    lto_register_canonical_types (sizetype_tab[i], false);
  for (i = 0; i < TI_MAX; ++i)
    lto_register_canonical_types (global_trees[i], false);

  if (!quiet_flag)
    fprintf (stderr, "Reading object files:");

  /* Read all of the object files specified on the command line.  */
  for (i = 0, last_file_ix = 0; i < nfiles; ++i)
    {
      struct lto_file_decl_data *file_data = NULL;
      if (!quiet_flag)
	{
	  fprintf (stderr, " %s", fnames[i]);
	  fflush (stderr);
	}

      current_lto_file = lto_obj_file_open (fnames[i], false);
      if (!current_lto_file)
	break;

      file_data = lto_file_read (current_lto_file, resolution, &count);
      if (!file_data)
	{
	  lto_obj_file_close (current_lto_file);
	  free (current_lto_file);
	  current_lto_file = NULL;
	  break;
	}

      decl_data[last_file_ix++] = file_data;

      lto_obj_file_close (current_lto_file);
      free (current_lto_file);
      current_lto_file = NULL;
    }

  lto_flatten_files (decl_data, count, last_file_ix);
  lto_stats.num_input_files = count;
  ggc_free(decl_data);
  real_file_decl_data = NULL;

  lto_register_canonical_types_for_odr_types ();

  if (resolution_file_name)
    fclose (resolution);

  /* Show the LTO report before launching LTRANS.  */
  if (flag_lto_report || (flag_wpa && flag_lto_report_wpa))
    print_lto_report_1 ();

  /* Free gimple type merging datastructures.  */
  delete tree_scc_hash;
  tree_scc_hash = NULL;
  obstack_free (&tree_scc_hash_obstack, NULL);
  htab_delete (gimple_canonical_types);
  gimple_canonical_types = NULL;
  delete canonical_type_hash_cache;
  canonical_type_hash_cache = NULL;

  /* At this stage we know that majority of GGC memory is reachable.
     Growing the limits prevents unnecesary invocation of GGC.  */
  ggc_grow ();
  report_heap_memory_use ();

  /* Set the hooks so that all of the ipa passes can read in their data.  */
  lto_set_in_hooks (all_file_decl_data, get_section_data, free_section_data);

  timevar_pop (TV_IPA_LTO_DECL_IN);

  if (!quiet_flag)
    fprintf (stderr, "\nReading the symbol table:");

  timevar_push (TV_IPA_LTO_CGRAPH_IO);
  /* Read the symtab.  */
  input_symtab ();

  input_offload_tables (!flag_ltrans);

  /* Store resolutions into the symbol table.  */

  FOR_EACH_SYMBOL (snode)
    if (snode->externally_visible && snode->real_symbol_p ()
	&& snode->lto_file_data && snode->lto_file_data->resolution_map
	&& !(TREE_CODE (snode->decl) == FUNCTION_DECL
	     && fndecl_built_in_p (snode->decl))
	&& !(VAR_P (snode->decl) && DECL_HARD_REGISTER (snode->decl)))
      {
	ld_plugin_symbol_resolution_t *res;

	res = snode->lto_file_data->resolution_map->get (snode->decl);
	if (!res || *res == LDPR_UNKNOWN)
	  {
	    if (snode->output_to_lto_symbol_table_p ())
	      fatal_error (input_location, "missing resolution data for %s",
			   IDENTIFIER_POINTER
			     (DECL_ASSEMBLER_NAME (snode->decl)));
	  }
	/* Symbol versions are always used externally, but linker does not
	   report that correctly.
	   This is binutils PR25924.  */
	else if (snode->symver && *res == LDPR_PREVAILING_DEF_IRONLY)
	  snode->resolution = LDPR_PREVAILING_DEF_IRONLY_EXP;
	else
	  snode->resolution = *res;
      }
  for (i = 0; all_file_decl_data[i]; i++)
    if (all_file_decl_data[i]->resolution_map)
      {
	delete all_file_decl_data[i]->resolution_map;
	all_file_decl_data[i]->resolution_map = NULL;
      }

  timevar_pop (TV_IPA_LTO_CGRAPH_IO);

  if (!quiet_flag)
    fprintf (stderr, "\nMerging declarations:");

  timevar_push (TV_IPA_LTO_DECL_MERGE);
  /* Merge global decls.  In ltrans mode we read merged cgraph, we do not
     need to care about resolving symbols again, we only need to replace
     duplicated declarations read from the callgraph and from function
     sections.  */
  if (!flag_ltrans)
    {
      lto_symtab_merge_decls ();

      /* If there were errors during symbol merging bail out, we have no
	 good way to recover here.  */
      if (seen_error ())
	fatal_error (input_location,
		     "errors during merging of translation units");

      /* Fixup all decls.  */
      lto_fixup_decls (all_file_decl_data);
    }
  if (tree_with_vars)
    ggc_free (tree_with_vars);
  tree_with_vars = NULL;
  /* During WPA we want to prevent ggc collecting by default.  Grow limits
     until after the IPA summaries are streamed in.  Basically all IPA memory
     is explcitly managed by ggc_free and ggc collect is not useful.
     Exception are the merged declarations.  */
  ggc_grow ();
  report_heap_memory_use ();

  timevar_pop (TV_IPA_LTO_DECL_MERGE);
  /* Each pass will set the appropriate timer.  */

  if (!quiet_flag)
    fprintf (stderr, "\nReading summaries:");

  /* Read the IPA summary data.  */
  if (flag_ltrans)
    ipa_read_optimization_summaries ();
  else
    ipa_read_summaries ();

  ggc_grow ();

  for (i = 0; all_file_decl_data[i]; i++)
    {
      gcc_assert (all_file_decl_data[i]->symtab_node_encoder);
      lto_symtab_encoder_delete (all_file_decl_data[i]->symtab_node_encoder);
      all_file_decl_data[i]->symtab_node_encoder = NULL;
      lto_in_decl_state *global_decl_state
	= all_file_decl_data[i]->global_decl_state;
      lto_free_function_in_decl_state (global_decl_state);
      all_file_decl_data[i]->global_decl_state = NULL;
      all_file_decl_data[i]->current_decl_state = NULL;
    }

  if (!flag_ltrans)
    {
      /* Finally merge the cgraph according to the decl merging decisions.  */
      timevar_push (TV_IPA_LTO_CGRAPH_MERGE);

      if (!quiet_flag)
	fprintf (stderr, "\nMerging symbols:");

      gcc_assert (!dump_file);
      dump_file = dump_begin (lto_link_dump_id, NULL);

      if (dump_file)
	{
	  fprintf (dump_file, "Before merging:\n");
	  symtab->dump (dump_file);
	}
      lto_symtab_merge_symbols ();
      /* Removal of unreachable symbols is needed to make verify_symtab to pass;
	 we are still having duplicated comdat groups containing local statics.
	 We could also just remove them while merging.  */
      symtab->remove_unreachable_nodes (dump_file);
      ggc_collect ();
      report_heap_memory_use ();

      if (dump_file)
	dump_end (lto_link_dump_id, dump_file);
      dump_file = NULL;
      timevar_pop (TV_IPA_LTO_CGRAPH_MERGE);
    }
  symtab->state = IPA_SSA;
  /* All node removals happening here are useless, because
     WPA should not stream them.  Still always perform remove_unreachable_nodes
     because we may reshape clone tree, get rid of dead masters of inline
     clones and remove symbol entries for read-only variables we keep around
     only to be able to constant fold them.  */
  if (flag_ltrans)
    {
      if (symtab->dump_file)
	 symtab->dump (symtab->dump_file);
      symtab->remove_unreachable_nodes (symtab->dump_file);
    }

  /* Indicate that the cgraph is built and ready.  */
  symtab->function_flags_ready = true;

  ggc_free (all_file_decl_data);
  all_file_decl_data = NULL;
}



/* Show various memory usage statistics related to LTO.  */
void
print_lto_report_1 (void)
{
  const char *pfx = (flag_lto) ? "LTO" : (flag_wpa) ? "WPA" : "LTRANS";
  fprintf (stderr, "%s statistics\n", pfx);

  fprintf (stderr, "[%s] read %lu unshared trees\n",
	   pfx, num_unshared_trees_read);
  fprintf (stderr, "[%s] read %lu mergeable SCCs of average size %f\n",
	   pfx, num_sccs_read, total_scc_size / (double)num_sccs_read);
  fprintf (stderr, "[%s] %lu tree bodies read in total\n", pfx,
	   total_scc_size + num_unshared_trees_read);
  if (flag_wpa && tree_scc_hash && num_sccs_read)
    {
      fprintf (stderr, "[%s] tree SCC table: size %ld, %ld elements, "
	       "collision ratio: %f\n", pfx,
	       (long) tree_scc_hash->size (),
	       (long) tree_scc_hash->elements (),
	       tree_scc_hash->collisions ());
      hash_table<tree_scc_hasher>::iterator hiter;
      tree_scc *scc, *max_scc = NULL;
      unsigned max_length = 0;
      FOR_EACH_HASH_TABLE_ELEMENT (*tree_scc_hash, scc, x, hiter)
	{
	  unsigned length = 0;
	  tree_scc *s = scc;
	  for (; s; s = s->next)
	    length++;
	  if (length > max_length)
	    {
	      max_length = length;
	      max_scc = scc;
	    }
	}
      fprintf (stderr, "[%s] tree SCC max chain length %u (size %u)\n",
	       pfx, max_length, max_scc->len);
      fprintf (stderr, "[%s] Compared %lu SCCs, %lu collisions (%f)\n", pfx,
	       num_scc_compares, num_scc_compare_collisions,
	       num_scc_compare_collisions / (double) num_scc_compares);
      fprintf (stderr, "[%s] Merged %lu SCCs\n", pfx, num_sccs_merged);
      fprintf (stderr, "[%s] Merged %lu tree bodies\n", pfx,
	       total_scc_size_merged);
      fprintf (stderr, "[%s] Merged %lu types\n", pfx, num_merged_types);
      fprintf (stderr, "[%s] %lu types prevailed (%lu associated trees)\n",
	       pfx, num_prevailing_types, num_type_scc_trees);
      fprintf (stderr, "[%s] GIMPLE canonical type table: size %ld, "
	       "%ld elements, %ld searches, %ld collisions (ratio: %f)\n", pfx,
	       (long) htab_size (gimple_canonical_types),
	       (long) htab_elements (gimple_canonical_types),
	       (long) gimple_canonical_types->searches,
	       (long) gimple_canonical_types->collisions,
	       htab_collisions (gimple_canonical_types));
      fprintf (stderr, "[%s] GIMPLE canonical type pointer-map: "
	       "%lu elements, %ld searches\n", pfx,
	       num_canonical_type_hash_entries,
	       num_canonical_type_hash_queries);
    }

  print_lto_report (pfx);
}

GTY(()) tree lto_eh_personality_decl;

/* Return the LTO personality function decl.  */

tree
lto_eh_personality (void)
{
  if (!lto_eh_personality_decl)
    {
      /* Use the first personality DECL for our personality if we don't
	 support multiple ones.  This ensures that we don't artificially
	 create the need for them in a single-language program.  */
      if (first_personality_decl && !dwarf2out_do_cfi_asm ())
	lto_eh_personality_decl = first_personality_decl;
      else
	lto_eh_personality_decl = lhd_gcc_personality ();
    }

  return lto_eh_personality_decl;
}

/* Set the process name based on the LTO mode.  */

static void
lto_process_name (void)
{
  if (flag_lto)
    setproctitle (flag_incremental_link == INCREMENTAL_LINK_LTO
		  ? "lto1-inclink" : "lto1-lto");
  if (flag_wpa)
    setproctitle ("lto1-wpa");
  if (flag_ltrans)
    setproctitle ("lto1-ltrans");
}


/* Initialize the LTO front end.  */

void
lto_fe_init (void)
{
  lto_process_name ();
  lto_streamer_hooks_init ();
  lto_reader_init ();
  lto_set_in_hooks (NULL, get_section_data, free_section_data);
  memset (&lto_stats, 0, sizeof (lto_stats));
  bitmap_obstack_initialize (NULL);
  gimple_register_cfg_hooks ();
#ifndef ACCEL_COMPILER
  unsigned char *table
    = ggc_vec_alloc<unsigned char> (MAX_MACHINE_MODE);
  for (int m = 0; m < MAX_MACHINE_MODE; m++)
    table[m] = m;
  lto_mode_identity_table = table;
#endif
}

#include "gt-lto-lto-common.h"
