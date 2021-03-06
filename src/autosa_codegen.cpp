#include <isl/aff.h>

#include "autosa_codegen.h"
#include "autosa_utils.h"
#include "autosa_print.h"
#include "autosa_schedule_tree.h"
#include "autosa_comm.h"

/* This function examines if the accessed elements of the I/O group 
 * are fully overlapped at the PE level.
 * We will create a relation "overlap"
 * 
 *  [[D -> R] -> [D' -> R']]
 * 
 * of pairs of domain iterations accessing the reference group and 
 * the domain iteraetions D' is lexicographically greater than D by one 
 * at the last array_part loop with PE loops equal.
 * 
 * This relation is intersected with all flow dependences to derive the 
 * pairs of iterations that overlapped due to the flow dependence.
 * 
 * Next, we construct a relation "external"
 * that contains pair of iteration domains with flow dependences that 
 * access the elements by the I/O group.
 * 
 * We substract "overlap" from "external". If the diff is null, clearly
 * the accessed elements are overlapped between different array partitions 
 * for one PE, we will return true for this case.
 * Otherwise, we return false.
 */
static isl_bool internal_group_in_out_overlap(
  __isl_keep isl_schedule_node *node,
  struct autosa_kernel *kernel,
  struct autosa_array_ref_group *group, int read)
{
  int empty;
  struct autosa_prog *prog = kernel->prog;
  isl_union_pw_multi_aff *tagger;
  isl_union_map *prefix;
  isl_union_map *access, *tagged;
  isl_union_set *domain;
  isl_set *prefix_range;
  isl_map *lt;
  int n_sched_dim;
  isl_union_map *overlap;
  isl_union_map *external, *universe;
  isl_union_set *access_domain;
  isl_union_set *tag_set;
  isl_map *sched_identity;
  int pe_depth, array_depth;

  node = isl_schedule_node_copy(node);
  node = autosa_tree_move_down_to_array(node, kernel->core);
  array_depth = isl_schedule_node_get_schedule_depth(node);
  node = autosa_tree_move_down_to_pe(node, kernel->core);
  pe_depth = isl_schedule_node_get_schedule_depth(node);
  prefix = isl_schedule_node_get_prefix_schedule_relation(node);
  prefix = isl_union_map_preimage_domain_union_pw_multi_aff(prefix,
              isl_union_pw_multi_aff_copy(kernel->contraction)); 
  isl_schedule_node_free(node);
  access = autosa_io_group_access_relation(group, read, !read); 
  tagged = group_tagged_access_relation(group); 
 
  /* Remove the local dependency first. */
  access = remove_local_accesses_group_flow(kernel, group, access, prefix, read);

  /* Tagger maps the tagged iteration domain to untagged iteration domain.
   * Iteration domain is tagged to the access function.
   * e.g. [S1[i,j,k] -> _pet_ref_1[]] -> S1[(i),(j),(k)]
   */
  tagger = isl_union_pw_multi_aff_copy(prog->scop->tagger);
  domain = isl_union_map_domain(isl_union_map_copy(tagged));
  tagger = isl_union_pw_multi_aff_intersect_domain(tagger, 
            isl_union_set_copy(domain));
  prefix = isl_union_map_preimage_domain_union_pw_multi_aff(prefix, tagger); 
  
  prefix_range = isl_set_from_union_set(isl_union_map_range(isl_union_map_copy(prefix))); 
  n_sched_dim = isl_set_dim(prefix_range, isl_dim_set);
  sched_identity = isl_set_identity(isl_set_copy(prefix_range));

  lt = isl_map_lex_lt_first(isl_map_get_space(sched_identity), array_depth); 
  isl_map_free(sched_identity);

  /* Set the space dims equal. */
  for (int i = array_depth; i < n_sched_dim; i++) {
    lt = isl_map_equate(lt, isl_dim_in, i, isl_dim_out, i);
  }
  lt = isl_map_intersect_domain(lt, isl_set_copy(prefix_range));
  lt = isl_map_intersect_range(lt, prefix_range);
  lt = isl_map_lexmin(lt); 
  
  overlap = isl_union_map_apply_range(isl_union_map_copy(prefix), 
              isl_union_map_from_map(lt));
  overlap = isl_union_map_apply_range(overlap, isl_union_map_reverse(prefix)); 

  /* Derive the overlapping set. */
  overlap = isl_union_map_intersect(overlap, 
              isl_union_map_copy(prog->scop->tagged_dep_flow)); 
  empty = isl_union_map_is_empty(overlap);

  external = isl_union_map_copy(prog->scop->tagged_dep_flow); 
  universe = isl_union_map_universe(isl_union_map_copy(access)); 
  access_domain = isl_union_map_domain(universe); 
  domain = isl_union_set_universe(domain); 
  universe = isl_union_set_unwrap(domain); 
  universe = isl_union_map_intersect_domain(universe, access_domain); 
  /* D -> __pet_ref_1 */
  domain = isl_union_map_wrap(universe);
  if (read)
    external = isl_union_map_intersect_range(external, domain);
  else
    external = isl_union_map_intersect_domain(external, domain);
  external = isl_union_map_intersect_params(external,
              isl_set_copy(prog->scop->context));
  /* external contains flow dep that are associated with the group access. */
  
  external = isl_union_map_subtract(external, overlap);
  /* external only contains access non-overlap RAW pairs. */

  if (read) {
    tag_set = isl_union_map_range(external);
    external = wrapped_reference_to_access(tag_set, tagged); 
  } else {
    tag_set = isl_union_map_domain(external);
    external = wrapped_reference_to_access(tag_set, tagged);
  }

  if (empty < 0) 
    external = isl_union_map_free(external);
  else if (empty)
    external = isl_union_map_universe(external);

  access = isl_union_map_intersect(access, external); 
  empty = isl_union_map_is_empty(access);
  isl_union_map_free(access);

  if (empty)
    return isl_bool_true;
  else
    return isl_bool_false;
}

/* Return if the current module is valid to be generated. 
 * There are several cases to consider:
 * - For I/O group with all RAR depenendence, no copy-out modules to be generated.
 * - For I/O group with either RAW/RAR dependence, if the next read equals 
 *   the previous write, no copy-in/copy-out to be generated.
 */
static isl_bool is_module_valid(
  __isl_keep isl_schedule_node *node,  
  struct autosa_kernel *kernel, 
  struct autosa_array_ref_group *group, int read)
{
  int external_group = 1;

  if (group->group_type == AUTOSA_PE_GROUP)
    return isl_bool_true;

  /* External group */
  for (int i = 0; i < group->n_ref; i++) {
    struct autosa_stmt_access *ref = group->refs[i];
    for (int j = 0; j < ref->n_io_info; j++) {
      struct autosa_io_info *io_info = ref->io_info[j];
      if (io_info->io_type == group->io_type && !isl_vec_cmp(io_info->dir, group->dir)) {
        if (io_info->dep->type != AUTOSA_DEP_RAR) {
          external_group = 0;
          break;
        }
      }
    }
  }

  if (external_group) {
    if (read)
      return isl_bool_true;
    else
      return isl_bool_false;
  }  

  /* Internal group */
  if (internal_group_in_out_overlap(node, kernel, group, read))
    return isl_bool_false;

  return isl_bool_true;
}

/* Generate the I/O module name.
 * [io_group_name]_IO_L[X]_in/out
 */
static char *generate_io_module_name(isl_ctx *ctx, 
  struct autosa_array_ref_group *group, int level, int read) {
  isl_printer *p;

  p = isl_printer_to_str(ctx);
  p = isl_printer_print_str(p, group->array->name);
  if (group->group_type == AUTOSA_IO_GROUP) {
    if (group->local_array->n_io_group > 1) {
      p = isl_printer_print_str(p, "_");
      p = isl_printer_print_int(p, group->nr);
    }
  } else if (group->group_type == AUTOSA_DRAIN_GROUP) {
    p = isl_printer_print_str(p, "_");
    p = isl_printer_print_str(p, "drain");
  }
  p = isl_printer_print_str(p, "_IO_L");
  p = isl_printer_print_int(p, level);
  if (read)
    p = isl_printer_print_str(p, "_in");
  else
    p = isl_printer_print_str(p, "_out");

  char *str = isl_printer_get_str(p);
  isl_printer_free(p);

  return str;
}

/* Add "len" parameters p[i] with identifiers "ids" and intersect "set"
 * with
 *
 *	{ : 0 <= p[i] < size[i] }
 *
 * or an overapproximation.
 */
static __isl_give isl_set *add_bounded_parameters_dynamic(
	__isl_take isl_set *set, __isl_keep isl_multi_pw_aff *size,
	__isl_keep isl_id_list *ids)
{
	int i, len;
	unsigned nparam;
	isl_space *space;
	isl_local_space *ls;

	len = isl_multi_pw_aff_dim(size, isl_dim_out);
	nparam = isl_set_dim(set, isl_dim_param);
	set = isl_set_add_dims(set, isl_dim_param, len);

	for (i = 0; i < len; ++i) {
		isl_id *id;

		id = isl_id_list_get_id(ids, i);
		set = isl_set_set_dim_id(set, isl_dim_param, nparam + i, id);
	}

	space = isl_space_params(isl_set_get_space(set));
	ls = isl_local_space_from_space(space);
	for (i = 0; i < len; ++i) {
		isl_pw_aff *param, *size_i, *zero;
		isl_set *bound;

		param = isl_pw_aff_var_on_domain(isl_local_space_copy(ls),
						isl_dim_param, nparam + i);

		size_i = isl_multi_pw_aff_get_pw_aff(size, i);
		bound = isl_pw_aff_lt_set(isl_pw_aff_copy(param), size_i);
		bound = isl_set_from_basic_set(isl_set_simple_hull(bound));
		set = isl_set_intersect_params(set, bound);

		zero = isl_pw_aff_zero_on_domain(isl_local_space_copy(ls));
		bound = isl_pw_aff_ge_set(param, zero);
		set = isl_set_intersect_params(set, bound);
	}
	isl_local_space_free(ls);

	return set;
}

/* Return an isl_multi_aff, with as elements the parameters in "space"
 * that have the names specified by the elements in "names".
 * If (some of) these parameters do not already appear in "space",
 * then they are added first.
 */
static __isl_give isl_multi_aff *parameter_vector(__isl_take isl_space *space,
	__isl_keep isl_id_list *names)
{
	int i, n;
	isl_local_space *ls;
	isl_multi_aff *ma;

	if (!names)
		space = isl_space_free(space);

	n = isl_id_list_n_id(names);
	for (i = 0; i < n; ++i) {
		int pos;
		isl_id *id;

		id = isl_id_list_get_id(names, i);
		pos = isl_space_find_dim_by_id(space, isl_dim_param, id);
		if (pos >= 0) {
			isl_id_free(id);
			continue;
		}
		pos = isl_space_dim(space, isl_dim_param);
		space = isl_space_add_dims(space, isl_dim_param, 1);
		space = isl_space_set_dim_id(space, isl_dim_param, pos, id);
	}
	ma = isl_multi_aff_zero(isl_space_copy(space));
	ls = isl_local_space_from_space(isl_space_domain(space));
	for (i = 0; i < n; ++i) {
		int pos;
		isl_id *id;
		isl_aff *aff;

		id = isl_id_list_get_id(names, i);
		pos = isl_space_find_dim_by_id(space, isl_dim_param, id);
		isl_id_free(id);
		aff = isl_aff_var_on_domain(isl_local_space_copy(ls),
					    isl_dim_param, pos);
		ma = isl_multi_aff_set_aff(ma, i, aff);
	}
	isl_local_space_free(ls);

	return ma;
}

/* Return constraints on the domain elements that are greater or equal 
 * to a sequence of parameters called "names", to the partial schedule of "node".
 * The number of members of the band node "node" should be smaller
 * than or equal to the number of elements in "names". 
 * If it is smaller, then the first elements of "names" are equated to zero.
 */
static __isl_give isl_union_set *set_schedule_ge(
  __isl_keep isl_schedule_node *node, __isl_keep isl_id_list *names)
{
  int n, n_zero;
  isl_multi_union_pw_aff *mupa, *mupa2;
  isl_multi_aff *ma;
  isl_space *space;
  isl_union_set *domain;

  if (!node)
    return NULL;
  n = isl_id_list_n_id(names);
  if (n == 0)
    return isl_schedule_node_get_universe_domain(node);
  n_zero = n - isl_schedule_node_band_n_member(node);

  mupa = isl_schedule_node_band_get_partial_schedule(node);
  space = isl_multi_union_pw_aff_get_space(mupa);
  space = isl_space_params(space);
  space = isl_space_set_from_params(space);
  space = isl_space_add_dims(space, isl_dim_set, n_zero);
  ma = isl_multi_aff_zero(space);
  domain = isl_schedule_node_get_universe_domain(node);
  /* Generate the mupa that is on the same domain of partial schedule, with
   * a function that maps to the n_zero dims to zero. */
  mupa2 = isl_multi_union_pw_aff_multi_aff_on_domain(
            isl_union_set_copy(domain), ma);
  
  /* Generate the mupa with the n_zero dims as paramters and equal zero. */
  mupa = isl_multi_union_pw_aff_range_product(mupa2, mupa);  
  space = isl_multi_union_pw_aff_get_space(mupa);
  ma = parameter_vector(space, names);
  /* Generate the mupa that is on the same domain of partial schedule, with
   * a function that maps the domain elements to the parameters. */ 
  mupa2 = isl_multi_union_pw_aff_multi_aff_on_domain(domain, ma);
  mupa = isl_multi_union_pw_aff_sub(mupa, mupa2);

  return isl_multi_union_pw_aff_nonneg_union_set(mupa);
}

/* Return constraints on the domain elements that less or equal to a sequence of
 * parameters called "names", to the partial schedule of "node".
 * The number of members of the band node "node" should be smaller
 * than or equal to the number of elements in "names". 
 * If it is smaller, then the first elements of "names" are equated to zero.
 */
static __isl_give isl_union_set *set_schedule_le(
  __isl_keep isl_schedule_node *node, __isl_keep isl_id_list *names)
{
  int n, n_zero;
  isl_multi_union_pw_aff *mupa, *mupa2;
  isl_multi_aff *ma;
  isl_space *space;
  isl_union_set *domain;

  if (!node)
    return NULL;
  n = isl_id_list_n_id(names);
  if (n == 0)
    return isl_schedule_node_get_universe_domain(node);
  n_zero = n - isl_schedule_node_band_n_member(node);

  mupa = isl_schedule_node_band_get_partial_schedule(node);
  space = isl_multi_union_pw_aff_get_space(mupa);
  space = isl_space_params(space);
  space = isl_space_set_from_params(space);
  space = isl_space_add_dims(space, isl_dim_set, n_zero);
  ma = isl_multi_aff_zero(space);
  domain = isl_schedule_node_get_universe_domain(node);
  /* Generate the mupa that is on the same domain of partial schedule, with
   * a function that maps to the n_zero dims to zero. */
  mupa2 = isl_multi_union_pw_aff_multi_aff_on_domain(
            isl_union_set_copy(domain), ma);
  
  /* Generate the mupa with the n_zero dims as paramters and equal zero. */
  mupa = isl_multi_union_pw_aff_range_product(mupa2, mupa);  
  space = isl_multi_union_pw_aff_get_space(mupa);
  ma = parameter_vector(space, names);
  /* Generate the mupa that is on the same domain of partial schedule, with
   * a function that maps the domain elements to the parameters. */ 
  mupa2 = isl_multi_union_pw_aff_multi_aff_on_domain(domain, ma);
  mupa = isl_multi_union_pw_aff_sub(mupa2, mupa);

  return isl_multi_union_pw_aff_nonneg_union_set(mupa);
}

/* Construct an isl_multi_val for use as tile sizes for tiling "node"
 * from the elements in "tile_size".
 */
static __isl_give isl_multi_val *construct_band_tiles_sizes(
	__isl_keep isl_schedule_node *node, int *tile_size)
{
	isl_space *space;

	if (!node)
		return NULL;

	space = isl_schedule_node_band_get_space(node);
	return ppcg_multi_val_from_int_list(space, tile_size);
}

/* Return constraints on the domain elements that equate a sequence of
 * parameters called "names", to the partial schedule
 * of "node" modulo the integers in "size".
 * The number of elements in the array "size" should be equal
 * to the number of elements in "names".
 * The number of members of the band node "node" should be smaller
 * than or equal to this number.  If it is smaller, then the first
 * elements of "names" are equated to zero.
 */
static __isl_give isl_union_set *set_schedule_modulo(
	__isl_keep isl_schedule_node *node, __isl_keep isl_id_list *names,
	int *size)
{
	int n, n_zero;
	isl_space *space;
	isl_multi_aff *ma;
	isl_multi_union_pw_aff *mupa, *mupa2;
	isl_multi_val *mv;
	isl_union_set *domain;

	if (!node)
		return NULL;
	n = isl_id_list_n_id(names);
	if (n == 0)
		return isl_schedule_node_get_universe_domain(node);
	n_zero = n - isl_schedule_node_band_n_member(node);

	mupa = isl_schedule_node_band_get_partial_schedule(node);
	mv = construct_band_tiles_sizes(node, size + n_zero);
	mupa = isl_multi_union_pw_aff_mod_multi_val(mupa, mv);
	space = isl_multi_union_pw_aff_get_space(mupa);
	space = isl_space_params(space);
	space = isl_space_set_from_params(space);
	space = isl_space_add_dims(space, isl_dim_set, n_zero);
	ma = isl_multi_aff_zero(space);

	domain = isl_schedule_node_get_universe_domain(node);
	mupa2 = isl_multi_union_pw_aff_multi_aff_on_domain(
						isl_union_set_copy(domain), ma);
	mupa = isl_multi_union_pw_aff_range_product(mupa2, mupa);

	space = isl_multi_union_pw_aff_get_space(mupa);
	ma = parameter_vector(space, names);

	mupa2 = isl_multi_union_pw_aff_multi_aff_on_domain(domain, ma);
	mupa = isl_multi_union_pw_aff_sub(mupa, mupa2);

	return isl_multi_union_pw_aff_zero_union_set(mupa);
}

/* Return constraints on the domain elements that equate a sequence of
 * parameters called "names", to the partial schedule of "node".
 * The number of members of the band node "node" should be smaller
 * than or equal to the number of elements in "names". 
 * If it is smaller, then the first elements of "names" are equated to zero.
 */
static __isl_give isl_union_set *set_schedule_eq(
  __isl_keep isl_schedule_node *node, __isl_keep isl_id_list *names)
{
  int n, n_zero;
  isl_multi_union_pw_aff *mupa, *mupa2;
  isl_multi_aff *ma;
  isl_space *space;
  isl_union_set *domain;

  if (!node)
    return NULL;
  n = isl_id_list_n_id(names);
  if (n == 0)
    return isl_schedule_node_get_universe_domain(node);
  n_zero = n - isl_schedule_node_band_n_member(node);

  mupa = isl_schedule_node_band_get_partial_schedule(node);
  space = isl_multi_union_pw_aff_get_space(mupa);
  space = isl_space_params(space);
  space = isl_space_set_from_params(space);
  space = isl_space_add_dims(space, isl_dim_set, n_zero);
  ma = isl_multi_aff_zero(space);

  domain = isl_schedule_node_get_universe_domain(node);
  /* Map the domain elements to "n_zero" zeros. */
  mupa2 = isl_multi_union_pw_aff_multi_aff_on_domain(
            isl_union_set_copy(domain), ma);
  /* Build a new mupa that mupa2 -> mupa */
  mupa = isl_multi_union_pw_aff_range_product(mupa2, mupa);  
  space = isl_multi_union_pw_aff_get_space(mupa);
  ma = parameter_vector(space, names);
  mupa2 = isl_multi_union_pw_aff_multi_aff_on_domain(domain, ma);
  mupa = isl_multi_union_pw_aff_sub(mupa, mupa2);

  return isl_multi_union_pw_aff_zero_union_set(mupa);
}

/* Generate two prefixes: fifo_prefix and buffer_prefix
 * fifo_prefix: fifo_A_0
 * buffer_prefix: local_A_0
 */
static void init_suffix(struct autosa_hw_module *module, 
  struct autosa_array_ref_group *group, char **fifo_suffix, char **buf_suffix) 
{
  isl_ctx *ctx = isl_map_get_ctx(group->access);

  isl_printer *p = isl_printer_to_str(ctx);
  p = autosa_array_ref_group_print_fifo_name(group, p);
  *fifo_suffix = isl_printer_get_str(p);
  isl_printer_free(p);

  p = isl_printer_to_str(ctx);
  p = isl_printer_print_str(p, "local_");
  p = isl_printer_print_str(p, group->array->name);
  if ((group->group_type == AUTOSA_IO_GROUP && group->local_array->n_io_group > 1) ||
    (group->group_type == AUTOSA_PE_GROUP && group->local_array->n_pe_group > 1))
  {
    p = isl_printer_print_str(p, "_");  
    p = isl_printer_print_int(p, group->nr);
  }  
  if (group->group_type == AUTOSA_DRAIN_GROUP) {
    p = isl_printer_print_str(p, "_");
    p = isl_printer_print_str(p, "drain");
  }
  *buf_suffix = isl_printer_get_str(p);
  isl_printer_free(p);
}

/* Return constraints on the domain elements that equate the partial schedule
 * of "node" to the lower bound of partial schedule. 
 */
static __isl_give isl_union_set *schedule_eq_lb(
  __isl_keep isl_schedule_node *node)
{
  int n, n_zero;
  isl_multi_union_pw_aff *mupa, *mupa2;
  isl_multi_aff *ma;
  isl_space *space;
  isl_union_set *domain;
  isl_union_map *umap;
  isl_union_set *uset;
  isl_schedule_node *node2;
  isl_bool under_extension = isl_bool_false;

  if (!node)
    return NULL;

  /* Test if it is under extension node */
  node2 = isl_schedule_node_copy(node);
  while (node2) {
    if (isl_schedule_node_get_type(node2) == isl_schedule_node_extension) {
      under_extension = isl_bool_true;
      break;
    }
    if (isl_schedule_node_has_parent(node2))
      node2 = isl_schedule_node_parent(node2);
    else 
      break;
  }
  isl_schedule_node_free(node2);
  
  umap = isl_schedule_node_band_get_partial_schedule_union_map(node);  
  if (!under_extension) {
    domain = isl_schedule_node_get_domain(node);
    umap = isl_union_map_intersect_domain(umap, domain);
  }
  uset = isl_union_map_range(isl_union_map_copy(umap));
  uset = isl_union_set_lexmin(uset);
  umap = isl_union_map_reverse(umap);
  uset = isl_union_set_apply(uset, umap);

  return uset; 
}

/* Return constraints on the domain elements that not equate the partial schedule
 * of "node" to the lower bound of partial schedule. 
 */
static __isl_give isl_union_set *schedule_neq_lb(
  __isl_keep isl_schedule_node *node)
{
  isl_union_set *uset, *domain;
  isl_union_map *umap;

  if (!node)
    return NULL;

  uset = schedule_eq_lb(node);
  umap = isl_schedule_node_band_get_partial_schedule_union_map(node);
  domain = isl_union_map_domain(umap);
  uset = isl_union_set_subtract(domain, uset);

  return uset;
}

/* Return constraints on the domain elements that equate the partial schedule
 * of "node" to the upper bound of partial schedule. 
 */
static __isl_give isl_union_set *schedule_eq_ub(
  __isl_keep isl_schedule_node *node)
{
  int n, n_zero;
  isl_multi_union_pw_aff *mupa, *mupa2;
  isl_multi_aff *ma;
  isl_space *space;
  isl_union_set *domain;
  isl_union_map *umap;
  isl_union_set *uset;

  if (!node)
    return NULL;

  domain = isl_schedule_node_get_domain(node);
  umap = isl_schedule_node_band_get_partial_schedule_union_map(node);
  umap = isl_union_map_intersect_domain(umap, domain);
  uset = isl_union_map_range(isl_union_map_copy(umap));
  uset = isl_union_set_lexmax(uset);
  umap = isl_union_map_reverse(umap);
  uset = isl_union_set_apply(uset, umap);

  return uset;  
}

/* Return constraints on the domain elements that not equate the partial schedule
 * of "node" to the upper bound of partial schedule. 
 */
static __isl_give isl_union_set *schedule_neq_ub(
  __isl_keep isl_schedule_node *node)
{
  isl_union_set *uset, *domain, *sched_domain;
  isl_union_map *umap;

  if (!node)
    return NULL;

  uset = schedule_eq_ub(node);
  domain = isl_schedule_node_get_domain(node);
  umap = isl_schedule_node_band_get_partial_schedule_union_map(node);
  umap = isl_union_map_intersect_domain(umap, domain);
  sched_domain = isl_union_map_domain(umap);
  uset = isl_union_set_subtract(sched_domain, uset);

  return uset;
}

/* Internal struct used for add_io_copies_stmt_acc. */
struct add_io_copies_stmt_acc_data {
  struct autosa_kernel *kernel;
  struct autosa_array_ref_group *group;
  struct autosa_stmt_access *ref;
  struct autosa_array_tile *local_tile;
  int n_lane;
  int read;
  char *stmt_name;
  int insert_dependence;
};

/* Create an IO statement. 
 * "io_group" is the current I/O group that is analyzed.
 * "local_tile" is the tile that the current IO stmt accesses.
 * "depth" is the schedule depth that the current stmt is inserted at.
 */
static __isl_give isl_multi_aff *autosa_create_io_access_stmt(
  isl_ctx *ctx,
  struct autosa_array_ref_group *local_group,
  struct autosa_array_ref_group *io_group,
  struct autosa_array_tile *tile,
  int depth,
  __isl_keep char *stmt_name)
{
  isl_space *space;
  isl_id *id;  
  char buf[100];
  struct autosa_array_ref_group_pair *pair = 
    (struct autosa_array_ref_group_pair *)malloc(
      sizeof(struct autosa_array_ref_group_pair));
  pair->local_group = local_group;
  pair->io_group = io_group;
  pair->local_tile = tile;
  pair->in_use = 0;

  space = isl_space_copy(io_group->array->space);
  space = isl_space_from_range(space);
  space = isl_space_add_dims(space, isl_dim_in, depth);
  space = isl_space_wrap(space);
  space = isl_space_map_from_set(space);
  
  sprintf(buf, "%s", stmt_name);

  id = isl_id_alloc(ctx, buf, pair);
  id = isl_id_set_free_user(id, &free_group_pair);
  space = isl_space_set_tuple_id(space, isl_dim_in, id);

  return isl_multi_aff_identity(space);
}

/* Test if the array access "ref" is stride-0 or stride-1 under the current
 * schedule node.
 */
static isl_bool is_acc_stride_one_at_node(
  __isl_keep isl_schedule_node *node, struct autosa_stmt_access *ref)
{
  isl_union_set *domain;
  isl_union_map *prefix;
  isl_map *acc;
  isl_bool is_zero = isl_bool_false, is_one = isl_bool_false;
  
  //domain = isl_schedule_node_get_domain(node);
  prefix = isl_schedule_node_get_prefix_schedule_union_map(node);

  /* Scalar access */
  if (ref->n_index == 0)
    return isl_bool_true;

  /* Transform the domain of access function to scheduling domains. */
  acc = isl_map_copy(ref->access);
  acc = isl_map_from_union_map(
          isl_union_map_apply_domain(isl_union_map_from_map(acc), prefix));  
#ifdef _DEBUG
  isl_printer *pd = isl_printer_to_file(isl_schedule_node_get_ctx(node), stdout);
  pd = isl_printer_print_map(pd, acc);
  pd = isl_printer_end_line(pd);
  isl_printer_free(pd);
#endif
  is_one = access_is_stride_one(acc, ref->n_index - 1);

  isl_map_free(acc);
  //isl_union_set_free(domain);
  return is_one;
}

/* Insert the copy statement at the statement level.
 */
static __isl_give isl_schedule_node *add_io_copies_stmt_acc_single(
  __isl_take isl_schedule_node *node, void *user)
{
  struct add_io_copies_stmt_acc_data *data = 
          (struct add_io_copies_stmt_acc_data *)(user);
  struct autosa_array_ref_group *group = data->group;
  struct autosa_stmt_access *ref = data->ref;
  char *stmt_name = data->stmt_name;
  int read = data->read;
  isl_union_set *uset, *empty_filter, *domain;
  isl_set *set;
  isl_space *space;
  isl_id *id, *id2;
  isl_ctx *ctx;
  isl_union_map *access;
  int empty;
  struct autosa_array_tile *tile;
  isl_multi_aff *ma, *from_access;
  isl_multi_pw_aff *mpa;
  isl_multi_union_pw_aff *mupa;
  isl_schedule_node *graft;
  int n_lane = data->n_lane;
  int is_simd;
  isl_id *hls_id;
  isl_bool stride_one;
  isl_bool insert_dependence = isl_bool_false;

  if (isl_schedule_node_get_type(node) != isl_schedule_node_leaf)
    return node;

  /* Examine if the statement contains the access. */
  uset = isl_schedule_node_get_domain(node);
  set = isl_set_from_union_set(isl_union_set_copy(uset));
  space = isl_set_get_space(set);
  isl_set_free(set);  
  id = isl_space_get_tuple_id(space, isl_dim_set);
  isl_space_free(space);
  space = isl_map_get_space(ref->access);
  id2 = isl_space_get_tuple_id(space, isl_dim_in);
  empty_filter = isl_union_set_empty(isl_union_set_get_space(uset));
  isl_union_set_free(uset);
  isl_space_free(space);

  if (id != id2) {
    isl_id_free(id);
    isl_id_free(id2);
    node = isl_schedule_node_insert_filter(node, empty_filter);
    return node;
  }
  isl_id_free(id);
  isl_id_free(id2);
  ctx = isl_schedule_node_get_ctx(node);
  is_simd = is_node_under_simd(node);

  access = io_comm_access_ref(data->kernel, node, group, ref, read);
  empty = isl_union_map_is_empty(access);
  if (empty < 0 || empty) {
    isl_union_map_free(access);
    isl_union_set_free(empty_filter);
    if (empty < 0)
      return isl_schedule_node_free(node);
    return node;
  }

  /* Update the stmt_name. */
  if (data->insert_dependence) {
    isl_schedule_node *node2;
    
    node2 = isl_schedule_node_copy(node);
    if (n_lane >= 1 && is_simd) {
      node2 = isl_schedule_node_parent(node);      
    }
    /* Test if the access is stride one at the current loop. */    
    stride_one = is_acc_stride_one_at_node(node2, ref);    
    if (stride_one) {
      /* Test if the loop bound/n_lane > 1. 
       * If so, insert a hls_dep mark.
       * Only do this when there is a single access in the group.
       */
      int *ubs = NULL;
      isl_schedule_node *node_copy = isl_schedule_node_copy(node2);
      while (node_copy && isl_schedule_node_has_parent(node_copy)) {
        if (isl_schedule_node_get_type(node_copy) == isl_schedule_node_band)
          break;
        node_copy = isl_schedule_node_parent(node_copy);
      }
      if (isl_schedule_node_get_type(node_copy) == isl_schedule_node_band) {
        int n = isl_schedule_node_band_n_member(node_copy);
        ubs = extract_band_upper_bounds(data->kernel, node_copy);
        if (ubs[n - 1] / n_lane > 1) {
          insert_dependence = isl_bool_true;          
          /* Update the stmt_name. */
          int coalesce_depth;          
          int coalesce_bound;

          coalesce_depth = isl_schedule_node_get_schedule_depth(node_copy) - 1;
          coalesce_bound = ubs[n - 1] / n_lane;

          isl_printer *p_str = isl_printer_to_str(ctx);
          p_str = isl_printer_print_str(p_str, stmt_name);
          p_str = isl_printer_print_str(p_str, ".");
          p_str = isl_printer_print_int(p_str, coalesce_depth);
          p_str = isl_printer_print_str(p_str, ".");
          p_str = isl_printer_print_int(p_str, coalesce_bound);
          free(stmt_name);
          stmt_name = isl_printer_get_str(p_str);
          isl_printer_free(p_str);
        }
      }
      free(ubs);
      isl_schedule_node_free(node_copy);
    }
    isl_schedule_node_free(node2);
  }  

  from_access = autosa_create_io_access_stmt(
    ctx, group, group, data->local_tile, 
    isl_schedule_node_get_schedule_depth(node), stmt_name);
  free(stmt_name);

  /* Create a register tiling. */
  tile = create_register_tiling(node, group, ref);
  ma = isl_multi_aff_copy(tile->tiling);
  ma = isl_multi_aff_pullback_multi_aff(ma, 
      isl_multi_aff_copy(from_access));
  mpa = isl_multi_pw_aff_from_multi_aff(ma);
  mupa = isl_multi_union_pw_aff_from_multi_pw_aff(mpa);

  domain = isl_union_map_range(access);
  /* Only for read, we extend the access to a rectangular hull which helps to 
   * improve the memory coalescing. 
   */
  if (read && !autosa_array_is_scalar(group->array)) {
    isl_map *map;
    isl_set *set;
    set = isl_map_domain(isl_map_from_union_map(isl_union_set_unwrap(domain)));
    map = group_tile_buffer(group, tile); 
    map = isl_map_intersect_domain(map, set);
    domain = isl_union_set_from_set(isl_map_wrap(map));
  }

  domain = isl_union_set_preimage_multi_aff(domain, from_access);
  access = isl_union_set_wrapped_domain_map(domain);
  access = isl_union_map_reverse(access);
  access = isl_union_map_coalesce(access);

  graft = isl_schedule_node_from_extension(access);
  graft = isl_schedule_node_child(graft, 0);
  graft = isl_schedule_node_insert_partial_schedule(graft, mupa);

  /* If the current statement is under the SIMD loop, we will add a filter 
   * to only transfer the data at one loop since we will later insert a 
   * statement to handle the data transfer of the entire SIMD loop.
   */
  if (n_lane >= 1 && is_simd) {
    /* The loop above is the SIMD loop.
     * Check the node is below the simd mark. 
     */
    int n_index;
    int tile_size[1];
    isl_id *id;
    isl_printer *p_str;
    isl_union_map *umap;
    isl_union_set *filter;

    /* Create a filter. */
    node = isl_schedule_node_parent(node);
    if (data->read) 
      filter = schedule_eq_lb(node);
    else
      filter = schedule_eq_ub(node);
    node = isl_schedule_node_insert_filter(node, filter);
    node = isl_schedule_node_child(node, 0);
    node = isl_schedule_node_child(node, 0);
  }

  /* Insert a "pipeline" mark under the band node. */
  hls_id = isl_id_alloc(ctx, "hls_pipeline", NULL);
  graft = isl_schedule_node_child(graft, 0);
  graft = isl_schedule_node_insert_mark(graft, hls_id);
  graft = isl_schedule_node_parent(graft);

#ifdef _DEBUG
  isl_printer *pd = isl_printer_to_file(ctx, stdout);
  pd = isl_printer_set_yaml_style(pd, ISL_YAML_STYLE_BLOCK);
  pd = isl_printer_print_schedule_node(pd, node);  
  pd = isl_printer_print_map(pd, ref->access);
  pd = isl_printer_end_line(pd);
  isl_printer_free(pd);
#endif

  if (insert_dependence) {
    char *mark_name;
    isl_id *id;
    isl_printer *p_str = isl_printer_to_str(ctx);
    p_str = isl_printer_print_str(p_str, "hls_dependence.");
    p_str = autosa_array_ref_group_print_name(group, p_str);
    mark_name = isl_printer_get_str(p_str);
    isl_printer_free(p_str);
    id = isl_id_alloc(ctx, mark_name, NULL);
    graft = isl_schedule_node_child(graft, 0);
    graft = isl_schedule_node_child(graft, 0);
    graft = isl_schedule_node_insert_mark(graft, id);
    free(mark_name);
  }

//  if (data->insert_dependence) {
//    isl_schedule_node *node2;
//    node2 = isl_schedule_node_copy(node);
//    if (n_lane >= 1 && is_simd) {
//      node2 = isl_schedule_node_parent(node);      
//    }
//    /* Test if the access is stride one at the current loop. */    
//    stride_one = is_acc_stride_one_at_node(node2, ref);    
//    if (stride_one) {
//      /* Test if the loop bound/n_lane > 1. 
//       * If so, insert a hls_dep mark.
//       * Only do this when there is a single access in the group.
//       */
//      int *ubs = NULL;
//      isl_schedule_node *node_copy = isl_schedule_node_copy(node2);
//      while (node_copy && isl_schedule_node_has_parent(node_copy)) {
//        if (isl_schedule_node_get_type(node_copy) == isl_schedule_node_band)
//          break;
//        node_copy = isl_schedule_node_parent(node_copy);
//      }
//      if (isl_schedule_node_get_type(node_copy) == isl_schedule_node_band) {
//        int n = isl_schedule_node_band_n_member(node_copy);
//        ubs = extract_band_upper_bounds(data->kernel, node_copy);
//        if (ubs[n - 1] / n_lane > 1) {
//          char *mark_name;
//          isl_id *id;
//          isl_printer *p_str = isl_printer_to_str(ctx);
//          p_str = isl_printer_print_str(p_str, "hls_dependence.");
//          p_str = autosa_array_ref_group_print_name(group, p_str);
//          mark_name = isl_printer_get_str(p_str);
//          isl_printer_free(p_str);
//          id = isl_id_alloc(ctx, mark_name, NULL);
//          graft = isl_schedule_node_child(graft, 0);
//          graft = isl_schedule_node_child(graft, 0);
//          graft = isl_schedule_node_insert_mark(graft, id);
//          free(mark_name);
//        }
//      }
//      free(ubs);
//      isl_schedule_node_free(node_copy);
//    }
//    isl_schedule_node_free(node2);
//  } 

  while (graft && isl_schedule_node_has_parent(graft))
    graft = isl_schedule_node_parent(graft);

  node = isl_schedule_node_graft_before(node, graft);
  node = isl_schedule_node_insert_filter(node, empty_filter);
  node = isl_schedule_node_parent(node);
  node = isl_schedule_node_parent(node);
  node = isl_schedule_node_parent(node);

  autosa_array_tile_free(tile);

  return node;
}

/* Add copies at the stmt level for each array reference in the "group" 
 * in the I/O modules.
 * 
 * "group" is an I/O group.
 * "read" denotes if copy-in or copy-out from/to the external memory.
 * "in" denotes the fifo direction.
 * "insert_dependence" determines if it is necessary to insert a hls dependence mark.
 */
__isl_give isl_schedule_node *add_io_copies_stmt_acc(
  struct autosa_kernel *kernel,
  struct autosa_array_ref_group *group,
  __isl_take isl_schedule_node *node,
  struct autosa_array_tile *tile,
  int n_lane,
  int read,
  __isl_take char *stmt_name,
  int before,
  int insert_dependence
) {
  struct add_io_copies_stmt_acc_data data = {
            kernel, group, NULL, tile, n_lane, read, stmt_name, 
            insert_dependence && group->n_ref == 1};

  for (int i = 0; i < group->n_ref; i++) {
    struct autosa_stmt_access *ref = group->refs[i];
    data.ref = ref;
    node = isl_schedule_node_map_descendant_bottom_up(
              node, &add_io_copies_stmt_acc_single, &data);
  }

  return node;
}

/* Insert the copy statement at the node level to transfer the entire tie.
 * If "is_buffer" is set, add a marker for dependence false. This is
 * only for Xilinx platform.
 */
static __isl_give isl_schedule_node *add_io_copies_stmt_tile(
  struct autosa_kernel *kernel,
  struct autosa_array_ref_group *group,
  __isl_take isl_schedule_node *node,
  struct autosa_array_tile *local_tile, /* Local buffer */
  struct autosa_array_tile *tile,       /* The tile to be copied */
  int n_lane,
  int read,
  __isl_take char *stmt_name,
  int before, int is_buffer,
  /* If it is proper to insert hls_pipeline for Xilinx platforms. */
  int insert_dependence                 
  )
{
  isl_union_map *access = NULL;
  int empty;
  isl_multi_aff *from_access;
  isl_multi_aff *ma;
  isl_multi_pw_aff *mpa;
  isl_multi_union_pw_aff *mupa;
  isl_union_set *domain;
  isl_schedule_node *graft;
  int n;
  isl_id *id;
  isl_ctx *ctx = kernel->ctx;
  int coalesce_depth;
  int coalesce_bound;
  isl_val *coalesce_bound_val;

  access = io_comm_access(kernel, node, group, read);

  empty = isl_union_map_is_empty(access);
  if (empty < 0 || empty) {
    isl_union_map_free(access);
    if (empty < 0)
      return isl_schedule_node_free(node);
    return node;
  }

  from_access = autosa_create_io_access_stmt(kernel->ctx, group, group, 
        local_tile, isl_schedule_node_get_schedule_depth(node), stmt_name);

  ma = isl_multi_aff_copy(tile->tiling);
  ma = isl_multi_aff_pullback_multi_aff(ma, 
      isl_multi_aff_copy(from_access));
  mpa = isl_multi_pw_aff_from_multi_aff(ma);
  mupa = isl_multi_union_pw_aff_from_multi_pw_aff(mpa);

  domain = isl_union_map_range(access);
  if (read && !autosa_array_is_scalar(group->array)) {
    isl_map *map;
    isl_set *set;
    set = isl_map_domain(isl_map_from_union_map(isl_union_set_unwrap(domain)));
    map = group_tile_buffer(group, tile); 
    map = isl_map_intersect_domain(map, set);
    domain = isl_union_set_from_set(isl_map_wrap(map));
  }

  domain = isl_union_set_preimage_multi_aff(domain, from_access);
  access = isl_union_set_wrapped_domain_map(domain);
  access = isl_union_map_reverse(access);
  access = isl_union_map_coalesce(access);

  graft = isl_schedule_node_from_extension(access);
  graft = isl_schedule_node_child(graft, 0);
  graft = isl_schedule_node_insert_partial_schedule(graft, mupa);

  /* Split off the last dimension. */
  n = isl_schedule_node_band_n_member(graft);
  if (n > 1) {
    graft = isl_schedule_node_band_split(graft, n - 1);
    graft = isl_schedule_node_child(graft, 0);
  }

  /* Insert a coalesce mark indicating the loop below could be used for
   * memory coalescing.
   */
  id = isl_id_alloc(ctx, "access_coalesce", NULL);
  graft = isl_schedule_node_insert_mark(graft, id);
  graft = isl_schedule_node_child(graft, 0);

  if (n_lane > 1) {
    /* Peform data packing. 
     * We will tile the last dimension by the factor of data packing.
     * Then we insert a filter to transfer data only once.
     */
    int tile_size[1];
    isl_id *id;
    isl_printer *p_str;
    isl_union_map *umap;
    isl_union_set *filter;
    int depth;

    /* Tile the last dimension. */
    tile_size[0] = n_lane;
    graft = autosa_tile_band(graft, tile_size);
    graft = isl_schedule_node_child(graft, 0);
    /* Create a filter. */
    filter = schedule_eq_lb(graft);
    graft = isl_schedule_node_insert_filter(graft, filter);
    /* Move to the tile loop */
    graft = isl_schedule_node_parent(graft);    
  }
  free(stmt_name);
  /* Insert a "pipeline" mark inside the band node. */
  id = isl_id_alloc(ctx, "hls_pipeline", NULL);

  graft = isl_schedule_node_child(graft, 0);
  graft = isl_schedule_node_insert_mark(graft, id);
  graft = isl_schedule_node_parent(graft);

  if (is_buffer && !read && insert_dependence) {    
    // TODO: should not be inter_trans or intra_trans.
    // TODO: only add this pragma for io_transfer statement which requires data packing.
    /* Insert a "dependence" mark. 
     * This is not safe. Currently only insert the mark when there is at least 
     * one level of coalesce loop (coalesce_bound > 1) and
     * when data_pack does not equal to the nxt_data_pack. 
     */
    char *mark_name;
    isl_printer *p_str = isl_printer_to_str(ctx);
    p_str = isl_printer_print_str(p_str, "hls_dependence.");
    p_str = autosa_array_ref_group_print_name(group, p_str);
    mark_name = isl_printer_get_str(p_str);
    isl_printer_free(p_str);
    id = isl_id_alloc(ctx, mark_name, NULL);
    graft = isl_schedule_node_child(graft, 0);
    graft = isl_schedule_node_child(graft, 0);
    graft = isl_schedule_node_insert_mark(graft, id);
    free(mark_name);
  }

  while (graft && isl_schedule_node_has_parent(graft))
    graft = isl_schedule_node_parent(graft);

  if (before) {
    node = isl_schedule_node_graft_before(node, graft);
  } else {
    node = isl_schedule_node_graft_after(node, graft);
  }

  return node;   
}

/* Generate the inter_trans module for the I/O group.
 * We will add data transfer statements into the schedule tree, 
 * filters that restrain the space loops to the current module,
 * and add the module and function type mark above the tree.
 */
static __isl_give isl_schedule *generate_io_module_inter_trans(
  __isl_keep isl_schedule *sched, struct autosa_hw_module *module,
  struct autosa_array_ref_group *group, 
  struct autosa_kernel *kernel, struct autosa_gen *gen,
  int io_level, int space_dim, int read, int boundary)
{
  isl_schedule *new_sched;
  isl_ctx *ctx;
  isl_printer *p;
  char *io_mark;
  int n_io_ids = 0;
  isl_id_list *io_ids;
  isl_id *id;
  int is_mark;
  isl_set *context;
  char *fifo_suffix, *buf_suffix;
  isl_union_set *empty_filter = NULL;
  isl_union_set *eq_filter = NULL;
  int depth;
  char *stmt_name;
  struct autosa_io_buffer *buf = NULL;
  isl_union_map *group_access;
  isl_union_set *group_domain;
  isl_schedule_node *node;
  int upper_io_level;
  int is_filter = 1;
  int is_buffer = 1;
  int i;

  new_sched = isl_schedule_dup(sched);
  node = isl_schedule_get_root(new_sched);
  isl_schedule_free(new_sched);
  ctx = isl_schedule_node_get_ctx(node);
 
  /* Generate the IO ids. */
  n_io_ids = space_dim - io_level + 1;
  io_ids = ppcg_scop_generate_names(gen->prog->scop, n_io_ids, "p"); 
  n_io_ids = 0;

  assert(module->to_mem == 0);
  upper_io_level = io_level + 1;

  /* Update the context by adding the constraints for the io ids. */
  context = isl_set_universe(isl_set_get_space(kernel->context));
  node = autosa_tree_move_down_to_array(node, kernel->core);
  while (!isl_schedule_node_is_io_mark(node, io_level)) {
    if (isl_schedule_node_get_type(node) == isl_schedule_node_band) {
      isl_union_map *umap;
      isl_union_set *uset;
      isl_multi_pw_aff *size;
      isl_id *id;
      isl_id_list *ids;

      umap = isl_schedule_node_band_get_partial_schedule_union_map(node);
      uset = isl_union_map_range(umap);
      size = ppcg_size_from_extent(isl_set_from_union_set(uset));
      ids = isl_id_list_from_id(isl_id_list_get_id(io_ids, n_io_ids));
      n_io_ids++;
      context = add_bounded_parameters_dynamic(context, size, ids);
      isl_id_list_free(ids);
      isl_multi_pw_aff_free(size);
    }
    node = isl_schedule_node_child(node, 0);
  }
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_context(node, context);

  /* Add the filters. 
   * We add filters to the I/O space loops such that:
   * - All the scheduled iterations equal to the io_id above the current I/O level.
   * - All the scheduled iterations are greater or equal to the io_id at the 
   *   current I/O level.
    */
  n_io_ids = 0;
  node = autosa_tree_move_down_to_array(node, kernel->core);
  while (!isl_schedule_node_is_io_mark(node, io_level)) {
    if (isl_schedule_node_get_type(node) == isl_schedule_node_band) {
      isl_id *id;
      isl_id_list *ids;
      isl_union_set *uset;

      ids = isl_id_list_from_id(isl_id_list_get_id(io_ids, n_io_ids));      
      if (n_io_ids == space_dim - io_level) {         
        uset = set_schedule_ge(node, ids);
      } else {
        uset = set_schedule_eq(node, ids);
      }
      n_io_ids++;
      node = isl_schedule_node_insert_filter(node, uset);
      isl_id_list_free(ids);
      node = isl_schedule_node_child(node, 0);
    }
    node = isl_schedule_node_child(node, 0);
  }
  node = autosa_tree_move_up_to_kernel(node);

  /* Add the data transfer statements. */
  node = autosa_tree_move_down_to_io_mark(node, kernel->core, io_level); 
  depth = isl_schedule_node_get_schedule_depth(node);
  /* Four types of I/O modules:
   * filter + no buffer
   * filter + buffer
   * no filter + no buffer
   * no filter + buffer
   */
  init_suffix(module, group, &fifo_suffix, &buf_suffix);

  /* Locate the next buffer. */
  for (i = io_level; i >= 1; i--) {
    buf = group->io_buffers[i - 1];
    if (buf->tile != NULL)
      break;
  }
  if (is_buffer) {
    if (i != io_level) {
      /* IO buffer is optimized out. */
      is_buffer = 0;
    }
  }

  /* Create a transfer statement with the format:
   * [in_trans/out_trans]_[dram]_[boundary].fifo_suffix_[local].
   * [is_filter].[is_buffer].[depth-1].[space_dim-io_level].
   * [data_pack_inter].[data_pack_intra].
   * [coalesce_depth].[coalesce_bound]
   */
  p = isl_printer_to_str(ctx);
  p = isl_printer_print_str(p, read? "in_trans" : "out_trans");
  if (module->to_mem) 
    p = isl_printer_print_str(p, "_dram");
  if (boundary) 
    p = isl_printer_print_str(p, "_boundary");
  p = isl_printer_print_str(p, ".");
  p = isl_printer_print_str(p, fifo_suffix);
  if (module->to_mem)
    p = isl_printer_print_str(p, "_local");
  p = isl_printer_print_str(p, is_filter == 0? ".0" : ".1");
  p = isl_printer_print_str(p, is_buffer == 0? ".0" : ".1");
  p = isl_printer_print_str(p, ".");
  p = isl_printer_print_int(p, depth - 1); 
  p = isl_printer_print_str(p, ".");
  p = isl_printer_print_int(p, space_dim - io_level);
  p = isl_printer_print_str(p, ".");
  p = isl_printer_print_int(p, buf->n_lane);

  /* Move the schedule node to the level of the buffer since the 
   * buffer may have been hoisted. 
   */
  node = autosa_tree_move_down_to_io_mark(node, kernel->core, buf->level); 
  node = isl_schedule_node_child(node, 0);
  if (!buf->tile) {     
    /* Add the I/O statement for each array reference in the group. */
    module->data_pack_inter = buf->n_lane;
    module->data_pack_intra = buf->n_lane;
    p = isl_printer_print_str(p, ".");
    p = isl_printer_print_int(p, buf->n_lane);
    stmt_name = isl_printer_get_str(p);
    isl_printer_free(p);
    node = add_io_copies_stmt_acc(kernel, group, node, 
              buf->tile, buf->n_lane, read, stmt_name, read? 1: 0,
              is_buffer && !read && 0 
                && kernel->options->autosa->insert_hls_dependence);
  } else {
    int coalesce_depth;
    isl_val *coalesce_bound_val;
    int coalesce_bound;

    /* Add the I/O statement for the entire group. */
    module->data_pack_inter = buf->n_lane;
    module->data_pack_intra = buf->n_lane;
    p = isl_printer_print_str(p, ".");
    p = isl_printer_print_int(p, buf->n_lane);

    /* Compute the coalesce loop depth and upper bounds. */
    coalesce_depth = isl_schedule_node_get_schedule_depth(node) + buf->tile->n - 1;
    coalesce_bound_val = buf->tile->bound[buf->tile->n - 1].size;
    coalesce_bound = isl_val_get_num_si(coalesce_bound_val) / buf->n_lane;
    if (coalesce_bound <= 1) {
      coalesce_depth = -1;
    }

    p = isl_printer_print_str(p, ".");
    p = isl_printer_print_int(p, coalesce_depth);
    p = isl_printer_print_str(p, ".");
    p = isl_printer_print_int(p, coalesce_bound);

    stmt_name = isl_printer_get_str(p);
    isl_printer_free(p);    
    node = add_io_copies_stmt_tile(kernel, group, node, 
              buf->tile, buf->tile, buf->n_lane, read, stmt_name, read? 1: 0, 
              is_buffer & 0, 
              coalesce_bound > 1 && 0 && kernel->options->autosa->insert_hls_dependence);
    node = isl_schedule_node_cut(node);
    /* Insert empty filter. */
    empty_filter = isl_union_set_from_set(isl_set_empty(
                      isl_set_get_space(kernel->context)));
    node = isl_schedule_node_insert_filter(node, empty_filter);
  }

  free(fifo_suffix);
  free(buf_suffix);

  /* Insert the "io_module.inter_trans" function mark. */
  node = autosa_tree_move_up_to_kernel(node);
  node = autosa_tree_move_down_to_io_mark(node, kernel->core, upper_io_level);
  node = isl_schedule_node_child(node, 0);
  id = isl_id_alloc(ctx, "io_module.inter_trans", NULL);
  node = isl_schedule_node_insert_mark(node, id);

  /* Compute the union of domains of all the array references in the group. */
  group_access = isl_union_map_empty(isl_map_get_space(group->access));
  for (int i = 0; i < group->n_ref; i++) {
    struct autosa_stmt_access *ref = group->refs[i];
    if (group->group_type == AUTOSA_IO_GROUP) {
      group_access = isl_union_map_union(group_access,
          autosa_io_group_ref_access_relation(group, ref, read, !read));
    } else if (group->group_type == AUTOSA_DRAIN_GROUP) {
      group_access = isl_union_map_union(group_access,
          autosa_drain_group_ref_access_relation(group, ref, read, !read, 
            kernel->expanded_domain));
    }
  }
  group_domain = isl_union_map_domain(group_access);
  group_domain = isl_union_set_coalesce(group_domain);
  /* Add the group domain as the filter. */
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0); // context
  node = isl_schedule_node_child(node, 0); 
  node = isl_schedule_node_insert_filter(node, group_domain);

  /* Add the module mark. */
  id = isl_id_alloc(ctx, "module", module);
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_mark(node, id);

  new_sched = isl_schedule_node_get_schedule(node);
  isl_schedule_node_free(node);
  isl_id_list_free(io_ids);

  return new_sched;
}

/* Generate the intra_trans module for the I/O group.
 * We will add data transfer statements into the schedule tree that 
 * transfer data to/from the lower-level modules,
 * filters that restrain the space loops to the current module,
 * and add the module and function type mark above the tree.
 */
static __isl_give isl_schedule *generate_io_module_intra_trans(
  __isl_keep isl_schedule *sched, struct autosa_hw_module *module,
  struct autosa_array_ref_group *group, 
  struct autosa_kernel *kernel, struct autosa_gen *gen,
  int io_level, int space_dim, int read, int is_buffer)
{
  isl_ctx *ctx;
  isl_printer *p;
  char *io_mark;
  int n_io_ids = 0;
  isl_id_list *io_ids;
  isl_id_list *ids;
  isl_id *id;
  int is_mark;
  isl_set *context;
  char *fifo_suffix, *buf_suffix;
  isl_union_set *empty_filter = NULL;
  isl_union_set *eq_filter = NULL;
  int depth;
  char *stmt_name;
  struct autosa_io_buffer *buf = NULL;
  isl_union_map *group_access;
  isl_union_set *group_domain;
  isl_schedule *new_sched;
  isl_schedule_node *node;
  int upper_io_level;
  int i;

  new_sched = isl_schedule_dup(sched);
  node = isl_schedule_get_root(new_sched);
  isl_schedule_free(new_sched);
  ctx = isl_schedule_node_get_ctx(node);
 
  n_io_ids = space_dim - io_level + 1;
  io_ids = ppcg_scop_generate_names(gen->prog->scop, n_io_ids, "p"); 
  n_io_ids = 0;

  assert(module->to_mem == 0);
  upper_io_level = io_level + 1;

  /* Update the context. */
  context = isl_set_universe(isl_set_get_space(kernel->context));
  node = autosa_tree_move_down_to_array(node, kernel->core);
  while (!isl_schedule_node_is_io_mark(node, io_level)) {
    if (isl_schedule_node_get_type(node) == isl_schedule_node_band) {
      isl_union_map *umap;
      isl_union_set *uset;
      isl_multi_pw_aff *size;
      isl_id *id;
      isl_id_list *ids;

      umap = isl_schedule_node_band_get_partial_schedule_union_map(node);
      uset = isl_union_map_range(umap);
      size = ppcg_size_from_extent(isl_set_from_union_set(uset));
      ids = isl_id_list_from_id(isl_id_list_get_id(io_ids, n_io_ids));
      n_io_ids++;
      context = add_bounded_parameters_dynamic(context, size, ids);
      isl_id_list_free(ids);
      isl_multi_pw_aff_free(size);
    }
    node = isl_schedule_node_child(node, 0);
  }
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_context(node, context);

  /* Add the filters.
   * All the space loops above the current io_level should equal to
   * the io_ids. 
   */
  n_io_ids = 0;
  node = autosa_tree_move_down_to_array(node, kernel->core);
  while (!isl_schedule_node_is_io_mark(node, upper_io_level)) {
    if (isl_schedule_node_get_type(node) == isl_schedule_node_band) {
      isl_id *id;
      isl_id_list *ids;
      isl_union_set *uset;

      ids = isl_id_list_from_id(isl_id_list_get_id(io_ids, n_io_ids));      
      uset = set_schedule_eq(node, ids);
      n_io_ids++;
      node = isl_schedule_node_insert_filter(node, uset);
      isl_id_list_free(ids);
      node = isl_schedule_node_child(node, 0);
    }
    node = isl_schedule_node_child(node, 0);
  }
  if (module->to_pe) {
    /* Add filter to only send data to boundary PEs. */
    while (!isl_schedule_node_is_io_mark(node, 1)) {
      if (isl_schedule_node_get_type(node) == isl_schedule_node_band) {
        isl_union_set *uset;

        if (read)
          uset = schedule_eq_lb(node);
        else
          uset = schedule_eq_ub(node);
        node = isl_schedule_node_insert_filter(node, uset);
        node = isl_schedule_node_child(node, 0);
      }
      node = isl_schedule_node_child(node, 0);
    }
  }
  node = autosa_tree_move_up_to_kernel(node);

  /* Add a filter node. 
   * The io_loop at the current io_level should equal to the io_id.
   */     
  node = autosa_tree_move_down_to_io_mark(node, kernel->core, io_level); 
  ids = isl_id_list_from_id(isl_id_list_get_id(io_ids, space_dim - io_level));
  node = isl_schedule_node_parent(node);
  eq_filter = set_schedule_eq(node, ids); 
  node = isl_schedule_node_child(node, 0);  
  isl_id_list_free(ids);  
  node = isl_schedule_node_parent(node);
  node = isl_schedule_node_insert_filter(node, eq_filter);  
  node = isl_schedule_node_child(node, 0);

  /* Add the data transfer statements. */
  init_suffix(module, group, &fifo_suffix, &buf_suffix);

  /* Locate the current buffer. */
  for (i = io_level; i >= 1; i--) {
    buf = group->io_buffers[i - 1];
    if (buf->tile != NULL)
      break;
  }
  if (is_buffer) {
    if (i != io_level) {
      /* IO buffer is optimized out. */
      is_buffer = 0;
    }
  }

  /* Insert the extra transfer statement. */
  p = isl_printer_to_str(ctx);
  p = isl_printer_print_str(p, read? "out_trans." : "in_trans.");
  p = isl_printer_print_str(p, fifo_suffix);
  p = isl_printer_print_str(p, "_local");
  p = isl_printer_print_str(p, ".0"); // filter
  p = isl_printer_print_str(p, is_buffer == 0? ".0" : ".1"); // buffer
  p = isl_printer_print_str(p, ".-1"); // sched_depth
  p = isl_printer_print_str(p, ".-1"); // param_id
  p = isl_printer_print_str(p, ".");
  p = isl_printer_print_int(p, buf->n_lane);

  /* Locate the next buffer after the current buffer. */
  int cur_level = buf->level;
  struct autosa_io_buffer *cur_buf = buf;
  for (int i = cur_level - 1; i >= 1; i--) {
    buf = group->io_buffers[i - 1];
    if (buf->tile != NULL)
      break;
  }

  if (cur_level > 1) { 
    /* Move the schedule node to the level of the next buffer. */
    node = autosa_tree_move_down_to_io_mark(node, kernel->core, buf->level);
    node = isl_schedule_node_child(node, 0);
  }
  if (cur_level == 1 || !buf->tile) {
    p = isl_printer_print_str(p, ".");
    p = isl_printer_print_int(p, group->n_lane);
    stmt_name = isl_printer_get_str(p);
    isl_printer_free(p);
    module->data_pack_intra = group->n_lane;
    // TODO: Test if we could insert a hls dependence at the current loop. 
    // We weed to test if the access under the innermost loop is stride-1.
    // If so, gather the loop bound and make sure the bound is greater than n_lane. 
    node = add_io_copies_stmt_acc(kernel, group, node, 
              cur_buf->tile, group->n_lane, read, stmt_name, read? 1: 0,
              is_buffer && !read && cur_buf->n_lane != group->n_lane 
                && kernel->options->autosa->insert_hls_dependence); 
  } else {
    int coalesce_depth;
    isl_val *coalesce_bound_val;
    int coalesce_bound;

    p = isl_printer_print_str(p, ".");
    p = isl_printer_print_int(p, buf->n_lane);

	  /* Compute the coalesce loop depth and upper bounds. */
    coalesce_depth = isl_schedule_node_get_schedule_depth(node) + buf->tile->n - 1;
    coalesce_bound_val = buf->tile->bound[buf->tile->n - 1].size;
    coalesce_bound = isl_val_get_num_si(coalesce_bound_val) / buf->n_lane;
    if (coalesce_bound <= 1) {
      coalesce_depth = -1;
    }

    p = isl_printer_print_str(p, ".");
    p = isl_printer_print_int(p, coalesce_depth);
    p = isl_printer_print_str(p, ".");
    p = isl_printer_print_int(p, coalesce_bound);    

    stmt_name = isl_printer_get_str(p);
    isl_printer_free(p);
    module->data_pack_intra = buf->n_lane;
    node = add_io_copies_stmt_tile(kernel, group, node, 
              cur_buf->tile, buf->tile, buf->n_lane, 
              read, stmt_name, read? 1: 0, is_buffer & 0,
              coalesce_bound > 1 && cur_buf->n_lane != buf->n_lane 
                && kernel->options->autosa->insert_hls_dependence
              );
    node = isl_schedule_node_cut(node);
    /* Insert empty filter. */
    empty_filter = isl_union_set_from_set(isl_set_empty(isl_set_get_space(kernel->context)));
    node = isl_schedule_node_insert_filter(node, empty_filter);    
  }

  free(fifo_suffix);
  free(buf_suffix);

  /* Insert the function mark. */
  node = autosa_tree_move_up_to_kernel(node);
  node = autosa_tree_move_down_to_io_mark(node, kernel->core, upper_io_level);
  node = isl_schedule_node_child(node, 0);
  id = isl_id_alloc(ctx, "io_module.intra_trans", NULL);
  node = isl_schedule_node_insert_mark(node, id);

  /* Compute the union of domains of all the array references in the group. */
  group_access = isl_union_map_empty(isl_map_get_space(group->access));
  for (int i = 0; i < group->n_ref; i++) {
    struct autosa_stmt_access *ref = group->refs[i];
    if (group->group_type == AUTOSA_IO_GROUP) {
      group_access = isl_union_map_union(group_access,
          autosa_io_group_ref_access_relation(group, ref, read, !read));
    } else if (group->group_type == AUTOSA_DRAIN_GROUP) {
      group_access = isl_union_map_union(group_access,
          autosa_drain_group_ref_access_relation(group, ref, read, !read, 
              kernel->expanded_domain));
    }
  }
  group_domain = isl_union_map_domain(group_access);
  group_domain = isl_union_set_coalesce(group_domain);
  /* Add the group domain as the filter. */
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0); // context
  node = isl_schedule_node_child(node, 0); 
  node = isl_schedule_node_insert_filter(node, group_domain);

  /* Add the module mark. */
  id = isl_id_alloc(ctx, "module", module);
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_mark(node, id);

  new_sched = isl_schedule_node_get_schedule(node);
  isl_schedule_node_free(node);

  isl_id_list_free(io_ids);

  return new_sched;
}

/* Create the local buffer variable for the "group".
 * Specifically, if "tile" is NULL, a register is created.
 * Otherwise, a local array is created. 
 * We will also update the last dimension of the array based on the 
 * data packing factor "n_lane".
 */
static void create_io_module_var(isl_ctx *ctx, 
  struct autosa_array_ref_group *group,
  struct autosa_array_tile *tile, struct autosa_kernel_var *var, int n_lane)
{
  isl_printer *p;

  var->array = group->array;
  var->type = autosa_array_ref_group_type(group);
  var->n_lane = n_lane;
  var->n_part = 1;

  p = isl_printer_to_str(ctx);
  p = autosa_array_ref_group_print_name(group, p);
  var->name = isl_printer_get_str(p);
  isl_printer_free(p);

  if (tile == NULL) {
    /* Create a register. */
    var->size = isl_vec_alloc(ctx, 1);
    var->size = isl_vec_set_element_si(var->size, 0, 1);
  } else {
    var->size = isl_vec_alloc(ctx, group->array->n_index);
    for (int i = 0; i < group->array->n_index; ++i) {
      isl_val *size;

      size = isl_val_copy(tile->bound[i].size);
      if (n_lane > 1 && i == group->array->n_index - 1) {
        size = isl_val_div(size, isl_val_int_from_si(ctx, n_lane));
      }
      var->size = isl_vec_set_element_val(var->size, i, size);
    }
  }
}

/* Create the local buffers inside the I/O modules. */
static isl_stat create_io_module_vars(
  struct autosa_hw_module *module, struct autosa_kernel *kernel, 
  struct autosa_array_tile *tile)
{
  module->var = isl_calloc_array(kernel->ctx, struct autosa_kernel_var, 1);
  if (!module->var)
    return isl_stat_error;
  module->n_var = 1;

  create_io_module_var(kernel->ctx, module->io_groups[0], 
                tile, &module->var[0], module->data_pack_inter);

  return isl_stat_ok;
}

/* Generate the io_module for the outer loops that contain the 
 * inter_trans and intra_trans modules.
 */
static __isl_give isl_schedule *generate_io_module_outer(
  __isl_keep isl_schedule *sched, struct autosa_hw_module *module,
  struct autosa_array_ref_group *group, 
  struct autosa_kernel *kernel, struct autosa_gen *gen,
  int io_level, int space_dim, int read, int boundary)
{
  isl_ctx *ctx;
  int n_io_ids = 0;
  isl_id_list *io_ids;
  isl_id *id;
  isl_set *context;
  isl_union_set *empty_filter = NULL;
  const char *stmt_name1, *stmt_name2, *stmt_name3, *stmt_name4, *stmt_name5;
  isl_union_map *group_access;
  isl_union_set *group_domain;
  isl_schedule_node *node, *graft1, *graft2, *graft3, *graft4, *graft5;
  isl_schedule *new_sched;
  int upper_io_level;
  isl_space *space;
  isl_union_set *domain;
  struct autosa_io_buffer *buf;

  new_sched = isl_schedule_dup(sched);
  node = isl_schedule_get_root(new_sched);
  isl_schedule_free(new_sched);
  ctx = isl_schedule_node_get_ctx(node);
  
  n_io_ids = space_dim - io_level + 1;
  io_ids = ppcg_scop_generate_names(gen->prog->scop, n_io_ids, "p"); 
  n_io_ids = 0;

  assert(module->to_mem == 0);
  upper_io_level = io_level + 1;

  /* Update the context. */
  context = isl_set_universe(isl_set_get_space(kernel->context));
  node = autosa_tree_move_down_to_array(node, kernel->core);
  while (!isl_schedule_node_is_io_mark(node, io_level)) {
    if (isl_schedule_node_get_type(node) == isl_schedule_node_band) {
      isl_union_map *umap;
      isl_union_set *uset;
      isl_multi_pw_aff *size;
      isl_id *id;
      isl_id_list *ids;

      umap = isl_schedule_node_band_get_partial_schedule_union_map(node);
      uset = isl_union_map_range(umap);
      size = ppcg_size_from_extent(isl_set_from_union_set(uset));
      ids = isl_id_list_from_id(isl_id_list_get_id(io_ids, n_io_ids));
      n_io_ids++;
      context = add_bounded_parameters_dynamic(context, size, ids);
      isl_id_list_free(ids);
      isl_multi_pw_aff_free(size);
    }
    node = isl_schedule_node_child(node, 0);
  }
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_context(node, context);

  /* Add the filters. */
  n_io_ids = 0;
  node = autosa_tree_move_down_to_array(node, kernel->core);
  while (!isl_schedule_node_is_io_mark(node, upper_io_level)) {
    if (isl_schedule_node_get_type(node) == isl_schedule_node_band) {
      isl_id *id;
      isl_id_list *ids;
      isl_union_set *uset;

      ids = isl_id_list_from_id(isl_id_list_get_id(io_ids, n_io_ids));      
      uset = set_schedule_eq(node, ids);
      n_io_ids++;
      node = isl_schedule_node_insert_filter(node, uset);
      isl_id_list_free(ids);
      node = isl_schedule_node_child(node, 0);
    }
    node = isl_schedule_node_child(node, 0);
  }
  
  node = autosa_tree_move_up_to_kernel(node);

  /* Add the inter_trans and intra_trans function calls. */
  stmt_name1 = boundary == 0? "io_module.inter_trans" : "io_module.inter_trans.boundary";
  stmt_name2 = "io_module.intra_trans";
  stmt_name3 = boundary == 0? "io_module.inter_intra" : "io_module.inter_intra.boundary";
  stmt_name4 = boundary == 0? "io_module.intra_inter" : "io_module.intra_inter.boundary"; 
  stmt_name5 = "io_module.state_handle";

  node = autosa_tree_move_down_to_io_mark(node, kernel->core, upper_io_level);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_cut(node);

  space = isl_space_set_alloc(ctx, 0, 0);
  space = isl_space_set_tuple_name(space, isl_dim_set, stmt_name1);
  domain = isl_union_set_from_set(isl_set_universe(space));
  graft1 = isl_schedule_node_from_domain(domain);

  space = isl_space_set_alloc(ctx, 0, 0);
  space = isl_space_set_tuple_name(space, isl_dim_set, stmt_name2);
  domain = isl_union_set_from_set(isl_set_universe(space));
  graft2 = isl_schedule_node_from_domain(domain);

  space = isl_space_set_alloc(ctx, 0, 0);
  space = isl_space_set_tuple_name(space, isl_dim_set, stmt_name3);
  domain = isl_union_set_from_set(isl_set_universe(space));
  graft3 = isl_schedule_node_from_domain(domain);

  space = isl_space_set_alloc(ctx, 0, 0);
  space = isl_space_set_tuple_name(space, isl_dim_set, stmt_name4);
  domain = isl_union_set_from_set(isl_set_universe(space));
  graft4 = isl_schedule_node_from_domain(domain);

  space = isl_space_set_alloc(ctx, 0, 0);
  space = isl_space_set_tuple_name(space, isl_dim_set, stmt_name5);
  domain = isl_union_set_from_set(isl_set_universe(space));
  graft5 = isl_schedule_node_from_domain(domain);

  if (read) {
    node = isl_schedule_node_graft_before(node, isl_schedule_node_copy(graft3));
  } else {
    node = isl_schedule_node_graft_before(node, isl_schedule_node_copy(graft4));
  }  
  if (module->double_buffer) {
    /* Add misc statements for saving and switching states. */
    node = isl_schedule_node_graft_before(node, isl_schedule_node_copy(graft5));
  }
  node = isl_schedule_node_cut(node);
  /* Insert an empty filter */
  empty_filter = isl_union_set_from_set(isl_set_empty(
                    isl_set_get_space(kernel->context)));
  node = isl_schedule_node_insert_filter(node, empty_filter);

  if (module->double_buffer) {
    /* Add the last function call. */
    node = autosa_tree_move_up_to_kernel(node);
    node = isl_schedule_node_child(node, 0);
    node = isl_schedule_node_child(node, 0);
    if (read)
      node = isl_schedule_node_graft_after(node, isl_schedule_node_copy(graft2));
    else
      node = isl_schedule_node_graft_after(node, isl_schedule_node_copy(graft1));
  }
  isl_schedule_node_free(graft1);
  isl_schedule_node_free(graft2);
  isl_schedule_node_free(graft3);
  isl_schedule_node_free(graft4);
  isl_schedule_node_free(graft5);

  /* Compute the union of domains of all the array references in the group. */
  group_access = isl_union_map_empty(isl_map_get_space(group->access));
  for (int i = 0; i < group->n_ref; i++) {
    struct autosa_stmt_access *ref = group->refs[i];
    if (group->group_type == AUTOSA_IO_GROUP) {
      group_access = isl_union_map_union(group_access,
          autosa_io_group_ref_access_relation(group, ref, read, !read));
    } else if (group->group_type == AUTOSA_DRAIN_GROUP) {
      group_access = isl_union_map_union(group_access,
          autosa_drain_group_ref_access_relation(group, ref, read, !read, 
              kernel->expanded_domain));
    }
  }
  group_domain = isl_union_map_domain(group_access);
  group_domain = isl_union_set_coalesce(group_domain);  
  /* Add the group domain as the filter. */
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0); // context
  node = isl_schedule_node_child(node, 0); 
  node = isl_schedule_node_insert_filter(node, group_domain);

  /* Add the module mark. */
  id = isl_id_alloc(ctx, "module", module);
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_mark(node, id);

  new_sched = isl_schedule_node_get_schedule(node);
  isl_schedule_node_free(node);

  /* Update module information. */
  if (!boundary) {
    module->type = (group->group_type == AUTOSA_DRAIN_GROUP)? 
                      DRAIN_MODULE : IO_MODULE;
    module->level = io_level;
    module->n_io_group++;
    module->io_groups = (struct autosa_array_ref_group **)realloc(module->io_groups,
        module->n_io_group * sizeof(struct autosa_array_ref_group *));
    module->io_groups[module->n_io_group - 1] = group;
    module->inst_ids = io_ids;
    module->kernel = kernel;
    module->is_buffer = 1;
    module->is_filter = 1;
    if (read)
      module->in = 1;
    else
      module->in = 0;
    /* Create IO module variables. */
    for (int i = io_level; i >= 1; i--) {
      buf = group->io_buffers[i - 1];
      if (buf->tile != NULL)
        break;
    }
    create_io_module_vars(module, kernel, buf->tile);
  } else {
    isl_id_list_free(io_ids);
  }

  return new_sched;
}

/* We will generate five seperate schedules for this type of I/O module.
 * Schedule 1: Outer loops contains two marks for inter_transfer 
 *             and intra_transfer modules
 * Schedule 2: Inter_transfer function
 * Schedule 3: Intra_transfer function
 * Schedule 4: The boundary module for outer loops that is the last module
 *             in the chain.
 * Schedule 5: The boundary module for inter_transfer that is the last module
 *             in the chain.
 */
static __isl_give struct autosa_hw_module *generate_filter_buffer_io_module(
  __isl_take struct autosa_hw_module *module, 
  __isl_keep isl_schedule_node *node,
  struct autosa_array_ref_group *group, struct autosa_kernel *kernel, 
  struct autosa_gen *gen,
  int io_level, int space_dim, int is_filter, int is_buffer, int read)
{
  isl_schedule *sched;
  isl_schedule *sched1, *sched2, *sched3;
  isl_schedule *boundary_sched2, *boundary_sched1;

  sched = isl_schedule_node_get_schedule(node);

  /* We only enable double buffer for external array. */
  if (gen->options->autosa->double_buffer) {
    if (group->local_array->array_type == AUTOSA_EXT_ARRAY) 
      module->double_buffer = 1;
    else
      module->double_buffer = 0;
  } else {
    module->double_buffer = 0;
  }

  /* Inter transfer function. */
  sched2 = generate_io_module_inter_trans(sched, module, group, kernel, gen,
      io_level, space_dim, read, 0);
  if (is_filter) {
    /* Add the boundary module schedule. */
    module->boundary = 1;
    boundary_sched2 = generate_io_module_inter_trans(sched, module, group, 
                        kernel, gen, io_level, space_dim, read, 1);
  }
  /* Intra transfer function. */
  sched3 = generate_io_module_intra_trans(sched, module, group, kernel, gen,
      io_level, space_dim, read, is_buffer);
  /* Outer loops. */
  sched1 = generate_io_module_outer(sched, module, group, kernel, gen,
      io_level, space_dim, read, 0);
  if (is_filter) {
    /* Add the boundary module schedule. */
    module->boundary = 1;
    boundary_sched1 = generate_io_module_outer(sched, module, group, kernel, gen,
        io_level, space_dim, read, 1);
  }

  isl_schedule_free(sched);

  module->sched = NULL;
  module->outer_sched = sched1;
  module->inter_sched = sched2;
  module->intra_sched = sched3;
  if (module->boundary) {
    module->boundary_outer_sched = boundary_sched1;
    module->boundary_inter_sched = boundary_sched2;
  }

  return module;
}

static isl_stat generate_default_io_module_schedule(
  __isl_take struct autosa_hw_module *module, __isl_keep isl_schedule_node *node,
  struct autosa_array_ref_group *group, struct autosa_kernel *kernel, 
  struct autosa_gen *gen,
  int io_level, int space_dim, int is_filter, int is_buffer, int read, int boundary) 
{
  isl_schedule *sched1, *sched2;
  isl_ctx *ctx;
  isl_printer *p;
  char *io_mark;
  int n_io_ids = 0;
  isl_id_list *io_ids;
  isl_id *id;
  int is_mark;
  isl_set *context;
  char *fifo_suffix, *buf_suffix;
  isl_union_set *empty_filter = NULL;
  isl_union_set *eq_filter = NULL;
  int depth;
  char *stmt_name;
  struct autosa_io_buffer *buf = NULL;
  isl_union_map *group_access;
  isl_union_set *group_domain;
  int i;

  ctx = isl_schedule_node_get_ctx(node);
  sched1 = isl_schedule_node_get_schedule(node);
  sched2 = isl_schedule_dup(sched1);
  isl_schedule_free(sched1);
  node = isl_schedule_get_root(sched2);
  isl_schedule_free(sched2);

  n_io_ids = space_dim - io_level + 1;
  io_ids = ppcg_scop_generate_names(gen->prog->scop, n_io_ids, "p"); 

  n_io_ids = 0;
  /* Update the context. */
  context = isl_set_universe(isl_set_get_space(kernel->context));
  node = autosa_tree_move_down_to_array(node, kernel->core);
  while (!isl_schedule_node_is_io_mark(node, io_level)) {
    if (isl_schedule_node_get_type(node) == isl_schedule_node_band) {
      isl_union_map *umap;
      isl_union_set *uset;
      isl_multi_pw_aff *size;
      isl_id *id;
      isl_id_list *ids;

      umap = isl_schedule_node_band_get_partial_schedule_union_map(node);
      uset = isl_union_map_range(umap);
      size = ppcg_size_from_extent(isl_set_from_union_set(uset));
      ids = isl_id_list_from_id(isl_id_list_get_id(io_ids, n_io_ids));
      n_io_ids++;
      context = add_bounded_parameters_dynamic(context, size, ids);
      isl_id_list_free(ids);
      isl_multi_pw_aff_free(size);
    }
    node = isl_schedule_node_child(node, 0);
  }
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_context(node, context);

  /* Add the filters. */
  n_io_ids = 0;
  node = autosa_tree_move_down_to_array(node, kernel->core);
  while (!isl_schedule_node_is_io_mark(node, io_level)) {
    if (isl_schedule_node_get_type(node) == isl_schedule_node_band) {
      isl_id *id;
      isl_id_list *ids;
      isl_union_set *uset;

      ids = isl_id_list_from_id(isl_id_list_get_id(io_ids, n_io_ids));      
      if (n_io_ids == space_dim - io_level) {
        if (is_filter) {
          uset = set_schedule_ge(node, ids);
        } else {
          uset = set_schedule_eq(node, ids);
        }
      } else {
        uset = set_schedule_eq(node, ids);
      }
      n_io_ids++;
      node = isl_schedule_node_insert_filter(node, uset);
      isl_id_list_free(ids);
      node = isl_schedule_node_child(node, 0);
    }
    node = isl_schedule_node_child(node, 0);
  }
  if (module->to_pe) {
    /* Add filter to only send data to boundary PEs. */
    while (!isl_schedule_node_is_io_mark(node, 1)) {
      if (isl_schedule_node_get_type(node) == isl_schedule_node_band) {
        isl_union_set *uset;
        if (read)
          uset = schedule_eq_lb(node);
        else
          uset = schedule_eq_ub(node);
        node = isl_schedule_node_insert_filter(node, uset);
        node = isl_schedule_node_child(node, 0);
      }
      node = isl_schedule_node_child(node, 0);
    }
  }
  node = autosa_tree_move_up_to_kernel(node);

  /* Add the data transfer statements. */
  node = autosa_tree_move_down_to_io_mark(node, kernel->core, io_level); 
  if (is_buffer && is_filter) {
    isl_id_list *ids;

    ids = isl_id_list_from_id(isl_id_list_get_id(io_ids, space_dim - io_level));
    node = isl_schedule_node_parent(node);
    eq_filter = set_schedule_eq(node, ids); 
    node = isl_schedule_node_child(node, 0);
    
    isl_id_list_free(ids);
  }
  depth = isl_schedule_node_get_schedule_depth(node);
  init_suffix(module, group, &fifo_suffix, &buf_suffix);
  /* Locate the next buffer. */
  for (i = io_level; i >= 1; i--) {
    buf = group->io_buffers[i - 1];
    if (buf->tile != NULL)
      break;
  }
  if (is_buffer) {
    if (i != io_level) {
      /* The buffer is optimized out at this level. */
      is_buffer = 0;
    }
  }

  p = isl_printer_to_str(ctx);
  p = isl_printer_print_str(p, read? "in_trans" : "out_trans");
  if (module->to_mem) 
    p = isl_printer_print_str(p, "_dram");
  if (boundary)
    p = isl_printer_print_str(p, "_boundary");
  p = isl_printer_print_str(p, ".");
  p = isl_printer_print_str(p, fifo_suffix);
  if (module->to_mem)
    p = isl_printer_print_str(p, "_local");
  p = isl_printer_print_str(p, is_filter == 0? ".0" : ".1");
  p = isl_printer_print_str(p, is_buffer == 0? ".0" : ".1");
  p = isl_printer_print_str(p, ".");
  p = isl_printer_print_int(p, depth - 1); 
  p = isl_printer_print_str(p, ".");
  p = isl_printer_print_int(p, space_dim - io_level);  
  p = isl_printer_print_str(p, ".");
  p = isl_printer_print_int(p, buf->n_lane);

  /* Move the schedule node to the level of the buffer. 
   * TODO: fix it when the buf->tile == NULL.
   */
  node = autosa_tree_move_up_to_kernel(node);
  node = autosa_tree_move_down_to_depth(node, buf->tile->depth, kernel->core); 

  if (!buf->tile) {
    module->data_pack_inter = buf->n_lane;
    module->data_pack_intra = buf->n_lane;
    p = isl_printer_print_str(p, ".");
    p = isl_printer_print_int(p, buf->n_lane);
    stmt_name = isl_printer_get_str(p);
    isl_printer_free(p);
    /* Add the I/O statement for each array reference in the group. */
    node = add_io_copies_stmt_acc(kernel, group, node, 
              buf->tile, buf->n_lane, read, stmt_name, read? 1: 0,
              is_buffer && !read && 0 
                && kernel->options->autosa->insert_hls_dependence);
  } else {
    int coalesce_depth;
    isl_val *coalesce_bound_val;
    int coalesce_bound;

    /* Add the I/O statement for the entire group. */
    module->data_pack_inter = buf->n_lane;
    module->data_pack_intra = buf->n_lane;
    p = isl_printer_print_str(p, ".");
    p = isl_printer_print_int(p, buf->n_lane);

	  /* Compute the coalesce loop depth and upper bounds. */
    coalesce_depth = isl_schedule_node_get_schedule_depth(node) + buf->tile->n - 1;
    coalesce_bound_val = buf->tile->bound[buf->tile->n - 1].size;
    coalesce_bound = isl_val_get_num_si(coalesce_bound_val) / buf->n_lane;
    if (coalesce_bound <= 1) {
      coalesce_depth = -1;
    }

    p = isl_printer_print_str(p, ".");
    p = isl_printer_print_int(p, coalesce_depth);
    p = isl_printer_print_str(p, ".");
    p = isl_printer_print_int(p, coalesce_bound);    

    stmt_name = isl_printer_get_str(p);
    isl_printer_free(p);
    node = add_io_copies_stmt_tile(kernel, group, node, 
              buf->tile, buf->tile, buf->n_lane, read, 
              stmt_name, read? 1: 0, is_buffer,
              coalesce_bound > 1 && 0 && kernel->options->autosa->insert_hls_dependence);
    if (!is_buffer) {
      node = isl_schedule_node_cut(node);
      empty_filter = isl_union_set_from_set(isl_set_empty(isl_set_get_space(kernel->context)));
      node = isl_schedule_node_insert_filter(node, empty_filter);
    }
  }

  if (is_buffer) {
    /* Add a filter node. */     
    if (is_filter) {
      node = isl_schedule_node_insert_filter(node, eq_filter);  
      node = isl_schedule_node_child(node, 0);
    }

    /* Insert the extra transfer statement. */
    p = isl_printer_to_str(ctx);
    p = isl_printer_print_str(p, read? "out_trans." : "in_trans.");
    p = isl_printer_print_str(p, fifo_suffix);
    p = isl_printer_print_str(p, "_local");
    p = isl_printer_print_str(p, ".0"); // filter
    p = isl_printer_print_str(p, ".1"); // buffer
    p = isl_printer_print_str(p, ".-1"); // sched_depth
    p = isl_printer_print_str(p, ".-1"); // param_id
    p = isl_printer_print_str(p, ".");
    p = isl_printer_print_int(p, buf->n_lane);
    /* Locate the next buffer after the current buffer. */
    int cur_level = buf->level;
    struct autosa_io_buffer *cur_buf = buf;
    for (int i = cur_level - 1; i >= 1; i--) {
      buf = group->io_buffers[i - 1];
      if (buf->tile != NULL)
        break;
    }

    if (cur_level > 1) { 
      /* Move the schedule node to the level of the buffer. */
      node = autosa_tree_move_down_to_io_mark(node, kernel->core, buf->level);
      node = isl_schedule_node_child(node, 0);
    }
    if (cur_level == 1 || !buf->tile) {
      p = isl_printer_print_str(p, ".");
      p = isl_printer_print_int(p, group->n_lane);
      stmt_name = isl_printer_get_str(p);
      isl_printer_free(p);
      module->data_pack_intra = group->n_lane; 
      node = add_io_copies_stmt_acc(kernel, group, node, cur_buf->tile, 
                group->n_lane, read, stmt_name, read? 1 : 0,
                is_buffer && !read && cur_buf->n_lane != group->n_lane 
                  && kernel->options->autosa->insert_hls_dependence); 
    } else {
      int coalesce_depth;
      isl_val *coalesce_bound_val;
      int coalesce_bound;

      p = isl_printer_print_str(p, ".");
      p = isl_printer_print_int(p, buf->n_lane);

      /* Compute the coalesce loop depth and upper bounds. */
      coalesce_depth = isl_schedule_node_get_schedule_depth(node) + buf->tile->n - 1;
      coalesce_bound_val = buf->tile->bound[buf->tile->n - 1].size;
      coalesce_bound = isl_val_get_num_si(coalesce_bound_val) / buf->n_lane;
      if (coalesce_bound <= 1) {
        coalesce_depth = -1;
      }

      p = isl_printer_print_str(p, ".");
      p = isl_printer_print_int(p, coalesce_depth);
      p = isl_printer_print_str(p, ".");
      p = isl_printer_print_int(p, coalesce_bound);

      stmt_name = isl_printer_get_str(p);
      isl_printer_free(p);
      module->data_pack_intra = buf->n_lane;
      node = add_io_copies_stmt_tile(kernel, group, node, cur_buf->tile, 
              buf->tile, buf->n_lane, read, stmt_name, read? 1 : 0, is_buffer,
              coalesce_bound > 1 && cur_buf->n_lane != buf->n_lane
                && kernel->options->autosa->insert_hls_dependence);
      node = isl_schedule_node_cut(node);
      empty_filter = isl_union_set_from_set(isl_set_empty(
              isl_set_get_space(kernel->context)));
      node = isl_schedule_node_insert_filter(node, empty_filter);
    }
  }

  free(fifo_suffix);
  free(buf_suffix);

  /* Compute the union of domains of all the array references in the group. */
  group_access = isl_union_map_empty(isl_map_get_space(group->access));
  for (int i = 0; i < group->n_ref; i++) {
    struct autosa_stmt_access *ref = group->refs[i];
    if (group->group_type == AUTOSA_IO_GROUP) {
      group_access = isl_union_map_union(group_access,
          autosa_io_group_ref_access_relation(group, ref, read, !read));
    } else if (group->group_type == AUTOSA_DRAIN_GROUP) {
      group_access = isl_union_map_union(group_access,
          autosa_drain_group_ref_access_relation(group, ref, read, !read, 
          kernel->expanded_domain));
    }
  }
  group_domain = isl_union_map_domain(group_access);
  group_domain = isl_union_set_coalesce(group_domain);
  /* Add the group domain as the filter. */
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0); // context
  node = isl_schedule_node_child(node, 0); 
  node = isl_schedule_node_insert_filter(node, group_domain);

  /* Add the module mark. */
  id = isl_id_alloc(ctx, "module", module);
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_mark(node, id);

  sched1 = isl_schedule_node_get_schedule(node);
  isl_schedule_node_free(node);

  if (!boundary) {
    module->sched = sched1;
    module->type = (group->group_type == AUTOSA_DRAIN_GROUP)? 
                      DRAIN_MODULE : IO_MODULE;
    module->level = io_level;
    module->n_io_group++;
    module->io_groups = (struct autosa_array_ref_group **)realloc(module->io_groups,
        module->n_io_group * sizeof(struct autosa_array_ref_group *));
    module->io_groups[module->n_io_group - 1] = group;
    module->inst_ids = io_ids;
    module->kernel = kernel;
    module->is_buffer = is_buffer;
    module->is_filter = is_filter;
    if (read)
      module->in = 1;
    else
      module->in = 0;
    /* Create IO module variables. */
    if (is_buffer) {
      for (int i = io_level; i >= 1; i--) {
        buf = group->io_buffers[i - 1];
        if (buf->tile != NULL)
          break;
      }
      create_io_module_vars(module, kernel, buf->tile);
    }
  } else {
    isl_id_list_free(io_ids);
    module->boundary_sched = sched1;
  }

  return isl_stat_ok;
}

/* Generate the default I/O module when either is_filter or is_buffer is zero.
 */
static __isl_give struct autosa_hw_module *generate_default_io_module(
  __isl_take struct autosa_hw_module *module, __isl_keep isl_schedule_node *node,
  struct autosa_array_ref_group *group, struct autosa_kernel *kernel, 
  struct autosa_gen *gen,
  int io_level, int space_dim, int is_filter, int is_buffer, int read)
{
  isl_ctx *ctx = gen->ctx;

  generate_default_io_module_schedule(module, node, group, 
      kernel, gen, io_level, space_dim, is_filter, is_buffer, read, 0);

  if (is_filter) {
    /* Add the boundary module schedule. */
    module->boundary = 1;
    generate_default_io_module_schedule(module, node, group,
        kernel, gen, io_level, space_dim, is_filter, is_buffer, read, 1);
  }

  return module;
}

/* Generate the I/O modules for transffering the data.
 * The I/O module is decribed by two features:
 * - is_filter: If the module is a filter node, it will keep the data 
 *   that belongs to it and sends to the lower-level I/O modules or PEs. 
 *   Else, it will simply pass the data to downstream modules.
 * - is buffer: If the module is buffered. We will allocate a local buffer 
 *   inside the module.
 */               
static __isl_give struct autosa_hw_module *generate_io_module_by_type(
  __isl_take struct autosa_hw_module *module, __isl_keep isl_schedule_node *node, 
  struct autosa_array_ref_group *group, struct autosa_kernel *kernel, 
  struct autosa_gen *gen, int io_level, int space_dim, 
  int is_filter, int is_buffer, int read)
{
  if (is_filter && is_buffer) {
    module = generate_filter_buffer_io_module(module, node, group, kernel, 
      gen, io_level, space_dim, is_filter, is_buffer, read);
  } else {
    module = generate_default_io_module(module, node, group, kernel, 
      gen, io_level, space_dim, is_filter, is_buffer, read);
  }

  return module;
}

/* This function builds a set of I/O modules for each I/O group.
 * We will first examine if any flow dependence that is associated with the 
 * current group is carried by the array part loops. 
 * In that case, credit control should be added to force the dependece.
 * TODO: to be implemented.
 * Next, we will generate the copy-in set and copy-out set of I/O modules for 
 * the I/O groups. At each I/O level, we generate one I/O module.
 * We apply the I/O module pruning by default here.
 * Specifically, if the copy-out set at the current array_part loops equals 
 * the copy-in set at of the next array_part loops, there is no need to generate
 * to go off-chip, we will prune away such I/O modules.
 * If the I/O group has interior I/O at the PE level, the data required for the 
 * next iteration should reside in the PEs.
 * Otherwise, we will connect the copy-out I/O modules to the copy-in I/O modules,
 * and buffer the data on-chip. (TODO: not supported yet.)
 */
static __isl_give struct autosa_hw_module **sa_io_module_gen(
  struct autosa_array_ref_group *group,
  struct autosa_gen *gen, int *n_modules, int in, int out)
{
  // TODO: Add the support for manual tuning.
  isl_schedule_node *node;
  isl_ctx *ctx;
  struct autosa_kernel *kernel;
  int space_dim;
  int io_level;
  struct autosa_hw_module **modules = NULL;
  int module_cnt = 0;
  int credit = 0;

  ctx = gen->ctx;
  node = isl_schedule_get_root(group->io_schedule);
  io_level = group->io_level;
  space_dim = group->space_dim;
  kernel = gen->kernel;
  node = autosa_tree_move_down_to_kernel(node);

  /* Test if the deps in this I/O group are carried by array part loops.
   * If so, data hazards are possible, and we will set the credit as true
   * so that we could enable credit control between read and write I/O modules to 
   * prevent the data hazards. 
   * TODO: This is not supported yet.
   */
  if (gen->options->autosa->credit_control) {
    if (group->local_array->array_type == AUTOSA_INT_ARRAY) {
      isl_bool carried = isl_bool_false;
      isl_union_map *umap;
  
      node = autosa_tree_move_down_to_array(node, kernel->core);
      node = isl_schedule_node_parent(node);
      umap = isl_schedule_node_band_get_partial_schedule_union_map(node);
      for (int i = 0; i < group->n_ref; i++) {
        struct autosa_stmt_access *ref = group->refs[i];
        for (int j = 0; j < ref->n_io_info; j++) {
          struct autosa_io_info *io_info = ref->io_info[j];
          if (io_info->io_type == group->io_type && 
                !isl_vec_cmp(io_info->dir, group->dir)) {
            isl_map *test;
            isl_map *schedule_dep;
            int dim;
            int is_parallel;
            isl_union_map *dep = isl_union_map_from_map(
                isl_map_factor_domain(
                isl_map_from_basic_map(isl_basic_map_copy(io_info->dep->isl_dep))));
            dep = isl_union_map_apply_range(dep, isl_union_map_copy(umap));
            dep = isl_union_map_apply_domain(dep, isl_union_map_copy(umap));
            if (isl_union_map_is_empty(dep)) {
              isl_union_map_free(dep);
              break;
            }
            schedule_dep = isl_map_from_union_map(dep);
            test = isl_map_universe(isl_map_get_space(schedule_dep));
            dim = isl_schedule_node_band_n_member(node);
            for (int n = 0; n < dim; n++) {
              test = isl_map_equate(test, isl_dim_in, n, isl_dim_out, n);
            }
            is_parallel = isl_map_is_subset(schedule_dep, test);
            isl_map_free(schedule_dep);
            isl_map_free(test);
  
            if (!is_parallel) { 
              /* Dependence is carried by the array part loops. */
              carried = isl_bool_true;
              break;
            }
          }
        }
      }
      isl_union_map_free(umap); 
      if (carried) {
        credit = 1;
      }
      node = autosa_tree_move_up_to_kernel(node);
    }
  }

  /* At each I/O level, generate one I/O module. */
  /* Copy-in group. */
  if (in && is_module_valid(node, kernel, group, 1)) {
    group->array_io_dir = (group->array_io_dir == IO_OUT)? IO_INOUT : IO_IN;
    for (int i = io_level; i >= 1; i--) {
      struct autosa_hw_module *module;
      char *module_name = NULL;
      char *io_mark = NULL;
      isl_printer *p_str;
      int is_filter;
      int is_buffer;
      int innermost, outermost;
    
      /* Classify the module type. */
      outermost = io_level;
      if (group->io_type == AUTOSA_INT_IO)
        innermost = 1;
      else
        innermost = 2; // IO_L1 is integrated into PEs. No need to generate.

      /* Since we perform I/O clustering automatically, all the I/O modules
       * except the outermost level will be in the filter mode:
       * which means that they will pass data to downstreaming modules
       * and filter out the data that they need for the lower-level modules
       * they are connected to.
       */
      if (i == outermost)
        is_filter = 0;
      else
        is_filter = 1;

      if (group->group_type == AUTOSA_DRAIN_GROUP) {
        if (i == innermost)
          is_buffer = 1;
        else
          is_buffer = 0;
      } else if (group->group_type == AUTOSA_IO_GROUP) {
        if (group->local_array->array_type == AUTOSA_INT_ARRAY) {
          if (group->io_type == AUTOSA_EXT_IO) {
            if (i == innermost)
              is_buffer = 1;
            else
              is_buffer = 0;
          } else if (group->io_type == AUTOSA_INT_IO) {
            is_buffer = 0;
          }
        } else if (group->local_array->array_type == AUTOSA_EXT_ARRAY) {
          if (i == innermost)
            is_buffer = 1;
          else
            is_buffer = 0;
        }
      }

      if (gen->options->autosa->two_level_buffer) {
        /* When two-level buffering is enabled, 
         * we will implement a second-level buffe at the outermost I/O module.
         */
        if (i == outermost)
          is_buffer = 1;
      }

      /* Generate the I/O module */
      if (i >= innermost && i <= outermost) {
        module = autosa_hw_module_alloc(gen);
        module_name = generate_io_module_name(ctx, group, i, 1);
        module->name = module_name;
        module->to_pe = (i == innermost)? 1 : 0;
        module->to_mem = (i == outermost)? 1 : 0;
        module->credit = (i == outermost)? credit : 0;
        module->n_array_ref = group->local_array->n_io_group_refs;
        if (module->to_mem)
          group->local_array->n_io_group_refs++;

        module = generate_io_module_by_type(module, node, group, kernel, 
            gen, i, space_dim, is_filter, is_buffer, 1);

        module_cnt++;
        modules = (struct autosa_hw_module **)realloc(modules,
            module_cnt * sizeof(struct autosa_hw_module *));
        modules[module_cnt - 1] = module;
      }
    }
  }

  /* Copy-out group. */
  if (out && is_module_valid(node, kernel, group, 0)) {
    group->array_io_dir = (group->array_io_dir == IO_IN)? IO_INOUT : IO_OUT;
    for (int i = 1; i <= io_level; i++) {
      struct autosa_hw_module *module;
      char *module_name = NULL;
      char *io_mark = NULL;
      isl_printer *p_str;
      int is_filter;
      int is_buffer;
      int innermost, outermost;
    
      /* Classify the module type. */
      outermost = io_level;
      if (group->io_type == AUTOSA_INT_IO)
        innermost = 1;
      else
        innermost = 2; // IO_L1 is integrated into PEs.

      if (i == outermost)
        is_filter = 0;
      else
        is_filter = 1;
      if (group->group_type == AUTOSA_DRAIN_GROUP) {
        if (i == innermost)
          is_buffer = 1;
        else
          is_buffer = 0;
      } else if (group->group_type == AUTOSA_IO_GROUP) {
        if (group->io_type == AUTOSA_INT_IO) 
          is_buffer = 0;
        else {
          if (i == innermost) 
            is_buffer = 1;
          else
            is_buffer = 0;
        }
      }

      if (gen->options->autosa->two_level_buffer) {
        /* When two-level buffering is enabled, 
         * we will implement a second-level buffer at the outermost I/O module.
         */
        if (i == outermost)
          is_buffer = 1;
      }

      /* Generate the I/O module. */
      if (i >= innermost && i <= outermost) {
        module = autosa_hw_module_alloc(gen);
        module_name = generate_io_module_name(ctx, group, i, 0);
        module->name = module_name;
        module->to_pe = (i == innermost)? 1 : 0;
        module->to_mem = (i == outermost)? 1 : 0;
        module->credit = (i == outermost)? credit : 0;
        module->n_array_ref = group->local_array->n_io_group_refs;
        if (module->to_mem)
          group->local_array->n_io_group_refs++;

        module = generate_io_module_by_type(module, node, group, kernel, 
            gen, i, space_dim, is_filter, is_buffer, 0);

        module_cnt++;
        modules = (struct autosa_hw_module **)realloc(modules,
            module_cnt * sizeof(struct autosa_hw_module *));
        modules[module_cnt - 1] = module;
      } 
    }
  }

  isl_schedule_node_free(node);
  *n_modules = module_cnt;
  return modules;
}

/* If the band node "node" has more than "n" members, then split off
 * the first "n" of them.
 */
static __isl_give isl_schedule_node *split_band(
	__isl_take isl_schedule_node *node, int n)
{
	int dim;

	dim = isl_schedule_node_band_n_member(node);
	if (n < dim)
		node = isl_schedule_node_band_split(node, n);

	return node;
}

/* Compute the effective sa size as a list of the sizes in each dimension.
 *
 * The sa size specified by the user or set by default
 * in read_array_part_tile_sizes() and applied by the PE filter,
 * may be too large for the given code in the sense that
 * it may contain PEs that don't need to execute anything.
 * We therefore don't return this sa size, but instead the
 * smallest grid size that ensures that all blocks that actually
 * execute code are included in the grid.
 *
 * We first extract a description of the grid, i.e., the possible values
 * of the PE ids, from the domain elements in "domain" and
 * kernel->pe_filter.
 * The PE ids are parameters in kernel->pe_filter.
 * We simply need to change them into set dimensions.
 *
 * Then, for each PE dimension, we compute the maximal value of the PE id
 * and add one.
 */
static __isl_give isl_multi_pw_aff *extract_sa_grid_size(
	struct autosa_kernel *kernel, __isl_take isl_union_set *domain)
{
	int i;
	isl_set *grid;
	isl_set *context;
	isl_multi_pw_aff *size;

	domain = isl_union_set_intersect(domain,
				    isl_union_set_copy(kernel->pe_filter));

	grid = isl_union_set_params(domain);
	grid = isl_set_from_params(grid);
	grid = isl_set_add_dims(grid, isl_dim_set, kernel->n_sa_dim);

	for (i = 0; i < kernel->n_sa_dim; ++i) {
		int pos;
		isl_id *id;

		if (!grid)
			return NULL;

		id = isl_id_list_get_id(kernel->pe_ids, i);
		pos = isl_set_find_dim_by_id(grid, isl_dim_param, id);
		isl_id_free(id);
		if (pos < 0)
			isl_die(isl_set_get_ctx(grid), isl_error_internal,
				"missing constraints on PE identifier",
				grid = isl_set_free(grid));
		grid = isl_set_equate(grid, isl_dim_param, pos, isl_dim_set, i);
		grid = isl_set_project_out(grid, isl_dim_param, pos, 1);
	}

	grid = isl_set_coalesce(grid);
	size = ppcg_size_from_extent(grid);
	context = isl_set_params(isl_set_copy(kernel->context));
	return isl_multi_pw_aff_gist(size, context);
}

/* Internal struct for add_pe_ext_io_copies. */
struct autosa_add_pe_ext_io_copies_data {
  struct autosa_kernel *kernel;
  struct autosa_array_ref_group *pe_group;
  struct autosa_array_ref_group *io_group;
  struct autosa_stmt_access *ref;
  int read;
  int dummy;
  isl_union_set *filter;
};

/* Find the PE group that contains the reference "ref" from the IO group.
 */
static struct autosa_array_ref_group *autosa_find_pe_group(
  struct autosa_local_array_info *local_array,
  struct autosa_array_ref_group *io_group, 
  struct autosa_stmt_access *ref)
{
  /* As all accesses from the array are merged together for internal array,
   * simply return the first PE group. 
   */
  if (local_array->array_type == AUTOSA_INT_ARRAY)
    return local_array->pe_groups[0];
  
  for (int i = 0; i < local_array->n_pe_group; i++) {
    struct autosa_array_ref_group *pe_group = local_array->pe_groups[i];
    if (pe_group->refs[0] == ref)
      return pe_group;
  }

  return NULL;
}

/* Given a schedule node "node" of the type "isl_schedule_node_leaf", 
 * we will test if it is under any extension node.
 * If so, we will then test if the current node intersect with the extension domain. 
 */
static isl_bool leaf_node_is_extended(__isl_keep isl_schedule_node *node)
{
  isl_schedule_node *node_e;
  isl_schedule_node *node_f;
  isl_union_set *filter;
  isl_union_map *extension;
  isl_union_set *extension_range;

  if (isl_schedule_node_get_type(node) != isl_schedule_node_leaf)
    return isl_bool_error;

  node_e = isl_schedule_node_copy(node); 
  node_f = isl_schedule_node_copy(node); 

  while (node_e && isl_schedule_node_has_parent(node_e)) {
    if (isl_schedule_node_get_type(node_e) == isl_schedule_node_extension)
      break;
    node_e = isl_schedule_node_parent(node_e);
  }

  if (node_e == NULL || isl_schedule_node_get_type(node_e) != isl_schedule_node_extension) {
    isl_schedule_node_free(node_e);
    isl_schedule_node_free(node_f);
    return isl_bool_false;
  }

  extension = isl_schedule_node_extension_get_extension(node_e); 

  while (node_f && isl_schedule_node_has_parent(node_f)) {
    if (isl_schedule_node_get_type(node_f) == isl_schedule_node_filter)
      break;
    node_f = isl_schedule_node_parent(node_f);
  }

  filter = isl_schedule_node_filter_get_filter(node_f); 
  extension_range = isl_union_map_range(extension); 
  filter = isl_union_set_intersect(filter, extension_range); 
  isl_schedule_node_free(node_e);
  isl_schedule_node_free(node_f);
  if (isl_union_set_is_empty(filter)) {
    isl_union_set_free(filter);
    return isl_bool_false;
  }
  
  isl_union_set_free(filter);
  return isl_bool_true;
}

/* Insert data transfer statements beside the program statements. 
 * If the statement is under the SIMD loop, the data transfer statements 
 * are inserted before/after the SIMD loop. 
 * Otherwise, it is inserted before/after the statement.
 */
__isl_give isl_schedule_node *add_pe_ext_io_copies_stmt(
  __isl_take isl_schedule_node *node, void *user)
{
  struct autosa_add_pe_ext_io_copies_data *data = 
          (struct autosa_add_pe_ext_io_copies_data *)(user);
  isl_union_set *domain;
  isl_space *space;
  isl_space *acc_space;
  isl_id *id;
  isl_union_map *access;
  int empty;
  isl_multi_aff *from_access;
  isl_ctx *ctx;
  isl_schedule_node *graft;
  isl_multi_aff *ma;
  isl_multi_pw_aff *mpa;
  isl_multi_union_pw_aff *mupa;
  struct autosa_array_ref_group *pe_group = data->pe_group;
  struct autosa_array_ref_group *io_group = data->io_group;
  struct autosa_array_tile *tile;
  int read = data->read; 
  isl_union_map *sched;
  isl_union_map *ref_access;
  isl_map *acc;
  isl_bool ok;
  int is_simd;
  isl_printer *p_str;
  char *stmt_name;
  isl_union_set *empty_filter;
  int n_lane = io_group->n_lane;

  /* Test if the current stmt contains the reference. */
  if (isl_schedule_node_get_type(node) != isl_schedule_node_leaf)
    return node;

  /* Test if the node is under any extension node and if the 
   * node is extended by the extension node. 
   */
  if (!leaf_node_is_extended(node)) {
    isl_set *set;
    isl_id *new_id;
    domain = isl_schedule_node_get_domain(node); 
    set = isl_set_from_union_set(domain); 
    space = isl_set_get_space(set); 
    isl_set_free(set);
    id = isl_space_get_tuple_id(space, isl_dim_set); 
    isl_space_free(space);
    acc_space = isl_map_get_space(data->ref->access); 
    new_id = isl_space_get_tuple_id(acc_space, isl_dim_in);
    if (id != new_id) {
      isl_space_free(acc_space);
      isl_id_free(id);
      isl_id_free(new_id);
      
      /* Insert empty filter for dummy module. */
      if (data->dummy) {
        empty_filter = isl_union_set_from_set(
                isl_set_empty(isl_set_get_space(data->kernel->context)));
        node = isl_schedule_node_insert_filter(node, empty_filter);
      }
      return node;
    }
    isl_id_free(id);
    isl_id_free(new_id);
    isl_space_free(acc_space);
  } else {
    /* Simply return for the extension nodes. */
    return node;
  }

  ctx = isl_schedule_node_get_ctx(node);
  tile = NULL;
  /* Examine if there is any SIMD mark above. */
  is_simd = is_node_under_simd(node); 

  /* Aggregate the copy-in/out access
   * S -> [D -> A]
   * S: statement domain elements
   * D: prefix schedule dimensions
   * A: access
   */
  if (is_simd) {
    /* We will insert the statements before/after the SIMD loop. */    
    if (data->dummy) {
      isl_union_set *empty_filter;
      empty_filter = isl_union_set_from_set(isl_set_empty(
                        isl_set_get_space(data->kernel->context)));
      node = isl_schedule_node_insert_filter(node, empty_filter);
    }
    node = autosa_tree_move_up_to_mark(node, "simd");
  }
  access = io_comm_access_ref(data->kernel, node, io_group, data->ref, read);
  empty = isl_union_map_is_empty(access);
  if (empty < 0 || empty) {
    isl_union_map_free(access);
    if (empty < 0)
      return isl_schedule_node_free(node);
    return autosa_tree_move_up_to_kernel(node);
  }

  if (data->dummy) {
    data->filter = isl_schedule_node_get_domain(node);
  }

  /* Update the group io_dir. */
  if (!data->dummy) {
    if (read) {
      io_group->pe_io_dir = (io_group->pe_io_dir == IO_OUT)? IO_INOUT : IO_IN; 
    } else {
      io_group->pe_io_dir = (io_group->pe_io_dir == IO_IN)? IO_INOUT : IO_OUT;
    }
  }

  pe_group->array->global = 1;
  pe_group->local_array->global = 1;

  /* read.fifoX[D -> A] -> [D -> A] */
  p_str = isl_printer_to_str(ctx);
  if (read)
    p_str = isl_printer_print_str(p_str, "in");
  else
    p_str = isl_printer_print_str(p_str, "out");
  if (data->dummy)
    p_str = isl_printer_print_str(p_str, "_dummy");
  p_str = isl_printer_print_str(p_str, ".");
  if (io_group->group_type != AUTOSA_PE_GROUP) {
    p_str = isl_printer_print_str(p_str, "fifo_");
  }
  p_str = isl_printer_print_str(p_str, io_group->array->name);
  if (io_group->group_type == AUTOSA_IO_GROUP) {
    if (io_group->local_array->n_io_group > 1) {
      p_str = isl_printer_print_str(p_str, "_");
      p_str = isl_printer_print_int(p_str, io_group->nr);
    }
  } else if (io_group->group_type == AUTOSA_DRAIN_GROUP) {
    p_str = isl_printer_print_str(p_str, "_");
    p_str = isl_printer_print_str(p_str, "drain");
  }
  p_str = isl_printer_print_str(p_str, ".");
  p_str = isl_printer_print_int(p_str, io_group->n_lane);
  p_str = isl_printer_print_str(p_str, ".1");
  stmt_name = isl_printer_get_str(p_str);
  isl_printer_free(p_str);
    
  from_access = autosa_create_io_access_stmt(ctx, pe_group, io_group, 
    autosa_array_ref_group_tile(pe_group),
    isl_schedule_node_get_schedule_depth(node), stmt_name);
  free(stmt_name);

  /* Create a register tiling. */
  tile = create_register_tiling(node, pe_group, data->ref); 
  /* [D -> A] -> T */
  ma = isl_multi_aff_copy(tile->tiling);
  ma = isl_multi_aff_pullback_multi_aff(ma,
      isl_multi_aff_copy(from_access));
  mpa = isl_multi_pw_aff_from_multi_aff(ma); 
  /* read.fifoX[D -> A] -> T */
  mupa = isl_multi_union_pw_aff_from_multi_pw_aff(mpa); 
  /* [D -> A] */
  domain = isl_union_map_range(access);   
  /* read.fifoX[D -> A] */
  domain = isl_union_set_preimage_multi_aff(domain, from_access); 
  /* read.fifoX[D -> A] -> D */
  access = isl_union_set_wrapped_domain_map(domain); 
  /* D -> read.fifoX[D -> A] */
  access = isl_union_map_reverse(access);
  access = isl_union_map_coalesce(access);

  graft = isl_schedule_node_from_extension(access);
  graft = isl_schedule_node_child(graft, 0);
  graft = isl_schedule_node_insert_partial_schedule(graft, mupa);

  if (n_lane > 1) {
    /* Perform data packing. */
    int n_index;
    int tile_size[1];
    isl_id *id;
    isl_union_map *umap;
    isl_union_set *filter;

    n_index = isl_schedule_node_band_n_member(graft);
    /* Split off the last dimension. */
    if (n_index > 1) {
      graft = isl_schedule_node_band_split(graft, n_index - 1);
      graft = isl_schedule_node_child(graft, 0);
    }
    /* Tile the last dimension. */
    tile_size[0] = n_lane;
    graft = autosa_tile_band(graft, tile_size);
    graft = isl_schedule_node_child(graft, 0);
    /* Create a filter. */
    filter = schedule_eq_lb(graft);
    graft = isl_schedule_node_insert_filter(graft, filter);
  }

  while (graft && isl_schedule_node_has_parent(graft))
    graft = isl_schedule_node_parent(graft);

  if (read) {
    node = isl_schedule_node_graft_before(node, graft);
  } else {
    node = isl_schedule_node_graft_after(node, graft);
  }

  if (data->dummy) {
    /* insert an empty filter. */
    empty_filter = isl_union_set_from_set(isl_set_empty(
                    isl_set_get_space(data->kernel->context)));
    node = isl_schedule_node_insert_filter(node, empty_filter);
  }

  node = isl_schedule_node_parent(node); // filter
  node = isl_schedule_node_parent(node); // sequence
  node = isl_schedule_node_parent(node); // extension

  autosa_array_tile_free(tile);

  return node;
}

/* The "node" is pointed to the "PE" mark.
 * Add data transfer statements for each array access in the group.
 */
static __isl_give isl_schedule_node *add_pe_ext_io_copies(
  struct autosa_kernel *kernel,
  struct autosa_local_array_info *local_array,
  struct autosa_array_ref_group *io_group,
  __isl_take isl_schedule_node *node, int read)
{
  for (int i = 0; i < io_group->n_ref; i++) {
    struct autosa_stmt_access *ref = io_group->refs[i];
    struct autosa_array_ref_group *pe_group = 
            autosa_find_pe_group(local_array, io_group, ref);
    struct autosa_add_pe_ext_io_copies_data data = 
            {kernel, pe_group, io_group, ref, read, 0, NULL};
    node = isl_schedule_node_map_descendant_bottom_up(node, 
              &add_pe_ext_io_copies_stmt, &data);
  }

  return node;
}

/* Add the statements for copy-in/out the data for array references associated with
 * interior I/O.
 * The "node" is pointed to the "PE" mark.
 */
__isl_give isl_schedule_node *add_pe_int_io_copies(
  struct autosa_kernel *kernel,
  struct autosa_local_array_info *local_array,
  struct autosa_array_ref_group *io_group,
  __isl_take isl_schedule_node *node, int read)
{
  struct autosa_array_tile *tile;
  isl_union_map *access;
  isl_schedule_node *graft;
  int empty;
  isl_multi_aff *from_access;
  isl_multi_aff *ma;
  isl_multi_pw_aff *mpa;
  isl_multi_union_pw_aff *mupa;
  isl_union_set *domain;
  struct autosa_array_ref_group *pe_group;
  int n_lane = io_group->n_lane;
  isl_printer *p_str;
  char *stmt_name;
  isl_id *id;

  node = isl_schedule_node_child(node, 0);
  /* For array references with interior I/O, 
   * search for the corresponding PE group. */
  pe_group = autosa_find_pe_group(local_array, io_group, NULL);
  tile = autosa_array_ref_group_tile(pe_group);

  /* Aggregate the copy-in/out access 
   * S -> [D -> A] 
   * S: statement domain elements
   * D: prefix schedule dimensions 
   * A: access */
  access = io_comm_access(kernel, node, io_group, read);
  empty = isl_union_map_is_empty(access);
  if (empty < 0 || empty) {
    isl_union_map_free(access);
    if (empty < 0)
      return isl_schedule_node_free(node);
    return autosa_tree_move_up_to_pe(node);
  }

  /* Update the group io_dir. */
  if (read) {
    io_group->pe_io_dir = (io_group->pe_io_dir == IO_OUT)? IO_INOUT: IO_IN;
  } else {
    io_group->pe_io_dir = (io_group->pe_io_dir == IO_IN)? IO_INOUT: IO_OUT;
  }

  pe_group->array->global = 1;
  pe_group->local_array->global = 1;

  /* read.fifoX[D -> A] -> [D -> A] */
  /* Generate statement name. */
  p_str = isl_printer_to_str(kernel->ctx);
  if (read)
    p_str = isl_printer_print_str(p_str, "in");
  else
    p_str = isl_printer_print_str(p_str, "out");
  p_str = isl_printer_print_str(p_str, ".");
  if (io_group->group_type != AUTOSA_PE_GROUP) {
    p_str = isl_printer_print_str(p_str, "fifo_");
  }
  p_str = isl_printer_print_str(p_str, io_group->array->name);
  if (io_group->group_type == AUTOSA_IO_GROUP) {
    if (io_group->local_array->n_io_group > 1) {
      p_str = isl_printer_print_str(p_str, "_");
      p_str = isl_printer_print_int(p_str, io_group->nr);
    }
  } else if (io_group->group_type == AUTOSA_DRAIN_GROUP) {
    p_str = isl_printer_print_str(p_str, "_");
    p_str = isl_printer_print_str(p_str, "drain");
  }
  p_str = isl_printer_print_str(p_str, ".");
  p_str = isl_printer_print_int(p_str, io_group->n_lane);
  p_str = isl_printer_print_str(p_str, ".1");
  stmt_name = isl_printer_get_str(p_str);
  isl_printer_free(p_str);

  from_access = autosa_create_io_access_stmt(kernel->ctx, pe_group, io_group, 
      autosa_array_ref_group_tile(pe_group), 
      isl_schedule_node_get_schedule_depth(node), stmt_name);
  free(stmt_name);

  /* [D -> A] -> T */
  ma = isl_multi_aff_copy(tile->tiling);
  ma = isl_multi_aff_pullback_multi_aff(ma,
          isl_multi_aff_copy(from_access));
  mpa = isl_multi_pw_aff_from_multi_aff(ma);
  /* read.fifoX[D -> A] -> T */
  mupa = isl_multi_union_pw_aff_from_multi_pw_aff(mpa);
  /* [D -> A] */
  domain = isl_union_map_range(access);
  /* If the array is not a scalar, then we copy in/out the entire
   * tile to/from the local memory. 
   */
  if (read && !autosa_array_is_scalar(io_group->array)) {
    isl_map *map;
    isl_set *set;
    set = isl_map_domain(isl_map_from_union_map(isl_union_set_unwrap(domain)));    
    map = group_tile_buffer(io_group, io_group->pe_tile);
    map = isl_map_intersect_domain(map, set); 
    domain = isl_union_set_from_set(isl_map_wrap(map));
  }

  /* read.fifoX[D -> A] */
  domain = isl_union_set_preimage_multi_aff(domain, from_access);
  access = isl_union_set_wrapped_domain_map(domain);
  access = isl_union_map_reverse(access);
  access = isl_union_map_coalesce(access);

  graft = isl_schedule_node_from_extension(access);
  graft = isl_schedule_node_child(graft, 0);
  graft = isl_schedule_node_insert_partial_schedule(graft, mupa);

  if (n_lane > 1) {
    /* Perform data packing. */
    int n_index;
    int tile_size[1];
    isl_id *id;
    isl_union_map *umap;
    isl_union_set *filter;

    n_index = isl_schedule_node_band_n_member(graft);
    /* Split off the last dimension. */
    if (n_index > 1) {
      graft = isl_schedule_node_band_split(graft, n_index - 1);
      graft = isl_schedule_node_child(graft, 0);
    }
    /* Tile the last dimension. */
    tile_size[0] = n_lane;
    graft = autosa_tile_band(graft, tile_size);
    graft = isl_schedule_node_child(graft, 0);
    /* Create a filter. */
    filter = schedule_eq_lb(graft);
    graft = isl_schedule_node_insert_filter(graft, filter);
    /* Move to the tile loop. */
    graft = isl_schedule_node_parent(graft);
  }

  /* Insert a "pipeline" mark inside the band node. */
  id = isl_id_alloc(kernel->ctx, "hls_pipeline", NULL);
  graft = isl_schedule_node_child(graft, 0);
  graft = isl_schedule_node_insert_mark(graft, id);
  graft = isl_schedule_node_parent(graft);

  while (graft && isl_schedule_node_has_parent(graft))
    graft = isl_schedule_node_parent(graft);

  if (read) {
    node = isl_schedule_node_graft_before(node, graft);
  } else {
    node = isl_schedule_node_graft_after(node, graft);
  }

  node = autosa_tree_move_up_to_pe(node);

  return node;
}

static isl_bool find_latency_mark(__isl_keep isl_schedule_node *node, void *user)
{
  if (isl_schedule_node_get_type(node) == isl_schedule_node_mark) {
    isl_id *id;

    id = isl_schedule_node_mark_get_id(node);
    if (!strcmp(isl_id_get_name(id), "latency")) {
      isl_id_free(id);
      return isl_bool_false;
    }
    isl_id_free(id);
  }

  return isl_bool_true;
}

/* Insert a "hls_pipeline" mark after the innermost "latency" mark.
 * The loop will be eventually pipelined.
 * The "hls_pipeline" mark is placed under the band node.
 */
static __isl_give isl_schedule_node *insert_pipeline_mark(
  __isl_take isl_schedule_node *node, void *user)
{
  struct autosa_kernel *kernel = (struct autosa_kernel *)user;
  isl_ctx *ctx = kernel->ctx;
  
  if (isl_schedule_node_get_type(node) == isl_schedule_node_mark) {
    isl_id *id;

    id = isl_schedule_node_mark_get_id(node);
    if (!strcmp(isl_id_get_name(id), "latency")) {
      /* Examine if there is any latency mark inside the current mark. */
      isl_bool no_inner_latency;
      node = isl_schedule_node_child(node, 0);
      no_inner_latency = isl_schedule_node_every_descendant(node,
          &find_latency_mark, NULL); 
      node = isl_schedule_node_parent(node);
      if (no_inner_latency) {
        /* Insert the "hls_pipeline" mark below the band node. */
        isl_id *hls_id;
        hls_id = isl_id_alloc(ctx, "hls_pipeline", NULL);
        node = isl_schedule_node_child(node, 0);
        node = isl_schedule_node_child(node, 0);
        node = isl_schedule_node_insert_mark(node, hls_id);

        node = isl_schedule_node_parent(node);
        node = isl_schedule_node_parent(node);
      }
    }
    isl_id_free(id);
  }

  return node;
}

/* Insert a "hls_unroll" mark after the "simd" mark.
 * The loop will be eventually unrolled.
 * The "hls_unroll" mark is placed under the band node.
 */
static __isl_give isl_schedule_node *insert_unroll_mark(
  __isl_take isl_schedule_node *node, void *user)
{
  struct autosa_kernel *kernel = (struct autosa_kernel *)user;
  isl_ctx *ctx = kernel->ctx;

  if (isl_schedule_node_get_type(node) == isl_schedule_node_mark) {
    isl_id *id;

    id = isl_schedule_node_mark_get_id(node);
    if (!strcmp(isl_id_get_name(id), "simd")) {
      isl_id *hls_id;
      hls_id = isl_id_alloc(ctx, "hls_unroll", NULL);
      node = isl_schedule_node_child(node, 0);
      node = isl_schedule_node_child(node, 0);
      node = isl_schedule_node_insert_mark(node, hls_id);
      node = isl_schedule_node_parent(node);
      node = isl_schedule_node_parent(node);
    }
    isl_id_free(id);
  }

  return node;
}

/* Insert a context node at "node" introducing the PE identifiers 
 * along with their bounds, which are stored in kernel->sa_grid_size.
 */
static __isl_give isl_schedule_node *insert_context(struct autosa_kernel *kernel,
	__isl_take isl_schedule_node *node)
{
	isl_set *context;

	context = isl_set_universe(isl_set_get_space(kernel->context));
	context = add_bounded_parameters_dynamic(context,
					kernel->sa_grid_size, kernel->pe_ids);
	node = isl_schedule_node_insert_context(node, context);

	return node;
}

/* Create the local buffer variables inside the PE.
 * Specifically, we will also scan through all IO groups for the array,
 * find the lcm of all the data packing factors to set as the array partitioning
 * factor for the local buffer so that all I/O groups should be able to 
 * access the packed elements without any bank conflict.
 */
static void create_pe_module_var(isl_ctx *ctx, 
  struct autosa_array_ref_group *group,
  struct autosa_kernel_var *var, struct autosa_local_array_info *local)
{
  struct autosa_array_tile *tile;
  isl_printer *p;
  isl_val *lcm = isl_val_int_from_si(ctx, 1);

  var->array = group->array;
  var->type = autosa_array_ref_group_type(group);  
  var->n_lane = 1;
  /* Scan all the I/O groups, and compute the lcm of the group SIMD factors,
   * set it as the partition factor of the variable. */
  for (int i = 0; i < local->n_io_group; i++) {
    struct autosa_array_ref_group *io_group = local->io_groups[i];
    isl_val *val = isl_val_int_from_si(ctx, io_group->n_lane);
    isl_val *product = isl_val_mul(isl_val_copy(val), isl_val_copy(lcm));
    isl_val *gcd = isl_val_gcd(val, lcm);
    lcm = isl_val_div(product, gcd);
  }
  var->n_part = isl_val_get_num_si(lcm);
  isl_val_free(lcm);

  tile = autosa_array_ref_group_tile(group);

  p = isl_printer_to_str(ctx);
  p = autosa_array_ref_group_print_name(group, p);
  var->name = isl_printer_get_str(p);
  isl_printer_free(p);

  if (tile == NULL) {
    var->size = isl_vec_alloc(ctx, 1);
    var->size = isl_vec_set_element_si(var->size, 0, 1);
  } else {
    var->size = isl_vec_alloc(ctx, group->array->n_index);
    for (int i = 0; i < group->array->n_index; ++i) {
      var->size = isl_vec_set_element_val(var->size, i,
          isl_val_copy(tile->bound[i].size));
    }
  }
}

/* Create the local buffer variables inside the PE module. */
static isl_stat create_pe_module_vars(struct autosa_hw_module *module, 
  struct autosa_kernel *kernel)
{
  int n = 0;
  for (int i = 0; i < kernel->n_array; ++i) {
    struct autosa_local_array_info *array = &kernel->array[i];

    for (int j = 0; j < array->n_pe_group; j++) {    
      struct autosa_array_ref_group *group = array->pe_groups[j];
      enum autosa_group_access_type type;
      
      type = autosa_array_ref_group_type(group);
      if (type != AUTOSA_ACCESS_GLOBAL)
        n++;
    }
  }

  module->var = isl_calloc_array(kernel->ctx, struct autosa_kernel_var, n);
  if (!module->var)
    return isl_stat_error;
  module->n_var = n;

  n = 0;
  for (int i = 0; i < kernel->n_array; ++i) {
    struct autosa_local_array_info *array = &kernel->array[i];

    for (int j = 0; j < array->n_pe_group; j++) {
      struct autosa_array_ref_group *group = array->pe_groups[j];
      enum autosa_group_access_type type;
      
      type = autosa_array_ref_group_type(group);
      if (type == AUTOSA_ACCESS_GLOBAL)
        continue;
      create_pe_module_var(kernel->ctx, group, &module->var[n], array);
      n++;
    }
  }
 
  return isl_stat_ok;
}

/* The "node" is pointed to the "PE" mark.
 */
static __isl_give isl_schedule_node *add_pe_ext_io_copies_dummy(
  struct autosa_kernel *kernel,
  struct autosa_local_array_info *local_array,
  struct autosa_array_ref_group *io_group,
  __isl_take isl_schedule_node *node, int read)
{
  isl_union_set *filter = isl_union_set_from_set(isl_set_empty(
                            isl_set_get_space(kernel->context)));
  for (int i = 0; i < io_group->n_ref; i++) {
    struct autosa_stmt_access *ref = io_group->refs[i];
    struct autosa_array_ref_group *pe_group = autosa_find_pe_group(
                                                local_array, io_group, ref);
    struct autosa_add_pe_ext_io_copies_data data = 
              {kernel, pe_group, io_group, ref, read, 1, NULL};
    node = isl_schedule_node_map_descendant_bottom_up(node, 
              &add_pe_ext_io_copies_stmt, &data);
    filter = isl_union_set_union(filter, data.filter);
  }

  filter = isl_union_set_coalesce(filter);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_filter(node, filter);
  node = isl_schedule_node_parent(node);
  return node;
}

/* Create the schedule for the PE dummy module that collects the dummy data.
 */
static __isl_give isl_schedule *pe_module_dummy_gen(struct autosa_gen *gen,
  struct autosa_hw_module *module, struct autosa_array_ref_group *group)
{
  isl_schedule *schedule;
  isl_schedule_node *node;
  isl_id *id, *hw_id;
  struct autosa_kernel *kernel;

  schedule = gen->schedule;
  schedule = isl_schedule_dup(schedule);
  node = isl_schedule_get_root(schedule);
  isl_schedule_free(schedule);
  node = autosa_tree_move_down_to_kernel(node);

  id = isl_schedule_node_mark_get_id(node);
  kernel = (struct autosa_kernel *)isl_id_get_user(id);
  isl_id_free(id);

  node = autosa_tree_move_down_to_array(node, kernel->core);
  node = isl_schedule_node_child(node, 0);
  node = split_band(node, kernel->n_sa_dim);
  node = autosa_tree_move_down_to_pe(node, kernel->core);
  node = add_pe_ext_io_copies_dummy(kernel, group->local_array, group, node, 1);

  /* Insert "pipeline" mark under the last "latency" mark. */
  node = isl_schedule_node_map_descendant_bottom_up(node,
      &insert_pipeline_mark, kernel);

  /* Insert "unroll" mark under the last "simd" mark. */
  node = isl_schedule_node_map_descendant_bottom_up(node,
      &insert_unroll_mark, kernel);

  /* Add module mark after the kernel mark. */
  hw_id = isl_id_alloc(gen->ctx, "module", module);
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_mark(node, hw_id);

  /* Add the PE id filter. */
  node = autosa_tree_move_up_to_kernel(node);
  isl_schedule_node_child(node, 0);
  node = insert_context(kernel, node);
  node = autosa_tree_move_down_to_array(node, kernel->core);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_filter(node,
      isl_union_set_copy(kernel->pe_filter));

  schedule = isl_schedule_node_get_schedule(node);
  isl_schedule_node_free(node);

  return schedule;
}

/* Modify the input "schedule" to describe the PE module.
 * Set the schedule dimensions of space loops as parameters.
 *
 * For interior I/O groups
 * - add copy-in before PE computation (RAW, RAR)
 * - add copy-out after PE computation (RAW)
 *   - domain: S -> type[D -> access]
 *   - schedule: type[D -> access] -> tiling
 * For exterior I/O groups
 *   for each access in the group
 *   - add copy-in before user statement (RAW, RAR)
 *   - add copy-out after user statement (RAW, RAR)
 *     - domain: S -> type[D -> access]
 *     - schedule: type[D -> access] -> tiling 
 *       (if any, otherwise, create a register tiling)
 * For WAW group 
 * - for each access in the group
 *   - add write-out after user statement (WAW)
 *     - domain: S -> type[D -> access]
 *     - schedule: type[D -> access] -> tiling
 */
static __isl_give struct autosa_hw_module *sa_pe_module_gen(struct autosa_gen *gen)
{
  isl_schedule_node *node;
  isl_id *id;
  struct autosa_kernel *kernel;
  isl_schedule *schedule, *new_schedule;
  int single_statement;
  isl_union_set *domain;
  struct autosa_hw_module *module;
  isl_id *hw_id;

  module = autosa_hw_module_alloc(gen);

  /* Add the filters for PEs. */
  schedule = gen->schedule;
  schedule = isl_schedule_dup(schedule);
  node = isl_schedule_get_root(schedule);
  node = autosa_tree_move_down_to_kernel(node);
  
  id = isl_schedule_node_mark_get_id(node);
  kernel = (struct autosa_kernel *)isl_id_get_user(id);
  isl_id_free(id);
  single_statement = kernel->single_statement;
  domain = isl_schedule_node_get_domain(node);

  node = autosa_tree_move_down_to_array(node, kernel->core);
  node = isl_schedule_node_child(node, 0);
  node = split_band(node, kernel->n_sa_dim);
  kernel->pe_ids = ppcg_scop_generate_names(gen->prog->scop,
      kernel->n_sa_dim, "p");
  kernel->pe_filter = set_schedule_modulo(node, kernel->pe_ids,
      kernel->sa_dim);
  kernel->sa_grid_size = extract_sa_grid_size(kernel, domain);

  /* Add the statements for I/O groups with exterior I/O at the user 
   * statement level. 
   * Add the statements for I/O group with interior I/O at the PE level.
   */
  node = autosa_tree_move_down_to_pe(node, kernel->core);
  /* Add copy-in/copy-out statements */
  for (int i = 0; i < kernel->n_array; ++i) {
    struct autosa_local_array_info *array = &kernel->array[i];
    for (int j = 0; j < array->n_io_group; j++) {
      struct autosa_array_ref_group *group = array->io_groups[j];
      if (group->array_io_dir == IO_NULL)
        continue;
      if (group->local_array->array_type == AUTOSA_EXT_ARRAY) {
        node = add_pe_ext_io_copies(kernel, array, group, node, 0);
        node = add_pe_ext_io_copies(kernel, array, group, node, 1);
      } else if (group->local_array->array_type == AUTOSA_INT_ARRAY) {
        if (group->io_type == AUTOSA_INT_IO) {
          node = add_pe_int_io_copies(kernel, array, group, node, 0);  
          node = add_pe_int_io_copies(kernel, array, group, node, 1); 
        } else {
          node = add_pe_ext_io_copies(kernel, array, group, node, 0); 
          node = add_pe_ext_io_copies(kernel, array, group, node, 1); 
        }
      }
      
      module->n_io_group++;
      module->io_groups = (struct autosa_array_ref_group **)realloc(
        module->io_groups,
        module->n_io_group * sizeof(struct autosa_array_ref_group *));
      module->io_groups[module->n_io_group - 1] = group;
    }
    if (array->drain_group && array->drain_group->array_io_dir != IO_NULL) {
      node = add_pe_ext_io_copies(kernel, array, array->drain_group, node, 0);

      module->n_io_group++;
      module->io_groups = (struct autosa_array_ref_group **)realloc(
        module->io_groups,
        module->n_io_group * sizeof(struct autosa_array_ref_group *));
      module->io_groups[module->n_io_group - 1] = array->drain_group;
    }
  }

  /* Insert "pipeline" mark under the last "latency" mark. */
  node = isl_schedule_node_map_descendant_bottom_up(node,
      &insert_pipeline_mark, kernel);

  /* Insert "unroll" mark under the last "simd" mark */
  node = isl_schedule_node_map_descendant_bottom_up(node,
      &insert_unroll_mark, kernel);

  /* Add module mark after the kernel mark. */
  hw_id = isl_id_alloc(gen->ctx, "module", module);
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_mark(node, hw_id);

  /* Add the PE id filter. */
  node = autosa_tree_move_up_to_kernel(node);
  isl_schedule_node_child(node, 0);
  node = insert_context(kernel, node); 
  node = autosa_tree_move_down_to_array(node, kernel->core);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_filter(node, 
      isl_union_set_copy(kernel->pe_filter));

  isl_schedule_free(schedule);
  new_schedule = isl_schedule_node_get_schedule(node);
  isl_schedule_node_free(node);

  module->sched = new_schedule;
  module->type = PE_MODULE;
  module->name = strdup("PE");
  module->inst_ids = isl_id_list_copy(kernel->pe_ids);
  create_pe_module_vars(module, kernel);
  module->kernel = kernel;

  /* For io group with exterior I/O, we create input and output ports for each
   * PE. However, for the first/last PE on the data transfer direction, 
   * the input/output port consumes/produces dummy data. 
   * We add dummy modules to handle these cases to consume the dummy data.
   */
  module->n_pe_dummy_modules = 0;
  module->pe_dummy_modules = NULL;
  for (int i = 0; i < kernel->n_array; ++i) {
    struct autosa_local_array_info *array = &kernel->array[i];
    if (array->array_type == AUTOSA_INT_ARRAY)
      continue;
    for (int j = 0; j < array->n_io_group; j++) {
      struct autosa_array_ref_group *group = array->io_groups[j];
      if (group->pe_io_dir != IO_INOUT)
        continue;
      /* Generate the dummy module. */
      isl_schedule *sched;
      sched = pe_module_dummy_gen(gen, module, group);
      module->n_pe_dummy_modules++;
      module->pe_dummy_modules = 
        (struct autosa_pe_dummy_module **)realloc(module->pe_dummy_modules,
            module->n_pe_dummy_modules * sizeof(struct autosa_pe_dummy_module *));
      struct autosa_pe_dummy_module *dummy_module = autosa_pe_dummy_module_alloc();
      dummy_module->module = module;
      dummy_module->io_group = group;
      dummy_module->sched = sched;
      module->pe_dummy_modules[module->n_pe_dummy_modules - 1] = dummy_module;
    }
  }

  return module;
}

/* The input modules are organized in the sequence of:
 * PE module
 * I/O module (copy-in and copy-out)
 * Drain module
 * We will reorder the modules following the below sequence:
 * I/O module (copy-in)
 * PE module
 * I/O module (copy-out)
 * Drain module
 * The reason for the re-ordering is for CSim to proceed in Xilinx environment.
 */
static __isl_give struct autosa_hw_module **hw_module_reorder(
  __isl_take struct autosa_hw_module **modules, int n_module)
{
  struct autosa_hw_module **modules_new = (struct autosa_hw_module **)
      malloc(n_module * sizeof(struct autosa_hw_module *));
  int pos = 0;

  /* I/O module (copy-in) */
  for (int i = 0; i < n_module; i++) {
    struct autosa_hw_module *module = modules[i];
    if (module->type == IO_MODULE && module->in) {
      modules_new[pos] = module;
      pos++;
    }
  }

  /* PE module */
  modules_new[pos] = modules[0];
  pos++;

  /* I/O module (copy-out) */
  for (int i = 0; i < n_module; i++) {
    struct autosa_hw_module *module = modules[i];
    if (module->type == IO_MODULE && !module->in) {
      modules_new[pos] = module;
      pos++;
    }
  }

  /* Drain module */
  for (int i = 0; i < n_module; i++) {
    struct autosa_hw_module *module = modules[i];
    if (module->type == DRAIN_MODULE) {
      modules_new[pos] = module;
      pos++;
    }
  }

  free(modules);
  return modules_new;
}

/* Create the schedule that calls all the PE dummy modules.
 * We will work on the transformed IO schedule for the io group.
 * We delete the schedule nodes above the array mark and below the PE mark,
 * add a filter to only consider the last module in the transfer chain.
 * Then insert the module call extension nodes right under the space bands.
 */
static __isl_give isl_schedule *pe_dummy_gen_module_call(struct autosa_gen *gen,
  struct autosa_pe_dummy_module *pe_dummy_module)
{
  struct autosa_array_ref_group *group;
  isl_schedule *sched;
  isl_schedule_node *node;
  struct autosa_kernel *kernel;
  struct autosa_hw_module *module;
  int n_member;
  isl_union_set *L1_filter;
  isl_bool insert_L1 = isl_bool_false;
  isl_printer *p_str;
  isl_ctx *ctx;
  char *stmt_name;
  isl_id *id;
  isl_union_map *prefix, *extension;
  isl_union_set *domain, *range;

  module = pe_dummy_module->module;
  kernel = module->kernel;
  ctx = gen->ctx;
  group = pe_dummy_module->io_group;
  sched = isl_schedule_dup(group->io_L1_schedule);
  node = isl_schedule_get_root(sched);
  isl_schedule_free(sched);
  isl_space *space;
  isl_union_set *empty_filter;
  isl_schedule_node *graft;

  /* Delete the node above the array mark. */
  node = autosa_tree_move_down_to_array(node, kernel->core);
  node = isl_schedule_node_parent(node);
  while (!autosa_tree_node_is_kernel(node)) {
    node = isl_schedule_node_delete(node);
    node = isl_schedule_node_parent(node);
  }

  /* Insert a filter. */
  node = autosa_tree_move_down_to_mark(node, kernel->core, "io_L1");
  node = isl_schedule_node_parent(node);
  n_member = isl_schedule_node_band_n_member(node);
  if (n_member > 1) {
    node = isl_schedule_node_band_split(node, n_member - 1);
    node = isl_schedule_node_child(node, 0);
  }
  if (isl_schedule_node_get_type(node) == isl_schedule_node_band) {
    L1_filter = schedule_eq_ub(node);
    insert_L1 = isl_bool_true;
  }

  node = autosa_tree_move_down_to_mark(node, kernel->core, "io_L1");
  node = isl_schedule_node_child(node, 0);
  if (insert_L1) {
    node = isl_schedule_node_insert_filter(node, L1_filter);
  }

  /* Delete the node under the pe mark. */
  node = autosa_tree_move_down_to_pe(node, kernel->core);
  node = isl_schedule_node_cut(node);

  /* Graft an extension node. */
  prefix = isl_schedule_node_get_prefix_schedule_relation(node);
  prefix = isl_union_map_preimage_domain_union_pw_multi_aff(prefix,
              isl_union_pw_multi_aff_copy(kernel->contraction));
  domain = isl_union_map_range(prefix);

  p_str = isl_printer_to_str(ctx);
  p_str = isl_printer_print_str(p_str, "module_call.");
  p_str = autosa_array_ref_group_print_prefix(group, p_str);
  p_str = isl_printer_print_str(p_str, "_PE_dummy");
  stmt_name = isl_printer_get_str(p_str);
  isl_printer_free(p_str);
  space = isl_space_set_alloc(ctx, 0, 0);
  space = isl_space_set_tuple_name(space, isl_dim_set, stmt_name);
  free(stmt_name);

  isl_point *pnt = isl_point_zero(space);
  isl_set *set = isl_set_from_point(pnt);
  range = isl_union_set_from_set(isl_set_copy(set));
  extension = isl_union_map_from_domain_and_range(domain, range);
  graft = isl_schedule_node_from_extension(extension);

  isl_map *map = isl_set_identity(set);
  map = isl_map_reset_tuple_id(map, isl_dim_out);
  isl_union_map *umap = isl_union_map_from_map(map);
  isl_multi_union_pw_aff *mupa = isl_multi_union_pw_aff_from_union_map(umap);

  graft = isl_schedule_node_child(graft, 0);
  graft = isl_schedule_node_insert_partial_schedule(graft, mupa);

  while (graft && isl_schedule_node_has_parent(graft))
    graft = isl_schedule_node_parent(graft);

  node = isl_schedule_node_graft_before(node, graft);

  /* Insert an empty filter. */
  empty_filter = isl_union_set_from_set(isl_set_empty(
                    isl_set_get_space(kernel->context)));
  node = isl_schedule_node_insert_filter(node, empty_filter);

  /* Add module mark after the kernel mark. */
  id = isl_id_alloc(ctx, "module", module);
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_mark(node, id);

  /* Add pe_dummy module mark after the module mark. */
  id = isl_id_alloc(ctx, "pe_dummy_module", pe_dummy_module);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_mark(node, id);

  sched = isl_schedule_node_get_schedule(node);
  isl_schedule_node_free(node);

  return sched;
}

/* Create the schedule that calls all the PE modules.
 * We delete the schedule nodes above the array mark and below the PE mark,
 * then insert the module call extension nodes right under the space bands.
 */
static isl_stat top_module_pe_gen_module_call(struct autosa_gen *gen,
  struct autosa_hw_top_module *top, struct autosa_hw_module *module)
{
  isl_schedule *schedule;
  isl_schedule_node *node, *graft;
  isl_id *id;
  struct autosa_kernel *kernel = gen->kernel;
  isl_space *space;
  isl_ctx *ctx;
  isl_union_set *domain;
  isl_union_set *empty_filter;
  isl_printer *p_str;
  char *stmt_name;

  schedule = gen->schedule;
  schedule = isl_schedule_dup(schedule);
  node = isl_schedule_get_root(schedule);
  isl_schedule_free(schedule);
  ctx = isl_schedule_node_get_ctx(node);

  /* Delete the node above the array mark. */
  node = autosa_tree_move_down_to_array(node, kernel->core);
  node = isl_schedule_node_parent(node);
  while (!autosa_tree_node_is_kernel(node)) {
    node = isl_schedule_node_delete(node);
    node = isl_schedule_node_parent(node);
  }

  /* Delete the node under the pe mark. */
  node = autosa_tree_move_down_to_array(node, kernel->core);
  node = isl_schedule_node_child(node, 0);
  node = split_band(node, kernel->n_sa_dim);

  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_cut(node);

  /* Graft an extension node. */
  p_str = isl_printer_to_str(ctx);
  p_str = isl_printer_print_str(p_str, "module_call.");
  p_str = isl_printer_print_str(p_str, module->name);
  stmt_name = isl_printer_get_str(p_str);
  isl_printer_free(p_str);
  space = isl_space_set_alloc(ctx, 0, 0);
  space = isl_space_set_tuple_name(space, isl_dim_set, stmt_name);
  free(stmt_name);
  domain = isl_union_set_from_set(isl_set_universe(space));
  graft = isl_schedule_node_from_domain(domain);

  node = isl_schedule_node_graft_before(node, graft);

  /* Insert an empty filter */
  empty_filter = isl_union_set_from_set(isl_set_empty(
                    isl_set_get_space(kernel->context)));
  node = isl_schedule_node_insert_filter(node, empty_filter);

  /* Add module mark after the kernel mark. */
  id = isl_id_alloc(ctx, "module", module);
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_mark(node, id);

  schedule = isl_schedule_node_get_schedule(node);
  isl_schedule_node_free(node);

  top->n_module_calls++;
  top->module_call_scheds = (isl_schedule **)realloc(top->module_call_scheds,
      top->n_module_calls * sizeof(isl_schedule *));
  top->module_call_scheds[top->n_module_calls - 1] = schedule;

  if (module->n_pe_dummy_modules > 0) {
    /* Generate dummy module calls. */
    for (int i = 0; i < module->n_pe_dummy_modules; i++) {
      struct autosa_pe_dummy_module *pe_dummy_module;
      isl_schedule *sched;

      pe_dummy_module = module->pe_dummy_modules[i];
      sched = pe_dummy_gen_module_call(gen, pe_dummy_module);

      top->n_module_calls++;
      top->module_call_scheds = (isl_schedule **)realloc(top->module_call_scheds,
          top->n_module_calls * sizeof(isl_schedule *));
      top->module_call_scheds[top->n_module_calls - 1] = sched;    
    }
  }

  return isl_stat_ok;
}

/* Generate the schedule that declares the fifos used in PEs. 
 * If the io group data transfer direciton at the PE level is INOUT,
 * we will add another extension node at the boundary of the transfer chain
 * to declare one more fifo.
 */
static isl_stat top_module_pe_gen_fifo_decl(struct autosa_gen *gen, 
  struct autosa_hw_top_module *top, struct autosa_hw_module *module)
{
  isl_schedule *schedule;
  isl_schedule_node *node, *graft;
  isl_id *id;
  struct autosa_kernel *kernel = gen->kernel;
  isl_space *space;
  isl_ctx *ctx = gen->ctx;
  isl_union_set *domain;
  isl_union_set *empty_filter;
  isl_printer *p_str;
  char *stmt_name;

  for (int i = 0; i < module->n_io_group; i++) {
    struct autosa_array_ref_group *group = module->io_groups[i];
    isl_multi_aff *io_trans;
    isl_mat *io_trans_mat;
    isl_id *id;
    isl_union_set *L1_filter = NULL;
    bool insert_L1 = isl_bool_false;

    schedule = isl_schedule_dup(group->io_L1_schedule); 
    node = isl_schedule_get_root(schedule);
    isl_schedule_free(schedule);

    /* Delete the node above the array mark. */
    node = autosa_tree_move_down_to_array(node, kernel->core);
    node = isl_schedule_node_parent(node);
    while (!autosa_tree_node_is_kernel(node)) {
      node = isl_schedule_node_delete(node);
      node = isl_schedule_node_parent(node);
    }

    if (group->pe_io_dir == IO_INOUT) {
      int n_member;
      node = autosa_tree_move_down_to_mark(node, kernel->core, "io_L1");
      node = isl_schedule_node_parent(node);
      n_member = isl_schedule_node_band_n_member(node);
      node = isl_schedule_node_band_split(node, n_member - 1);
      node = isl_schedule_node_child(node, 0);
      if (isl_schedule_node_get_type(node) == isl_schedule_node_band) {
        L1_filter = schedule_eq_ub(node);
        insert_L1 = isl_bool_true;
      }
      node = autosa_tree_move_up_to_array(node);
    }
    
    /* Delete the node under the pe mark. */
    node = autosa_tree_move_down_to_pe(node, kernel->core);
    node = isl_schedule_node_cut(node);

    /* Graft an extension node. */
    p_str = isl_printer_to_str(ctx);
    p_str = isl_printer_print_str(p_str, "fifo_decl.");
    p_str = autosa_array_ref_group_print_fifo_name(group, p_str);
    stmt_name = isl_printer_get_str(p_str);
    isl_printer_free(p_str);
    space = isl_space_set_alloc(ctx, 0, 0);
    id = isl_id_alloc(ctx, stmt_name, group);
    space = isl_space_set_tuple_id(space, isl_dim_set, id);
    free(stmt_name);
    domain = isl_union_set_from_set(isl_set_universe(space));
    graft = isl_schedule_node_from_domain(domain);

    node = isl_schedule_node_graft_before(node, graft);

    if (insert_L1) {
      isl_set *set;
      isl_multi_union_pw_aff *mupa;
      isl_union_map *prefix;
      isl_union_set *domain;
      isl_union_set *range;
      isl_union_map *extension;
      isl_map *map;
      isl_union_map *umap;

      /* Graft an extension node for boundary PE. */
      node = isl_schedule_node_insert_filter(node, L1_filter);
      node = isl_schedule_node_child(node, 0);
      prefix = isl_schedule_node_get_prefix_schedule_relation(node);
      prefix = isl_union_map_preimage_domain_union_pw_multi_aff(prefix,
                  isl_union_pw_multi_aff_copy(kernel->contraction));
      domain = isl_union_map_range(prefix); 

      p_str = isl_printer_to_str(ctx);
      p_str = isl_printer_print_str(p_str, "fifo_decl_boundary.");
      p_str = autosa_array_ref_group_print_fifo_name(group, p_str);
      stmt_name = isl_printer_get_str(p_str);
      isl_printer_free(p_str);
      space = isl_space_set_alloc(ctx, 0, 1);
      id = isl_id_alloc(ctx, stmt_name, group);
      space = isl_space_set_tuple_id(space, isl_dim_set, id);
      free(stmt_name);
  
      isl_point *pnt = isl_point_zero(space);
      set = isl_set_from_point(pnt); 
      range = isl_union_set_from_set(isl_set_copy(set));

      extension = isl_union_map_from_domain_and_range(domain, range); 
      graft = isl_schedule_node_from_extension(extension);

      map = isl_set_identity(set);
      map = isl_map_reset_tuple_id(map, isl_dim_out);
      umap = isl_union_map_from_map(map);
      mupa = isl_multi_union_pw_aff_from_union_map(umap); 

      graft = isl_schedule_node_child(graft, 0);
      graft = isl_schedule_node_insert_partial_schedule(graft, mupa);

      while (graft && isl_schedule_node_has_parent(graft)) 
        graft = isl_schedule_node_parent(graft);

      node = isl_schedule_node_graft_before(node, graft);     
    } else {
      isl_union_set_free(L1_filter);
    }

    /* Insert an empty filter. */
    empty_filter = isl_union_set_from_set(isl_set_empty(
                      isl_set_get_space(kernel->context)));
    node = isl_schedule_node_insert_filter(node, empty_filter);

    /* Add module mark after the kernel mark. */
    id = isl_id_alloc(ctx, "module", module);
    node = autosa_tree_move_up_to_kernel(node);
    node = isl_schedule_node_child(node, 0);
    node = isl_schedule_node_insert_mark(node, id);

    schedule = isl_schedule_node_get_schedule(node);
    isl_schedule_node_free(node);

    top->n_fifo_decls++;
    top->fifo_decl_scheds = (isl_schedule **)realloc(top->fifo_decl_scheds,
        top->n_fifo_decls * sizeof(isl_schedule *));
    top->fifo_decl_scheds[top->n_fifo_decls - 1] = schedule;
    top->fifo_decl_names = (char **)realloc(top->fifo_decl_names,
        top->n_fifo_decls * sizeof(char *));
    /* Generate fifo_decl name in the format of 
     * [fifo_name].[fifo_width] 
     */
    p_str = isl_printer_to_str(ctx);
    p_str = autosa_array_ref_group_print_fifo_name(group, p_str);
    p_str = isl_printer_print_str(p_str, "_");
    p_str = isl_printer_print_str(p_str, module->name);
    p_str = isl_printer_print_str(p_str, ".");
    int n_lane = get_io_group_n_lane(module, group);
    int data_size = group->array->size;
    int width = data_size * n_lane; // in bytes
    p_str = isl_printer_print_int(p_str, width);
    top->fifo_decl_names[top->n_fifo_decls - 1] = isl_printer_get_str(p_str);
    isl_printer_free(p_str);
  }

  return isl_stat_ok;
}

/* Generate module calls and fifo decls for the PE module. 
 */
static isl_stat top_module_pe_gen(struct autosa_gen *gen, 
  struct autosa_hw_top_module *top, struct autosa_hw_module *module)
{
  /* Generate the function call schedule. */
  top_module_pe_gen_module_call(gen, top, module);

  /* Generate the fifo declaration schedule. */
  top_module_pe_gen_fifo_decl(gen, top, module);

  return isl_stat_ok;
}

/* The input "node" points to the node below io_[module->level] mark.
 * Return the node points to the "kernel" mark.
 * We will insert two module call extension nodes: 
 * module_call_upper: which contains the module name and arguments for the 
 * inter-module transfer
 * module_call_lower: which contains arguments for the intra-module transfer
 * (i.e., transfer to the lower-level modules)
 */
static __isl_give isl_schedule_node *io_gen_module_call(
  __isl_take isl_schedule_node *node, struct autosa_hw_module *module,
  struct autosa_kernel *kernel, struct autosa_array_ref_group *group,
  int boundary)
{
  isl_printer *p_str;
  char *stmt_name;
  isl_space *space;
  isl_union_set *domain, *empty_filter, *lower_level_filter;
  isl_schedule_node *graft;
  isl_bool insert_lower = isl_bool_false;
  isl_ctx *ctx = isl_schedule_node_get_ctx(node);
  isl_id *id;
  isl_union_map *prefix, *extension, *umap;
  isl_union_set *range;
  isl_set *set;
  isl_map *map;
  isl_multi_union_pw_aff *mupa;

  /* Collect the filter for the lower I/O module. */
  if (isl_schedule_node_get_type(node) == isl_schedule_node_band) {
    if (module->level > 1) {
      lower_level_filter = schedule_eq_lb(node);
      insert_lower = isl_bool_true;
    }
  }
  
  /* Graft an extension node for module call. */
  prefix = isl_schedule_node_get_prefix_schedule_relation(node);
  prefix = isl_union_map_preimage_domain_union_pw_multi_aff(prefix,
      isl_union_pw_multi_aff_copy(kernel->contraction));
  domain = isl_union_map_range(prefix);

  p_str = isl_printer_to_str(ctx);
  p_str = isl_printer_print_str(p_str, "module_call_upper.");
  p_str = isl_printer_print_str(p_str, module->name);
  if (boundary)
    p_str = isl_printer_print_str(p_str, ".boundary");
  stmt_name = isl_printer_get_str(p_str);
  isl_printer_free(p_str);
  space = isl_space_set_alloc(ctx, 0, 0);
  space = isl_space_set_tuple_name(space, isl_dim_set, stmt_name);
  free(stmt_name);

  isl_point *pnt = isl_point_zero(space);
  set = isl_set_from_point(pnt);  
  range = isl_union_set_from_set(isl_set_copy(set));

  extension = isl_union_map_from_domain_and_range(domain, range);
  graft = isl_schedule_node_from_extension(extension);

  map = isl_set_identity(set);
  map = isl_map_reset_tuple_id(map, isl_dim_out);
  umap = isl_union_map_from_map(map);
  mupa = isl_multi_union_pw_aff_from_union_map(umap);

  graft = isl_schedule_node_child(graft, 0);
  graft = isl_schedule_node_insert_partial_schedule(graft, mupa);

  while (graft && isl_schedule_node_has_parent(graft))
    graft = isl_schedule_node_parent(graft);

  node = isl_schedule_node_graft_before(node, graft);

  if (module->level > 1) {
    node = autosa_tree_move_down_to_io_mark(node, kernel->core, module->level - 1);
  }
  node = isl_schedule_node_cut(node);

  /* Graft an extension node for lower level transfer. */
  if (insert_lower) {
    node = isl_schedule_node_insert_filter(node, lower_level_filter);
    node = isl_schedule_node_child(node, 0);
  }
  {
    isl_union_map *prefix;
    isl_union_set *domain, *range;
    isl_point *pnt;
    isl_set *set;
    isl_union_map *extension, *umap;
    isl_map *map;
    isl_multi_union_pw_aff *mupa;

    prefix = isl_schedule_node_get_prefix_schedule_relation(node);
    prefix = isl_union_map_preimage_domain_union_pw_multi_aff(prefix,
                isl_union_pw_multi_aff_copy(kernel->contraction));
    domain = isl_union_map_range(prefix);  
    
    p_str = isl_printer_to_str(ctx);
    p_str = isl_printer_print_str(p_str, "module_call_lower.");
    p_str = isl_printer_print_str(p_str, module->name);
    if (boundary)
      p_str = isl_printer_print_str(p_str, ".boundary");

    stmt_name = isl_printer_get_str(p_str);
    isl_printer_free(p_str);
    space = isl_space_set_alloc(ctx, 0, 0);
    id = isl_id_alloc(ctx, stmt_name, group);
    space = isl_space_set_tuple_id(space, isl_dim_set, id);
    free(stmt_name);

    pnt = isl_point_zero(space);
    set = isl_set_from_point(pnt);
    range = isl_union_set_from_set(isl_set_copy(set));

    extension = isl_union_map_from_domain_and_range(domain, range);
    graft = isl_schedule_node_from_extension(extension);

    map = isl_set_identity(set);
    map = isl_map_reset_tuple_id(map, isl_dim_out);
    umap = isl_union_map_from_map(map);
    mupa = isl_multi_union_pw_aff_from_union_map(umap);

    graft = isl_schedule_node_child(graft, 0);
    graft = isl_schedule_node_insert_partial_schedule(graft, mupa);

    while (graft && isl_schedule_node_has_parent(graft))
      graft = isl_schedule_node_parent(graft);

    node = isl_schedule_node_graft_after(node, graft);
  }

  /* Insert an empty filter. */
  empty_filter = isl_union_set_from_set(isl_set_empty(isl_set_get_space(kernel->context)));
  node = isl_schedule_node_insert_filter(node, empty_filter);

  node = autosa_tree_move_up_to_kernel(node);

  return node;
}

/* Generate the module calls for the io module. */
static isl_stat top_module_io_gen_module_call(
  struct autosa_gen *gen, struct autosa_hw_top_module *top,
  struct autosa_hw_module *module,
  struct autosa_array_ref_group *group) 
{
  isl_schedule *schedule;
  isl_ctx *ctx = gen->ctx;
  isl_schedule_node *node, *graft;
  isl_id *id;
  struct autosa_kernel *kernel = gen->kernel;
  isl_printer *p_str;
  char *stmt_name;
  isl_space *space;
  isl_union_set *domain, *empty_filter, *lower_level_filter;
  isl_bool insert_lower = isl_bool_false;
  int boundary = module->boundary;
  isl_union_set *boundary_filter, *non_boundary_filter;
  isl_union_set_list *boundary_filters;

  /* Transform the schedule. */
  schedule = isl_schedule_dup(group->io_schedule);
  node = isl_schedule_get_root(schedule);
  isl_schedule_free(schedule);

  /* Delete the node above the array mark. */
  node = autosa_tree_move_down_to_array(node, kernel->core);
  node = isl_schedule_node_parent(node);
  while (!autosa_tree_node_is_kernel(node)) {
    node = isl_schedule_node_delete(node);
    node = isl_schedule_node_parent(node);
  }

  /* Collect the filter for the boundary and non-boundary I/O module. */
  if (boundary) {
    node = autosa_tree_move_down_to_io_mark(node, kernel->core, module->level);
    node = isl_schedule_node_parent(node);
    if (isl_schedule_node_get_type(node) == isl_schedule_node_band) {
      boundary_filter = schedule_eq_ub(node);
      non_boundary_filter = schedule_neq_ub(node);
      boundary_filters = isl_union_set_list_from_union_set(non_boundary_filter);
      boundary_filters = isl_union_set_list_add(boundary_filters, boundary_filter);

      node = isl_schedule_node_child(node, 0); // io_mark
      node = isl_schedule_node_child(node, 0); // band
      node = isl_schedule_node_insert_sequence(node, boundary_filters);
      /* The node now is right below the io_[module->level] mark. */
    }
  } else {
    node = autosa_tree_move_down_to_io_mark(node, kernel->core, module->level);
    node = isl_schedule_node_child(node, 0);
  }

  if (boundary) {
    node = isl_schedule_node_child(node, 0); // filter
    node = isl_schedule_node_child(node, 0); // band
    /* non-boundary */
    node = io_gen_module_call(node, module, kernel, group, 0);
    node = autosa_tree_move_down_to_io_mark(node, kernel->core, module->level);
    node = isl_schedule_node_child(node, 0); // sequence
    node = isl_schedule_node_child(node, 1); // filter
    node = isl_schedule_node_child(node, 0); // band
    /* boundary */
    node = io_gen_module_call(node, module, kernel, group, 1);
  } else {
    node = io_gen_module_call(node, module, kernel, group, 0);
  }

  /* Add module mark after the kernel mark.auto */
  id = isl_id_alloc(ctx, "module", module);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_mark(node, id);

  schedule = isl_schedule_node_get_schedule(node);
  isl_schedule_node_free(node);

  top->n_module_calls++;
  top->module_call_scheds = (isl_schedule **)realloc(top->module_call_scheds,
      top->n_module_calls * sizeof(isl_schedule *));
  top->module_call_scheds[top->n_module_calls - 1] = schedule;

  return isl_stat_ok;
}

/* Generate fifo decls for the I/O module.
 * Currently only works for filter I/O modules.
 */
static isl_stat top_module_io_gen_fifo_decl(struct autosa_gen *gen,
  struct autosa_hw_top_module *top,
  struct autosa_hw_module *module, struct autosa_array_ref_group *group)
{
  isl_schedule *schedule;
  isl_schedule_node *node, *graft;
  isl_union_set *filter = NULL, *empty_filter;
  struct autosa_kernel *kernel = gen->kernel;
  bool insert_filter = isl_bool_false;
  char *stmt_name;
  isl_space *space;
  isl_union_set *domain;
  isl_printer *p_str;
  isl_id *id;
  isl_ctx *ctx = gen->ctx;

  if (module->to_mem) 
    return isl_stat_ok;

  schedule = isl_schedule_dup(group->io_schedule);
  node = isl_schedule_get_root(schedule);
  isl_schedule_free(schedule);

  /* Delete the node above the array mark. */
  node = autosa_tree_move_down_to_array(node, kernel->core);
  node = isl_schedule_node_parent(node);
  while (!autosa_tree_node_is_kernel(node)) {
    node = isl_schedule_node_delete(node);
    node = isl_schedule_node_parent(node);
  }
 
  node = autosa_tree_move_down_to_io_mark(node, kernel->core, module->level);
  node = isl_schedule_node_parent(node);  
  if (isl_schedule_node_get_type(node) == isl_schedule_node_band) {
    filter = schedule_eq_ub(node);
    insert_filter = isl_bool_true;
  }
  node = autosa_tree_move_up_to_array(node);
  node = autosa_tree_move_down_to_io_mark(node, kernel->core, module->level);
  node = isl_schedule_node_cut(node);

  /* Graft an extension node. */
  p_str = isl_printer_to_str(ctx);
  p_str = isl_printer_print_str(p_str, "fifo_decl.");
  p_str = autosa_array_ref_group_print_fifo_name(group, p_str);
  stmt_name = isl_printer_get_str(p_str);
  isl_printer_free(p_str);
  space = isl_space_set_alloc(ctx, 0, 0);
  id = isl_id_alloc(ctx, stmt_name, group);
  space = isl_space_set_tuple_id(space, isl_dim_set, id);
  free(stmt_name);
  domain = isl_union_set_from_set(isl_set_universe(space));
  graft = isl_schedule_node_from_domain(domain);

  node = isl_schedule_node_graft_before(node, graft);

  if (insert_filter) {
    isl_union_map *prefix, *extension, *umap;
    isl_union_set *domain, *range;
    isl_point *pnt;
    isl_set *set;
    isl_map *map;
    isl_multi_union_pw_aff *mupa;

    node = isl_schedule_node_insert_filter(node, filter);
    node = isl_schedule_node_child(node, 0);
    
    prefix = isl_schedule_node_get_prefix_schedule_relation(node);
    prefix = isl_union_map_preimage_domain_union_pw_multi_aff(prefix,
                isl_union_pw_multi_aff_copy(kernel->contraction));
    domain = isl_union_map_range(prefix);

    p_str = isl_printer_to_str(ctx);
    p_str = isl_printer_print_str(p_str, "fifo_decl_boundary.");
    p_str = autosa_array_ref_group_print_fifo_name(group, p_str);
    stmt_name = isl_printer_get_str(p_str);
    isl_printer_free(p_str);
    space = isl_space_set_alloc(ctx, 0, 1);
    id = isl_id_alloc(ctx, stmt_name, group);
    space = isl_space_set_tuple_id(space, isl_dim_set, id);
    free(stmt_name);

    pnt = isl_point_zero(space);
    set = isl_set_from_point(pnt);
    range = isl_union_set_from_set(isl_set_copy(set));

    extension = isl_union_map_from_domain_and_range(domain, range);
    graft = isl_schedule_node_from_extension(extension);
    map = isl_set_identity(set);
    map = isl_map_reset_tuple_id(map, isl_dim_out);
    umap = isl_union_map_from_map(map);
    mupa = isl_multi_union_pw_aff_from_union_map(umap);

    graft = isl_schedule_node_child(graft, 0);
    graft = isl_schedule_node_insert_partial_schedule(graft, mupa);

    while (graft && isl_schedule_node_has_parent(graft))
      graft = isl_schedule_node_parent(graft);

    node = isl_schedule_node_graft_before(node, graft);
  }

  /* Insert an empty filter. */
  empty_filter = isl_union_set_from_set(isl_set_empty(
                    isl_set_get_space(kernel->context)));
  node = isl_schedule_node_insert_filter(node, empty_filter);

  /* Add module mark after the kernel mark. */
  id = isl_id_alloc(ctx, "module", module);
  node = autosa_tree_move_up_to_kernel(node);
  node = isl_schedule_node_child(node, 0);
  node = isl_schedule_node_insert_mark(node, id);

  schedule = isl_schedule_node_get_schedule(node);
  isl_schedule_node_free(node);

  top->n_fifo_decls++;
  top->fifo_decl_scheds = (isl_schedule **)realloc(top->fifo_decl_scheds,
      top->n_fifo_decls * sizeof(isl_schedule *));
  top->fifo_decl_scheds[top->n_fifo_decls - 1] = schedule;
  top->fifo_decl_names = (char **)realloc(top->fifo_decl_names,
      top->n_fifo_decls * sizeof(char *));
  /* Generate fifo_decl name in the format of
   * [fifo_name].[fifo_width]
   */
  p_str = isl_printer_to_str(ctx);
  p_str = autosa_array_ref_group_print_fifo_name(group, p_str);
  p_str = isl_printer_print_str(p_str, "_");
  p_str = isl_printer_print_str(p_str, module->name);
  p_str = isl_printer_print_str(p_str, ".");
  int n_lane = get_io_group_n_lane(module, group);
  int data_size = group->array->size;
  int width = data_size * n_lane; // in bytes
  p_str = isl_printer_print_int(p_str, width);
  top->fifo_decl_names[top->n_fifo_decls - 1] = isl_printer_get_str(p_str);
  isl_printer_free(p_str);

  return isl_stat_ok; 
}

/* Generate the module calls and fifo decls for the io group. */
static isl_stat top_module_io_gen(struct autosa_gen *gen, 
  struct autosa_hw_top_module *top,
  struct autosa_hw_module *module)
{
  struct autosa_array_ref_group *group;
  assert(module->n_io_group == 1);
  group = module->io_groups[0];

  /* Generate the function call schedule. */
  top_module_io_gen_module_call(gen, top, module, group);

  /* Generate the fifo declaration schedule. */
  top_module_io_gen_fifo_decl(gen, top, module, group);

  return isl_stat_ok;
}

/* Generate the top module that contains module calls and fifo declarations. */
__isl_give struct autosa_hw_top_module *sa_top_module_gen(struct autosa_gen *gen)
{
  struct autosa_hw_top_module *top_module;

  top_module = autosa_hw_top_module_alloc();
  top_module->hw_modules = gen->hw_modules;
  top_module->kernel = gen->kernel;
  top_module->n_hw_modules = gen->n_hw_modules;

  for (int i = 0; i < gen->n_hw_modules; i++) {
    struct autosa_hw_module *module = gen->hw_modules[i];
    if (module->type == PE_MODULE) {      
      top_module_pe_gen(gen, top_module, gen->hw_modules[i]);
    } else {
      top_module_io_gen(gen, top_module, gen->hw_modules[i]);
    }
  }

  return top_module;
}

/* Build new schedules for each hardware components.
 * The total number of schedules = 
 * [1. the default schedule (CPU code)]
 * 2. PE schedule
 * 3. I/O module schedule
 * 4. drain module schedule
 * 5. top module schedule
 */
void generate_hw_modules(__isl_take isl_schedule *schedule,
  struct autosa_gen *gen, struct autosa_kernel *kernel)
{  
  gen->schedule = schedule;
  gen->n_hw_modules = 1;
  gen->hw_modules = isl_calloc_array(gen->ctx, 
    struct autosa_hw_module *, gen->n_hw_modules);
  gen->hw_modules[0] = NULL;
  /* IO module */
  for (int i = 0; i < kernel->n_array; i++) {
    struct autosa_local_array_info *info = &kernel->array[i];
    info->n_io_group_refs = 0;
    for (int j = 0; j < info->n_io_group; j++) {
      int n_hw_modules = 0;
      struct autosa_hw_module **hw_modules;
      hw_modules = sa_io_module_gen(info->io_groups[j], gen, &n_hw_modules, 1, 1);
      
      gen->hw_modules = (struct autosa_hw_module **)realloc(gen->hw_modules, 
          (gen->n_hw_modules + n_hw_modules) * sizeof(struct polysa_hw_module *));
      for (int k = 0; k < n_hw_modules; k++) {
        gen->hw_modules[gen->n_hw_modules + k] = hw_modules[k];
      }
      gen->n_hw_modules += n_hw_modules;
      if (hw_modules)
        free(hw_modules);
    }
  }
  /* Drain module */
  for (int i = 0; i < kernel->n_array; i++) {
    struct autosa_local_array_info *info = &kernel->array[i];
    if (!info->drain_group)
      continue;
    int n_hw_modules = 0;
    struct autosa_hw_module **hw_modules;
    hw_modules = sa_io_module_gen(info->drain_group, gen, &n_hw_modules, 0, 1);

    if (n_hw_modules > 0) {
      gen->hw_modules = (struct autosa_hw_module **)realloc(gen->hw_modules, 
          (gen->n_hw_modules + n_hw_modules) * sizeof(struct polysa_hw_module *));
      for (int j = 0; j < n_hw_modules; j++) {
        gen->hw_modules[gen->n_hw_modules + j] = hw_modules[j];
      }
      gen->n_hw_modules += n_hw_modules;
    }
    if (hw_modules)
      free(hw_modules);
  }
  /* PE module */
  gen->hw_modules[0] = sa_pe_module_gen(gen); 

  /* Reorder the sequence of the modules. */
  gen->hw_modules = hw_module_reorder(gen->hw_modules, gen->n_hw_modules); 

  /* top module */
  struct autosa_hw_top_module *top_module = sa_top_module_gen(gen); 
  gen->hw_top_module = top_module;
}

/* Replace any reference to an array element in the range of "copy"
 * by a reference to all array elements (defined by the extent of the array).
 */
static __isl_give isl_union_map *approximate_copy_out(
	__isl_take isl_union_map *copy, struct autosa_prog *prog)
{
	int i;
	isl_union_map *res;

	res = isl_union_map_empty(isl_union_map_get_space(copy));

	for (i = 0; i < prog->n_array; ++i) {
		isl_space *space;
		isl_set *set;
		isl_union_map *copy_i;
		isl_union_set *extent, *domain;

		space = isl_space_copy(prog->array[i].space);
		extent = isl_union_set_from_set(isl_set_universe(space));
		copy_i = isl_union_map_copy(copy);
		copy_i = isl_union_map_intersect_range(copy_i, extent);
		set = isl_set_copy(prog->array[i].extent);
		extent = isl_union_set_from_set(set);
		domain = isl_union_map_domain(copy_i);
		copy_i = isl_union_map_from_domain_and_range(domain, extent);
		res = isl_union_map_union(res, copy_i);
	}

	isl_union_map_free(copy);

	return res;
}

/* Internal data structure for node_may_persist.
 *
 * "tagger" maps tagged iteration domains to the corresponding untagged
 *	iteration domain.
 *
 * "may_persist_flow" is the set of all tagged dataflow dependences
 * with those dependences removed that either precede or follow
 * the kernel launch in a sequence.
 * "inner_band_flow" is the set of all tagged dataflow dependences
 * that are local to a given iteration of the outer band nodes
 * with respect to the current node.
 * "local_flow" is equal to "inner_band_flow", except that the domain
 * and the range have been intersected with intermediate filters
 * on children of sets or sequences.
 */
struct ppcg_may_persist_data {
	isl_union_pw_multi_aff *tagger;

	isl_union_map *local_flow;
	isl_union_map *inner_band_flow;
	isl_union_map *may_persist_flow;
};

/* Update the information in "data" based on the band ancestor "node".
 *
 * In particular, we restrict the dependences in data->local_flow
 * to those dependence where the source and the sink occur in
 * the same iteration of the given band node.
 * We also update data->inner_band_flow to the new value of
 * data->local_flow.
 */
static int update_may_persist_at_band(__isl_keep isl_schedule_node *node,
	struct ppcg_may_persist_data *data)
{
	isl_multi_union_pw_aff *partial;
	isl_union_pw_multi_aff *contraction;
	isl_union_map *flow;

	if (isl_schedule_node_band_n_member(node) == 0)
		return 0;

	partial = isl_schedule_node_band_get_partial_schedule(node);
	contraction = isl_schedule_node_get_subtree_contraction(node);
	partial = isl_multi_union_pw_aff_pullback_union_pw_multi_aff(partial,
								contraction);
	partial = isl_multi_union_pw_aff_pullback_union_pw_multi_aff(partial,
				isl_union_pw_multi_aff_copy(data->tagger));

	flow = data->local_flow;
	flow = isl_union_map_eq_at_multi_union_pw_aff(flow, partial);
	data->local_flow = flow;

	isl_union_map_free(data->inner_band_flow);
	data->inner_band_flow = isl_union_map_copy(data->local_flow);

	return 0;
}

/* Given a set of local reaching domain elements "domain",
 * expand them to the corresponding leaf domain elements using "contraction"
 * and insert the array references tags using data->tagger.
 */
static __isl_give isl_union_set *expand_and_tag(
	__isl_take isl_union_set *domain,
	__isl_take isl_union_pw_multi_aff *contraction,
	struct ppcg_may_persist_data *data)
{
	domain = isl_union_set_preimage_union_pw_multi_aff(domain,
			    contraction);
	domain = isl_union_set_preimage_union_pw_multi_aff(domain,
			    isl_union_pw_multi_aff_copy(data->tagger));
	return domain;
}

/* Given a filter node that is the child of a set or sequence node,
 * restrict data->local_flow to refer only to those elements
 * in the filter of the node.
 * "contraction" maps the leaf domain elements of the schedule tree
 * to the corresponding domain elements at (the parent of) "node".
 */
static int filter_flow(__isl_keep isl_schedule_node *node,
	struct ppcg_may_persist_data *data,
	__isl_take isl_union_pw_multi_aff *contraction)
{
	isl_union_set *filter;
	isl_union_map *flow;

	flow = data->local_flow;
	filter = isl_schedule_node_filter_get_filter(node);
	filter = expand_and_tag(filter, contraction, data);
	flow = isl_union_map_intersect_domain(flow, isl_union_set_copy(filter));
	flow = isl_union_map_intersect_range(flow, filter);
	data->local_flow = flow;

	return 0;
}

/* Given a filter node "node", collect the filters on all preceding siblings
 * (which are also filter nodes), add them to "filters" and return the result.
 */
static __isl_give isl_union_set *add_previous_filters(
	__isl_take isl_union_set *filters, __isl_keep isl_schedule_node *node)
{
	isl_schedule_node *sibling;

	sibling = isl_schedule_node_copy(node);
	while (sibling && isl_schedule_node_has_previous_sibling(sibling)) {
		isl_union_set *filter;

		sibling = isl_schedule_node_previous_sibling(sibling);
		filter = isl_schedule_node_filter_get_filter(sibling);
		filters = isl_union_set_union(filters, filter);
	}
	isl_schedule_node_free(sibling);
	if (!sibling)
		return isl_union_set_free(filters);

	return filters;
}

/* Given a filter node "node", collect the filters on all following siblings
 * (which are also filter nodes), add them to "filters" and return the result.
 */
static __isl_give isl_union_set *add_next_filters(
	__isl_take isl_union_set *filters, __isl_keep isl_schedule_node *node)
{
	isl_schedule_node *sibling;

	sibling = isl_schedule_node_copy(node);
	while (sibling && isl_schedule_node_has_next_sibling(sibling)) {
		isl_union_set *filter;

		sibling = isl_schedule_node_next_sibling(sibling);
		filter = isl_schedule_node_filter_get_filter(sibling);
		filters = isl_union_set_union(filters, filter);
	}
	isl_schedule_node_free(sibling);
	if (!sibling)
		return isl_union_set_free(filters);

	return filters;
}

/* Remove those flow dependences from data->may_persist_flow
 * that flow between elements of "domain" within the same iteration
 * of all outer band nodes.
 * "contraction" maps the leaf domain elements of the schedule tree
 * to the corresponding elements "domain".
 */
static void remove_external_flow(struct ppcg_may_persist_data *data,
	__isl_take isl_union_set *domain,
	__isl_keep isl_union_pw_multi_aff *contraction)
{
	isl_union_map *flow;

	contraction = isl_union_pw_multi_aff_copy(contraction);
	domain = expand_and_tag(domain, contraction, data);
	flow = isl_union_map_copy(data->local_flow);
	flow = isl_union_map_intersect_domain(flow, isl_union_set_copy(domain));
	flow = isl_union_map_intersect_range(flow, domain);

	data->may_persist_flow = isl_union_map_subtract(data->may_persist_flow,
							flow);
}

/* Update the information in "data" based on the filter ancestor "node".
 * We only need to modify anything if the filter is the child
 * of a set or sequence node.
 *
 * In the case of a sequence, we remove the dependences between
 * statement instances that are both executed either before or
 * after the subtree that will be mapped to a kernel, within
 * the same iteration of outer bands.
 *
 * In both cases, we restrict data->local_flow to the current child.
 */
static int update_may_persist_at_filter(__isl_keep isl_schedule_node *node,
	struct ppcg_may_persist_data *data)
{
	enum isl_schedule_node_type type;
	isl_schedule_node *parent;
	isl_space *space;
	isl_union_pw_multi_aff *contraction;
	isl_union_set *before, *after, *filter;

	type = isl_schedule_node_get_parent_type(node);
	if (type != isl_schedule_node_sequence && type != isl_schedule_node_set)
		return 0;

	parent = isl_schedule_node_copy(node);
	parent = isl_schedule_node_parent(parent);
	contraction = isl_schedule_node_get_subtree_contraction(parent);
	isl_schedule_node_free(parent);

	if (type == isl_schedule_node_set)
		return filter_flow(node, data, contraction);

	filter = isl_schedule_node_filter_get_filter(node);
	space = isl_union_set_get_space(filter);
	isl_union_set_free(filter);
	before = isl_union_set_empty(space);
	after = isl_union_set_copy(before);
	before = add_previous_filters(before, node);
	after = add_next_filters(after, node);

	remove_external_flow(data, before, contraction);
	remove_external_flow(data, after, contraction);

	return filter_flow(node, data, contraction);
}

/* Update the information in "data" based on the ancestor "node".
 */
static isl_stat update_may_persist_at(__isl_keep isl_schedule_node *node,
	void *user)
{
	struct ppcg_may_persist_data *data = (struct ppcg_may_persist_data *)user;

	switch (isl_schedule_node_get_type(node)) {
	case isl_schedule_node_error:
		return isl_stat_error;
	case isl_schedule_node_context:
	case isl_schedule_node_domain:
	case isl_schedule_node_expansion:
	case isl_schedule_node_extension:
	case isl_schedule_node_guard:
	case isl_schedule_node_leaf:
	case isl_schedule_node_mark:
	case isl_schedule_node_sequence:
	case isl_schedule_node_set:
		break;
	case isl_schedule_node_band:
		if (update_may_persist_at_band(node, data) < 0)
			return isl_stat_error;
		break;
	case isl_schedule_node_filter:
		if (update_may_persist_at_filter(node, data) < 0)
			return isl_stat_error;
		break;
	}

	return isl_stat_ok;
}

/* Determine the set of array elements that may need to be perserved
 * by a kernel constructed from the subtree at "node".
 * This includes the set of array elements that may need to be preserved
 * by the entire scop (prog->may_persist) and the elements for which
 * there is a potential flow dependence that may cross a kernel launch.
 *
 * To determine the second set, we start from all flow dependences.
 * From this set of dependences, we remove those that cannot possibly
 * require data to be preserved by a kernel launch.
 * In particular, we consider the following sets of dependences.
 * - dependences of which the write occurs inside the kernel.
 *   If the data is needed outside the kernel, then it will
 *   be copied out immediately after the kernel launch, so there
 *   is no need for any special care.
 * - dependences of which the read occurs inside the kernel and the
 *   corresponding write occurs inside the same iteration of the
 *   outer band nodes.  This means that the data is needed in
 *   the first kernel launch after the write, which is already
 *   taken care of by the standard copy-in.  That is, the data
 *   do not need to be preserved by any intermediate call to
 *   the same kernel.
 * - dependences of which the write and the read either both occur
 *   before the kernel launch or both occur after the kernel launch,
 *   within the same iteration of the outer band nodes with respect
 *   to the sequence that determines the ordering of the dependence
 *   and the kernel launch.  Such flow dependences cannot cross
 *   any kernel launch.
 *
 * For the remaining (tagged) dependences, we take the domain
 * (i.e., the tagged writes) and apply the tagged access relation
 * to obtain the accessed data elements.
 * These are then combined with the elements that may need to be
 * preserved by the entire scop.
 */
static __isl_give isl_union_set *node_may_persist(
	__isl_keep isl_schedule_node *node, struct autosa_prog *prog)
{
	struct ppcg_may_persist_data data;
	isl_union_pw_multi_aff *contraction;
	isl_union_set *domain;
	isl_union_set *persist;
	isl_union_map *flow, *local_flow;

	data.tagger = prog->scop->tagger;

	flow = isl_union_map_copy(prog->scop->tagged_dep_flow);
	data.local_flow = isl_union_map_copy(flow);
	data.inner_band_flow = isl_union_map_copy(flow);
	data.may_persist_flow = flow;
	if (isl_schedule_node_foreach_ancestor_top_down(node,
					&update_may_persist_at, &data) < 0) 
		data.may_persist_flow =
				    isl_union_map_free(data.may_persist_flow);
	flow = data.may_persist_flow;
	isl_union_map_free(data.local_flow);

	domain = isl_schedule_node_get_domain(node);
	contraction = isl_schedule_node_get_subtree_contraction(node);
	domain = isl_union_set_preimage_union_pw_multi_aff(domain,
				    contraction);
	domain = isl_union_set_preimage_union_pw_multi_aff(domain,
				    isl_union_pw_multi_aff_copy(data.tagger));
  /* Substract the case 1. */ 
	flow = isl_union_map_subtract_domain(flow, isl_union_set_copy(domain)); 
	local_flow = data.inner_band_flow;
	local_flow = isl_union_map_intersect_range(local_flow, domain);
  /* Substract the case 2. */
	flow = isl_union_map_subtract(flow, local_flow);

	persist = isl_union_map_domain(flow);
	persist = isl_union_set_apply(persist,
			isl_union_map_copy(prog->scop->tagged_may_writes));
	persist = isl_union_set_union(persist,
			isl_union_set_copy(prog->may_persist));

	return persist;
}

/* Return (the universe spaces of) the arrays that are declared
 * inside the scop corresponding to "prog" and for which all
 * potential writes inside the scop form a subset of "domain".
 */
static __isl_give isl_union_set *extract_local_accesses(struct autosa_prog *prog,
	__isl_keep isl_union_set *domain)
{
	int i;
	isl_union_set *local;

	local = isl_union_set_empty(isl_union_set_get_space(domain));

	for (i = 0; i < prog->n_array; ++i) {
		isl_set *set;
		isl_union_map *to_outer;
		isl_union_map *may_write;
		isl_union_set *write_domain;
		isl_union_set *fields;
		int subset;

		if (!prog->array[i].local)
			continue;

		set = isl_set_universe(isl_space_copy(prog->array[i].space));
		to_outer = isl_union_map_copy(prog->to_outer);
		to_outer = isl_union_map_intersect_range(to_outer,
				    isl_union_set_from_set(isl_set_copy(set)));
		fields = isl_union_map_domain(to_outer);
		may_write = isl_union_map_copy(prog->may_write);
		may_write = isl_union_map_intersect_range(may_write, fields);
		write_domain = isl_union_map_domain(may_write);
		subset = isl_union_set_is_subset(write_domain, domain);
		isl_union_set_free(write_domain);

		if (subset < 0) {
			isl_set_free(set);
			return isl_union_set_free(local);
		} else if (subset) {
			local = isl_union_set_add_set(local, set);
		} else {
			isl_set_free(set);
		}
	}

	return local;
}

/* For each array in "prog" of which an element appears in "accessed" and
 * that is not a read only scalar, create a zero-dimensional universe set
 * of which the tuple id has name "<prefix>_<name of array>" and a user
 * pointer pointing to the array (autosa_array_info).
 *
 * If the array is local to "prog", then make sure it will be declared
 * in the host code.
 *
 * Return the list of these universe sets.
 */
static __isl_give isl_union_set_list *create_copy_filters(struct autosa_prog *prog,
	const char *prefix, __isl_take isl_union_set *accessed)
{
	int i;
	isl_ctx *ctx;
	isl_union_set_list *filters;

	ctx = prog->ctx;
	filters = isl_union_set_list_alloc(ctx, 0);
	for (i = 0; i < prog->n_array; ++i) {
		struct autosa_array_info *array = &prog->array[i];
		isl_space *space;
		isl_set *accessed_i;
		int empty;
		char *name;
		isl_id *id;
		isl_union_set *uset;

		if (autosa_array_is_read_only_scalar(array))
			continue;

		space = isl_space_copy(array->space);
		accessed_i = isl_union_set_extract_set(accessed, space);
		empty = isl_set_plain_is_empty(accessed_i);
		isl_set_free(accessed_i);
		if (empty < 0) {
			filters = isl_union_set_list_free(filters);
			break;
		}
		if (empty)
			continue;

		array->global = 1;
		if (array->local)
			array->declare_local = 1;

		name = concat(ctx, prefix, array->name);
		id = name ? isl_id_alloc(ctx, name, array) : NULL;
		free(name);
		space = isl_space_set_alloc(ctx, 0, 0);
		space = isl_space_set_tuple_id(space, isl_dim_set, id);
		uset = isl_union_set_from_set(isl_set_universe(space));

		filters = isl_union_set_list_add(filters, uset);
	}
	isl_union_set_free(accessed);

	return filters;
}

/* Return the set of parameter values for which the array has a positive
 * size in all dimensions.
 * If the sizes are only valid for some parameter values, then those
 * constraints are also taken into account.
 */
__isl_give isl_set *autosa_array_positive_size_guard(struct autosa_array_info *array)
{
	int i;
	isl_space *space;
	isl_set *guard;

	if (!array)
		return NULL;

	space = isl_space_params(isl_space_copy(array->space));
	guard = isl_set_universe(space);

	for (i = 0; i < array->n_index; ++i) {
		isl_pw_aff *bound;
		isl_set *guard_i, *zero;

		bound = isl_multi_pw_aff_get_pw_aff(array->bound, i);
		guard_i = isl_pw_aff_nonneg_set(isl_pw_aff_copy(bound));
		zero = isl_pw_aff_zero_set(bound);
		guard_i = isl_set_subtract(guard_i, zero);
		guard = isl_set_intersect(guard, guard_i);
	}

	return guard;
}

/* Make sure that code for the statements in "filters" that
 * copy arrays to or from the device is only generated when
 * the size of the corresponding array is positive.
 * That is, add a set node underneath "graft" with "filters" as children
 * and for each child add a guard that the selects the parameter
 * values for which the corresponding array has a positive size.
 * The array is available in the user pointer of the statement identifier.
 * "depth" is the schedule depth of the position where "graft"
 * will be added.
 */
static __isl_give isl_schedule_node *insert_positive_size_guards(
	__isl_take isl_schedule_node *graft,
	__isl_take isl_union_set_list *filters, int depth)
{
	int i, n;

	graft = isl_schedule_node_child(graft, 0);
	graft = isl_schedule_node_insert_set(graft, filters);
	n = isl_schedule_node_n_children(graft);
	for (i = 0; i < n; ++i) {
		isl_union_set *filter;
		isl_set *domain, *guard;
		isl_id *id;
		struct autosa_array_info *array;

		graft = isl_schedule_node_child(graft, i);
		filter = isl_schedule_node_filter_get_filter(graft);
		domain = isl_set_from_union_set(filter);
		id = isl_set_get_tuple_id(domain);
		array = (struct autosa_array_info *)isl_id_get_user(id);
		isl_id_free(id);
		isl_set_free(domain);
		guard = autosa_array_positive_size_guard(array);
		guard = isl_set_from_params(guard);
		guard = isl_set_add_dims(guard, isl_dim_set, depth);
		graft = isl_schedule_node_child(graft, 0);
		graft = isl_schedule_node_insert_guard(graft, guard);
		graft = isl_schedule_node_parent(graft);
		graft = isl_schedule_node_parent(graft);
	}
	graft = isl_schedule_node_parent(graft);

	return graft;
}

/* Create a graft for copying arrays to or from the device,
 * whenever the size of the array is strictly positive.
 * Each statement is called "<prefix>_<name of array>" and
 * the identifier has a user pointer pointing to the array.
 * The graft will be added at the position specified by "node".
 * "copy" contains the array elements that need to be copied.
 * Only arrays of which some elements need to be copied
 * will have a corresponding statement in the graph.
 * Note though that each such statement will copy the entire array.
 */
static __isl_give isl_schedule_node *create_copy_device(struct autosa_prog *prog,
	__isl_keep isl_schedule_node *node, const char *prefix,
	__isl_take isl_union_set *copy)
{
	int depth;
	isl_ctx *ctx;
	isl_space *space;
	isl_union_set *all, *domain;
	isl_union_set_list *filters;
	isl_union_map *extension;
	isl_schedule_node *graft;

	ctx = prog->ctx;
	depth = isl_schedule_node_get_schedule_depth(node);
	filters = create_copy_filters(prog, prefix, copy);
	all = isl_union_set_list_union(isl_union_set_list_copy(filters));

	space = depth < 0 ? NULL : isl_space_set_alloc(ctx, 0, depth);
	domain = isl_union_set_from_set(isl_set_universe(space));
	extension = isl_union_map_from_domain_and_range(domain, all);
	graft = isl_schedule_node_from_extension(extension);

	if (!filters)
		return isl_schedule_node_free(graft);
	if (isl_union_set_list_n_union_set(filters) == 0) {
		isl_union_set_list_free(filters);
		return graft;
	}

	return insert_positive_size_guards(graft, filters, depth);
}

/* Add nodes for copying outer arrays in and out of the device
 * before and after the subtree "node", which contains one or more kernels.
 * "domain" contains the original statement instances, i.e.,
 * those that correspond to the domains of the access relations in "prog".
 * In particular, the domain has not been contracted in any way.
 * "prefix" contains the prefix schedule at that point, in terms
 * of the same original statement instances.
 *
 * We first compute the sets of outer array elements that need
 * to be copied in and out and then graft in the nodes for
 * performing this copying.
 *
 * In particular, for each array that is possibly written anywhere in
 * the subtree "node" and that may be used after "node"
 * or that may be visible outside the corresponding scop,
 * we copy out its entire extent.
 *
 * Any array elements that is read without first being written inside
 * the subtree "node" needs to be copied in.
 * Furthermore, if there are any array elements that
 * are copied out, but that may not be written inside "node", then
 * they also need to be copied in to ensure that the value after execution
 * is the same as the value before execution, at least for those array
 * elements that may have their values preserved by the scop or that
 * may be written before "node" and read after "node".
 * In case the array elements are structures, we need to take into
 * account that all members of the structures need to be written
 * by "node" before we can avoid copying the data structure in.
 *
 * Note that the may_write relation is intersected with the domain,
 * which has been intersected with the context.
 * This helps in those cases where the arrays are declared with a fixed size,
 * while the accesses are parametric and the context assigns a fixed value
 * to the parameters.
 *
 * If an element from a local array is read without first being written,
 * then there is no point in copying it in since it cannot have been
 * written prior to the scop. Warn about the uninitialized read instead.
 */
__isl_give isl_schedule_node *sa_add_to_from_device(
  __isl_take isl_schedule_node *node, __isl_take isl_union_set *domain,
  __isl_take isl_union_map *prefix, struct autosa_prog *prog)
{
  isl_union_set *local;
  isl_union_set *may_persist;
  isl_union_map *may_write, *must_write, *copy_out, *not_written;
  isl_union_map *read, *copy_in;
  isl_union_map *tagged;
  isl_union_map *local_uninitialized;
  isl_schedule_node *graft;

  /* Compute the copy-out that contains the live-out union
   * domain of non-local flow dep. 
   */
  tagged = isl_union_map_copy(prog->scop->tagged_reads);
  tagged = isl_union_map_union(tagged,
            isl_union_map_copy(prog->scop->tagged_may_writes));
  may_write = isl_union_map_copy(prog->may_write);
  may_write = isl_union_map_intersect_domain(may_write,
      isl_union_set_copy(domain));
  /* Keep ouly the live-out union domain of non-local flow. */
  may_write = remove_local_accesses(prog,
      isl_union_map_copy(tagged), may_write,
      isl_union_map_copy(prefix), 0);
  may_write = isl_union_map_apply_range(may_write,
      isl_union_map_copy(prog->to_outer));
  may_write = isl_union_map_apply_domain(may_write,
      isl_union_map_copy(prefix));
  may_write = approximate_copy_out(may_write, prog); 
  copy_out = isl_union_map_copy(may_write);

  /* Compute the copy-in. */
  may_write = isl_union_map_apply_range(may_write,
      isl_union_map_copy(prog->to_inner));
  must_write = isl_union_map_copy(prog->must_write);
  must_write = isl_union_map_apply_domain(must_write,
      isl_union_map_copy(prefix));

  may_persist = node_may_persist(node, prog); 
  may_write = isl_union_map_intersect_range(may_write, may_persist);
  not_written = isl_union_map_subtract(may_write, must_write);

  /* Detect the unitialized reads. */
  /* "local" contains (universal space) of arrays that are declared locally and 
   * written by "domain". */
  local = extract_local_accesses(prog, domain); 
  local = isl_union_set_apply(local, isl_union_map_copy(prog->to_inner));
  local_uninitialized = isl_union_map_copy(prog->scop->live_in);
  /* The local unitialized is defined as a read of a local array without 
   * first being written. */
  local_uninitialized = isl_union_map_intersect_range(local_uninitialized,
      local);
  read = isl_union_map_copy(prog->read);
  read = isl_union_map_intersect_domain(read, domain);
  read = remove_local_accesses(prog, tagged, read,
      isl_union_map_copy(prefix), 1);
  local_uninitialized = isl_union_map_intersect(local_uninitialized,
      isl_union_map_copy(read));
  if (!isl_union_map_is_empty(local_uninitialized)) {
    fprintf(stderr,
        "possibly uninitialized reads (not copied in):\n");
    isl_union_map_dump(local_uninitialized);
  }
  read = isl_union_map_subtract(read, local_uninitialized);
  read = isl_union_map_apply_domain(read, prefix);
  copy_in = isl_union_map_union(read, not_written);
  copy_in = isl_union_map_apply_range(copy_in,
      isl_union_map_copy(prog->to_outer));

  /* Add in the copy-in/copy-out nodes. */
  graft = create_copy_device(prog, node, "to_device",
      isl_union_map_range(copy_in)); 
  node = isl_schedule_node_graft_before(node, graft);
  graft = create_copy_device(prog, node, "from_device",
      isl_union_map_range(copy_out)); 
  node = isl_schedule_node_graft_after(node, graft);
 
  return node;
}

/* Add nodes for initializing ("init_device") and clearing ("clear_device")
 * the device before and after "node".
 */
__isl_give isl_schedule_node *sa_add_init_clear_device(
	__isl_take isl_schedule_node *node)
{
	isl_ctx *ctx;
	isl_space *space;
	isl_union_set *domain;
	isl_schedule_node *graft;

	ctx = isl_schedule_node_get_ctx(node);

	space = isl_space_set_alloc(ctx, 0, 0);
	space = isl_space_set_tuple_name(space, isl_dim_set, "init_device");
	domain = isl_union_set_from_set(isl_set_universe(space));
	graft = isl_schedule_node_from_domain(domain);

	node = isl_schedule_node_graft_before(node, graft);

	space = isl_space_set_alloc(ctx, 0, 0);
	space = isl_space_set_tuple_name(space, isl_dim_set, "clear_device");
	domain = isl_union_set_from_set(isl_set_universe(space));
	graft = isl_schedule_node_from_domain(domain);

	node = isl_schedule_node_graft_after(node, graft);

	return node;
}

/***************************************************************
 * AST Codegen
 ***************************************************************/
/* Internal data structure for at_domain.
 * "prog" represents the entire scop.
 * "kernel" points to the kernel to which the current schedule node
 * belongs. It is set by before_mark and reset by after_mark.
 * It may be NULL if we are outside any kernel.
 */
struct autosa_at_domain_data {
  struct autosa_prog *prog;
  struct autosa_kernel *kernel;
  struct autosa_hw_module *module;
  struct autosa_hw_top_module *top;
  struct autosa_pe_dummy_module *pe_dummy_module;
  int filter_buffer;
  int boundary;
  int pe_dummy;

  /* Under a "pipeline" mark */
  int under_pipeline;
  /* Under a "unroll" mark */
  int under_unroll;
  /* Inside a "pipeline" for loop */
  int in_pipeline_for;
  /* Inside a "unroll" for loop */
  int in_unroll_for;
};

/* Internal data structure for the index and AST expression transformation
 * callbacks for pet_stmt_build_ast_exprs.
 *
 * "kernel" is the kernel for which are computing AST expressions and
 * may be NULL if we are not inside a kernel.
 * "accesses" is the list of polysa_stmt_access in the statement.
 * "iterator_map" expresses the statement iterators in terms of
 * the AST loop iterators.
 * "sched2copy" expresses the outer copy_schedule_dim dimensions of
 * the kernel schedule in terms of the AST loop iterators and
 * may be NULL if we are not inside a kernel.
 *
 * The following fields are set in transform_index and used in transform_expr.
 * "array" is the array that is being accessed.
 * "global" is set if the global array is accessed (rather than
 * shared/private memory).
 * "local_array" refers to information on the array specialized
 * to the current kernel.
 */
struct autosa_transform_data {
	struct autosa_kernel *kernel;
	struct autosa_stmt_access *accesses;
	isl_pw_multi_aff *iterator_map;
	isl_pw_multi_aff *sched2copy;

	struct autosa_array_info *array;
	int global;
  int reg;
	struct autosa_local_array_info *local_array;
  struct autosa_array_ref_group *group;
};

/* Set *depth (initialized to 0 by the caller) to the maximum
 * of the schedule depths of the leaf nodes for which this function is called.
 */
static isl_bool update_depth(__isl_keep isl_schedule_node *node, void *user)
{
	int *depth = (int *)user;
	int node_depth;

	if (isl_schedule_node_get_type(node) != isl_schedule_node_leaf)
		return isl_bool_true;
	node_depth = isl_schedule_node_get_schedule_depth(node);
	if (node_depth > *depth)
		*depth = node_depth;

	return isl_bool_false;
}

/* Given a mapping "iterator_map" from the AST schedule to a domain,
 * return the corresponding mapping from the AST schedule
 * to the outer kernel->copy_schedule_dim dimensions of
 * the schedule computed by AutoSA for this kernel.
 *
 * Note that kernel->copy_schedule_dim is at least as large as
 * the largest depth of any array reference group associated to the kernel.
 * This is needed as the returned schedule is used to extract a mapping
 * to the outer tile->depth dimensions in transform_index.
 */
static __isl_give isl_pw_multi_aff *compute_sched_to_copy(
	struct autosa_kernel *kernel, __isl_take isl_pw_multi_aff *iterator_map)
{
	isl_union_pw_multi_aff *upma;
	isl_pw_multi_aff *pma;
	isl_space *space;

	space = isl_space_range(isl_pw_multi_aff_get_space(iterator_map));
	space = isl_space_from_domain(space);
	space = isl_space_add_dims(space, isl_dim_out,
					kernel->copy_schedule_dim);

	upma = isl_union_pw_multi_aff_copy(kernel->copy_schedule);
	pma = isl_union_pw_multi_aff_extract_pw_multi_aff(upma, space);
	isl_union_pw_multi_aff_free(upma);

	return isl_pw_multi_aff_pullback_pw_multi_aff(pma, iterator_map);
}

/* Return the autosa_stmt_access in the list "accesses" that corresponds
 * to "ref_id".
 */
static struct autosa_stmt_access *find_access(struct autosa_stmt_access *accesses,
	__isl_keep isl_id *ref_id)
{
	struct autosa_stmt_access *access;

	for (access = accesses; access; access = access->next)
		if (access->ref_id == ref_id)
			return access;

	return NULL;
}

/* Return the name of the outer array (of structs) accessed by "access".
 */
static const char *get_outer_array_name(__isl_keep isl_map *access)
{
	isl_space *space;
	const char *name;

	space = isl_space_range(isl_map_get_space(access));
	while (space && isl_space_is_wrapping(space))
		space = isl_space_domain(isl_space_unwrap(space));
	name = isl_space_get_tuple_name(space, isl_dim_set);
	isl_space_free(space);

	return name;
}

/* Return the index of the array called "name" in the list of arrays.
 */
static int find_array_index(struct autosa_kernel *kernel, const char *name)
{
	int i;

	for (i = 0; i < kernel->n_array; ++i)
		if (!strcmp(name, kernel->array[i].array->name))
			return i;

	return -1;
}

/* Return a pointer to the autosa_array_ref_group in "local"
 * that contains the reference "access".
 * Return NULL if no such group can be found.
 */
static struct autosa_array_ref_group *find_ref_group(
	struct autosa_local_array_info *local, struct autosa_stmt_access *access)
{
	int i, j;

	for (i = 0; i < local->n_group; ++i) {
		struct autosa_array_ref_group *group = local->groups[i];

		for (j = 0; j < group->n_ref; ++j)
			if (group->refs[j] == access)
				return group;
	}

	return NULL;
}

/* Given a mapping "iterator_map" from the AST schedule to a domain,
 * return the corresponding mapping from the AST schedule
 * to the outer group->copy_schedule_dim dimensions of
 * the schedule computed by AutoSA for this kernel.
 *
 * Note that group->copy_schedule_dim is at least as large as
 * the largest depth of any array references associated to the group.
 * This is needed as the returned schedule is used to extract a mapping
 * to the outer tile->depth dimensions in transform_index.
 */
static __isl_give isl_pw_multi_aff *compute_sched_to_copy_group(
  __isl_take isl_pw_multi_aff *iterator_map, 
  struct autosa_array_ref_group *group)
{
  isl_union_pw_multi_aff *upma;
  isl_pw_multi_aff *pma;
  isl_space *space;

  space = isl_space_range(isl_pw_multi_aff_get_space(iterator_map));
  space = isl_space_from_domain(space);
  space = isl_space_add_dims(space, isl_dim_out,
            group->copy_schedule_dim);
  
  upma = isl_union_pw_multi_aff_copy(group->copy_schedule);
  pma = isl_union_pw_multi_aff_extract_pw_multi_aff(upma, space);
  isl_union_pw_multi_aff_free(upma);

  return isl_pw_multi_aff_pullback_pw_multi_aff(pma, iterator_map);
}

/* Given an index expression "index" of the form
 *
 *	L -> F(A),
 *
 * with F(A) either A or some subfield of A and L the AST loop iterators,
 * and a tiling "tiling" of the form
 *
 *	[L -> A] -> T
 *
 * apply the tiling to the outer array in the index expression to obtain
 *
 *	L -> T(A)
 *
 * If F(A) is some subfield of A, then separate the member access
 * into the base index expression and the field index expression,
 * apply the tiling to the base index expression and combine the result
 * with the field index expression.
 *
 * If F(A) is A, then modify index to keep track of the iterators
 *
 *	L -> [L -> A]
 *
 * and combine the result with the tiling to obtain a tiled index expression
 * in terms of the AST loop iterators
 *
 *	L -> T
 */
static __isl_give isl_multi_pw_aff *tile_outer(
	__isl_take isl_multi_pw_aff *index, __isl_take isl_multi_pw_aff *tiling)
{
	isl_bool is_wrapping;
	isl_space *space;
	isl_multi_pw_aff *mpa;

	is_wrapping = isl_multi_pw_aff_range_is_wrapping(index);
	if (is_wrapping < 0)
		goto error;
	if (is_wrapping) {
		isl_multi_pw_aff *field;

		field = isl_multi_pw_aff_copy(index);
		field = isl_multi_pw_aff_range_factor_range(field);
		index = isl_multi_pw_aff_range_factor_domain(index);
		index = tile_outer(index, tiling);
		return isl_multi_pw_aff_range_product(index, field);
	}

	space = isl_space_domain(isl_multi_pw_aff_get_space(index));
	space = isl_space_map_from_set(space);
	mpa = isl_multi_pw_aff_identity(space);
	index = isl_multi_pw_aff_range_product(mpa, index);
	index = isl_multi_pw_aff_pullback_multi_pw_aff(tiling, index);

	return index;
error:
	isl_multi_pw_aff_free(index);
	isl_multi_pw_aff_free(tiling);
	return NULL;
}

/* Index transformation callback for pet_stmt_build_ast_exprs.
 *
 * "index" expresses the array indices in terms of statement iterators
 *
 * We first reformulate "index" in terms of the AST loop iterators.
 * Then we check if we are accessing the global array or
 * a shared/private copy.  In particular, if we are not inside a kernel
 * then we must be accessing a global array.
 * In the former case, we simply return
 * the updated index.  If "index" is an affine expression rather
 * than an array access, then we also return the updated index here.
 *
 * If no reference groups have been computed for the array,
 * then we can only be accessing the global array.
 *
 * Otherwise, we apply the tiling to the index.
 * This tiling is of the form
 *
 *	[D -> A] -> T
 *
 * where D corresponds to the outer tile->depth dimensions of
 * the kernel schedule.
 * The index is of the form
 *
 *	L -> A
 *
 * We update the tiling to refer to the AST loop iterators
 *
 *	[L -> A] -> T
 *
 * and combine it with the index to obtain a tiled index expression in terms
 * of the AST loop iterators
 *
 *	L -> T
 *
 * Note that while the tiling applies directly to an outer array.
 * the index may refer to some subfield of this outer array.
 * In such cases, the result will refer to the same subfield of the tile.
 * That is, an index expression of the form  L -> F(A) will be transformed
 * into an index expression of the form L -> F(T).
 */
static __isl_give isl_multi_pw_aff *transform_index(
	__isl_take isl_multi_pw_aff *index, __isl_keep isl_id *ref_id,
	void *user)
{
	struct autosa_transform_data *data = (struct autosa_transform_data *)user;
	struct autosa_stmt_access *access;
	struct autosa_array_ref_group *group;
	struct autosa_array_tile *tile;
	isl_pw_multi_aff *iterator_map;
	int i;
	int dim;
	const char *name;
	isl_space *space;
	isl_multi_pw_aff *tiling;
	isl_pw_multi_aff *pma;
	isl_pw_multi_aff *sched2depth;
  isl_pw_multi_aff *sched2copy;

	data->array = NULL;

	iterator_map = isl_pw_multi_aff_copy(data->iterator_map);
	index = isl_multi_pw_aff_pullback_pw_multi_aff(index, iterator_map);

	if (!data->kernel)
		return index;

	access = find_access(data->accesses, ref_id);
	if (!access)
		return index;
	if (!isl_map_has_tuple_name(access->access, isl_dim_out)) 
		return index;

	name = get_outer_array_name(access->access);
	if (!name)
		return isl_multi_pw_aff_free(index);
	i = find_array_index(data->kernel, name);
	if (i < 0)
		isl_die(isl_multi_pw_aff_get_ctx(index), isl_error_internal,
			"cannot find array",
			return isl_multi_pw_aff_free(index));
	data->local_array = &data->kernel->array[i];
	data->array = data->local_array->array;
	group = find_ref_group(data->local_array, access);
  data->group = group;
	if (!group) {
		data->global = 1;
    data->reg = 1;
		return index;
	}

	tile = autosa_array_ref_group_tile(group);
	data->global = !tile;
  data->reg = !tile;
	if (!tile)
		return index;

  /* recompute the sched2copy for each index. */
  if (group->group_type == AUTOSA_PE_GROUP) {
    sched2copy = compute_sched_to_copy_group(isl_pw_multi_aff_copy(
      data->iterator_map), group); 
  }

	space = isl_space_domain(isl_multi_aff_get_space(tile->tiling));
	space = isl_space_range(isl_space_unwrap(space));
	space = isl_space_map_from_set(space);
	pma = isl_pw_multi_aff_identity(space);
  if (group->group_type == AUTOSA_PE_GROUP) {
    sched2depth = sched2copy;
  } else {
	  sched2depth = isl_pw_multi_aff_copy(data->sched2copy);
  }
	dim = isl_pw_multi_aff_dim(sched2depth, isl_dim_out);
	sched2depth = isl_pw_multi_aff_drop_dims(sched2depth, isl_dim_out,
					    tile->depth, dim - tile->depth);
	pma = isl_pw_multi_aff_product(sched2depth, pma);
	tiling = isl_multi_pw_aff_from_multi_aff(
				    isl_multi_aff_copy(tile->tiling));
	tiling = isl_multi_pw_aff_pullback_pw_multi_aff(tiling, pma);

	index = tile_outer(index, tiling);

	return index;
}

/* Dereference "expr" by adding an index [0].
 * The original "expr" is assumed not to have any indices.
 *
 * If "expr" is a member access, then the dereferencing needs
 * to be applied to the structure argument of this member access.
 */
static __isl_give isl_ast_expr *dereference(__isl_take isl_ast_expr *expr)
{
	isl_ctx *ctx;
	isl_ast_expr *arg0, *res;
	isl_ast_expr_list *list;

	arg0 = isl_ast_expr_get_op_arg(expr, 0);
	if (!arg0)
		return isl_ast_expr_free(expr);
	if (isl_ast_expr_get_type(arg0) == isl_ast_expr_op &&
	    isl_ast_expr_get_op_type(arg0) == isl_ast_op_member) {
		isl_ast_expr *arg;

		arg = isl_ast_expr_get_op_arg(arg0, 0);
		arg = dereference(arg);
		arg0 = isl_ast_expr_set_op_arg(arg0, 0, arg);
		expr = isl_ast_expr_set_op_arg(expr, 0, arg0);

		return expr;
	}
	isl_ast_expr_free(arg0);

	ctx = isl_ast_expr_get_ctx(expr);
	res = isl_ast_expr_from_val(isl_val_zero(ctx));
	list = isl_ast_expr_list_from_ast_expr(res);
	res = isl_ast_expr_get_op_arg(expr, 0);
	res = isl_ast_expr_access(res, list);
	isl_ast_expr_free(expr);

	return res;
}

/* Linearize the index expression "expr" based on the array bounds
 * of "array".
 *
 * That is, transform expression
 *
 *	A[i_0][i_1]...[i_n]
 *
 * to
 *
 *	A[(..((i_0 * b_1 + i_1) ... ) * b_n + i_n]
 *
 * where b_0, b_1, ..., b_n are the bounds on the array.
 *
 * If the base of "expr" is a member access, then the linearization needs
 * to be applied to the structure argument of this member access.
 *
 * In the base case, if "expr" has no arguments (other than the name of
 * the array), then we are passing an entire array to a function.
 * In this case, there is nothing to linearize.
 * Note that at this point an expression with no arguments can
 * only be an entire array because the scalar case and
 * the case of single struct are handled by the caller.
 *
 * If the number of specified index expressions in "expr"
 * is smaller than the dimension of the accessed array,
 * then the missing i_j also do not appear in the linearized expression.
 * Furthermore, since such an expression does not refer to a single
 * element while the default linearized expression would refer to
 * a single element, we return the expression
 *
 *	A + (..((i_0 * b_1 + i_1) ... ) * b_l + i_l)
 *
 * instead.  Note that because of the special case handling above,
 * we can assume here that there is at least one index expression.
 */
__isl_give isl_ast_expr *autosa_local_array_info_linearize_index(
	struct autosa_local_array_info *array, __isl_take isl_ast_expr *expr)
{
	int i, n;
	isl_ast_expr *arg0;
	isl_ast_expr *res;
	isl_ast_expr_list *list;

	arg0 = isl_ast_expr_get_op_arg(expr, 0);
	if (isl_ast_expr_get_type(arg0) == isl_ast_expr_op &&
	    isl_ast_expr_get_op_type(arg0) == isl_ast_op_member) {
		isl_ast_expr *arg;

		arg = isl_ast_expr_get_op_arg(arg0, 0);
		arg = autosa_local_array_info_linearize_index(array, arg);
		arg0 = isl_ast_expr_set_op_arg(arg0, 0, arg);
		expr = isl_ast_expr_set_op_arg(expr, 0, arg0);

		return expr;
	}
	isl_ast_expr_free(arg0);

	if (isl_ast_expr_get_op_n_arg(expr) == 1)
		return expr;

	n = isl_ast_expr_get_op_n_arg(expr);
	res = isl_ast_expr_get_op_arg(expr, 1);
	for (i = 1; i < array->n_index; ++i) {
		isl_ast_expr *expr_i;

		expr_i = isl_ast_expr_get_op_arg(array->bound_expr, 1 + i);
		res = isl_ast_expr_mul(res, expr_i);

		if (i + 1 >= n)
			continue;
		expr_i = isl_ast_expr_get_op_arg(expr, i + 1);
		res = isl_ast_expr_add(res, expr_i);
	}

	if (1 + array->n_index > n) {
		res = isl_ast_expr_add(isl_ast_expr_get_op_arg(expr, 0), res);
	} else {
		list = isl_ast_expr_list_from_ast_expr(res);
		res = isl_ast_expr_get_op_arg(expr, 0);
		res = isl_ast_expr_access(res, list);
	}

	isl_ast_expr_free(expr);

	return res;
}

/* AST expression transformation callback for pet_stmt_build_ast_exprs.
 *
 * If the AST expression refers to an array that is not accessed
 * at all, then this means the value of the expression is not used,
 * so we might as well print zero (NULL pointer) instead.
 *
 * If the AST expression refers to a global scalar that is not
 * a read-only scalar, then its address was passed to the kernel and
 * we need to dereference it.
 *
 * If the AST expression refers to an access to a global array,
 * then we linearize the access exploiting the bounds in data->local_array.
 */
static __isl_give isl_ast_expr *transform_expr(__isl_take isl_ast_expr *expr,
	__isl_keep isl_id *id, void *user)
{
	struct autosa_transform_data *data = (struct autosa_transform_data *)user;

	if (!data->array)
		return expr;

	if (!data->array->accessed) {
		isl_ctx *ctx;

		ctx = isl_ast_expr_get_ctx(expr);
		isl_ast_expr_free(expr);
		return isl_ast_expr_from_val(isl_val_zero(ctx));
	}
	if (autosa_array_is_read_only_scalar(data->array))
		return expr;
	if (!data->global)
		return expr;
	if (data->array->n_index == 0)
		return dereference(expr);
	if (!data->array->linearize)
		return expr;

	return autosa_local_array_info_linearize_index(data->local_array, expr);
}

/* This function is called for each instance of a user statement
 * in the kernel "kernel", identified by "autosa_stmt".
 * "kernel" may be NULL if we are not inside a kernel.
 *
 * We attach a struct autosa_kernel_stmt to the "node", containing
 * a computed AST expression for each access, through an annotation
 * with name "user".
 * These AST expressions are computed from iterator_map,
 * which expresses the domain elements in terms of the generated loops, 
 * and sched2copy, which expresses the outer copy_schedule_dim dimensions of
 * the kernel schedule computed by AutoSA in terms of the generated loops.
 */
static __isl_give isl_ast_node *create_domain_leaf(
	struct autosa_kernel *kernel, __isl_take isl_ast_node *node,
	__isl_keep isl_ast_build *build, struct autosa_stmt *polysa_stmt)
{
	struct autosa_transform_data data;
	struct autosa_kernel_stmt *stmt;
	isl_ctx *ctx;
	isl_id *id;
	isl_pw_multi_aff *sched2copy;
	isl_map *map;
	isl_pw_multi_aff *iterator_map;
	isl_union_map *schedule;

	if (!node)
		return NULL;
	ctx = isl_ast_node_get_ctx(node);

	stmt = isl_calloc_type(ctx, struct autosa_kernel_stmt);
	if (!stmt)
		return isl_ast_node_free(node);

	schedule = isl_ast_build_get_schedule(build); 
	map = isl_map_reverse(isl_map_from_union_map(schedule));
	iterator_map = isl_pw_multi_aff_from_map(map);
	if (kernel)
		sched2copy = compute_sched_to_copy(kernel,
					isl_pw_multi_aff_copy(iterator_map)); 
	else
		sched2copy = NULL;

	stmt->type = AUTOSA_KERNEL_STMT_DOMAIN;
	stmt->u.d.stmt = polysa_stmt;

	data.kernel = kernel;
	data.accesses = stmt->u.d.stmt->accesses;
	data.iterator_map = iterator_map;
	data.sched2copy = sched2copy;
	stmt->u.d.ref2expr = pet_stmt_build_ast_exprs(stmt->u.d.stmt->stmt,
					    build, &transform_index, &data,
					    &transform_expr, &data);

	isl_pw_multi_aff_free(iterator_map);
	isl_pw_multi_aff_free(sched2copy);

	id = isl_id_alloc(ctx, "user", stmt);
	id = isl_id_set_free_user(id, &autosa_kernel_stmt_free);
	if (!id)
		autosa_kernel_stmt_free(stmt);
	return isl_ast_node_set_annotation(node, id);
}

/* Does "array" need to be allocated on the device?
 * If it is a read-only scalar, then it will be passed as an argument
 * to the kernel and therefore does not require any allocation.
 * If this device memory is not accessed at all, then it does not
 * need to be allocated either.
 */
int autosa_array_requires_device_allocation(struct autosa_array_info *array)
{
	if (autosa_array_is_read_only_scalar(array))
		return 0;
	if (!array->global)
		return 0;
	return 1;
}

/* Build AST expressions for the device array sizes of all arrays in "prog"
 * that require allocation on the device using "build", as well as
 * for the original array sizes of all arrays that need to be declared
 * on the host.
 * "node" is freed in case of error.
 */
static __isl_give isl_ast_node *build_array_bounds(
	__isl_take isl_ast_node *node, struct autosa_prog *prog,
	__isl_keep isl_ast_build *build)
{
	int i;

	for (i = 0; i < prog->n_array; ++i) {
		struct autosa_array_info *array = &prog->array[i];
		isl_multi_pw_aff *size;
		isl_ast_expr *expr;

		if (!autosa_array_requires_device_allocation(array))
			continue;

		size = isl_multi_pw_aff_copy(array->bound);
		expr = ppcg_build_size_expr(size, build);
		array->bound_expr = expr;
		if (!expr)
			return isl_ast_node_free(node);
	}

	for (i = 0; i < prog->n_array; ++i) {
		struct autosa_array_info *array = &prog->array[i];
		isl_set *extent;
		isl_multi_pw_aff *size;
		isl_ast_expr *expr;

		if (!array->declare_local)
			continue;
		extent = isl_set_copy(array->declared_extent);
		size = ppcg_size_from_extent(extent);
		expr = ppcg_build_size_expr(size, build);
		array->declared_size = expr;
		if (!expr)
			return isl_ast_node_free(node);
	}

	return node;
}

/* This function is called for each statement node in the AST
 * for copying to or from local memory.
 * Attach a pointer to a polysa_kernel_stmt representing the copy
 * statement to the node.
 * The statement name is "read" or "write", depending on whether we are
 * reading from global memory or writing to global memory.
 *
 * The schedule is of the form
 *
 *	type[D -> A] -> L
 *
 * where D corresponds to the outer tile->depth dimensions of
 * the kernel schedule, A to the global array and L to the outer
 * generated AST schedule.
 * We compute the inverse and strip off the type, resulting in
 *
 *	L -> [D -> A]
 *
 * We combine this mapping with on the one hand the projection
 *
 *	[D -> A] -> A
 *
 * and on the other hand the group tiling
 *
 *	[D -> A] -> T
 *
 * resulting in
 *
 *	L -> A		and 	L -> T
 *
 * and store the corresponding expressions in stmt->index and stmt->local_index,
 * where stmt points to the ppcg_kernel_stmt that is attached to the node.
 * stmt->index is linearized if the global memory array is linearized.
 */
static __isl_give isl_ast_node *create_access_leaf(struct autosa_kernel *kernel,
	struct autosa_array_ref_group *group, __isl_take isl_ast_node *node,
	__isl_keep isl_ast_build *build)
{
	struct autosa_kernel_stmt *stmt;
	struct autosa_array_tile *tile;
	isl_id *id;
	isl_ast_expr *expr;
	isl_space *space;
	isl_map *access;
	isl_pw_multi_aff *pma, *pma2;
	const char *type;

	stmt = isl_calloc_type(kernel->ctx, struct autosa_kernel_stmt);
	if (!stmt)
		return isl_ast_node_free(node);

  /* type[D -> A] -> L */
	access = isl_map_from_union_map(isl_ast_build_get_schedule(build));
	type = isl_map_get_tuple_name(access, isl_dim_in);
	stmt->u.c.read = type && !strcmp(type, "read");
  /* L -> type[D -> A] */
	access = isl_map_reverse(access);
	pma = isl_pw_multi_aff_from_map(access);
	pma = isl_pw_multi_aff_reset_tuple_id(pma, isl_dim_out);
	space = isl_space_range(isl_pw_multi_aff_get_space(pma));
	space = isl_space_unwrap(space);
  /* [D -> A] -> A */
	pma2 = isl_pw_multi_aff_range_map(space);
  /* L -> A */
	pma2 = isl_pw_multi_aff_pullback_pw_multi_aff(pma2,
						    isl_pw_multi_aff_copy(pma));
	expr = isl_ast_build_access_from_pw_multi_aff(build, pma2);
	if (group->array->linearize)
		expr = autosa_local_array_info_linearize_index(group->local_array,
							    expr);
	stmt->u.c.index = expr;

	tile = autosa_array_ref_group_tile(group);
  /* [D -> A] -> T */
	pma2 = isl_pw_multi_aff_from_multi_aff(
					    isl_multi_aff_copy(tile->tiling));
  /* L -> T */
	pma2 = isl_pw_multi_aff_pullback_pw_multi_aff(pma2, pma);
	expr = isl_ast_build_access_from_pw_multi_aff(build, pma2);
	stmt->u.c.local_index = expr;

	stmt->u.c.array = group->array;
	stmt->u.c.local_array = group->local_array;
	stmt->type = AUTOSA_KERNEL_STMT_COPY;

	id = isl_id_alloc(kernel->ctx, "copy", stmt);
	id = isl_id_set_free_user(id, &autosa_kernel_stmt_free);
	if (!id)
		autosa_kernel_stmt_free(stmt);
	return isl_ast_node_set_annotation(node, id);
}

/* This function is called for each instance of a user statement
 * in the kernel. This may be one of the original user statements
 * or a statement introduced by AutoSA.
 *
 * We first check if the statement id corresponds to a autosa statement,
 * which indicates the statement is an original user statement. Any statement
 * that is not an original user statement has been introduced by AutoSA and
 * requires special handling.
 *
 * If the user statement is one of the original user statements, then we call
 * create_domain_leaf.  
 * If it is "init_device", then we call build_array_bounds.  
 * Otherwise, we check if it is a copy statement and call the appropriate 
 * functions.  
 * Statements that copy an array to/from the device do not need any 
 * further treatment. Neither does "clear_device".
 */
static __isl_give isl_ast_node *at_domain(__isl_take isl_ast_node *node,
	__isl_keep isl_ast_build *build, void *user)
{
	struct autosa_at_domain_data *data = (struct autosa_at_domain_data *)user;
	struct autosa_stmt *device_stmt;
	isl_ast_expr *expr, *arg;
	isl_id *id;
	int is_sync;
	const char *name;
	void *p;

	expr = isl_ast_node_user_get_expr(node);
	arg = isl_ast_expr_get_op_arg(expr, 0);
	id = isl_ast_expr_get_id(arg);
	name = isl_id_get_name(id);
	p = isl_id_get_user(id);
	isl_ast_expr_free(expr);
	isl_ast_expr_free(arg);

	device_stmt = find_stmt(data->prog, id);
  isl_id_free(id);

  if (device_stmt)
    return create_domain_leaf(data->kernel, node, build, device_stmt); 
  if (!prefixcmp(name, "to_device_") || !prefixcmp(name, "from_device_"))
    return node;
  if (!strcmp(name, "init_device"))
    return build_array_bounds(node, data->prog, build); 
  if (!strcmp(name, "clear_device"))
    return node;
  if (!strcmp(name, "read") || !strcmp(name, "write")) {
    struct autosa_array_ref_group *group = (struct autosa_array_ref_group *)p;
    return create_access_leaf(data->kernel, group, node, build);
  }

  return node;
}

/* Build an access AST expression for the effective grid size using "build".
 * Store the result in kernel->grid_size_expr.
 */
static isl_stat build_grid_size(struct autosa_kernel *kernel,
	__isl_keep isl_ast_build *build)
{
	isl_multi_pw_aff *size;

	size = isl_multi_pw_aff_copy(kernel->grid_size);
	size = isl_multi_pw_aff_set_tuple_name(size, isl_dim_out, "grid");
	kernel->grid_size_expr = ppcg_build_size_expr(size, build);

	if (!kernel->grid_size_expr)
		return isl_stat_error;
	return isl_stat_ok;
}

/* Build access AST expressions for the localized array sizes using "build".
 * Store the result in local->bound_expr.
 * Only do this for arrays for which localized bounds have been computed.
 */
static isl_stat build_local_array_sizes(struct autosa_kernel *kernel,
	__isl_keep isl_ast_build *build)
{
	int i;

	for (i = 0; i < kernel->n_array; ++i) {
		struct autosa_local_array_info *local = &kernel->array[i];
		isl_multi_pw_aff *size;

		if (local->n_group == 0)
			continue;
		size = isl_multi_pw_aff_copy(local->bound);
		local->bound_expr = ppcg_build_size_expr(size, build);
		if (!local->bound_expr)
			return isl_stat_error;
	}

	return isl_stat_ok;
}

/* Build access AST expressions for the effective grid size and
 * the localized array sizes using "build".
 */
static isl_stat build_grid_and_local_array_sizes(struct autosa_kernel *kernel,
	__isl_keep isl_ast_build *build)
{
	if (build_grid_size(kernel, build) < 0)
		return isl_stat_error;
	if (build_local_array_sizes(kernel, build) < 0)
		return isl_stat_error;
	return isl_stat_ok;
}

/* This function is called before the AST generator starts traversing
 * the schedule subtree of a node with mark "mark".
 *
 * If the mark is called "kernel", store the kernel pointer in data->kernel
 * for use in at_domain and build AST expressions for the grid size and
 * the localized array sizes.
 */
static isl_stat before_mark(__isl_keep isl_id *mark,
	__isl_keep isl_ast_build *build, void *user)
{
	struct autosa_at_domain_data *data = (struct autosa_at_domain_data *)user;

	if (!mark)
		return isl_stat_error;
	if (!strcmp(isl_id_get_name(mark), "kernel")) {
		data->kernel = (struct autosa_kernel *)isl_id_get_user(mark);
		if (build_grid_and_local_array_sizes(data->kernel, build) < 0)
			return isl_stat_error;
	}
	return isl_stat_ok;
}

/* This function is called after the AST generator has finished traversing
 * the schedule subtree of a mark node. "node" points to the corresponding
 * mark AST node.
 *
 * If the mark is called "kernel", then replace "node" by a user node
 * that "calls" the kernel, representing the launch of the kernel.
 * The original "node" is stored inside the kernel object so that
 * it can be used to print the device code.
 * Note that this assumes that a kernel is only launched once.
 * Also clear data->kernel.
 */
static __isl_give isl_ast_node *after_mark(__isl_take isl_ast_node *node,
  __isl_keep isl_ast_build *build, void *user)
{
	isl_ctx *ctx;
	isl_id *id;
	isl_ast_expr *expr;
	isl_ast_expr_list *list;
	struct autosa_kernel *kernel;
	struct autosa_at_domain_data *data = (struct autosa_at_domain_data *)user;

	ctx = isl_ast_node_get_ctx(node);
	id = isl_ast_node_mark_get_id(node);
	if (!id)
		return isl_ast_node_free(node);
	if (strcmp(isl_id_get_name(id), "kernel") || !data->kernel) {
		isl_id_free(id);
		return node;
	}
	kernel = data->kernel;
	data->kernel = NULL;
	kernel->space = isl_ast_build_get_schedule_space(build);
	kernel->tree = isl_ast_node_mark_get_node(node);
	isl_ast_node_free(node);
	expr = isl_ast_expr_from_id(isl_id_copy(id));
	list = isl_ast_expr_list_alloc(ctx, 0);
	expr = isl_ast_expr_call(expr, list);
	node = isl_ast_node_alloc_user(expr);
	node = isl_ast_node_set_annotation(node, id);

	return node;
}

/* Use isl to generate code for both the host and the device
 * from "schedule".
 * The device code is marked by "kernel" mark nodes in the schedule tree,
 * containing a pointer to a polysa_kernel object.
 * The returned AST only contains the AST for the host code.
 * The ASTs for the device code are embedded in polysa_kernel objects
 * attached to the leaf nodes that call "kernel".
 */
__isl_give isl_ast_node *sa_generate_code(struct autosa_gen *gen,
    __isl_take isl_schedule *schedule)
{
  struct autosa_at_domain_data data;
  isl_ast_build *build;
  isl_ast_node *tree;
  isl_id_list *iterators;
  int depth;

  if (schedule == NULL)
    return NULL;

  data.prog = gen->prog;
  data.kernel = NULL;

  depth = 0;
  if (isl_schedule_foreach_schedule_node_top_down(schedule, &update_depth, 
        &depth) < 0)
    schedule = isl_schedule_free(schedule);
  build = isl_ast_build_alloc(gen->prog->ctx);
  iterators = ppcg_scop_generate_names(gen->prog->scop, depth, "c");
  build = isl_ast_build_set_iterators(build, iterators);
  build = isl_ast_build_set_at_each_domain(build, &at_domain, &data);
  build = isl_ast_build_set_before_each_mark(build, &before_mark, &data);
  build = isl_ast_build_set_after_each_mark(build, &after_mark, &data);
  if (gen->prog->scop->options->debug->dump_final_schedule)
    isl_schedule_dump(schedule);
  tree = isl_ast_build_node_from_schedule(build, schedule);
  isl_ast_build_free(build);

  return tree;
}

/* Initialize the autosa_at_domain_data struct. */
static void autosa_at_domain_data_init(
  struct autosa_at_domain_data *data, struct autosa_gen *gen)
{
  data->prog = gen->prog;
  data->kernel = NULL;
  data->module = NULL;
  data->filter_buffer = 0;
  data->under_unroll = 0;
  data->under_pipeline = 0;
  data->in_unroll_for = 0;
  data->in_pipeline_for = 0;
  data->boundary = 0;
  data->pe_dummy = 0;
  data->pe_dummy_module = NULL;
}

/* Return a pointer to the autosa_array_ref_group in "local"
 * that contains the reference "access".
 * Return NULL if no such group can be found.
 */
static struct autosa_array_ref_group *find_ref_group_module(
	struct autosa_local_array_info *local, struct autosa_stmt_access *access)
{
	int i, j;

	for (i = 0; i < local->n_pe_group; ++i) {
		struct autosa_array_ref_group *group = local->pe_groups[i];

		for (j = 0; j < group->n_ref; ++j)
			if (group->refs[j] == access)
				return group;
	}

	return NULL;
}

/* Index transformation callback for pet_stmt_build_ast_exprs.
 *
 * "index" expresses the array indices in terms of statement iterators
 *
 * We first reformulate "index" in terms of the AST loop iterators.
 * Then we check if we are accessing the global array or
 * a shared/private copy.  In particular, if we are not inside a kernel
 * then we must be accessing a global array.
 * In the former case, we simply return
 * the updated index.  If "index" is an affine expression rather
 * than an array access, then we also return the updated index here.
 *
 * If no reference groups have been computed for the array,
 * then we can only be accessing the global array.
 *
 * Otherwise, we apply the tiling to the index.
 * This tiling is of the form
 *
 *	[D -> A] -> T
 *
 * where D corresponds to the outer tile->depth dimensions of
 * the kernel schedule.
 * The index is of the form
 *
 *	L -> A
 *
 * We update the tiling to refer to the AST loop iterators
 *
 *	[L -> A] -> T
 *
 * and combine it with the index to obtain a tiled index expression in terms
 * of the AST loop iterators
 *
 *	L -> T
 *
 * Note that while the tiling applies directly to an outer array.
 * the index may refer to some subfield of this outer array.
 * In such cases, the result will refer to the same subfield of the tile.
 * That is, an index expression of the form  L -> F(A) will be transformed
 * into an index expression of the form L -> F(T).
 */
static __isl_give isl_multi_pw_aff *transform_index_module(
	__isl_take isl_multi_pw_aff *index, __isl_keep isl_id *ref_id,
	void *user)
{
	struct autosa_transform_data *data = (struct autosa_transform_data *)user;
	struct autosa_stmt_access *access;
	struct autosa_array_ref_group *group;
	struct autosa_array_tile *tile;
	isl_pw_multi_aff *iterator_map;
	int i;
	int dim;
	const char *name;
	isl_space *space;
	isl_multi_pw_aff *tiling;
	isl_pw_multi_aff *pma;
	isl_pw_multi_aff *sched2depth;
  isl_pw_multi_aff *sched2copy;

	data->array = NULL;

	iterator_map = isl_pw_multi_aff_copy(data->iterator_map);
	index = isl_multi_pw_aff_pullback_pw_multi_aff(index, iterator_map);

	if (!data->kernel)
		return index;

	access = find_access(data->accesses, ref_id);
	if (!access)
		return index;
	if (!isl_map_has_tuple_name(access->access, isl_dim_out)) 
		return index;

	name = get_outer_array_name(access->access);
	if (!name)
		return isl_multi_pw_aff_free(index);
	i = find_array_index(data->kernel, name);
	if (i < 0)
		isl_die(isl_multi_pw_aff_get_ctx(index), isl_error_internal,
			"cannot find array",
			return isl_multi_pw_aff_free(index));
	data->local_array = &data->kernel->array[i];
	data->array = data->local_array->array;

	group = find_ref_group_module(data->local_array, access);
  data->group = group;
	if (!group) {
		data->global = 1;
    data->reg = 1;
		return index;
	}

	tile = autosa_array_ref_group_tile(group);
	data->global = !tile;
  data->reg = !tile;
	if (!tile)
		return index;

  /* recompute the sched2copy for each index. */
  if (group->group_type == AUTOSA_PE_GROUP) {
    sched2copy = compute_sched_to_copy_group(
                    isl_pw_multi_aff_copy(data->iterator_map), group); 
  }

	space = isl_space_domain(isl_multi_aff_get_space(tile->tiling));
	space = isl_space_range(isl_space_unwrap(space));
	space = isl_space_map_from_set(space);
	pma = isl_pw_multi_aff_identity(space);
  if (group->group_type == AUTOSA_PE_GROUP) {
    sched2depth = sched2copy;
  } else {
	  sched2depth = isl_pw_multi_aff_copy(data->sched2copy);
  }
	dim = isl_pw_multi_aff_dim(sched2depth, isl_dim_out);
	sched2depth = isl_pw_multi_aff_drop_dims(sched2depth, isl_dim_out,
					    tile->depth, dim - tile->depth);
	pma = isl_pw_multi_aff_product(sched2depth, pma);
	tiling = isl_multi_pw_aff_from_multi_aff(
				    isl_multi_aff_copy(tile->tiling));
	tiling = isl_multi_pw_aff_pullback_pw_multi_aff(tiling, pma);
	index = tile_outer(index, tiling);

	return index;
}

/* AST expression transformation callback for pet_stmt_build_ast_exprs.
 *
 * If the AST expression refers to an array that is not accessed
 * at all, then this means the value of the expression is not used,
 * so we might as well print zero (NULL pointer) instead.
 *
 * If the AST expression refers to a global scalar that is not
 * a read-only scalar, then its address was passed to the kernel and
 * we need to dereference it.
 *
 * If the AST expression refers to an array reference that is put in 
 * the registers. We will modify the expr to a register access.
 *
 * If the AST expression refers to an access to a global array,
 * then we linearize the access exploiting the bounds in data->local_array.
 */
static __isl_give isl_ast_expr *transform_expr_module(__isl_take isl_ast_expr *expr,
	__isl_keep isl_id *id, void *user)
{
	struct autosa_transform_data *data = (struct autosa_transform_data *)user;

	if (!data->array)
		return expr; 

	if (!data->array->accessed) {
		isl_ctx *ctx;

		ctx = isl_ast_expr_get_ctx(expr);
		isl_ast_expr_free(expr);
		return isl_ast_expr_from_val(isl_val_zero(ctx));
	}
	if (autosa_array_is_read_only_scalar(data->array))
		return expr;
  if (!data->reg)
    return expr;
  if (data->reg) {
    isl_ctx *ctx;
    char *local_name;
    char buf[50];
    isl_id *id;
    isl_ast_expr *array;
    isl_ast_expr_list *indices;
    isl_ast_expr *indice;

    ctx = isl_ast_expr_get_ctx(expr);
    isl_ast_expr_free(expr);
    
    /* Create a register access. */
    isl_printer *p_str = isl_printer_to_str(ctx);    
	  p_str = autosa_array_ref_group_print_name(data->group, p_str);
    local_name = isl_printer_get_str(p_str);
    isl_printer_free(p_str);
    sprintf(buf, "%s", local_name);
    free(local_name);

    id = isl_id_alloc(ctx, buf, NULL);
    array = isl_ast_expr_from_id(id);
    indice = isl_ast_expr_from_val(isl_val_zero(ctx));
    indices = isl_ast_expr_list_from_ast_expr(indice);
    expr = isl_ast_expr_access(array, indices);

    return expr;
  }
	if (data->array->n_index == 0)
		return dereference(expr);
	if (!data->array->linearize)
		return expr;

	return autosa_local_array_info_linearize_index(data->local_array, expr);
}

/* This function is called for each instance of a user statement
 * in the kernel "kernel", identified by "autosa_stmt".
 * "kernel" may be NULL if we are not inside a kernel.
 *
 * We attach a struct autosa_kernel_stmt to the "node", containing
 * a computed AST expression for each access, through an annotation
 * with name "user".
 * These AST expressions are computed from iterator_map,
 * which expresses the domain
 * elements in terms of the generated loops, and sched2copy,
 * which expresses the outer copy_schedule_dim dimensions of
 * the kernel schedule computed by PPCG in terms of the generated loops.
 */
static __isl_give isl_ast_node *create_domain_leaf_module(
	struct autosa_kernel *kernel, __isl_take isl_ast_node *node,
	__isl_keep isl_ast_build *build, struct autosa_stmt *autosa_stmt)
{
	struct autosa_transform_data data;
	struct autosa_kernel_stmt *stmt;
	isl_ctx *ctx;
	isl_id *id;
	isl_pw_multi_aff *sched2copy;
	isl_map *map;
	isl_pw_multi_aff *iterator_map;
	isl_union_map *schedule;

	if (!node)
		return NULL;
	ctx = isl_ast_node_get_ctx(node);

	stmt = isl_calloc_type(ctx, struct autosa_kernel_stmt);
	if (!stmt)
		return isl_ast_node_free(node);

	schedule = isl_ast_build_get_schedule(build); 
	map = isl_map_reverse(isl_map_from_union_map(schedule));
	iterator_map = isl_pw_multi_aff_from_map(map);
	if (kernel)
		sched2copy = compute_sched_to_copy(kernel,
					isl_pw_multi_aff_copy(iterator_map)); 
	else
		sched2copy = NULL;

	stmt->type = AUTOSA_KERNEL_STMT_DOMAIN;
	stmt->u.d.stmt = autosa_stmt;

	data.kernel = kernel;
	data.accesses = stmt->u.d.stmt->accesses;
	data.iterator_map = iterator_map;
	data.sched2copy = sched2copy;
	stmt->u.d.ref2expr = pet_stmt_build_ast_exprs(stmt->u.d.stmt->stmt,
					    build, &transform_index_module, &data,
					    &transform_expr_module, &data);

	isl_pw_multi_aff_free(iterator_map);
	isl_pw_multi_aff_free(sched2copy);

	id = isl_id_alloc(ctx, "user", stmt);
	id = isl_id_set_free_user(id, &autosa_kernel_stmt_free);
	if (!id)
		autosa_kernel_stmt_free(stmt);
	return isl_ast_node_set_annotation(node, id);
}

/* Extract the is_filter field from the I/O statement type.
 * The I/O statement type is in the format of:
 * in/out_trans[_dram].[fifo_name].[is_filter].[is_buffer].[sched_depth].[param_id]
 */
static int extract_is_filter(const char *type)
{
  char ch;
  int loc = 0;
  int n_dot = 0;
  int val;

  while ((ch = type[loc]) != '\0') {
    if (ch == '.')
      n_dot++;
    if (n_dot == 2)
      break;
    loc++;
  }

  loc++;
  ch = type[loc];
  val = ch - '0';
  return val;
}

/* Extract the is_buffer field from the I/O statemnt type.
 * The I/O statement type is in the format of:
 * in/out_trans[_dram].[fifo_name].[is_filter].[is_buffer].[sched_depth].[param_id].[pack_lane]
 * or 
 * in/out.[fifo_name].[pack_lane].[nxt_pack_lane]
 */
static int extract_is_buffer(const char *type)
{
  char ch;
  int loc = 0;
  int n_dot = 0;
  int val;

  while ((ch = type[loc]) != '\0') {
    if (ch == '.')
      n_dot++;
    if (n_dot == 3)
      break;
    loc++;
  }

  loc++;
  ch = type[loc];
  val = ch - '0';
  return val;
}

/* Extract the sched_depth field from the I/O statemnt type.
 * The I/O statement type is in the format of:
 * in/out_trans[_dram].[fifo_name].[is_filter].[is_buffer].[sched_depth].[param_id].[pack_lane]
 * or 
 * in/out.[fifo_name].[pack_lane].[nxt_pack_lane]
 */
static int extract_sched_depth(isl_ctx *ctx, const char *type) 
{
  int loc = 0;
  char ch;
  int dot_time = 0;
  isl_printer *p_str;
  char *depth_str;
  int depth;

  while ((ch = type[loc]) != '\0') {
    if (ch == '.') 
      dot_time++;
    if (dot_time == 4)
      break;
    loc++;
  }

  p_str = isl_printer_to_str(ctx);
  loc++;
  while (((ch = type[loc]) != '\0') && ((ch = type[loc]) != '.')) {
    char buf[2];
    buf[0] = ch;
    buf[1] = '\0';
    p_str = isl_printer_print_str(p_str, buf);
    loc++;
  }

  depth_str = isl_printer_get_str(p_str);
  depth = atoi(depth_str);
  free(depth_str);
  isl_printer_free(p_str);

  return depth;
}

/* Extract the param_id field from the I/O statemnt type.
 * The I/O statement type is in the format of:
 * in/out_trans[_dram].[fifo_name].[is_filter].[is_buffer].[sched_depth].[param_id].[pack_lane]
 * or 
 * in/out.[fifo_name].[pack_lane].[nxt_pack_lane]
 */
static int extract_param_id(isl_ctx *ctx, const char *type)
{
  int loc = 0;
  char ch;
  int dot_time = 0;
  isl_printer *p_str;
  char *depth_str;
  int depth;

  while ((ch = type[loc]) != '\0') {
    if (ch == '.') 
      dot_time++;
    if (dot_time == 5)
      break;
    loc++;
  }

  p_str = isl_printer_to_str(ctx);
  loc++;
  while (((ch = type[loc]) != '\0') && ((ch = type[loc]) != '.')) {
    char buf[2];
    buf[0] = ch;
    buf[1] = '\0';
    p_str = isl_printer_print_str(p_str, buf);
    loc++;
  }

  depth_str = isl_printer_get_str(p_str);
  depth = atoi(depth_str);
  free(depth_str);
  isl_printer_free(p_str);

  return depth;
}

/* Extract the data_pack field from the I/O statemnt type.
 * The I/O statement type is in the format of:
 * in/out_trans[_dram].[fifo_name].[is_filter].[is_buffer].[sched_depth].[param_id].[pack_lane].[nxt_pack_lane]
 * or 
 * in/out.[fifo_name].[pack_lane].[nxt_pack_lane]
 */
static int extract_data_pack(isl_ctx *ctx, const char *type, int is_trans)
{
  int loc = 0;
  char ch;
  int dot_time = 0;
  isl_printer *p_str;
  char *depth_str;
  int depth;

  while ((ch = type[loc]) != '\0') {
    if (ch == '.') 
      dot_time++;
    if (dot_time == (is_trans? 6 : 2))
      break;
    loc++;
  }

  if (dot_time < (is_trans? 6 : 2))
    return -1;

  p_str = isl_printer_to_str(ctx);
  loc++;
  while (((ch = type[loc]) != '\0') && ((ch = type[loc]) != '.')) {
    char buf[2];
    buf[0] = ch;
    buf[1] = '\0';
    p_str = isl_printer_print_str(p_str, buf);
    loc++;
  }

  depth_str = isl_printer_get_str(p_str);
  depth = atoi(depth_str);
  free(depth_str);
  isl_printer_free(p_str);

  return depth; 
}

/* Extract the next_data_pack field from the I/O statemnt type.
 * The I/O statement type is in the format of:
 * in/out_trans[_dram].[fifo_name].[is_filter].[is_buffer].[sched_depth].[param_id].[pack_lane]
 * or 
 * in/out.[fifo_name].[pack_lane].[nxt_pack_lane]
 */
static int extract_next_data_pack(isl_ctx *ctx, const char *type, int is_trans) 
{
  int loc = 0;
  char ch;
  int dot_time = 0;
  isl_printer *p_str;
  char *depth_str;
  int depth;

  while ((ch = type[loc]) != '\0') {
    if (ch == '.') 
      dot_time++;
    if (dot_time == (is_trans? 7 : 3))
      break;
    loc++;
  }

  if (dot_time < (is_trans? 7 : 3))
    return -1;

  p_str = isl_printer_to_str(ctx);
  loc++;
  while (((ch = type[loc]) != '\0') && ((ch = type[loc]) != '.')) {
    char buf[2];
    buf[0] = ch;
    buf[1] = '\0';
    p_str = isl_printer_print_str(p_str, buf);
    loc++;
  }

  depth_str = isl_printer_get_str(p_str);
  depth = atoi(depth_str);
  free(depth_str);
  isl_printer_free(p_str);

  return depth; 
}

/* Extract the coalesce_depth field from the I/O statemnt type.
 * The I/O statement type is in the format of:
 * in/out_trans[_dram].[fifo_name].[is_filter].[is_buffer].[sched_depth].[param_id]
 * .[pack_lane].[nxt_pack_lane].[coalesce_depth].[coalesce_bound]
 * or 
 * in/out.[fifo_name].[pack_lane].[nxt_pack_lane]
 */
static int extract_coalesce_depth(isl_ctx *ctx, const char *type, int is_trans)
{
  int loc = 0;
  char ch;
  int dot_time = 0;
  isl_printer *p_str;
  char *depth_str;
  int depth;

  if (!is_trans) 
    return -1;

  while ((ch = type[loc]) != '\0') {
    if (ch == '.') 
      dot_time++;
    if (dot_time == 8)
      break;
    loc++;
  }

  if (dot_time < 8)
    return -1;

  p_str = isl_printer_to_str(ctx);
  loc++;
  while (((ch = type[loc]) != '\0') && ((ch = type[loc]) != '.')) {
    char buf[2];
    buf[0] = ch;
    buf[1] = '\0';
    p_str = isl_printer_print_str(p_str, buf);
    loc++;
  }

  depth_str = isl_printer_get_str(p_str);
  depth = atoi(depth_str);
  free(depth_str);
  isl_printer_free(p_str);

  return depth;   
}

/* Extract the coalesce_bound field from the I/O statemnt type.
 * The I/O statement type is in the format of:
 * in/out_trans[_dram].[fifo_name].[is_filter].[is_buffer].[sched_depth].[param_id]
 * .[pack_lane].[nxt_pack_lane].[coalesce_depth].[coalesce_bound]
 * or 
 * in/out.[fifo_name].[pack_lane].[nxt_pack_lane]
 */
static int extract_coalesce_bound(isl_ctx *ctx, const char *type, int is_trans)
{
  int loc = 0;
  char ch;
  int dot_time = 0;
  isl_printer *p_str;
  char *depth_str;
  int depth;

  if (!is_trans) 
    return -1;

  while ((ch = type[loc]) != '\0') {
    if (ch == '.') 
      dot_time++;
    if (dot_time == 9)
      break;
    loc++;
  }

  if (dot_time < 9)
    return -1;

  p_str = isl_printer_to_str(ctx);
  loc++;
  while (((ch = type[loc]) != '\0') && ((ch = type[loc]) != '.')) {
    char buf[2];
    buf[0] = ch;
    buf[1] = '\0';
    p_str = isl_printer_print_str(p_str, buf);
    loc++;
  }

  depth_str = isl_printer_get_str(p_str);
  depth = atoi(depth_str);
  free(depth_str);
  isl_printer_free(p_str);

  return depth;   
}

/* Given the fifo field from the I/O statement type.
 * The I/O statement type is in the format of:
 * in/out_trans[_dram].[fifo_name].[is_filter].[is_buffer].[sched_depth].[param_id].[pack_lane]
 * or 
 * in/out.[fifo_name].[pack_lane].[nxt_pack_lane]
 */
static __isl_give char *extract_fifo_suffix(isl_ctx *ctx, const char *type) 
{
  char *fifo_name;
  int loc = 0;
  char ch;
  isl_printer *p_str;
  int n_dot = 0;

  while ((ch = type[loc]) != '\0') {
    if (ch == '.')
      n_dot++;
    if (n_dot == 1)
      break;
    loc++;
  }

  p_str = isl_printer_to_str(ctx);
  loc++;
  while ((ch = type[loc]) != '\0') {
    if (ch == '.')
      break;
    char buf[2];
    buf[0] = ch;
    buf[1] = '\0';
    p_str = isl_printer_print_str(p_str, buf);
    loc++;
  }

  fifo_name = isl_printer_get_str(p_str);
  isl_printer_free(p_str);

  return fifo_name;
}

/* This function is called for each statement node in the AST
 * for transferring through fifos.
 * Attach a pointer to an autosa_kernel_stmt representing the io
 * statemet to the node.
 * The statement name is "in" or "out", depending on whether we are 
 * transferring in or out via fifos.
 *
 * The schedule is of the form
 *
 *  type[D -> A] -> L
 *
 * where D corresponds to the outer tile->depth dimensions of 
 * the kernel schedule, A to the global array and L to the outer 
 * generated AST schedule.
 * We compute the inverse and strip off the type, resulting in
 *
 *  L -> [D -> A]
 *
 * We combine this mapping with the group tiling
 *
 *  [D -> A] -> T
 *
 * resulting in
 *   
 *  L -> T
 *
 * and store the corresponding expressions in stmt->local_index,
 * where stmt points to the autosa_kernel_stmt that is attached to the node.
 */
static __isl_give isl_ast_node *create_io_leaf(struct autosa_kernel *kernel,
  struct autosa_hw_module *module, struct autosa_array_ref_group_pair *pair, 
  __isl_take isl_ast_node *node, __isl_keep isl_ast_build *build)
{
  struct autosa_kernel_stmt *stmt;
  struct autosa_array_tile *tile;
  isl_multi_aff *new_tiling;
  isl_map *access;
  const char *type;
  isl_pw_multi_aff *pma, *pma2;
  isl_space *space;
  isl_ast_expr *expr;
  isl_id *id;
  int is_trans;        // i/o transfer statment betwen on-chip modules
  int is_trans_dram;   // i/o transfer statement betwen dram and on-chip modules
  int is_trans_filter; // i/o transfer statment with filters
  int is_trans_buf;    // i/o transfer statment with local buffers
  int is_trans_boundary;
  int is_dummy;
  struct autosa_array_ref_group *group = pair->local_group;
  int depth;
  isl_ctx *ctx;

  stmt = isl_calloc_type(kernel->ctx, struct autosa_kernel_stmt); 
  if (!stmt)
    return isl_ast_node_free(node);
  ctx = kernel->ctx;

  /* type[D -> A] -> L */
  access = isl_map_from_union_map(isl_ast_build_get_schedule(build)); 
  isl_set *set = isl_map_domain(isl_set_unwrap(isl_map_domain(isl_map_copy(access)))); 
  depth = isl_set_dim(set, isl_dim_set);
  isl_set_free(set);

  type = isl_map_get_tuple_name(access, isl_dim_in);
  /* Classify the io stmt type. */
  is_trans = !prefixcmp(type, "in_trans") || !prefixcmp(type, "out_trans");
  is_trans_dram = !prefixcmp(type, "in_trans_dram") || !prefixcmp(type, "out_trans_dram");
  is_trans_boundary = !prefixcmp(type, "in_trans_boundary") || !prefixcmp(type, "out_trans_boundary");
  if (is_trans) {
    is_trans_filter = extract_is_filter(type);
    is_trans_buf = extract_is_buffer(type);
  }
  if (!is_trans) {
    is_dummy = !prefixcmp(type, "in_dummy") || !prefixcmp(type, "out_dummy");
  } else {
    is_dummy = 0;
  }
  stmt->u.i.dummy = is_dummy;
  stmt->u.i.in = type && !prefixcmp(type, "in");
  stmt->u.i.buf = is_trans_buf;
  stmt->u.i.filter = is_trans_filter;
  stmt->u.i.data_pack = extract_data_pack(ctx, type, is_trans);
  stmt->u.i.nxt_data_pack = extract_next_data_pack(ctx, type, is_trans);
  stmt->u.i.coalesce_depth = extract_coalesce_depth(ctx, type, is_trans);
  stmt->u.i.coalesce_bound = extract_coalesce_bound(ctx, type, is_trans);
    
  /* Compute the global index. */
  /* L -> type[D -> A] */
  access = isl_map_reverse(access); 
  pma = isl_pw_multi_aff_from_map(access); 
  pma = isl_pw_multi_aff_reset_tuple_id(pma, isl_dim_out); 

  space = isl_space_range(isl_pw_multi_aff_get_space(pma)); 
  space = isl_space_unwrap(space);
  /* [D -> A] -> A */
  pma2 = isl_pw_multi_aff_range_map(space); 
  /* L -> A */
  pma2 = isl_pw_multi_aff_pullback_pw_multi_aff(pma2,
            isl_pw_multi_aff_copy(pma));
  expr = isl_ast_build_access_from_pw_multi_aff(build, pma2); 
  if (group->array->linearize) {
    expr = autosa_local_array_info_linearize_index(group->local_array,
              expr);

    if (stmt->u.i.data_pack > 1) {
      /* Update the last dimension,
       * divide it by the data packing factor.
       */
      isl_ast_expr *arg, *div;
      arg = isl_ast_expr_get_op_arg(expr, 1);
      div = isl_ast_expr_from_val(isl_val_int_from_si(kernel->ctx, stmt->u.i.data_pack));
      arg = isl_ast_expr_div(arg, div); 
      expr = isl_ast_expr_set_op_arg(expr, 1, arg);
    }
  } else {
    if (stmt->u.i.data_pack > 1) {
      /* Update the last dimension,
       * divide it by the data packing factor.
       */
      int n_arg;
      isl_ast_expr *arg, *div;
      n_arg = isl_ast_expr_get_op_n_arg(expr);
      arg = isl_ast_expr_get_op_arg(expr, n_arg - 1);
      div = isl_ast_expr_from_val(isl_val_int_from_si(kernel->ctx, stmt->u.i.data_pack));
      arg = isl_ast_expr_div(arg, div);
      expr = isl_ast_expr_set_op_arg(expr, n_arg - 1, arg);
    }
  }

  stmt->u.i.index = expr; 

  /* Compute the local index. */
  tile = pair->local_tile;
  if (tile) {
    isl_ast_expr *arg, *div;
    int n_arg;

    /* [D -> A] -> T */
    pma2 = isl_pw_multi_aff_from_multi_aff(
              isl_multi_aff_copy(tile->tiling)); 
    if (tile->depth < depth) {
      /* Extend the D dimension to depth in pma2. */
      new_tiling = autosa_array_ref_group_recompute_tiling(tile, group, depth); 
      isl_pw_multi_aff_free(pma2);
      pma2 = isl_pw_multi_aff_from_multi_aff(new_tiling);
    }
    
    /* L -> T */
    pma2 = isl_pw_multi_aff_pullback_pw_multi_aff(pma2, pma);
    expr = isl_ast_build_access_from_pw_multi_aff(build, pma2);
    stmt->u.i.local_index = expr;
    stmt->u.i.reg = 0;
  } else {
    /* Create a scalar expr. */
    isl_printer *p_str;
    char *local_name;
    char buf[50];
    isl_ast_expr *array, *indice;
    isl_ast_expr_list *indices;

    isl_pw_multi_aff_free(pma);
    p_str = isl_printer_to_str(kernel->ctx);
    p_str = autosa_array_ref_group_print_name(group, p_str);
    local_name = isl_printer_get_str(p_str);
    isl_printer_free(p_str);
    sprintf(buf, "%s", local_name);
    free(local_name);

    id = isl_id_alloc(kernel->ctx, buf, NULL);
    array = isl_ast_expr_from_id(id);
    indice = isl_ast_expr_from_val(isl_val_zero(kernel->ctx));
    indices = isl_ast_expr_list_from_ast_expr(indice);
    expr = isl_ast_expr_access(array, indices);
    stmt->u.i.local_index = expr;
    stmt->u.i.reg = 1;
  }

  isl_printer *p_str = isl_printer_to_str(isl_ast_node_get_ctx(node));
  char *fifo_name = extract_fifo_suffix(ctx, type);
  p_str = isl_printer_print_str(p_str, fifo_name);
  free(fifo_name);
  stmt->u.i.fifo_name = isl_printer_get_str(p_str);
  isl_printer_free(p_str);
  
  stmt->u.i.group = pair->io_group;
  stmt->u.i.module = module;
  stmt->u.i.array = group->array;
  stmt->u.i.local_array = group->local_array;   
  if (is_trans) {
    if (is_trans_dram) {
      stmt->type = AUTOSA_KERNEL_STMT_IO_DRAM;
    } else {      
      stmt->type = AUTOSA_KERNEL_STMT_IO_TRANSFER;
      if (is_trans_filter) {
        stmt->u.i.filter_sched_depth = extract_sched_depth(ctx, type);
        stmt->u.i.filter_param_id = extract_param_id(ctx, type);
      } else {
        stmt->u.i.filter_sched_depth = -1;
        stmt->u.i.filter_param_id = -1;
      }
      if (is_trans_boundary) {
        stmt->u.i.boundary = 1;
      } else {
        stmt->u.i.boundary = 0;
      }
    }
  } else {
    stmt->type = AUTOSA_KERNEL_STMT_IO;
  }

  id = isl_id_alloc(kernel->ctx, "io", stmt);
  id = isl_id_set_free_user(id, &autosa_kernel_stmt_free);
  if (!id)
    autosa_kernel_stmt_free(stmt);
  return isl_ast_node_set_annotation(node, id);
}

/* Exatract the boundary field from the module call type, which is in the format of:
 * io_module.[].boundary
 * or 
 * module_call.module_name.boundary
 * */
static int extract_is_boundary(const char *type)
{
  char ch;
  int loc = 0;
  int n_dot = 0;

  while ((ch = type[loc]) != '\0') {
    if (ch == '.')
      n_dot++;
    if (n_dot == 2)
      break;
    loc++;
  }

  if (n_dot < 2)
    return 0;

  return 1;
}

/* Extract the module_name field from the module call type, which is in the format of:
 * module_call.module_name.boundary 
 */
static char *extract_module_name(isl_ctx *ctx, const char *type)
{
  char ch;
  int loc = 0;
  int n_dot = 0;
  isl_printer *p_str;
  char *module_name;

  while ((ch = type[loc]) != '\0') {
    if (ch == '.')
      n_dot++;
    if (n_dot == 1)
      break;
    loc++;
  }
  
  loc++;
  p_str = isl_printer_to_str(ctx);
  while ((ch = type[loc]) != '\0') {
    if (ch == '.')
      break;
    char buf[2];
    buf[0]= ch;
    buf[1] = '\0';
    p_str = isl_printer_print_str(p_str, buf);
    loc++;
  }

  module_name = isl_printer_get_str(p_str);
  isl_printer_free(p_str);

  return module_name; 
}

/* There are two types of module call statements:
 * module_call_upper and module_call_lower
 * For module_call_lower, if the module is connected to PEs,
 * we will calculate the AST expression io_pe_expr which is the 
 * PE indices described by IO ids.
 */
static __isl_give isl_ast_node *create_module_call_leaf(
  struct autosa_kernel *kernel,
  __isl_take isl_ast_node *node, struct autosa_hw_module *module,
  struct autosa_pe_dummy_module *pe_dummy_module,
  struct autosa_array_ref_group *group, const char *name, 
  __isl_keep isl_ast_build *build)
{
  struct autosa_kernel_stmt *stmt;
  isl_id *id;
  isl_ctx *ctx;
  isl_multi_aff *trans;
  isl_map *map;
  isl_pw_multi_aff *pma;
  isl_ast_expr *expr;

  ctx = isl_ast_node_get_ctx(node);
  stmt = isl_calloc_type(ctx, struct autosa_kernel_stmt);
  if (!stmt)
    return isl_ast_node_free(node);
 
  stmt->type = AUTOSA_KERNEL_STMT_MODULE_CALL;
  stmt->u.m.module = module;
  stmt->u.m.group = group;
  stmt->u.m.boundary = extract_is_boundary(name);
  stmt->u.m.module_name = extract_module_name(ctx, name);
  stmt->u.m.dummy = !suffixcmp(stmt->u.m.module_name, "dummy");
  stmt->u.m.pe_dummy_module = pe_dummy_module;
  if (!prefixcmp(name, "module_call_lower")) {
    stmt->u.m.lower = 1;
    stmt->u.m.upper = 0;
  } else if (!prefixcmp(name, "module_call_upper")) {
    stmt->u.m.lower = 0;
    stmt->u.m.upper = 1;
  } else {
    stmt->u.m.lower = 0;
    stmt->u.m.upper = 0;
  }

  if (stmt->u.m.lower) {
    if (!stmt->u.m.boundary) {
      if ((module->type == IO_MODULE || module->type == DRAIN_MODULE) 
            && !group->io_pe_expr) {
        if (module->to_pe) {
          isl_union_map *umap = isl_ast_build_get_schedule(build);
          isl_union_set *uset = isl_union_map_range(umap);
          isl_set *set = isl_set_from_union_set(uset);
          isl_map *map = isl_set_identity(set);
          map = isl_map_flatten_range(map);
          trans = isl_multi_aff_copy(group->io_trans);
          isl_map *map2 = isl_map_from_multi_aff(trans);
          map2 = isl_map_reverse(map2);
          map = isl_map_apply_range(map, map2);
          isl_pw_multi_aff *pma = isl_pw_multi_aff_from_map(map);
          expr = isl_ast_build_access_from_pw_multi_aff(build, pma);
          group->io_pe_expr = expr;
        }
      }
    }
    /* boundary module */
    if (stmt->u.m.boundary) {
      if ((module->type == IO_MODULE || module->type == DRAIN_MODULE) && !group->io_pe_expr_boundary) {
        if (module->to_pe) {
          isl_union_map *umap = isl_ast_build_get_schedule(build);
          isl_union_set *uset = isl_union_map_range(umap);
          isl_set *set = isl_set_from_union_set(uset);
          isl_map *map = isl_set_identity(set);
          map = isl_map_flatten_range(map);
          trans = isl_multi_aff_copy(group->io_trans);
          isl_map *map2 = isl_map_from_multi_aff(trans);
          map2 = isl_map_reverse(map2);
          map = isl_map_apply_range(map, map2);
          isl_pw_multi_aff *pma = isl_pw_multi_aff_from_map(map);
          expr = isl_ast_build_access_from_pw_multi_aff(build, pma);
          group->io_pe_expr_boundary = expr;
        }
      }
    }
  }

  id = isl_id_alloc(ctx, "module_call", stmt);
  id = isl_id_set_free_user(id, &autosa_kernel_stmt_free);
  if (!id)
    autosa_kernel_stmt_free(stmt);
  return isl_ast_node_set_annotation(node, id);
}

/* For fifo decleration statements, we will compute the AST expressions of 
 * PE indices that are described by the IO ids if the fifo is connected to 
 * PEs.
 */
static __isl_give isl_ast_node *create_fifo_decl_leaf(
  struct autosa_kernel *kernel,
  __isl_take isl_ast_node *node, struct autosa_hw_module *module,
  struct autosa_array_ref_group *group, const char *name, 
  __isl_keep isl_ast_build *build)
{
  struct autosa_kernel_stmt *stmt;
  isl_id *id;
  isl_ctx *ctx;
  isl_multi_aff *trans;
  isl_map *map;
  isl_pw_multi_aff *pma;
  isl_ast_expr *expr;

  ctx = isl_ast_node_get_ctx(node);
  stmt = isl_calloc_type(ctx, struct autosa_kernel_stmt);
  if (!stmt)
    return isl_ast_node_free(node);

  /* Generate the AST expr of io_trans. */ 
  if (module->type == PE_MODULE && !group->io_L1_pe_expr) {
    isl_union_map *umap = isl_ast_build_get_schedule(build);
    isl_union_set *uset = isl_union_map_range(umap);
    isl_set *set = isl_set_from_union_set(uset);
    isl_map *map = isl_set_identity(set);
    map = isl_map_flatten_range(map);
    trans = group->io_L1_trans;
    isl_map *map2 = isl_map_from_multi_aff(isl_multi_aff_copy(trans));
    map2 = isl_map_reverse(map2);
    map = isl_map_apply_range(map, map2);
    isl_pw_multi_aff *pma = isl_pw_multi_aff_from_map(map);
    expr = isl_ast_build_access_from_pw_multi_aff(build, pma);
    group->io_L1_pe_expr = expr;
  } 

  stmt->type = AUTOSA_KERNEL_STMT_FIFO_DECL;
  stmt->u.m.module = module;
  stmt->u.m.group = group;
  if (!prefixcmp(name, "fifo_decl_boundary"))
    stmt->u.m.boundary = 1;
  else
    stmt->u.m.boundary = 0;
  id = isl_id_alloc(ctx, "fifo_decl", stmt);
  id = isl_id_set_free_user(id, &autosa_kernel_stmt_free);
  if (!id)
    autosa_kernel_stmt_free(stmt);
  return isl_ast_node_set_annotation(node, id);
}

/* Attach a statement to the user node that describes the IO module type.
 */
static __isl_give isl_ast_node *create_io_module_call_leaf(
  struct autosa_kernel *kernel,
  __isl_take isl_ast_node *node, struct autosa_hw_module *module,
  const char *name, __isl_keep isl_ast_build *build)
{
  isl_id *id;
  isl_ctx *ctx;
  struct autosa_kernel_stmt *stmt;
  
  ctx = isl_ast_node_get_ctx(node);
  stmt = isl_calloc_type(ctx, struct autosa_kernel_stmt);
  if (!stmt)
    return isl_ast_node_free(node);

  stmt->u.f.module = module;
  stmt->u.f.boundary = extract_is_boundary(name);
  if (!prefixcmp(name, "io_module.inter_trans"))
    stmt->type = AUTOSA_KERNEL_STMT_IO_MODULE_CALL_INTER_TRANS;
  else if (!prefixcmp(name, "io_module.intra_trans"))
    stmt->type = AUTOSA_KERNEL_STMT_IO_MODULE_CALL_INTRA_TRANS;
  else if (!prefixcmp(name, "io_module.inter_intra"))
    stmt->type = AUTOSA_KERNEL_STMT_IO_MODULE_CALL_INTER_INTRA;
  else if (!prefixcmp(name, "io_module.intra_inter"))
    stmt->type = AUTOSA_KERNEL_STMT_IO_MODULE_CALL_INTRA_INTER;
  else if (!prefixcmp(name, "io_module.state_handle"))
    stmt->type = AUTOSA_KERNEL_STMT_IO_MODULE_CALL_STATE_HANDLE;
  id = isl_id_alloc(ctx, name, stmt);
  id = isl_id_set_free_user(id, &autosa_kernel_stmt_free);
  if (!id)
    autosa_kernel_stmt_free(stmt);
  return isl_ast_node_set_annotation(node, id);
}

/* This function is called for each instance of a user statement
 * in the kernel. This may be one of the original user statements
 * or a statement introduced by AutoSA.
 *
 * We first check if the statement id corresponds to a autosa statement,
 * which indicates the statement is an original user statement. Any statement
 * that is not an original user statement has been introduced by AutoSA and
 * requires special handling.
 *
 * If the user statement is one of the original user statements, then we call
 * create_domain_leaf.  
 * If it is "init_device", then we call build_array_bounds.  
 * Otherwise, we check if it is a copy statement and call the appropriate 
 * functions.  
 * Statements that copy an array to/from the device do not need any 
 * further treatment. Neither does "clear_device".
 */
static __isl_give isl_ast_node *at_domain_module(__isl_take isl_ast_node *node,
	__isl_keep isl_ast_build *build, void *user)
{
	struct autosa_at_domain_data *data = (struct autosa_at_domain_data *)user;
	struct autosa_stmt *device_stmt;
	isl_ast_expr *expr, *arg;
	isl_id *id;
	int is_sync;
	const char *name;
	void *p;

	expr = isl_ast_node_user_get_expr(node);
	arg = isl_ast_expr_get_op_arg(expr, 0);
	id = isl_ast_expr_get_id(arg);
	name = isl_id_get_name(id);
	p = isl_id_get_user(id);
	isl_ast_expr_free(expr);
	isl_ast_expr_free(arg);

	device_stmt = find_stmt(data->prog, id);
  isl_id_free(id);

  if (device_stmt)
    return create_domain_leaf_module(data->kernel, node, build, device_stmt); 

  if (!prefixcmp(name, "to_device_") || !prefixcmp(name, "from_device_"))
    return node;
  if (!strcmp(name, "init_device"))
    return build_array_bounds(node, data->prog, build); 
  if (!strcmp(name, "clear_device"))
    return node;
  if (!strcmp(name, "read") || !strcmp(name, "write")) {
    struct autosa_array_ref_group *group = (struct autosa_array_ref_group *)p;
    return create_access_leaf(data->kernel, group, node, build);
  }
  if (!prefixcmp(name, "in") || !prefixcmp(name, "out")) {
    struct autosa_array_ref_group_pair *pair = (struct autosa_array_ref_group_pair *)p;
    return create_io_leaf(data->kernel, data->module, pair, node, build);
  }
  if (!prefixcmp(name, "module_call")) {
    /* module_call.[module_name]
     * module_call_lower.[module_name]
     */
    struct autosa_array_ref_group *group = NULL;
    if (!prefixcmp(name, "module_call_lower"))
      group = (struct autosa_array_ref_group *)p;
    return create_module_call_leaf(data->kernel, node, data->module, data->pe_dummy_module, group, name, build);
  }
  if (!prefixcmp(name, "fifo_decl")) {
    /* fifo_decl.[fifo_name]
     * fifo_decl_boundary.[fifo_name]
     */
    struct autosa_array_ref_group *group = (struct autosa_array_ref_group *)p;
    return create_fifo_decl_leaf(data->kernel, node, data->module, group, name, build);
  }
  if (!prefixcmp(name, "io_module")) {
    return create_io_module_call_leaf(data->kernel, node, data->module, name, build);
  }

  return node;
}

/* This function is called before the AST generator starts traversing
 * the schedule subtree of a node with mark "mark".
 *
 * If the mark is called "kernel", store the kernel pointer in data->kernel
 * for use in at_domain_module.
 * If the mark is called "module", store the kernel pointer in data->module
 * for use in at_domain_module.
 */
static isl_stat before_mark_module(__isl_keep isl_id *mark,
	__isl_keep isl_ast_build *build, void *user)
{
	struct autosa_at_domain_data *data = (struct autosa_at_domain_data *)user;

	if (!mark)
		return isl_stat_error;
  if (!strcmp(isl_id_get_name(mark), "kernel")) {
    data->kernel = (struct autosa_kernel *)isl_id_get_user(mark);
  }
	if (!strcmp(isl_id_get_name(mark), "module")) {
		data->module = (struct autosa_hw_module *)isl_id_get_user(mark);
	}
  if (!strcmp(isl_id_get_name(mark), "pe_dummy_module")) {
    data->pe_dummy_module = (struct autosa_pe_dummy_module *)isl_id_get_user(mark);
  }
  if (!strcmp(isl_id_get_name(mark), "io_module.inter_trans") ||
      !strcmp(isl_id_get_name(mark), "io_module.intra_trans")) {
    data->filter_buffer = 1;
  }
  if (!strcmp(isl_id_get_name(mark), "hls_pipeline")) {
    data->under_pipeline = 1;
  }
  if (!strcmp(isl_id_get_name(mark), "hls_unroll")) {
    data->under_unroll = 1;
  }

	return isl_stat_ok;
}

/* This function is called after the AST generator has finished traversing
 * the schedule subtree of a mark node. "node" points to the corresponding
 * mark AST node.
 *
 * If the mark is called "module", then replace "node" by a user node
 * that "calls" the module, representing the launch of the module.
 * The original "node" is stored inside the module object so that
 * it can be used to print the device code.
 * Also clear data->module.
 */
static __isl_give isl_ast_node *after_mark_module(__isl_take isl_ast_node *node,
        __isl_keep isl_ast_build *build, void *user)
{
	isl_ctx *ctx;
	isl_id *id;
	isl_ast_expr *expr;
	isl_ast_expr_list *list;
	struct autosa_kernel *kernel;
	struct autosa_at_domain_data *data = (struct autosa_at_domain_data *)user;
  struct autosa_hw_module *module;
  struct autosa_pe_dummy_module *pe_dummy_module;

	ctx = isl_ast_node_get_ctx(node);
	id = isl_ast_node_mark_get_id(node);
	if (!id)
		return isl_ast_node_free(node);

  if (!strcmp(isl_id_get_name(id), "kernel") && data->kernel) {
    isl_id_free(id);
    if (!data->kernel->space)
      data->kernel->space = isl_ast_build_get_schedule_space(build);
    data->kernel = NULL;
    return node;
  }
  if (!strcmp(isl_id_get_name(id), "io_module.inter_trans")) {
    module = data->module;
    if (!module->inter_space)
      module->inter_space = isl_ast_build_get_schedule_space(build);
    
    if (!data->boundary)
      module->inter_tree = isl_ast_node_mark_get_node(node);
    else
      module->boundary_inter_tree = isl_ast_node_mark_get_node(node);
    isl_ast_node_free(node);
    
    expr = isl_ast_expr_from_id(isl_id_copy(id));
    list = isl_ast_expr_list_alloc(ctx, 0);
    expr = isl_ast_expr_call(expr, list);
    node = isl_ast_node_alloc_user(expr);
    node = isl_ast_node_set_annotation(node, id);  

    return node;
  }
  if (!strcmp(isl_id_get_name(id), "io_module.intra_trans")) {
    module = data->module;
    if (!module->intra_space)
      module->intra_space = isl_ast_build_get_schedule_space(build);

    module->intra_tree = isl_ast_node_mark_get_node(node);
    isl_ast_node_free(node);

    expr = isl_ast_expr_from_id(isl_id_copy(id));
    list = isl_ast_expr_list_alloc(ctx, 0);
    expr = isl_ast_expr_call(expr, list);
    node = isl_ast_node_alloc_user(expr);
    node = isl_ast_node_set_annotation(node, id);

    return node;
  }
  if (!strcmp(isl_id_get_name(id), "hls_pipeline")) {
    isl_id_free(id);
    data->under_pipeline = 0;

    return node;
  }
  if (!strcmp(isl_id_get_name(id), "hls_unroll")) {
    isl_id_free(id);
    data->under_unroll = 0;

    return node;
  }

	if (strcmp(isl_id_get_name(id), "module") || !data->module) {
		isl_id_free(id);
		return node;
	}
  /* Prepare for boundary I/O module. */
  if (data->boundary && data->filter_buffer == 0) {
    module = data->module;
    data->module = NULL;
    module->boundary_tree = isl_ast_node_mark_get_node(node);
    isl_ast_node_free(node);
    if (!module->space)
      module->space = isl_ast_build_get_schedule_space(build);

	  expr = isl_ast_expr_from_id(isl_id_copy(id));
	  list = isl_ast_expr_list_alloc(ctx, 0);
	  expr = isl_ast_expr_call(expr, list);
	  node = isl_ast_node_alloc_user(expr);
	  node = isl_ast_node_set_annotation(node, id);

    return node;
  }

  /* Prepare for PE dummy module */
  if (data->pe_dummy && data->filter_buffer == 0) {
    module = data->module;
    data->module = NULL;
    pe_dummy_module = data->pe_dummy_module;
    data->pe_dummy_module = NULL;
    pe_dummy_module->device_tree = isl_ast_node_mark_get_node(node);
    isl_ast_node_free(node);
    if (!module->space)
      module->space = isl_ast_build_get_schedule_space(build);

	  expr = isl_ast_expr_from_id(isl_id_copy(id));
	  list = isl_ast_expr_list_alloc(ctx, 0);
	  expr = isl_ast_expr_call(expr, list);
	  node = isl_ast_node_alloc_user(expr);
	  node = isl_ast_node_set_annotation(node, id);

    return node;
  }

  if (!data->boundary && data->filter_buffer == 0) {
    module = data->module;
    data->module = NULL;
    module->device_tree = isl_ast_node_mark_get_node(node);
	  isl_ast_node_free(node);
    if (!module->space)
      module->space = isl_ast_build_get_schedule_space(build);

	  expr = isl_ast_expr_from_id(isl_id_copy(id));
	  list = isl_ast_expr_list_alloc(ctx, 0);
	  expr = isl_ast_expr_call(expr, list);
	  node = isl_ast_node_alloc_user(expr);
	  node = isl_ast_node_set_annotation(node, isl_id_copy(id));
  }
  isl_id_free(id);

	return node;
}

/* Generate AST from the schedule for AutoSA hardware modules. 
 */
static __isl_give isl_ast_node *autosa_generate_ast_from_schedule(
  __isl_take isl_schedule *schedule,
  struct autosa_at_domain_data data, struct autosa_gen *gen)
{
  isl_ast_build *build;
  isl_ast_node *tree;
  isl_id_list *iterators;
  int depth;

  if (schedule == NULL)
    return NULL;

  depth = 0;
  if (isl_schedule_foreach_schedule_node_top_down(schedule, &update_depth,
        &depth) < 0)
    schedule = isl_schedule_free(schedule);
  build = isl_ast_build_alloc(gen->prog->ctx);
  iterators = ppcg_scop_generate_names(gen->prog->scop, depth, "c");
  build = isl_ast_build_set_iterators(build, iterators);
  build = isl_ast_build_set_at_each_domain(build, &at_domain_module, &data);
  build = isl_ast_build_set_before_each_mark(build, &before_mark_module, &data);
  build = isl_ast_build_set_after_each_mark(build, &after_mark_module, &data);

  if (gen->prog->scop->options->debug->dump_final_schedule)
    isl_schedule_dump(schedule);
  tree = isl_ast_build_node_from_schedule(build, schedule);
  isl_ast_build_free(build);

  return tree;
}

/* There are three schedules to handle in this module:
 * - outer loop schedule
 * - inter trans schedule
 * - intra trans schedule
 * We will first generate AST for inter trans function and intra trans function.
 * The AST trees below the inter trans and intra trans mark are stored 
 * seperately.
 * The outer loop AST will print out these two AST trees while handling 
 * the inter trans and intra trans function calls.
 */
isl_stat sa_filter_buffer_io_module_generate_code(struct autosa_gen *gen,
  struct autosa_hw_module *module)
{
  isl_schedule *schedule;
  struct autosa_at_domain_data data;
  isl_ast_node *tree;

  /* Generate AST for inter transfer function call. */
  schedule = module->inter_sched;
  autosa_at_domain_data_init(&data, gen);
  tree = autosa_generate_ast_from_schedule(schedule, data, gen);
  isl_ast_node_free(tree);

  if (module->boundary) {
    /* Generate boundary module AST. */
    schedule = module->boundary_inter_sched;
    autosa_at_domain_data_init(&data, gen);
    data.boundary = 1;
    tree = autosa_generate_ast_from_schedule(schedule, data, gen);
    isl_ast_node_free(tree);
  }

  /* Generate AST for intra transfer function call. */
  schedule = module->intra_sched;
  autosa_at_domain_data_init(&data, gen);
  tree = autosa_generate_ast_from_schedule(schedule, data, gen);
  isl_ast_node_free(tree);
 
  /* Generate AST for outer loop function call. */
  schedule = module->outer_sched;
  autosa_at_domain_data_init(&data, gen);
  tree = autosa_generate_ast_from_schedule(schedule, data, gen);
  module->tree = tree;

  if (module->boundary) {
    /* Generate boundary module AST. */
    schedule = module->boundary_outer_sched;
    autosa_at_domain_data_init(&data, gen);
    data.boundary = 1;
    tree = autosa_generate_ast_from_schedule(schedule, data, gen);
    isl_ast_node_free(tree);
  }

  return isl_stat_ok;
}

/* Use isl to generate code for the hw module from "schedule".
 * The device code of the hw module is marked by "module" mark nodes in the 
 * schedule tree, containing a pointer to a autosa_hw_module object.
 * The returned AST only contains the AST for the host code.
 * The ASTs for the device code are embedded in autosa_hw_module objects
 * attached to the leaf nodes that call "module".
 */
isl_stat sa_module_generate_code(struct autosa_gen *gen,
  struct autosa_hw_module *module)
{
  isl_schedule *schedule;
  struct autosa_at_domain_data data;
  isl_ast_node *tree;

  schedule = module->sched;
  autosa_at_domain_data_init(&data, gen);
  tree = autosa_generate_ast_from_schedule(schedule, data, gen);
  module->tree = tree;

  if (module->boundary) {
    /* Generate boundary module AST */
    schedule = module->boundary_sched;
    autosa_at_domain_data_init(&data, gen);
    data.boundary = 1;
    tree = autosa_generate_ast_from_schedule(schedule, data, gen);
    isl_ast_node_free(tree); 
  }

  if (module->n_pe_dummy_modules > 0) {
    /* Generate dummy module AST */
    for (int i = 0; i < module->n_pe_dummy_modules; i++) {
      struct autosa_pe_dummy_module *dummy_module = module->pe_dummy_modules[i];
      schedule = dummy_module->sched;
      autosa_at_domain_data_init(&data, gen);
      data.pe_dummy = 1;
      data.pe_dummy_module = dummy_module;
      tree = autosa_generate_ast_from_schedule(schedule, data, gen);
      isl_ast_node_free(tree);
    }
  }

  return isl_stat_ok;
}

/* This function is called after the AST generator has finished traversing
 * the schedule subtree of a mark node. "node" points to the corresponding
 * mark AST node.
 *
 * If the mark is called "fifo_decl", then replace "node" by a user node
 * that "calls" the fifo_decl, representing the printing of fifo decls.
 * We will store the AST node into the fifo_decl_wrapped_trees.
 */
static __isl_give isl_ast_node *after_mark_fifo_decl(
  __isl_take isl_ast_node *node,
  __isl_keep isl_ast_build *build, void *user)
{
	isl_ctx *ctx;
	isl_id *id;
	isl_ast_expr *expr;
	isl_ast_expr_list *list;
	struct autosa_kernel *kernel;
	struct autosa_at_domain_data *data = (struct autosa_at_domain_data *)user;
  struct autosa_hw_module *module;
  struct autosa_hw_top_module *top;

	ctx = isl_ast_node_get_ctx(node);
	id = isl_ast_node_mark_get_id(node);
	if (!id)
		return isl_ast_node_free(node);

  if (!strcmp(isl_id_get_name(id), "kernel") && data->kernel) {
    isl_id_free(id);
    if (!data->kernel->space)
      data->kernel->space = isl_ast_build_get_schedule_space(build);
    data->kernel = NULL;
    return node;
  }
	if (strcmp(isl_id_get_name(id), "module") || !data->module) {
		isl_id_free(id);
		return node;
	}
  top = data->top;
  data->top = NULL;
  top->n_fifo_decl_wrapped++;
  top->fifo_decl_wrapped_trees = (isl_ast_node **)realloc(
    top->fifo_decl_wrapped_trees,
    top->n_fifo_decl_wrapped * sizeof(isl_ast_node *));
  top->fifo_decl_wrapped_trees[top->n_fifo_decl_wrapped - 1] = 
    isl_ast_node_mark_get_node(node);
	isl_ast_node_free(node);

	expr = isl_ast_expr_from_id(isl_id_copy(id));
	list = isl_ast_expr_list_alloc(ctx, 0);
	expr = isl_ast_expr_call(expr, list);
	node = isl_ast_node_alloc_user(expr);
	node = isl_ast_node_set_annotation(node, id);

	return node;
}

/* Generate code for declaring fifos given the input schedule "schedule". 
 */
__isl_give isl_ast_node *sa_fifo_decl_generate_code(
  struct autosa_gen *gen, __isl_take isl_schedule *schedule)
{
  struct autosa_at_domain_data data;
  isl_ast_build *build;
  isl_ast_node *tree;
  isl_id_list *iterators;

  int depth;

  if (schedule == NULL)
    return NULL;

  data.prog = gen->prog;
  data.kernel = NULL;
  data.module = NULL;
  data.top = gen->hw_top_module;

  depth = 0;
  if (isl_schedule_foreach_schedule_node_top_down(schedule, &update_depth,
        &depth) < 0)
    schedule = isl_schedule_free(schedule);
  build = isl_ast_build_alloc(gen->prog->ctx);
  iterators = ppcg_scop_generate_names(gen->prog->scop, depth, "c");
  build = isl_ast_build_set_iterators(build, iterators);
  build = isl_ast_build_set_at_each_domain(build, &at_domain_module, &data);
  build = isl_ast_build_set_before_each_mark(build, &before_mark_module, &data);
  build = isl_ast_build_set_after_each_mark(build, &after_mark_fifo_decl, &data);
  if (gen->prog->scop->options->debug->dump_final_schedule)
    isl_schedule_dump(schedule);
  tree = isl_ast_build_node_from_schedule(build, schedule);
  isl_ast_build_free(build);

  return tree; 
}

/* This function is called after the AST generator has finished traversing
 * the schedule subtree of a mark node. "node" points to the corresponding
 * mark AST node.
 *
 * If the mark is called "module call", then replace "node" by a user node
 * that "calls" the module call, representing the printing of module calls.
 * We will store the AST node into the module_call_wrapped_trees.
 */
static __isl_give isl_ast_node *after_mark_module_call(
  __isl_take isl_ast_node *node,
  __isl_keep isl_ast_build *build, void *user)
{
	isl_ctx *ctx;
	isl_id *id;
	isl_ast_expr *expr;
	isl_ast_expr_list *list;
	struct autosa_kernel *kernel;
	struct autosa_at_domain_data *data = (struct autosa_at_domain_data *)user;
  struct autosa_hw_module *module;
  struct autosa_hw_top_module *top;

	ctx = isl_ast_node_get_ctx(node);
	id = isl_ast_node_mark_get_id(node);
	if (!id)
		return isl_ast_node_free(node);

  if (!strcmp(isl_id_get_name(id), "kernel") && data->kernel) {
    isl_id_free(id);
    if (!data->kernel->space)
      data->kernel->space = isl_ast_build_get_schedule_space(build);
    data->kernel = NULL;
    return node;
  }
	if (strcmp(isl_id_get_name(id), "module") || !data->module) {
		isl_id_free(id);
		return node;
	}
  top = data->top;
  data->top = NULL;
  top->n_module_call_wrapped++;
  top->module_call_wrapped_trees = (isl_ast_node **)realloc(
    top->module_call_wrapped_trees,
    top->n_module_call_wrapped * sizeof(isl_ast_node *));
  top->module_call_wrapped_trees[top->n_module_call_wrapped - 1] = 
    isl_ast_node_mark_get_node(node);
	isl_ast_node_free(node);

	expr = isl_ast_expr_from_id(isl_id_copy(id));
	list = isl_ast_expr_list_alloc(ctx, 0);
	expr = isl_ast_expr_call(expr, list);
	node = isl_ast_node_alloc_user(expr);
	node = isl_ast_node_set_annotation(node, id);

	return node;
}

/* Generate code for calling modules given the input schedule "schedule". 
 */
__isl_give isl_ast_node *sa_module_call_generate_code(
  struct autosa_gen *gen, __isl_take isl_schedule *schedule)
{
  struct autosa_at_domain_data data;
  isl_ast_build *build;
  isl_ast_node *tree;
  isl_id_list *iterators;

  int depth;

  if (schedule == NULL)
    return NULL;

  data.prog = gen->prog;
  data.kernel = NULL;
  data.module = NULL;
  data.pe_dummy_module = NULL;
  data.top = gen->hw_top_module;

  depth = 0;
  if (isl_schedule_foreach_schedule_node_top_down(schedule, &update_depth,
        &depth) < 0)
    schedule = isl_schedule_free(schedule);
  build = isl_ast_build_alloc(gen->prog->ctx);
  iterators = ppcg_scop_generate_names(gen->prog->scop, depth, "c");
  build = isl_ast_build_set_iterators(build, iterators);
  build = isl_ast_build_set_at_each_domain(build, &at_domain_module, &data);
  build = isl_ast_build_set_before_each_mark(build, &before_mark_module, &data);
  build = isl_ast_build_set_after_each_mark(build, &after_mark_module_call, &data);
  if (gen->prog->scop->options->debug->dump_final_schedule)
    isl_schedule_dump(schedule);
  tree = isl_ast_build_node_from_schedule(build, schedule);
  isl_ast_build_free(build);

  return tree; 
}

/* Generate AST for module calls and fifo decls in the top module.
 */
isl_stat sa_top_module_generate_code(struct autosa_gen *gen) 
{
  struct autosa_hw_top_module *top = gen->hw_top_module;
  /* fifo declaration */
  top->fifo_decl_trees = (isl_ast_node **)malloc(
    top->n_fifo_decls * sizeof(isl_ast_node *));
  for (int i = 0; i < top->n_fifo_decls; i++) {
    top->fifo_decl_trees[i] = sa_fifo_decl_generate_code(gen,
        top->fifo_decl_scheds[i]);
  }

  /* module call */
  top->module_call_trees = (isl_ast_node **)malloc(
    top->n_module_calls * sizeof(isl_ast_node *));
  for (int i = 0; i < top->n_module_calls; i++) {
    top->module_call_trees[i] = sa_module_call_generate_code(gen,
        top->module_call_scheds[i]);
  }

  return isl_stat_ok;
}