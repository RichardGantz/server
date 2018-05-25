/* Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2009, 2017, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/**
  @file

  @brief
  This file defines all numerical functions
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "sql_plugin.h"
#include "sql_priv.h"
/*
  It is necessary to include set_var.h instead of item.h because there
  are dependencies on include order for set_var.h and item.h. This
  will be resolved later.
*/
#include "sql_class.h"                          // set_var.h: THD
#include "set_var.h"
#include "slave.h"				// for wait_for_master_pos
#include "sql_show.h"                           // append_identifier
#include "strfunc.h"                            // find_type
#include "sql_parse.h"                          // is_update_query
#include "sql_acl.h"                            // EXECUTE_ACL
#include "mysqld.h"                             // LOCK_short_uuid_generator
#include "rpl_mi.h"
#include "sql_time.h"
#include <m_ctype.h>
#include <hash.h>
#include <time.h>
#include <ft_global.h>
#include <my_bit.h>

#include "sp_head.h"
#include "sp_rcontext.h"
#include "sp.h"
#include "set_var.h"
#include "debug_sync.h"
#include "sql_cte.h"

#ifdef NO_EMBEDDED_ACCESS_CHECKS
#define sp_restore_security_context(A,B) while (0) {}
#endif

bool check_reserved_words(LEX_STRING *name)
{
  if (!my_strcasecmp(system_charset_info, name->str, "GLOBAL") ||
      !my_strcasecmp(system_charset_info, name->str, "LOCAL") ||
      !my_strcasecmp(system_charset_info, name->str, "SESSION"))
    return TRUE;
  return FALSE;
}


/**
   Test if the sum of arguments overflows the ulonglong range.
*/
static inline bool test_if_sum_overflows_ull(ulonglong arg1, ulonglong arg2)
{
  return ULONGLONG_MAX - arg1 < arg2;
}


void Item_args::set_arguments(THD *thd, List<Item> &list)
{
  arg_count= list.elements;
  if (arg_count <= 2)
  {
    args= tmp_arg;
  }
  else if (!(args= (Item**) thd->alloc(sizeof(Item*) * arg_count)))
  {
    arg_count= 0;
    return;
  }
  uint i= 0;
  List_iterator_fast<Item> li(list);
  Item *item;
  while ((item= li++))
    args[i++]= item;
}


Item_args::Item_args(THD *thd, const Item_args *other)
  :arg_count(other->arg_count)
{
  if (arg_count <= 2)
  {
    args= tmp_arg;
  }
  else if (!(args= (Item**) thd->alloc(sizeof(Item*) * arg_count)))
  {
    arg_count= 0;
    return;
  }
  memcpy(args, other->args, sizeof(Item*) * arg_count);
}


void Item_func::sync_with_sum_func_and_with_field(List<Item> &list)
{
  List_iterator_fast<Item> li(list);
  Item *item;
  while ((item= li++))
  {
    with_sum_func|= item->with_sum_func;
    with_window_func|= item->with_window_func;
    with_field|= item->with_field;
    with_param|= item->with_param;
  }
}


/*
  Resolve references to table column for a function and its argument

  SYNOPSIS:
  fix_fields()
  thd		Thread object
  ref		Pointer to where this object is used.  This reference
		is used if we want to replace this object with another
		one (for example in the summary functions).

  DESCRIPTION
    Call fix_fields() for all arguments to the function.  The main intention
    is to allow all Item_field() objects to setup pointers to the table fields.

    Sets as a side effect the following class variables:
      maybe_null        Set if any argument may return NULL
      with_sum_func     Set if any of the arguments contains a sum function
      with_window_func  Set if any of the arguments contain a window function
      with_field        Set if any of the arguments contains or is a field
      used_tables_cache Set to union of the tables used by arguments

      str_value.charset If this is a string function, set this to the
			character set for the first argument.
			If any argument is binary, this is set to binary

   If for any item any of the defaults are wrong, then this can
   be fixed in the fix_length_and_dec() function that is called
   after this one or by writing a specialized fix_fields() for the
   item.

  RETURN VALUES
  FALSE	ok
  TRUE	Got error.  Stored with my_error().
*/

bool
Item_func::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  Item **arg,**arg_end;
  uchar buff[STACK_BUFF_ALLOC];			// Max argument in function

  /*
    The Used_tables_and_const_cache of "this" was initialized by
    the constructor, or by Item_func::cleanup().
  */
  DBUG_ASSERT(used_tables_cache == 0);
  DBUG_ASSERT(const_item_cache == true);

  not_null_tables_cache= 0;

  /*
    Use stack limit of STACK_MIN_SIZE * 2 since
    on some platforms a recursive call to fix_fields
    requires more than STACK_MIN_SIZE bytes (e.g. for
    MIPS, it takes about 22kB to make one recursive
    call to Item_func::fix_fields())
  */
  if (check_stack_overrun(thd, STACK_MIN_SIZE * 2, buff))
    return TRUE;				// Fatal error if flag is set!
  if (arg_count)
  {						// Print purify happy
    for (arg=args, arg_end=args+arg_count; arg != arg_end ; arg++)
    {
      Item *item;
      /*
	We can't yet set item to *arg as fix_fields may change *arg
	We shouldn't call fix_fields() twice, so check 'fixed' field first
      */
      if ((!(*arg)->fixed && (*arg)->fix_fields(thd, arg)))
	return TRUE;				/* purecov: inspected */
      item= *arg;

      if (allowed_arg_cols)
      {
        if (item->check_cols(allowed_arg_cols))
          return 1;
      }
      else
      {
        /*  we have to fetch allowed_arg_cols from first argument */
        DBUG_ASSERT(arg == args); // it is first argument
        allowed_arg_cols= item->cols();
        DBUG_ASSERT(allowed_arg_cols); // Can't be 0 any more
      }

      if (item->maybe_null)
	maybe_null=1;

      with_sum_func= with_sum_func || item->with_sum_func;
      with_param= with_param || item->with_param;
      with_window_func= with_window_func || item->with_window_func;
      with_field= with_field || item->with_field;
      used_tables_and_const_cache_join(item);
      not_null_tables_cache|= item->not_null_tables();
      with_subselect|=        item->has_subquery();
    }
  }
  if (fix_length_and_dec())
    return TRUE;
  fixed= 1;
  return FALSE;
}

void
Item_func::quick_fix_field()
{
  Item **arg,**arg_end;
  if (arg_count)
  {
    for (arg=args, arg_end=args+arg_count; arg != arg_end ; arg++)
    {
      if (!(*arg)->fixed)
        (*arg)->quick_fix_field();
    }
  }
  fixed= 1;
}


bool
Item_func::eval_not_null_tables(void *opt_arg)
{
  Item **arg,**arg_end;
  not_null_tables_cache= 0;
  if (arg_count)
  {		
    for (arg=args, arg_end=args+arg_count; arg != arg_end ; arg++)
    {
      not_null_tables_cache|= (*arg)->not_null_tables();
    }
  }
  return FALSE;
}


void Item_func::fix_after_pullout(st_select_lex *new_parent, Item **ref,
                                  bool merge)
{
  Item **arg,**arg_end;

  used_tables_and_const_cache_init();
  not_null_tables_cache= 0;

  if (arg_count)
  {
    for (arg=args, arg_end=args+arg_count; arg != arg_end ; arg++)
    {
      (*arg)->fix_after_pullout(new_parent, arg, merge);
      Item *item= *arg;

      used_tables_and_const_cache_join(item);
      not_null_tables_cache|= item->not_null_tables();
    }
  }
}


void Item_func::traverse_cond(Cond_traverser traverser,
                              void *argument, traverse_order order)
{
  if (arg_count)
  {
    Item **arg,**arg_end;

    switch (order) {
    case(PREFIX):
      (*traverser)(this, argument);
      for (arg= args, arg_end= args+arg_count; arg != arg_end; arg++)
      {
	(*arg)->traverse_cond(traverser, argument, order);
      }
      break;
    case (POSTFIX):
      for (arg= args, arg_end= args+arg_count; arg != arg_end; arg++)
      {
	(*arg)->traverse_cond(traverser, argument, order);
      }
      (*traverser)(this, argument);
    }
  }
  else
    (*traverser)(this, argument);
}


bool Item_args::transform_args(THD *thd, Item_transformer transformer, uchar *arg)
{
  for (uint i= 0; i < arg_count; i++)
  {
    Item *new_item= args[i]->transform(thd, transformer, arg);
    if (!new_item)
      return true;
    /*
      THD::change_item_tree() should be called only if the tree was
      really transformed, i.e. when a new item has been created.
      Otherwise we'll be allocating a lot of unnecessary memory for
      change records at each execution.
    */
    if (args[i] != new_item)
      thd->change_item_tree(&args[i], new_item);
  }
  return false;
}


/**
  Transform an Item_func object with a transformer callback function.

    The function recursively applies the transform method to each
    argument of the Item_func node.
    If the call of the method for an argument item returns a new item
    the old item is substituted for a new one.
    After this the transformer is applied to the root node
    of the Item_func object. 
  @param transformer   the transformer callback function to be applied to
                       the nodes of the tree of the object
  @param argument      parameter to be passed to the transformer

  @return
    Item returned as the result of transformation of the root node
*/

Item *Item_func::transform(THD *thd, Item_transformer transformer, uchar *argument)
{
  DBUG_ASSERT(!thd->stmt_arena->is_stmt_prepare());
  if (transform_args(thd, transformer, argument))
    return 0;
  return (this->*transformer)(thd, argument);
}


/**
  Compile Item_func object with a processor and a transformer
  callback functions.

    First the function applies the analyzer to the root node of
    the Item_func object. Then if the analizer succeeeds (returns TRUE)
    the function recursively applies the compile method to each argument
    of the Item_func node.
    If the call of the method for an argument item returns a new item
    the old item is substituted for a new one.
    After this the transformer is applied to the root node
    of the Item_func object. 
    The compile function is not called if the analyzer returns NULL
    in the parameter arg_p. 

  @param analyzer      the analyzer callback function to be applied to the
                       nodes of the tree of the object
  @param[in,out] arg_p parameter to be passed to the processor
  @param transformer   the transformer callback function to be applied to the
                       nodes of the tree of the object
  @param arg_t         parameter to be passed to the transformer

  @return
    Item returned as the result of transformation of the root node
*/

Item *Item_func::compile(THD *thd, Item_analyzer analyzer, uchar **arg_p,
                         Item_transformer transformer, uchar *arg_t)
{
  if (!(this->*analyzer)(arg_p))
    return 0;
  if (*arg_p && arg_count)
  {
    Item **arg,**arg_end;
    for (arg= args, arg_end= args+arg_count; arg != arg_end; arg++)
    {
      /* 
        The same parameter value of arg_p must be passed
        to analyze any argument of the condition formula.
      */
      uchar *arg_v= *arg_p;
      Item *new_item= (*arg)->compile(thd, analyzer, &arg_v, transformer,
                                      arg_t);
      if (new_item && *arg != new_item)
        thd->change_item_tree(arg, new_item);
    }
  }
  return (this->*transformer)(thd, arg_t);
}


void Item_args::propagate_equal_fields(THD *thd,
                                       const Item::Context &ctx,
                                       COND_EQUAL *cond)
{
  uint i;
  for (i= 0; i < arg_count; i++)
    args[i]->propagate_equal_fields_and_change_item_tree(thd, ctx, cond,
                                                         &args[i]);
}


Sql_mode_dependency Item_args::value_depends_on_sql_mode_bit_or() const
{
  Sql_mode_dependency res;
  for (uint i= 0; i < arg_count; i++)
    res|= args[i]->value_depends_on_sql_mode();
  return res;
}


/**
  See comments in Item_cond::split_sum_func()
*/

void Item_func::split_sum_func(THD *thd,  Ref_ptr_array ref_pointer_array,
                               List<Item> &fields, uint flags)
{
  Item **arg, **arg_end;
  for (arg= args, arg_end= args+arg_count; arg != arg_end ; arg++)
    (*arg)->split_sum_func2(thd, ref_pointer_array, fields, arg,
                            flags | SPLIT_SUM_SKIP_REGISTERED);
}


table_map Item_func::not_null_tables() const
{
  return not_null_tables_cache;
}


void Item_func::print(String *str, enum_query_type query_type)
{
  str->append(func_name());
  str->append('(');
  print_args(str, 0, query_type);
  str->append(')');
}


void Item_func::print_args(String *str, uint from, enum_query_type query_type)
{
  for (uint i=from ; i < arg_count ; i++)
  {
    if (i != from)
      str->append(',');
    args[i]->print(str, query_type);
  }
}


void Item_func::print_op(String *str, enum_query_type query_type)
{
  for (uint i=0 ; i < arg_count-1 ; i++)
  {
    args[i]->print_parenthesised(str, query_type, precedence());
    str->append(' ');
    str->append(func_name());
    str->append(' ');
  }
  args[arg_count-1]->print_parenthesised(str, query_type,
                                         (enum precedence)(precedence() + 1));
}


bool Item_func::eq(const Item *item, bool binary_cmp) const
{
  /* Assume we don't have rtti */
  if (this == item)
    return 1;
  /*
    Ensure that we are comparing two functions and that the function
    is deterministic.
  */
  if (item->type() != FUNC_ITEM || (used_tables() & RAND_TABLE_BIT))
    return 0;
  Item_func *item_func=(Item_func*) item;
  Item_func::Functype func_type;
  if ((func_type= functype()) != item_func->functype() ||
      arg_count != item_func->arg_count ||
      (func_type != Item_func::FUNC_SP &&
       func_name() != item_func->func_name()) ||
      (func_type == Item_func::FUNC_SP &&
       my_strcasecmp(system_charset_info, func_name(), item_func->func_name())))
    return 0;
  for (uint i=0; i < arg_count ; i++)
    if (!args[i]->eq(item_func->args[i], binary_cmp))
      return 0;
  return 1;
}


/*
bool Item_func::is_expensive_processor(uchar *arg)
{
  return is_expensive();
}
*/

my_decimal *Item_func::val_decimal(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed);
  longlong nr= val_int();
  if (null_value)
    return 0; /* purecov: inspected */
  int2my_decimal(E_DEC_FATAL_ERROR, nr, unsigned_flag, decimal_value);
  return decimal_value;
}


String *Item_real_func::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  double nr= val_real();
  if (null_value)
    return 0; /* purecov: inspected */
  str->set_real(nr, decimals, collation.collation);
  return str;
}


my_decimal *Item_real_func::val_decimal(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed);
  double nr= val_real();
  if (null_value)
    return 0; /* purecov: inspected */
  double2my_decimal(E_DEC_FATAL_ERROR, nr, decimal_value);
  return decimal_value;
}


#ifdef HAVE_DLOPEN
void Item_udf_func::fix_num_length_and_dec()
{
  uint fl_length= 0;
  decimals=0;
  for (uint i=0 ; i < arg_count ; i++)
  {
    set_if_bigger(decimals,args[i]->decimals);
    set_if_bigger(fl_length, args[i]->max_length);
  }
  max_length=float_length(decimals);
  if (fl_length > max_length)
  {
    decimals= NOT_FIXED_DEC;
    max_length= float_length(NOT_FIXED_DEC);
  }
}
#endif


/**
  Count max_length and decimals for temporal functions.

  @param item    Argument array
  @param nitems  Number of arguments in the array.

  @retval        False on success, true on error.
*/
void Item_func::count_datetime_length(enum_field_types field_type_arg,
                                      Item **item, uint nitems)
{
  unsigned_flag= 0;
  decimals= 0;
  if (field_type_arg != MYSQL_TYPE_DATE)
  {
    for (uint i= 0; i < nitems; i++)
      set_if_bigger(decimals, item[i]->decimals);
  }
  set_if_smaller(decimals, TIME_SECOND_PART_DIGITS);
  uint len= decimals ? (decimals + 1) : 0;
  len+= mysql_temporal_int_part_length(field_type_arg);
  fix_char_length(len);
}

/**
  Set max_length/decimals of function if function is fixed point and
  result length/precision depends on argument ones.
*/

void Item_func::count_decimal_length(Item **item, uint nitems)
{
  int max_int_part= 0;
  decimals= 0;
  unsigned_flag= 1;
  for (uint i=0 ; i < nitems ; i++)
  {
    set_if_bigger(decimals, item[i]->decimals);
    set_if_bigger(max_int_part, item[i]->decimal_int_part());
    set_if_smaller(unsigned_flag, item[i]->unsigned_flag);
  }
  int precision= MY_MIN(max_int_part + decimals, DECIMAL_MAX_PRECISION);
  fix_char_length(my_decimal_precision_to_length_no_truncation(precision,
                                                               decimals,
                                                               unsigned_flag));
}


/**
  Set max_length of if it is maximum length of its arguments.
*/

void Item_func::count_only_length(Item **item, uint nitems)
{
  uint32 char_length= 0;
  unsigned_flag= 0;
  for (uint i= 0; i < nitems ; i++)
  {
    set_if_bigger(char_length, item[i]->max_char_length());
    set_if_bigger(unsigned_flag, item[i]->unsigned_flag);
  }
  fix_char_length(char_length);
}


/**
  Set max_length/decimals of function if function is floating point and
  result length/precision depends on argument ones.
*/

void Item_func::count_real_length(Item **items, uint nitems)
{
  uint32 length= 0;
  decimals= 0;
  max_length= 0;
  unsigned_flag= false;
  for (uint i=0 ; i < nitems ; i++)
  {
    if (decimals < FLOATING_POINT_DECIMALS)
    {
      set_if_bigger(decimals, items[i]->decimals);
      /* Will be ignored if items[i]->decimals >= FLOATING_POINT_DECIMALS */
      set_if_bigger(length, (items[i]->max_length - items[i]->decimals));
    }
    set_if_bigger(max_length, items[i]->max_length);
  }
  if (decimals < FLOATING_POINT_DECIMALS)
  {
    max_length= length;
    length+= decimals;
    if (length < max_length)  // If previous operation gave overflow
      max_length= UINT_MAX32;
    else
      max_length= length;
  }
}


/**
  Calculate max_length and decimals for STRING_RESULT functions.

  @param field_type  Field type.
  @param items       Argument array.
  @param nitems      Number of arguments.

  @retval            False on success, true on error.
*/
bool Item_func::count_string_result_length(enum_field_types field_type_arg,
                                           Item **items, uint nitems)
{
  if (agg_arg_charsets_for_string_result(collation, items, nitems, 1))
    return true;
  if (is_temporal_type(field_type_arg))
    count_datetime_length(field_type_arg, items, nitems);
  else
  {
    count_only_length(items, nitems);
    decimals= max_length ? NOT_FIXED_DEC : 0;
  }
  return false;
}


void Item_func::signal_divide_by_null()
{
  THD *thd= current_thd;
  if (thd->variables.sql_mode & MODE_ERROR_FOR_DIVISION_BY_ZERO)
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN, ER_DIVISION_BY_ZERO,
                 ER_THD(thd, ER_DIVISION_BY_ZERO));
  null_value= 1;
}


Item *Item_func::get_tmp_table_item(THD *thd)
{
  if (!with_sum_func && !const_item())
    return new (thd->mem_root) Item_temptable_field(thd, result_field);
  return copy_or_same(thd);
}

double Item_int_func::val_real()
{
  DBUG_ASSERT(fixed == 1);

  return unsigned_flag ? (double) ((ulonglong) val_int()) : (double) val_int();
}


String *Item_int_func::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  longlong nr=val_int();
  if (null_value)
    return 0;
  str->set_int(nr, unsigned_flag, collation.collation);
  return str;
}


bool Item_func_connection_id::fix_length_and_dec()
{
  if (Item_int_func::fix_length_and_dec())
    return TRUE;
  max_length= 10;
  return FALSE;
}


bool Item_func_connection_id::fix_fields(THD *thd, Item **ref)
{
  if (Item_int_func::fix_fields(thd, ref))
    return TRUE;
  thd->thread_specific_used= TRUE;
  value= thd->variables.pseudo_thread_id;
  return FALSE;
}


/**
  Check arguments here to determine result's type for a numeric
  function of two arguments.
*/

bool Item_num_op::fix_length_and_dec(void)
{
  DBUG_ENTER("Item_num_op::fix_length_and_dec");
  DBUG_PRINT("info", ("name %s", func_name()));
  DBUG_ASSERT(arg_count == 2);
  Item_result r0= args[0]->cast_to_int_type();
  Item_result r1= args[1]->cast_to_int_type();

  if (r0 == REAL_RESULT || r1 == REAL_RESULT ||
      r0 == STRING_RESULT || r1 ==STRING_RESULT)
  {
    count_real_length(args, arg_count);
    max_length= float_length(decimals);
    set_handler_by_result_type(REAL_RESULT);
  }
  else if (r0 == DECIMAL_RESULT || r1 == DECIMAL_RESULT ||
           r0 == TIME_RESULT || r1 == TIME_RESULT)
  {
    set_handler_by_result_type(DECIMAL_RESULT);
    result_precision();
    fix_decimals();
    if ((r0 == TIME_RESULT || r1 == TIME_RESULT) && decimals == 0)
      set_handler_by_result_type(INT_RESULT);
  }
  else
  {
    DBUG_ASSERT(r0 == INT_RESULT && r1 == INT_RESULT);
    set_handler_by_result_type(INT_RESULT);
    result_precision();
    decimals= 0;
  }
  DBUG_PRINT("info", ("Type: %s",
             (result_type() == REAL_RESULT ? "REAL_RESULT" :
              result_type() == DECIMAL_RESULT ? "DECIMAL_RESULT" :
              result_type() == INT_RESULT ? "INT_RESULT" :
              "--ILLEGAL!!!--")));
  DBUG_RETURN(FALSE);
}


/**
  Set result type for a numeric function of one argument
  (can be also used by a numeric function of many arguments, if the result
  type depends only on the first argument)
*/

bool Item_func_num1::fix_length_and_dec()
{
  DBUG_ENTER("Item_func_num1::fix_length_and_dec");
  DBUG_PRINT("info", ("name %s", func_name()));
  // Note, cast_to_int_type() can return TIME_RESULT
  switch (args[0]->cast_to_int_type()) {
  case INT_RESULT:
    set_handler_by_result_type(INT_RESULT);
    max_length= args[0]->max_length;
    unsigned_flag= args[0]->unsigned_flag;
    break;
  case STRING_RESULT:
  case REAL_RESULT:
    set_handler_by_result_type(REAL_RESULT);
    decimals= args[0]->decimals; // Preserve NOT_FIXED_DEC
    max_length= float_length(decimals);
    break;
  case TIME_RESULT:
  case DECIMAL_RESULT:
    set_handler_by_result_type(DECIMAL_RESULT);
    decimals= args[0]->decimal_scale(); // Do not preserve NOT_FIXED_DEC
    max_length= args[0]->max_length;
    break;
  case ROW_RESULT:
    DBUG_ASSERT(0);
  }
  DBUG_PRINT("info", ("Type: %s",
                      (result_type() == REAL_RESULT ? "REAL_RESULT" :
                       result_type() == DECIMAL_RESULT ? "DECIMAL_RESULT" :
                       result_type() == INT_RESULT ? "INT_RESULT" :
                       "--ILLEGAL!!!--")));
  DBUG_RETURN(FALSE);
}


String *Item_func_hybrid_field_type::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  switch (Item_func_hybrid_field_type::cmp_type()) {
  case DECIMAL_RESULT:
  {
    my_decimal decimal_value, *val;
    if (!(val= decimal_op_with_null_check(&decimal_value)))
      return 0;                                 // null is set
    my_decimal_round(E_DEC_FATAL_ERROR, val, decimals, FALSE, val);
    str->set_charset(collation.collation);
    my_decimal2string(E_DEC_FATAL_ERROR, val, 0, 0, 0, str);
    break;
  }
  case INT_RESULT:
  {
    longlong nr= int_op();
    if (null_value)
      return 0; /* purecov: inspected */
    str->set_int(nr, unsigned_flag, collation.collation);
    break;
  }
  case REAL_RESULT:
  {
    double nr= real_op();
    if (null_value)
      return 0; /* purecov: inspected */
    str->set_real(nr, decimals, collation.collation);
    break;
  }
  case TIME_RESULT:
  {
    MYSQL_TIME ltime;
    if (date_op_with_null_check(&ltime) ||
        (null_value= str->alloc(MAX_DATE_STRING_REP_LENGTH)))
      return (String *) 0;
    ltime.time_type= mysql_type_to_time_type(field_type());
    str->length(my_TIME_to_str(&ltime, const_cast<char*>(str->ptr()), decimals));
    str->set_charset(&my_charset_bin);
    DBUG_ASSERT(!null_value);
    return str;
  }
  case STRING_RESULT:
    return str_op_with_null_check(&str_value);
  case ROW_RESULT:
    DBUG_ASSERT(0);
  }
  DBUG_ASSERT(!null_value || (str == NULL));
  return str;
}


double Item_func_hybrid_field_type::val_real()
{
  DBUG_ASSERT(fixed == 1);
  switch (Item_func_hybrid_field_type::cmp_type()) {
  case DECIMAL_RESULT:
  {
    my_decimal decimal_value, *val;
    double result;
    if (!(val= decimal_op_with_null_check(&decimal_value)))
      return 0.0;                               // null is set
    my_decimal2double(E_DEC_FATAL_ERROR, val, &result);
    return result;
  }
  case INT_RESULT:
  {
    longlong result= int_op();
    return unsigned_flag ? (double) ((ulonglong) result) : (double) result;
  }
  case REAL_RESULT:
    return real_op();
  case TIME_RESULT:
  {
    MYSQL_TIME ltime;
    if (date_op_with_null_check(&ltime))
      return 0;
    ltime.time_type= mysql_type_to_time_type(field_type());
    return TIME_to_double(&ltime);
  }
  case STRING_RESULT:
  {
    String *res= str_op_with_null_check(&str_value);
    return res ? double_from_string_with_check(res) : 0.0;
  }
  case ROW_RESULT:
    DBUG_ASSERT(0);
  }
  return 0.0;
}


longlong Item_func_hybrid_field_type::val_int()
{
  DBUG_ASSERT(fixed == 1);
  switch (Item_func_hybrid_field_type::cmp_type()) {
  case DECIMAL_RESULT:
  {
    my_decimal decimal_value, *val;
    if (!(val= decimal_op_with_null_check(&decimal_value)))
      return 0;                                 // null is set
    longlong result;
    my_decimal2int(E_DEC_FATAL_ERROR, val, unsigned_flag, &result);
    return result;
  }
  case INT_RESULT:
    return int_op();
  case REAL_RESULT:
    return Converter_double_to_longlong(real_op(), unsigned_flag).result();
  case TIME_RESULT:
  {
    MYSQL_TIME ltime;
    if (date_op_with_null_check(&ltime))
      return 0;
    ltime.time_type= mysql_type_to_time_type(field_type());
    return TIME_to_ulonglong(&ltime);
  }
  case STRING_RESULT:
  {
    String *res= str_op_with_null_check(&str_value);
    return res ? longlong_from_string_with_check(res) : 0;
  }
  case ROW_RESULT:
    DBUG_ASSERT(0);
  }
  return 0;
}


my_decimal *Item_func_hybrid_field_type::val_decimal(my_decimal *decimal_value)
{
  my_decimal *val= decimal_value;
  DBUG_ASSERT(fixed == 1);
  switch (Item_func_hybrid_field_type::cmp_type()) {
  case DECIMAL_RESULT:
    val= decimal_op_with_null_check(decimal_value);
    break;
  case INT_RESULT:
  {
    longlong result= int_op();
    if (null_value)
      return NULL;
    int2my_decimal(E_DEC_FATAL_ERROR, result, unsigned_flag, decimal_value);
    break;
  }
  case REAL_RESULT:
  {
    double result= (double)real_op();
    if (null_value)
      return NULL;
    double2my_decimal(E_DEC_FATAL_ERROR, result, decimal_value);
    break;
  }
  case TIME_RESULT:
  {
    MYSQL_TIME ltime;
    if (date_op_with_null_check(&ltime))
    {
      my_decimal_set_zero(decimal_value);
      return 0;
    }
    ltime.time_type= mysql_type_to_time_type(field_type());
    return date2my_decimal(&ltime, decimal_value);
  }
  case STRING_RESULT:
  {
    String *res= str_op_with_null_check(&str_value);
    return res ? decimal_from_string_with_check(decimal_value, res) : 0;
  }  
  case ROW_RESULT:
    DBUG_ASSERT(0);
  }
  return val;
}


bool Item_func_hybrid_field_type::get_date(MYSQL_TIME *ltime,
                                            ulonglong fuzzydate)
{
  DBUG_ASSERT(fixed == 1);
  switch (Item_func_hybrid_field_type::cmp_type()) {
  case DECIMAL_RESULT:
  {
    my_decimal value, *res;
    if (!(res= decimal_op_with_null_check(&value)) ||
        decimal_to_datetime_with_warn(res, ltime, fuzzydate,
                                      field_table_or_null(),
                                      field_name_or_null()))
      goto err;
    break;
  }
  case INT_RESULT:
  {
    longlong value= int_op();
    bool neg= !unsigned_flag && value < 0;
    if (null_value || int_to_datetime_with_warn(neg, neg ? -value : value,
                                                ltime, fuzzydate,
                                                field_table_or_null(),
                                                field_name_or_null()))
      goto err;
    break;
  }
  case REAL_RESULT:
  {
    double value= real_op();
    if (null_value || double_to_datetime_with_warn(value, ltime, fuzzydate,
                                                   field_table_or_null(),
                                                   field_name_or_null()))
      goto err;
    break;
  }
  case TIME_RESULT:
    return date_op(ltime,
                   (uint)(fuzzydate |
                   (field_type() == MYSQL_TYPE_TIME ? TIME_TIME_ONLY : 0)));
  case STRING_RESULT:
  {
    char buff[40];
    String tmp(buff,sizeof(buff), &my_charset_bin),*res;
    if (!(res= str_op_with_null_check(&tmp)) ||
        str_to_datetime_with_warn(res->charset(), res->ptr(), res->length(),
                                  ltime, fuzzydate))
      goto err;
    break;
  }
  case ROW_RESULT:
    DBUG_ASSERT(0);
  }

  return (null_value= 0);

err:
  bzero(ltime, sizeof(*ltime));
  return null_value|= !(fuzzydate & TIME_FUZZY_DATES);  
}


void Item_func_signed::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as signed)"));

}


longlong Item::val_int_from_str(int *error)
{
  char buff[MAX_FIELD_WIDTH];
  String tmp(buff,sizeof(buff), &my_charset_bin), *res;

  /*
    For a string result, we must first get the string and then convert it
    to a longlong
  */

  if (!(res= val_str(&tmp)))
  {
    *error= 0;
    return 0;
  }
  Converter_strtoll10_with_warn cnv(NULL, Warn_filter_all(),
                                    res->charset(), res->ptr(), res->length());
  *error= cnv.error();
  return cnv.result();
}


longlong Item::val_int_signed_typecast()
{
  if (cast_to_int_type() != STRING_RESULT)
    return val_int();

  int error;
  longlong value= val_int_from_str(&error);
  if (!null_value && value < 0 && error == 0)
    push_note_converted_to_negative_complement(current_thd);
  return value;
}


void Item_func_unsigned::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as unsigned)"));

}


longlong Item::val_int_unsigned_typecast()
{
  if (cast_to_int_type() == DECIMAL_RESULT)
  {
    longlong value;
    my_decimal tmp, *dec= val_decimal(&tmp);
    if (!null_value)
      my_decimal2int(E_DEC_FATAL_ERROR, dec, 1, &value);
    else
      value= 0;
    return value;
  }
  else if (cast_to_int_type() != STRING_RESULT)
  {
    longlong value= val_int();
    if (!null_value && unsigned_flag == 0 && value < 0)
      push_note_converted_to_positive_complement(current_thd);
    return value;
  }

  int error;
  longlong value= val_int_from_str(&error);
  if (!null_value && error < 0)
    push_note_converted_to_positive_complement(current_thd);
  return value;
}


String *Item_decimal_typecast::val_str(String *str)
{
  my_decimal tmp_buf, *tmp= val_decimal(&tmp_buf);
  if (null_value)
    return NULL;
  my_decimal2string(E_DEC_FATAL_ERROR, tmp, 0, 0, 0, str);
  return str;
}


double Item_decimal_typecast::val_real()
{
  my_decimal tmp_buf, *tmp= val_decimal(&tmp_buf);
  double res;
  if (null_value)
    return 0.0;
  my_decimal2double(E_DEC_FATAL_ERROR, tmp, &res);
  return res;
}


longlong Item_decimal_typecast::val_int()
{
  my_decimal tmp_buf, *tmp= val_decimal(&tmp_buf);
  longlong res;
  if (null_value)
    return 0;
  my_decimal2int(E_DEC_FATAL_ERROR, tmp, unsigned_flag, &res);
  return res;
}


my_decimal *Item_decimal_typecast::val_decimal(my_decimal *dec)
{
  my_decimal tmp_buf, *tmp= args[0]->val_decimal(&tmp_buf);
  bool sign;
  uint precision;

  if ((null_value= args[0]->null_value))
    return NULL;
  my_decimal_round(E_DEC_FATAL_ERROR, tmp, decimals, FALSE, dec);
  sign= dec->sign();
  if (unsigned_flag)
  {
    if (sign)
    {
      my_decimal_set_zero(dec);
      goto err;
    }
  }
  precision= my_decimal_length_to_precision(max_length,
                                            decimals, unsigned_flag);
  if (precision - decimals < (uint) my_decimal_intg(dec))
  {
    max_my_decimal(dec, precision, decimals);
    dec->sign(sign);
    goto err;
  }
  return dec;

err:
  THD *thd= current_thd;
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_WARN_DATA_OUT_OF_RANGE,
                      ER_THD(thd, ER_WARN_DATA_OUT_OF_RANGE),
                      name, 1L);
  return dec;
}


void Item_decimal_typecast::print(String *str, enum_query_type query_type)
{
  char len_buf[20*3 + 1];
  char *end;

  uint precision= my_decimal_length_to_precision(max_length, decimals,
                                                 unsigned_flag);
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as decimal("));

  end=int10_to_str(precision, len_buf,10);
  str->append(len_buf, (uint32) (end - len_buf));

  str->append(',');

  end=int10_to_str(decimals, len_buf,10);
  str->append(len_buf, (uint32) (end - len_buf));

  str->append(')');
  str->append(')');
}


double Item_double_typecast::val_real()
{
  int error;
  double tmp= args[0]->val_real();
  if ((null_value= args[0]->null_value))
    return 0.0;

  if ((error= truncate_double(&tmp, max_length, decimals, 0, DBL_MAX)))
  {
    THD *thd= current_thd;
    push_warning_printf(thd,
                        Sql_condition::WARN_LEVEL_WARN,
                        ER_WARN_DATA_OUT_OF_RANGE,
                        ER_THD(thd, ER_WARN_DATA_OUT_OF_RANGE),
                        name, 1);
    if (error < 0)
    {
      null_value= 1;                            // Illegal value
      tmp= 0.0;
    }
  }
  return tmp;
}


void Item_double_typecast::print(String *str, enum_query_type query_type)
{
  char len_buf[20*3 + 1];
  char *end;

  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as double"));
  if (decimals != NOT_FIXED_DEC)
  {
    str->append('(');
    end= int10_to_str(max_length, len_buf,10);
    str->append(len_buf, (uint32) (end - len_buf));
    str->append(',');
    end= int10_to_str(decimals, len_buf,10);
    str->append(len_buf, (uint32) (end - len_buf));
    str->append(')');
  }
  str->append(')');
}

double Item_func_plus::real_op()
{
  double value= args[0]->val_real() + args[1]->val_real();
  if ((null_value=args[0]->null_value || args[1]->null_value))
    return 0.0;
  return check_float_overflow(value);
}


longlong Item_func_plus::int_op()
{
  longlong val0= args[0]->val_int();
  longlong val1= args[1]->val_int();
  longlong res= val0 + val1;
  bool     res_unsigned= FALSE;

  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0;

  /*
    First check whether the result can be represented as a
    (bool unsigned_flag, longlong value) pair, then check if it is compatible
    with this Item's unsigned_flag by calling check_integer_overflow().
  */
  if (args[0]->unsigned_flag)
  {
    if (args[1]->unsigned_flag || val1 >= 0)
    {
      if (test_if_sum_overflows_ull((ulonglong) val0, (ulonglong) val1))
        goto err;
      res_unsigned= TRUE;
    }
    else
    {
      /* val1 is negative */
      if ((ulonglong) val0 > (ulonglong) LONGLONG_MAX)
        res_unsigned= TRUE;
    }
  }
  else
  {
    if (args[1]->unsigned_flag)
    {
      if (val0 >= 0)
      {
        if (test_if_sum_overflows_ull((ulonglong) val0, (ulonglong) val1))
          goto err;
        res_unsigned= TRUE;
      }
      else
      {
        if ((ulonglong) val1 > (ulonglong) LONGLONG_MAX)
          res_unsigned= TRUE;
      }
    }
    else
    {
      if (val0 >=0 && val1 >= 0)
        res_unsigned= TRUE;
      else if (val0 < 0 && val1 < 0 && res >= 0)
        goto err;
    }
  }
  return check_integer_overflow(res, res_unsigned);

err:
  return raise_integer_overflow();
}


/**
  Calculate plus of two decimals.

  @param decimal_value	Buffer that can be used to store result

  @retval
    0  Value was NULL;  In this case null_value is set
  @retval
    \# Value of operation as a decimal
*/

my_decimal *Item_func_plus::decimal_op(my_decimal *decimal_value)
{
  my_decimal value1, *val1;
  my_decimal value2, *val2;
  val1= args[0]->val_decimal(&value1);
  if ((null_value= args[0]->null_value))
    return 0;
  val2= args[1]->val_decimal(&value2);
  if (!(null_value= (args[1]->null_value ||
                     check_decimal_overflow(my_decimal_add(E_DEC_FATAL_ERROR &
                                                           ~E_DEC_OVERFLOW,
                                                           decimal_value,
                                                           val1, val2)) > 3)))
    return decimal_value;
  return 0;
}

/**
  Set precision of results for additive operations (+ and -)
*/
void Item_func_additive_op::result_precision()
{
  decimals= MY_MAX(args[0]->decimal_scale(), args[1]->decimal_scale());
  int arg1_int= args[0]->decimal_precision() - args[0]->decimal_scale();
  int arg2_int= args[1]->decimal_precision() - args[1]->decimal_scale();
  int precision= MY_MAX(arg1_int, arg2_int) + 1 + decimals;

  DBUG_ASSERT(arg1_int >= 0);
  DBUG_ASSERT(arg2_int >= 0);

  /* Integer operations keep unsigned_flag if one of arguments is unsigned */
  if (result_type() == INT_RESULT)
    unsigned_flag= args[0]->unsigned_flag | args[1]->unsigned_flag;
  else
    unsigned_flag= args[0]->unsigned_flag & args[1]->unsigned_flag;
  max_length= my_decimal_precision_to_length_no_truncation(precision, decimals,
                                                           unsigned_flag);
}


/**
  The following function is here to allow the user to force
  subtraction of UNSIGNED BIGINT to return negative values.
*/

bool Item_func_minus::fix_length_and_dec()
{
  if (Item_num_op::fix_length_and_dec())
    return TRUE;
  if ((m_depends_on_sql_mode_no_unsigned_subtraction= unsigned_flag) &&
      (current_thd->variables.sql_mode & MODE_NO_UNSIGNED_SUBTRACTION))
    unsigned_flag= false;
  return FALSE;
}


Sql_mode_dependency Item_func_minus::value_depends_on_sql_mode() const
{
  Sql_mode_dependency dep= Item_func_additive_op::value_depends_on_sql_mode();
  if (m_depends_on_sql_mode_no_unsigned_subtraction)
    dep|= Sql_mode_dependency(0, MODE_NO_UNSIGNED_SUBTRACTION);
  return dep;
}


double Item_func_minus::real_op()
{
  double value= args[0]->val_real() - args[1]->val_real();
  if ((null_value=args[0]->null_value || args[1]->null_value))
    return 0.0;
  return check_float_overflow(value);
}


longlong Item_func_minus::int_op()
{
  longlong val0= args[0]->val_int();
  longlong val1= args[1]->val_int();
  longlong res= val0 - val1;
  bool     res_unsigned= FALSE;

  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0;

  /*
    First check whether the result can be represented as a
    (bool unsigned_flag, longlong value) pair, then check if it is compatible
    with this Item's unsigned_flag by calling check_integer_overflow().
  */
  if (args[0]->unsigned_flag)
  {
    if (args[1]->unsigned_flag)
    {
      if ((ulonglong) val0 < (ulonglong) val1)
      {
        if (res >= 0)
          goto err;
      }
      else
        res_unsigned= TRUE;
    }
    else
    {
      if (val1 >= 0)
      {
        if ((ulonglong) val0 > (ulonglong) val1)
          res_unsigned= TRUE;
      }
      else
      {
        if (test_if_sum_overflows_ull((ulonglong) val0, (ulonglong) -val1))
          goto err;
        res_unsigned= TRUE;
      }
    }
  }
  else
  {
    if (args[1]->unsigned_flag)
    {
      if ((ulonglong) (val0 - LONGLONG_MIN) < (ulonglong) val1)
        goto err;
    }
    else
    {
      if (val0 > 0 && val1 < 0)
        res_unsigned= TRUE;
      else if (val0 < 0 && val1 > 0 && res >= 0)
        goto err;
    }
  }
  return check_integer_overflow(res, res_unsigned);

err:
  return raise_integer_overflow();
}


/**
  See Item_func_plus::decimal_op for comments.
*/

my_decimal *Item_func_minus::decimal_op(my_decimal *decimal_value)
{
  my_decimal value1, *val1;
  my_decimal value2, *val2= 

  val1= args[0]->val_decimal(&value1);
  if ((null_value= args[0]->null_value))
    return 0;
  val2= args[1]->val_decimal(&value2);
  if (!(null_value= (args[1]->null_value ||
                     (check_decimal_overflow(my_decimal_sub(E_DEC_FATAL_ERROR &
                                                            ~E_DEC_OVERFLOW,
                                                            decimal_value, val1,
                                                            val2)) > 3))))
    return decimal_value;
  return 0;
}


double Item_func_mul::real_op()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real() * args[1]->val_real();
  if ((null_value=args[0]->null_value || args[1]->null_value))
    return 0.0;
  return check_float_overflow(value);
}


longlong Item_func_mul::int_op()
{
  DBUG_ASSERT(fixed == 1);
  longlong a= args[0]->val_int();
  longlong b= args[1]->val_int();
  longlong res;
  ulonglong res0, res1;
  ulong a0, a1, b0, b1;
  bool     res_unsigned= FALSE;
  bool     a_negative= FALSE, b_negative= FALSE;

  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0;

  /*
    First check whether the result can be represented as a
    (bool unsigned_flag, longlong value) pair, then check if it is compatible
    with this Item's unsigned_flag by calling check_integer_overflow().

    Let a = a1 * 2^32 + a0 and b = b1 * 2^32 + b0. Then
    a * b = (a1 * 2^32 + a0) * (b1 * 2^32 + b0) = a1 * b1 * 2^64 +
            + (a1 * b0 + a0 * b1) * 2^32 + a0 * b0;
    We can determine if the above sum overflows the ulonglong range by
    sequentially checking the following conditions:
    1. If both a1 and b1 are non-zero.
    2. Otherwise, if (a1 * b0 + a0 * b1) is greater than ULONG_MAX.
    3. Otherwise, if (a1 * b0 + a0 * b1) * 2^32 + a0 * b0 is greater than
    ULONGLONG_MAX.

    Since we also have to take the unsigned_flag for a and b into account,
    it is easier to first work with absolute values and set the
    correct sign later.
  */
  if (!args[0]->unsigned_flag && a < 0)
  {
    a_negative= TRUE;
    a= -a;
  }
  if (!args[1]->unsigned_flag && b < 0)
  {
    b_negative= TRUE;
    b= -b;
  }

  a0= 0xFFFFFFFFUL & a;
  a1= ((ulonglong) a) >> 32;
  b0= 0xFFFFFFFFUL & b;
  b1= ((ulonglong) b) >> 32;

  if (a1 && b1)
    goto err;

  res1= (ulonglong) a1 * b0 + (ulonglong) a0 * b1;
  if (res1 > 0xFFFFFFFFUL)
    goto err;

  res1= res1 << 32;
  res0= (ulonglong) a0 * b0;

  if (test_if_sum_overflows_ull(res1, res0))
    goto err;
  res= res1 + res0;

  if (a_negative != b_negative)
  {
    if ((ulonglong) res > (ulonglong) LONGLONG_MIN + 1)
      goto err;
    res= -res;
  }
  else
    res_unsigned= TRUE;

  return check_integer_overflow(res, res_unsigned);

err:
  return raise_integer_overflow();
}


/** See Item_func_plus::decimal_op for comments. */

my_decimal *Item_func_mul::decimal_op(my_decimal *decimal_value)
{
  my_decimal value1, *val1;
  my_decimal value2, *val2;
  val1= args[0]->val_decimal(&value1);
  if ((null_value= args[0]->null_value))
    return 0;
  val2= args[1]->val_decimal(&value2);
  if (!(null_value= (args[1]->null_value ||
                     (check_decimal_overflow(my_decimal_mul(E_DEC_FATAL_ERROR &
                                                            ~E_DEC_OVERFLOW,
                                                            decimal_value, val1,
                                                            val2)) > 3))))
    return decimal_value;
  return 0;
}


void Item_func_mul::result_precision()
{
  /* Integer operations keep unsigned_flag if one of arguments is unsigned */
  if (result_type() == INT_RESULT)
    unsigned_flag= args[0]->unsigned_flag | args[1]->unsigned_flag;
  else
    unsigned_flag= args[0]->unsigned_flag & args[1]->unsigned_flag;
  decimals= MY_MIN(args[0]->decimal_scale() + args[1]->decimal_scale(),
                DECIMAL_MAX_SCALE);
  uint est_prec = args[0]->decimal_precision() + args[1]->decimal_precision();
  uint precision= MY_MIN(est_prec, DECIMAL_MAX_PRECISION);
  max_length= my_decimal_precision_to_length_no_truncation(precision, decimals,
                                                           unsigned_flag);
}


double Item_func_div::real_op()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  double val2= args[1]->val_real();
  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0.0;
  if (val2 == 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return check_float_overflow(value/val2);
}


my_decimal *Item_func_div::decimal_op(my_decimal *decimal_value)
{
  my_decimal value1, *val1;
  my_decimal value2, *val2;
  int err;

  val1= args[0]->val_decimal(&value1);
  if ((null_value= args[0]->null_value))
    return 0;
  val2= args[1]->val_decimal(&value2);
  if ((null_value= args[1]->null_value))
    return 0;
  if ((err= check_decimal_overflow(my_decimal_div(E_DEC_FATAL_ERROR &
                                                  ~E_DEC_OVERFLOW &
                                                  ~E_DEC_DIV_ZERO,
                                                  decimal_value,
                                                  val1, val2,
                                                  prec_increment))) > 3)
  {
    if (err == E_DEC_DIV_ZERO)
      signal_divide_by_null();
    null_value= 1;
    return 0;
  }
  return decimal_value;
}


void Item_func_div::result_precision()
{
  /*
    We need to add args[1]->divisor_precision_increment(),
    to properly handle the cases like this:
      SELECT 5.05 / 0.014; -> 360.714286
    i.e. when the divisor has a zero integer part
    and non-zero digits appear only after the decimal point.
    Precision in this example is calculated as
      args[0]->decimal_precision()           +  // 3
      args[1]->divisor_precision_increment() +  // 3
      prec_increment                            // 4
    which gives 10 decimals digits.
  */
  uint precision=MY_MIN(args[0]->decimal_precision() + 
                     args[1]->divisor_precision_increment() + prec_increment,
                     DECIMAL_MAX_PRECISION);

  /* Integer operations keep unsigned_flag if one of arguments is unsigned */
  if (result_type() == INT_RESULT)
    unsigned_flag= args[0]->unsigned_flag | args[1]->unsigned_flag;
  else
    unsigned_flag= args[0]->unsigned_flag & args[1]->unsigned_flag;
  decimals= MY_MIN(args[0]->decimal_scale() + prec_increment, DECIMAL_MAX_SCALE);
  max_length= my_decimal_precision_to_length_no_truncation(precision, decimals,
                                                           unsigned_flag);
}


bool Item_func_div::fix_length_and_dec()
{
  DBUG_ENTER("Item_func_div::fix_length_and_dec");
  prec_increment= current_thd->variables.div_precincrement;
  if (Item_num_op::fix_length_and_dec())
    DBUG_RETURN(TRUE);
  switch (Item_func_div::result_type()) {
  case REAL_RESULT:
  {
    decimals=MY_MAX(args[0]->decimals,args[1]->decimals)+prec_increment;
    set_if_smaller(decimals, NOT_FIXED_DEC);
    uint tmp=float_length(decimals);
    if (decimals == NOT_FIXED_DEC)
      max_length= tmp;
    else
    {
      max_length=args[0]->max_length - args[0]->decimals + decimals;
      set_if_smaller(max_length,tmp);
    }
    break;
  }
  case INT_RESULT:
    set_handler_by_result_type(DECIMAL_RESULT);
    DBUG_PRINT("info", ("Type changed: DECIMAL_RESULT"));
    result_precision();
    break;
  case DECIMAL_RESULT:
    result_precision();
    fix_decimals();
    break;
  case STRING_RESULT:
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);
  }
  maybe_null= 1; // devision by zero
  DBUG_RETURN(FALSE);
}


/* Integer division */
longlong Item_func_int_div::val_int()
{
  DBUG_ASSERT(fixed == 1);

  /*
    Perform division using DECIMAL math if either of the operands has a
    non-integer type
  */
  if (args[0]->result_type() != INT_RESULT ||
      args[1]->result_type() != INT_RESULT)
  {
    my_decimal tmp;
    my_decimal *val0p= args[0]->val_decimal(&tmp);
    if ((null_value= args[0]->null_value))
      return 0;
    my_decimal val0= *val0p;

    my_decimal *val1p= args[1]->val_decimal(&tmp);
    if ((null_value= args[1]->null_value))
      return 0;
    my_decimal val1= *val1p;

    int err;
    if ((err= my_decimal_div(E_DEC_FATAL_ERROR & ~E_DEC_DIV_ZERO, &tmp,
                             &val0, &val1, 0)) > 3)
    {
      if (err == E_DEC_DIV_ZERO)
        signal_divide_by_null();
      return 0;
    }

    my_decimal truncated;
    const bool do_truncate= true;
    if (my_decimal_round(E_DEC_FATAL_ERROR, &tmp, 0, do_truncate, &truncated))
      DBUG_ASSERT(false);

    longlong res;
    if (my_decimal2int(E_DEC_FATAL_ERROR, &truncated, unsigned_flag, &res) &
        E_DEC_OVERFLOW)
      raise_integer_overflow();
    return res;
  }
  
  longlong val0=args[0]->val_int();
  longlong val1=args[1]->val_int();
  bool val0_negative, val1_negative, res_negative;
  ulonglong uval0, uval1, res;
  if ((null_value= (args[0]->null_value || args[1]->null_value)))
    return 0;
  if (val1 == 0)
  {
    signal_divide_by_null();
    return 0;
  }

  val0_negative= !args[0]->unsigned_flag && val0 < 0;
  val1_negative= !args[1]->unsigned_flag && val1 < 0;
  res_negative= val0_negative != val1_negative;
  uval0= (ulonglong) (val0_negative ? -val0 : val0);
  uval1= (ulonglong) (val1_negative ? -val1 : val1);
  res= uval0 / uval1;
  if (res_negative)
  {
    if (res > (ulonglong) LONGLONG_MAX)
      return raise_integer_overflow();
    res= (ulonglong) (-(longlong) res);
  }
  return check_integer_overflow(res, !res_negative);
}


bool Item_func_int_div::fix_length_and_dec()
{
  Item_result argtype= args[0]->result_type();
  /* use precision ony for the data type it is applicable for and valid */
  uint32 char_length= args[0]->max_char_length() -
                      (argtype == DECIMAL_RESULT || argtype == INT_RESULT ?
                       args[0]->decimals : 0);
  fix_char_length(char_length > MY_INT64_NUM_DECIMAL_DIGITS ?
                  MY_INT64_NUM_DECIMAL_DIGITS : char_length);
  maybe_null=1;
  unsigned_flag=args[0]->unsigned_flag | args[1]->unsigned_flag;
  return false;
}


longlong Item_func_mod::int_op()
{
  DBUG_ASSERT(fixed == 1);
  longlong val0= args[0]->val_int();
  longlong val1= args[1]->val_int();
  bool val0_negative, val1_negative;
  ulonglong uval0, uval1;
  ulonglong res;

  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0; /* purecov: inspected */
  if (val1 == 0)
  {
    signal_divide_by_null();
    return 0;
  }

  /*
    '%' is calculated by integer division internally. Since dividing
    LONGLONG_MIN by -1 generates SIGFPE, we calculate using unsigned values and
    then adjust the sign appropriately.
  */
  val0_negative= !args[0]->unsigned_flag && val0 < 0;
  val1_negative= !args[1]->unsigned_flag && val1 < 0;
  uval0= (ulonglong) (val0_negative ? -val0 : val0);
  uval1= (ulonglong) (val1_negative ? -val1 : val1);
  res= uval0 % uval1;
  return check_integer_overflow(val0_negative ? -(longlong) res : res,
                                !val0_negative);
}

double Item_func_mod::real_op()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  double val2=  args[1]->val_real();
  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0.0; /* purecov: inspected */
  if (val2 == 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return fmod(value,val2);
}


my_decimal *Item_func_mod::decimal_op(my_decimal *decimal_value)
{
  my_decimal value1, *val1;
  my_decimal value2, *val2;

  val1= args[0]->val_decimal(&value1);
  if ((null_value= args[0]->null_value))
    return 0;
  val2= args[1]->val_decimal(&value2);
  if ((null_value= args[1]->null_value))
    return 0;
  switch (my_decimal_mod(E_DEC_FATAL_ERROR & ~E_DEC_DIV_ZERO, decimal_value,
                         val1, val2)) {
  case E_DEC_TRUNCATED:
  case E_DEC_OK:
    return decimal_value;
  case E_DEC_DIV_ZERO:
    signal_divide_by_null();
    /* fall through */
  default:
    null_value= 1;
    return 0;
  }
}


void Item_func_mod::result_precision()
{
  unsigned_flag= args[0]->unsigned_flag;
  decimals= MY_MAX(args[0]->decimal_scale(), args[1]->decimal_scale());
  uint prec= MY_MAX(args[0]->decimal_precision(), args[1]->decimal_precision());
  fix_char_length(my_decimal_precision_to_length_no_truncation(prec, decimals,
                                                               unsigned_flag));
}


bool Item_func_mod::fix_length_and_dec()
{
  if (Item_num_op::fix_length_and_dec())
    return true;
  maybe_null= 1;
  /*
    result_precision() sets unsigned_flag for INT_RESULT and DECIMAL_RESULT.
    Here we need to set it in case of REAL_RESULT.
  */
  unsigned_flag= args[0]->unsigned_flag;
  return false;
}


double Item_func_neg::real_op()
{
  double value= args[0]->val_real();
  null_value= args[0]->null_value;
  return -value;
}


longlong Item_func_neg::int_op()
{
  longlong value= args[0]->val_int();
  if ((null_value= args[0]->null_value))
    return 0;
  if (args[0]->unsigned_flag &&
      (ulonglong) value > (ulonglong) LONGLONG_MAX + 1)
    return raise_integer_overflow();

  if (value == LONGLONG_MIN)
  {
    if (args[0]->unsigned_flag != unsigned_flag)
      /* negation of LONGLONG_MIN is LONGLONG_MIN. */
      return LONGLONG_MIN; 
    else
      return raise_integer_overflow();
  }

  return check_integer_overflow(-value, !args[0]->unsigned_flag && value < 0);
}


my_decimal *Item_func_neg::decimal_op(my_decimal *decimal_value)
{
  my_decimal val, *value= args[0]->val_decimal(&val);
  if (!(null_value= args[0]->null_value))
  {
    my_decimal2decimal(value, decimal_value);
    my_decimal_neg(decimal_value);
    return decimal_value;
  }
  return 0;
}


bool Item_func_neg::fix_length_and_dec()
{
  DBUG_ENTER("Item_func_neg::fix_length_and_dec");
  if (Item_func_num1::fix_length_and_dec())
    DBUG_RETURN(TRUE);
  /* 1 add because sign can appear */
  max_length= args[0]->max_length + 1;

  /*
    If this is in integer context keep the context as integer if possible
    (This is how multiplication and other integer functions works)
    Use val() to get value as arg_type doesn't mean that item is
    Item_int or Item_float due to existence of Item_param.
  */
  if (Item_func_neg::result_type() == INT_RESULT && args[0]->const_item())
  {
    longlong val= args[0]->val_int();
    if ((ulonglong) val >= (ulonglong) LONGLONG_MIN &&
        ((ulonglong) val != (ulonglong) LONGLONG_MIN ||
          args[0]->type() != INT_ITEM))        
    {
      /*
        Ensure that result is converted to DECIMAL, as longlong can't hold
        the negated number
      */
      set_handler_by_result_type(DECIMAL_RESULT);
      DBUG_PRINT("info", ("Type changed: DECIMAL_RESULT"));
    }
  }
  unsigned_flag= 0;
  DBUG_RETURN(FALSE);
}


double Item_func_abs::real_op()
{
  double value= args[0]->val_real();
  null_value= args[0]->null_value;
  return fabs(value);
}


longlong Item_func_abs::int_op()
{
  longlong value= args[0]->val_int();
  if ((null_value= args[0]->null_value))
    return 0;
  if (unsigned_flag)
    return value;
  /* -LONGLONG_MIN = LONGLONG_MAX + 1 => outside of signed longlong range */
  if (value == LONGLONG_MIN)
    return raise_integer_overflow();
  return (value >= 0) ? value : -value;
}


my_decimal *Item_func_abs::decimal_op(my_decimal *decimal_value)
{
  my_decimal val, *value= args[0]->val_decimal(&val);
  if (!(null_value= args[0]->null_value))
  {
    my_decimal2decimal(value, decimal_value);
    if (decimal_value->sign())
      my_decimal_neg(decimal_value);
    return decimal_value;
  }
  return 0;
}


bool Item_func_abs::fix_length_and_dec()
{
  if (Item_func_num1::fix_length_and_dec())
    return TRUE;
  unsigned_flag= args[0]->unsigned_flag;
  return FALSE;
}


/** Gateway to natural LOG function. */
double Item_func_ln::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value= args[0]->null_value))
    return 0.0;
  if (value <= 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return log(value);
}

/** 
  Extended but so slower LOG function.

  We have to check if all values are > zero and first one is not one
  as these are the cases then result is not a number.
*/ 
double Item_func_log::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value= args[0]->null_value))
    return 0.0;
  if (value <= 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  if (arg_count == 2)
  {
    double value2= args[1]->val_real();
    if ((null_value= args[1]->null_value))
      return 0.0;
    if (value2 <= 0.0 || value == 1.0)
    {
      signal_divide_by_null();
      return 0.0;
    }
    return log(value2) / log(value);
  }
  return log(value);
}

double Item_func_log2::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();

  if ((null_value=args[0]->null_value))
    return 0.0;
  if (value <= 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return log(value) / M_LN2;
}

double Item_func_log10::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value= args[0]->null_value))
    return 0.0;
  if (value <= 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return log10(value);
}

double Item_func_exp::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0; /* purecov: inspected */
  return check_float_overflow(exp(value));
}

double Item_func_sqrt::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=(args[0]->null_value || value < 0)))
    return 0.0; /* purecov: inspected */
  return sqrt(value);
}

double Item_func_pow::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  double val2= args[1]->val_real();
  if ((null_value=(args[0]->null_value || args[1]->null_value)))
    return 0.0; /* purecov: inspected */
  return check_float_overflow(pow(value,val2));
}

// Trigonometric functions

double Item_func_acos::val_real()
{
  DBUG_ASSERT(fixed == 1);
  /* One can use this to defer SELECT processing. */
  DEBUG_SYNC(current_thd, "before_acos_function");
  // the volatile's for BUG #2338 to calm optimizer down (because of gcc's bug)
  volatile double value= args[0]->val_real();
  if ((null_value=(args[0]->null_value || (value < -1.0 || value > 1.0))))
    return 0.0;
  return acos(value);
}

double Item_func_asin::val_real()
{
  DBUG_ASSERT(fixed == 1);
  // the volatile's for BUG #2338 to calm optimizer down (because of gcc's bug)
  volatile double value= args[0]->val_real();
  if ((null_value=(args[0]->null_value || (value < -1.0 || value > 1.0))))
    return 0.0;
  return asin(value);
}

double Item_func_atan::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  if (arg_count == 2)
  {
    double val2= args[1]->val_real();
    if ((null_value=args[1]->null_value))
      return 0.0;
    return check_float_overflow(atan2(value,val2));
  }
  return atan(value);
}

double Item_func_cos::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  return cos(value);
}

double Item_func_sin::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  return sin(value);
}

double Item_func_tan::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  return check_float_overflow(tan(value));
}


double Item_func_cot::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  return check_float_overflow(1.0 / tan(value));
}


// Shift-functions, same as << and >> in C/C++


longlong Item_func_shift_left::val_int()
{
  DBUG_ASSERT(fixed == 1);
  uint shift;
  ulonglong res= ((ulonglong) args[0]->val_int() <<
		  (shift=(uint) args[1]->val_int()));
  if (args[0]->null_value || args[1]->null_value)
  {
    null_value=1;
    return 0;
  }
  null_value=0;
  return (shift < sizeof(longlong)*8 ? (longlong) res : 0);
}

longlong Item_func_shift_right::val_int()
{
  DBUG_ASSERT(fixed == 1);
  uint shift;
  ulonglong res= (ulonglong) args[0]->val_int() >>
    (shift=(uint) args[1]->val_int());
  if (args[0]->null_value || args[1]->null_value)
  {
    null_value=1;
    return 0;
  }
  null_value=0;
  return (shift < sizeof(longlong)*8 ? (longlong) res : 0);
}


longlong Item_func_bit_neg::val_int()
{
  DBUG_ASSERT(fixed == 1);
  ulonglong res= (ulonglong) args[0]->val_int();
  if ((null_value=args[0]->null_value))
    return 0;
  return ~res;
}


// Conversion functions

bool Item_func_int_val::fix_length_and_dec()
{
  DBUG_ENTER("Item_func_int_val::fix_length_and_dec");
  DBUG_PRINT("info", ("name %s", func_name()));

  ulonglong tmp_max_length= (ulonglong ) args[0]->max_length - 
    (args[0]->decimals ? args[0]->decimals + 1 : 0) + 2;
  max_length= tmp_max_length > (ulonglong) 4294967295U ?
    (uint32) 4294967295U : (uint32) tmp_max_length;
  uint tmp= float_length(decimals);
  set_if_smaller(max_length,tmp);
  decimals= 0;

  // Note, cast_to_int_type() can return TIME_RESULT
  switch (args[0]->cast_to_int_type())
  {
  case STRING_RESULT:
  case REAL_RESULT:
    set_handler_by_result_type(REAL_RESULT);
    max_length= float_length(decimals);
    break;
  case INT_RESULT:
  case TIME_RESULT:
  case DECIMAL_RESULT:
    /*
      -2 because in most high position can't be used any digit for longlong
      and one position for increasing value during operation
    */
    if ((args[0]->max_length - args[0]->decimals) >=
        (DECIMAL_LONGLONG_DIGITS - 2))
    {
      set_handler_by_result_type(DECIMAL_RESULT);
    }
    else
    {
      unsigned_flag= args[0]->unsigned_flag;
      set_handler_by_result_type(INT_RESULT);
    }
    break;
  case ROW_RESULT:
    DBUG_ASSERT(0);
  }
  DBUG_PRINT("info", ("Type: %s",
                      (result_type() == REAL_RESULT ? "REAL_RESULT" :
                       result_type() == DECIMAL_RESULT ? "DECIMAL_RESULT" :
                       result_type() == INT_RESULT ? "INT_RESULT" :
                       "--ILLEGAL!!!--")));

  DBUG_RETURN(FALSE);
}


longlong Item_func_ceiling::int_op()
{
  longlong result;
  switch (args[0]->result_type()) {
  case INT_RESULT:
    result= args[0]->val_int();
    null_value= args[0]->null_value;
    break;
  case DECIMAL_RESULT:
  {
    my_decimal dec_buf, *dec;
    if ((dec= Item_func_ceiling::decimal_op(&dec_buf)))
      my_decimal2int(E_DEC_FATAL_ERROR, dec, unsigned_flag, &result);
    else
      result= 0;
    break;
  }
  default:
    result= (longlong)Item_func_ceiling::real_op();
  };
  return result;
}


double Item_func_ceiling::real_op()
{
  /*
    the volatile's for BUG #3051 to calm optimizer down (because of gcc's
    bug)
  */
  volatile double value= args[0]->val_real();
  null_value= args[0]->null_value;
  return ceil(value);
}


my_decimal *Item_func_ceiling::decimal_op(my_decimal *decimal_value)
{
  my_decimal val, *value= args[0]->val_decimal(&val);
  if (!(null_value= (args[0]->null_value ||
                     my_decimal_ceiling(E_DEC_FATAL_ERROR, value,
                                        decimal_value) > 1)))
    return decimal_value;
  return 0;
}


longlong Item_func_floor::int_op()
{
  longlong result;
  switch (args[0]->result_type()) {
  case INT_RESULT:
    result= args[0]->val_int();
    null_value= args[0]->null_value;
    break;
  case DECIMAL_RESULT:
  {
    my_decimal dec_buf, *dec;
    if ((dec= Item_func_floor::decimal_op(&dec_buf)))
      my_decimal2int(E_DEC_FATAL_ERROR, dec, unsigned_flag, &result);
    else
      result= 0;
    break;
  }
  default:
    result= (longlong)Item_func_floor::real_op();
  };
  return result;
}


double Item_func_floor::real_op()
{
  /*
    the volatile's for BUG #3051 to calm optimizer down (because of gcc's
    bug)
  */
  volatile double value= args[0]->val_real();
  null_value= args[0]->null_value;
  return floor(value);
}


my_decimal *Item_func_floor::decimal_op(my_decimal *decimal_value)
{
  my_decimal val, *value= args[0]->val_decimal(&val);
  if (!(null_value= (args[0]->null_value ||
                     my_decimal_floor(E_DEC_FATAL_ERROR, value,
                                      decimal_value) > 1)))
    return decimal_value;
  return 0;
}


bool Item_func_round::fix_length_and_dec()
{
  int      decimals_to_set;
  longlong val1;
  bool     val1_unsigned;
  
  unsigned_flag= args[0]->unsigned_flag;
  if (!args[1]->const_item())
  {
    decimals= args[0]->decimals;
    max_length= float_length(decimals);
    if (args[0]->result_type() == DECIMAL_RESULT)
    {
      max_length++;
      set_handler_by_result_type(DECIMAL_RESULT);
    }
    else
      set_handler_by_result_type(REAL_RESULT);
    return FALSE;
  }

  val1= args[1]->val_int();
  if ((null_value= args[1]->null_value))
    return FALSE;

  val1_unsigned= args[1]->unsigned_flag;
  if (val1 < 0)
    decimals_to_set= val1_unsigned ? INT_MAX : 0;
  else
    decimals_to_set= (val1 > INT_MAX) ? INT_MAX : (int) val1;

  if (args[0]->decimals == NOT_FIXED_DEC)
  {
    decimals= MY_MIN(decimals_to_set, NOT_FIXED_DEC);
    max_length= float_length(decimals);
    set_handler_by_result_type(REAL_RESULT);
    return FALSE;
  }
  
  switch (args[0]->result_type()) {
  case REAL_RESULT:
  case STRING_RESULT:
    set_handler_by_result_type(REAL_RESULT);
    decimals= MY_MIN(decimals_to_set, NOT_FIXED_DEC);
    max_length= float_length(decimals);
    break;
  case INT_RESULT:
    if ((!decimals_to_set && truncate) || (args[0]->decimal_precision() < DECIMAL_LONGLONG_DIGITS))
    {
      int length_can_increase= MY_TEST(!truncate && (val1 < 0) &&
                                       !val1_unsigned);
      max_length= args[0]->max_length + length_can_increase;
      /* Here we can keep INT_RESULT */
      set_handler_by_result_type(INT_RESULT);
      decimals= 0;
      break;
    }
    /* fall through */
  case DECIMAL_RESULT:
  {
    set_handler_by_result_type(DECIMAL_RESULT);
    decimals_to_set= MY_MIN(DECIMAL_MAX_SCALE, decimals_to_set);
    int decimals_delta= args[0]->decimals - decimals_to_set;
    int precision= args[0]->decimal_precision();
    int length_increase= ((decimals_delta <= 0) || truncate) ? 0:1;

    precision-= decimals_delta - length_increase;
    decimals= MY_MIN(decimals_to_set, DECIMAL_MAX_SCALE);
    max_length= my_decimal_precision_to_length_no_truncation(precision,
                                                             decimals,
                                                             unsigned_flag);
    break;
  }
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0); /* This result type isn't handled */
  }
  return FALSE;
}

double my_double_round(double value, longlong dec, bool dec_unsigned,
                       bool truncate)
{
  double tmp;
  bool dec_negative= (dec < 0) && !dec_unsigned;
  ulonglong abs_dec= dec_negative ? -dec : dec;
  /*
    tmp2 is here to avoid return the value with 80 bit precision
    This will fix that the test round(0.1,1) = round(0.1,1) is true
    Tagging with volatile is no guarantee, it may still be optimized away...
  */
  volatile double tmp2;

  tmp=(abs_dec < array_elements(log_10) ?
       log_10[abs_dec] : pow(10.0,(double) abs_dec));

  // Pre-compute these, to avoid optimizing away e.g. 'floor(v/tmp) * tmp'.
  volatile double value_div_tmp= value / tmp;
  volatile double value_mul_tmp= value * tmp;

  if (!dec_negative && std::isinf(tmp)) // "dec" is too large positive number
    return value;

  if (dec_negative && std::isinf(tmp))
    tmp2= 0.0;
  else if (!dec_negative && std::isinf(value_mul_tmp))
    tmp2= value;
  else if (truncate)
  {
    if (value >= 0.0)
      tmp2= dec < 0 ? floor(value_div_tmp) * tmp : floor(value_mul_tmp) / tmp;
    else
      tmp2= dec < 0 ? ceil(value_div_tmp) * tmp : ceil(value_mul_tmp) / tmp;
  }
  else
    tmp2=dec < 0 ? rint(value_div_tmp) * tmp : rint(value_mul_tmp) / tmp;

  return tmp2;
}


double Item_func_round::real_op()
{
  double value= args[0]->val_real();

  if (!(null_value= args[0]->null_value))
  {
    longlong dec= args[1]->val_int();
    if (!(null_value= args[1]->null_value))
      return my_double_round(value, dec, args[1]->unsigned_flag, truncate);
  }
  return 0.0;
}

/*
  Rounds a given value to a power of 10 specified as the 'to' argument,
  avoiding overflows when the value is close to the ulonglong range boundary.
*/

static inline ulonglong my_unsigned_round(ulonglong value, ulonglong to)
{
  ulonglong tmp= value / to * to;
  return (value - tmp < (to >> 1)) ? tmp : tmp + to;
}


longlong Item_func_round::int_op()
{
  longlong value= args[0]->val_int();
  longlong dec= args[1]->val_int();
  decimals= 0;
  ulonglong abs_dec;
  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0;
  if ((dec >= 0) || args[1]->unsigned_flag)
    return value; // integer have not digits after point

  abs_dec= -dec;
  longlong tmp;
  
  if(abs_dec >= array_elements(log_10_int))
    return 0;
  
  tmp= log_10_int[abs_dec];
  
  if (truncate)
    value= (unsigned_flag) ?
      ((ulonglong) value / tmp) * tmp : (value / tmp) * tmp;
  else
    value= (unsigned_flag || value >= 0) ?
      my_unsigned_round((ulonglong) value, tmp) :
      -(longlong) my_unsigned_round((ulonglong) -value, tmp);
  return value;
}


my_decimal *Item_func_round::decimal_op(my_decimal *decimal_value)
{
  my_decimal val, *value= args[0]->val_decimal(&val);
  longlong dec= args[1]->val_int();
  if (dec >= 0 || args[1]->unsigned_flag)
    dec= MY_MIN((ulonglong) dec, decimals);
  else if (dec < INT_MIN)
    dec= INT_MIN;
    
  if (!(null_value= (args[0]->null_value || args[1]->null_value ||
                     my_decimal_round(E_DEC_FATAL_ERROR, value, (int) dec,
                                      truncate, decimal_value) > 1))) 
    return decimal_value;
  return 0;
}


void Item_func_rand::seed_random(Item *arg)
{
  /*
    TODO: do not do reinit 'rand' for every execute of PS/SP if
    args[0] is a constant.
  */
  uint32 tmp;
#ifdef WITH_WSREP
  THD *thd= current_thd;
  if (WSREP(thd))
  {
    if (thd->wsrep_exec_mode==REPL_RECV)
      tmp= thd->wsrep_rand;
    else
      tmp= thd->wsrep_rand= (uint32) arg->val_int();
   }
  else
#endif /* WITH_WSREP */
    tmp= (uint32) arg->val_int();

  my_rnd_init(rand, (uint32) (tmp*0x10001L+55555555L),
             (uint32) (tmp*0x10000001L));
}


bool Item_func_rand::fix_fields(THD *thd,Item **ref)
{
  if (Item_real_func::fix_fields(thd, ref))
    return TRUE;
  used_tables_cache|= RAND_TABLE_BIT;
  if (arg_count)
  {					// Only use argument once in query
    /*
      Allocate rand structure once: we must use thd->stmt_arena
      to create rand in proper mem_root if it's a prepared statement or
      stored procedure.

      No need to send a Rand log event if seed was given eg: RAND(seed),
      as it will be replicated in the query as such.
    */
    if (!rand && !(rand= (struct my_rnd_struct*)
                   thd->stmt_arena->alloc(sizeof(*rand))))
      return TRUE;
  }
  else
  {
    /*
      Save the seed only the first time RAND() is used in the query
      Once events are forwarded rather than recreated,
      the following can be skipped if inside the slave thread
    */
    if (!thd->rand_used)
    {
      thd->rand_used= 1;
      thd->rand_saved_seed1= thd->rand.seed1;
      thd->rand_saved_seed2= thd->rand.seed2;
    }
    rand= &thd->rand;
  }
  return FALSE;
}

void Item_func_rand::update_used_tables()
{
  Item_real_func::update_used_tables();
  used_tables_cache|= RAND_TABLE_BIT;
}


double Item_func_rand::val_real()
{
  DBUG_ASSERT(fixed == 1);
  if (arg_count)
  {
    if (!args[0]->const_item())
      seed_random(args[0]);
    else if (first_eval)
    {
      /*
        Constantness of args[0] may be set during JOIN::optimize(), if arg[0]
        is a field item of "constant" table. Thus, we have to evaluate
        seed_random() for constant arg there but not at the fix_fields method.
      */
      first_eval= FALSE;
      seed_random(args[0]);
    }
  }
  return my_rnd(rand);
}

longlong Item_func_sign::val_int()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  null_value=args[0]->null_value;
  return value < 0.0 ? -1 : (value > 0 ? 1 : 0);
}


double Item_func_units::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0;
  return check_float_overflow(value * mul + add);
}


bool Item_func_min_max::fix_length_and_dec()
{
  uint unsigned_count= 0;
  int max_int_part=0;
  decimals=0;
  max_length=0;
  maybe_null=0;
  Item_result tmp_cmp_type= args[0]->cmp_type();
  uint string_type_count= 0;
  uint temporal_type_count= 0;
  enum_field_types temporal_field_type= MYSQL_TYPE_DATETIME;

  for (uint i=0 ; i < arg_count ; i++)
  {
    set_if_bigger(max_length, args[i]->max_length);
    set_if_bigger(decimals, args[i]->decimals);
    set_if_bigger(max_int_part, args[i]->decimal_int_part());
    unsigned_count+= args[i]->unsigned_flag;
    if (args[i]->maybe_null)
      maybe_null= 1;
    tmp_cmp_type= item_cmp_type(tmp_cmp_type, args[i]->cmp_type());
    string_type_count+= args[i]->cmp_type() == STRING_RESULT;
    if (args[i]->cmp_type() == TIME_RESULT)
    {
      if (!temporal_type_count)
        temporal_field_type= args[i]->field_type();
      else
        temporal_field_type= Field::field_type_merge(temporal_field_type,
                                                     args[i]->field_type());
      temporal_type_count++;
    }
  }
  unsigned_flag= unsigned_count == arg_count; // if all args are unsigned

  switch (tmp_cmp_type) {
  case TIME_RESULT:
  {
    // At least one temporal argument was found.
    if (temporal_type_count < arg_count)
      maybe_null= true; // Non-temporal-to-temporal conversion can return NULL
    collation.set_numeric();
    set_handler_by_field_type(temporal_field_type);
    if (is_temporal_type_with_time(temporal_field_type))
      set_if_smaller(decimals, TIME_SECOND_PART_DIGITS);
    else
      decimals= 0;
    uint len= decimals ? (decimals + 1) : 0;
    len+= mysql_temporal_int_part_length(temporal_field_type);
    fix_char_length(len);
    break;
  }
  case STRING_RESULT:
    /*
      All arguments are of string-alike types:
        CHAR, VARCHAR, TEXT, BINARY, VARBINARY, BLOB, SET, ENUM
      No numeric and no temporal types were found.
    */
    agg_arg_charsets_for_string_result_with_comparison(collation,
                                                       args, arg_count);
    set_handler_by_field_type(agg_field_type(args, arg_count, false));
    break;

  case INT_RESULT:
    /*
       All arguments have INT-alike types:
       TINY, SHORT, LONG, LONGLONG, INT24, YEAR, BIT.
    */
    collation.set_numeric();
    fix_char_length(my_decimal_precision_to_length_no_truncation(max_int_part +
                                                                 decimals,
                                                                 decimals,
                                                                 unsigned_flag));
    if (unsigned_count != 0 && unsigned_count != arg_count)
    {
      /*
        If all args are of INT-alike type, but have different unsigned_flag,
        then change type to DECIMAL.
      */
      set_handler_by_field_type(MYSQL_TYPE_NEWDECIMAL);
    }
    else
    {
      /*
        There are only INT-alike arguments with equal unsigned_flag.
        Aggregate types to get the best covering type.
        Treat BIT as LONGLONG when aggregating to non-BIT types.
        Possible final type: TINY, SHORT, LONG, LONGLONG, INT24, YEAR, BIT.
      */
      set_handler_by_field_type(agg_field_type(args, arg_count, true));
    }
    break;

  case DECIMAL_RESULT:
    // All arguments are of DECIMAL type
    collation.set_numeric();
    fix_char_length(my_decimal_precision_to_length_no_truncation(max_int_part +
                                                                 decimals,
                                                                 decimals,
                                                                 unsigned_flag));
    set_handler_by_field_type(MYSQL_TYPE_NEWDECIMAL);
    break;

  case ROW_RESULT:
    DBUG_ASSERT(0);
    // Pass through
  case REAL_RESULT:
    collation.set_numeric();
    fix_char_length(float_length(decimals));
    /*
      Set type to DOUBLE, as Item_func::create_tmp_field() does not
      distinguish between DOUBLE and FLOAT and always creates Field_double.
      Perhaps we should eventually change this to use agg_field_type() here,
      and fix Item_func::create_tmp_field() to create Field_float when possible.
    */
    set_handler_by_field_type(MYSQL_TYPE_DOUBLE);
    break;
  }
  return FALSE;
}


/*
  Compare item arguments in the DATETIME context.

  DESCRIPTION
    Compare item arguments as DATETIME values and return the index of the
    least/greatest argument in the arguments array.
    The correct DATE/DATETIME value of the found argument is
    stored to the value pointer, if latter is provided.

  RETURN
   1	If one of arguments is NULL or there was a execution error
   0    Otherwise
*/

bool Item_func_min_max::get_date(MYSQL_TIME *ltime, ulonglong fuzzy_date)
{
  longlong UNINIT_VAR(min_max);
  DBUG_ASSERT(fixed == 1);

  /*
    just like ::val_int() method of a string item can be called,
    for example, SELECT CONCAT("10", "12") + 1,
    ::get_date() can be called for non-temporal values,
    for example, SELECT MONTH(GREATEST("2011-11-21", "2010-10-09"))

  */
  if (Item_func_min_max::cmp_type() != TIME_RESULT)
    return Item_func::get_date(ltime, fuzzy_date);

  for (uint i=0; i < arg_count ; i++)
  {
    longlong res= args[i]->val_temporal_packed(Item_func_min_max::field_type());

    /* Check if we need to stop (because of error or KILL) and stop the loop */
    if (args[i]->null_value)
      return (null_value= 1);

    if (i == 0 || (res < min_max ? cmp_sign : -cmp_sign) > 0)
      min_max= res;
  }
  unpack_time(min_max, ltime);

  if (Item_func_min_max::field_type() == MYSQL_TYPE_DATE)
  {
    ltime->time_type= MYSQL_TIMESTAMP_DATE;
    ltime->hour= ltime->minute= ltime->second= ltime->second_part= 0;
  }
  else if (Item_func_min_max::field_type() == MYSQL_TYPE_TIME)
  {
    ltime->time_type= MYSQL_TIMESTAMP_TIME;
    ltime->hour+= (ltime->month * 32 + ltime->day) * 24;
    ltime->year= ltime->month= ltime->day= 0;
    if (adjust_time_range_with_warn(ltime,
                                    MY_MIN(decimals, TIME_SECOND_PART_DIGITS)))
      return (null_value= true);
  }

  if (!(fuzzy_date & TIME_TIME_ONLY) &&
      ((null_value= check_date_with_warn(ltime, fuzzy_date,
                                         MYSQL_TIMESTAMP_ERROR))))
    return true;

  return (null_value= 0);
}


String *Item_func_min_max::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  if (Item_func_min_max::cmp_type() == TIME_RESULT)
    return val_string_from_date(str);
  switch (Item_func_min_max::result_type()) {
  case INT_RESULT:
    return val_string_from_int(str);
  case DECIMAL_RESULT:
    return val_string_from_decimal(str);
  case REAL_RESULT:
    return val_string_from_real(str);
  case STRING_RESULT:
  {
    String *UNINIT_VAR(res);
    for (uint i=0; i < arg_count ; i++)
    {
      if (i == 0)
	res=args[i]->val_str(str);
      else
      {
	String *res2;
	res2= args[i]->val_str(res == str ? &tmp_value : str);
	if (res2)
	{
	  int cmp= sortcmp(res,res2,collation.collation);
	  if ((cmp_sign < 0 ? cmp : -cmp) < 0)
	    res=res2;
	}
      }
      if ((null_value= args[i]->null_value))
        return 0;
    }
    res->set_charset(collation.collation);
    return res;
  }
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);                // This case should never be chosen
    return 0;
  }
  return 0;					// Keep compiler happy
}


double Item_func_min_max::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value=0.0;
  if (Item_func_min_max::cmp_type() == TIME_RESULT)
  {
    MYSQL_TIME ltime;
    if (get_date(&ltime, 0))
      return 0;

    return TIME_to_double(&ltime);
  }
  for (uint i=0; i < arg_count ; i++)
  {
    if (i == 0)
      value= args[i]->val_real();
    else
    {
      double tmp= args[i]->val_real();
      if (!args[i]->null_value && (tmp < value ? cmp_sign : -cmp_sign) > 0)
	value=tmp;
    }
    if ((null_value= args[i]->null_value))
      break;
  }
  return value;
}


longlong Item_func_min_max::val_int()
{
  DBUG_ASSERT(fixed == 1);
  longlong value=0;
  if (Item_func_min_max::cmp_type() == TIME_RESULT)
  {
    MYSQL_TIME ltime;
    if (get_date(&ltime, 0))
      return 0;

    return TIME_to_ulonglong(&ltime);
  }
  for (uint i=0; i < arg_count ; i++)
  {
    if (i == 0)
      value=args[i]->val_int();
    else
    {
      longlong tmp=args[i]->val_int();
      if (!args[i]->null_value && (tmp < value ? cmp_sign : -cmp_sign) > 0)
	value=tmp;
    }
    if ((null_value= args[i]->null_value))
      break;
  }
  return value;
}


my_decimal *Item_func_min_max::val_decimal(my_decimal *dec)
{
  DBUG_ASSERT(fixed == 1);
  my_decimal tmp_buf, *tmp, *UNINIT_VAR(res);

  if (Item_func_min_max::cmp_type() == TIME_RESULT)
  {
    MYSQL_TIME ltime;
    if (get_date(&ltime, 0))
      return 0;

    return date2my_decimal(&ltime, dec);
  }
  for (uint i=0; i < arg_count ; i++)
  {
    if (i == 0)
      res= args[i]->val_decimal(dec);
    else
    {
      tmp= args[i]->val_decimal(&tmp_buf);      // Zero if NULL
      if (tmp && (my_decimal_cmp(tmp, res) * cmp_sign) < 0)
      {
        if (tmp == &tmp_buf)
        {
          /* Move value out of tmp_buf as this will be reused on next loop */
          my_decimal2decimal(tmp, dec);
          res= dec;
        }
        else
          res= tmp;
      }
    }
    if ((null_value= args[i]->null_value))
    {
      res= 0;
      break;
    }
  }
  return res;
}


longlong Item_func_length::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;
  return (longlong) res->length();
}


longlong Item_func_char_length::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;
  return (longlong) res->numchars();
}


longlong Item_func_coercibility::val_int()
{
  DBUG_ASSERT(fixed == 1);
  null_value= 0;
  return (longlong) args[0]->collation.derivation;
}


bool Item_func_locate::fix_length_and_dec()
{
  max_length= MY_INT32_NUM_DECIMAL_DIGITS;
  return agg_arg_charsets_for_comparison(cmp_collation, args, 2);
}


longlong Item_func_locate::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *a=args[0]->val_str(&value1);
  String *b=args[1]->val_str(&value2);
  if (!a || !b)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;
  /* must be longlong to avoid truncation */
  longlong start=  0; 
  longlong start0= 0;
  my_match_t match;

  if (arg_count == 3)
  {
    start0= start= args[2]->val_int() - 1;

    if ((start < 0) || (start > a->length()))
      return 0;

    /* start is now sufficiently valid to pass to charpos function */
    start= a->charpos((int) start);

    if (start + b->length() > a->length())
      return 0;
  }

  if (!b->length())				// Found empty string at start
    return start + 1;
  
  if (!cmp_collation.collation->coll->instr(cmp_collation.collation,
                                            a->ptr()+start,
                                            (uint) (a->length()-start),
                                            b->ptr(), b->length(),
                                            &match, 1))
    return 0;
  return (longlong) match.mb_len + start0 + 1;
}


void Item_func_locate::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("locate("));
  args[1]->print(str, query_type);
  str->append(',');
  args[0]->print(str, query_type);
  if (arg_count == 3)
  {
    str->append(',');
    args[2]->print(str, query_type);
  }
  str->append(')');
}


longlong Item_func_field::val_int()
{
  DBUG_ASSERT(fixed == 1);

  if (cmp_type == STRING_RESULT)
  {
    String *field;
    if (!(field= args[0]->val_str(&value)))
      return 0;
    for (uint i=1 ; i < arg_count ; i++)
    {
      String *tmp_value=args[i]->val_str(&tmp);
      if (tmp_value && !sortcmp(field,tmp_value,cmp_collation.collation))
        return (longlong) (i);
    }
  }
  else if (cmp_type == INT_RESULT)
  {
    longlong val= args[0]->val_int();
    if (args[0]->null_value)
      return 0;
    for (uint i=1; i < arg_count ; i++)
    {
      if (val == args[i]->val_int() && !args[i]->null_value)
        return (longlong) (i);
    }
  }
  else if (cmp_type == DECIMAL_RESULT)
  {
    my_decimal dec_arg_buf, *dec_arg,
               dec_buf, *dec= args[0]->val_decimal(&dec_buf);
    if (args[0]->null_value)
      return 0;
    for (uint i=1; i < arg_count; i++)
    {
      dec_arg= args[i]->val_decimal(&dec_arg_buf);
      if (!args[i]->null_value && !my_decimal_cmp(dec_arg, dec))
        return (longlong) (i);
    }
  }
  else
  {
    double val= args[0]->val_real();
    if (args[0]->null_value)
      return 0;
    for (uint i=1; i < arg_count ; i++)
    {
      if (val == args[i]->val_real() && !args[i]->null_value)
        return (longlong) (i);
    }
  }
  return 0;
}


bool Item_func_field::fix_length_and_dec()
{
  maybe_null=0; max_length=3;
  cmp_type= args[0]->result_type();
  for (uint i=1; i < arg_count ; i++)
    cmp_type= item_cmp_type(cmp_type, args[i]->result_type());
  if (cmp_type == STRING_RESULT)
    return agg_arg_charsets_for_comparison(cmp_collation, args, arg_count);
  return FALSE;
}


longlong Item_func_ascii::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0;
  }
  null_value=0;
  return (longlong) (res->length() ? (uchar) (*res)[0] : (uchar) 0);
}

longlong Item_func_ord::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0;
  }
  null_value=0;
  if (!res->length()) return 0;
#ifdef USE_MB
  if (use_mb(res->charset()))
  {
    const char *str=res->ptr();
    uint32 n=0, l=my_ismbchar(res->charset(),str,str+res->length());
    if (!l)
      return (longlong)((uchar) *str);
    while (l--)
      n=(n<<8)|(uint32)((uchar) *str++);
    return (longlong) n;
  }
#endif
  return (longlong) ((uchar) (*res)[0]);
}

	/* Search after a string in a string of strings separated by ',' */
	/* Returns number of found type >= 1 or 0 if not found */
	/* This optimizes searching in enums to bit testing! */

bool Item_func_find_in_set::fix_length_and_dec()
{
  decimals=0;
  max_length=3;					// 1-999
  if (args[0]->const_item() && args[1]->type() == FIELD_ITEM)
  {
    Field *field= ((Item_field*) args[1])->field;
    if (field->real_type() == MYSQL_TYPE_SET)
    {
      String *find=args[0]->val_str(&value);
      if (find)
      {
        // find is not NULL pointer so args[0] is not a null-value
        DBUG_ASSERT(!args[0]->null_value);
	enum_value= find_type(((Field_enum*) field)->typelib,find->ptr(),
			      find->length(), 0);
	enum_bit=0;
	if (enum_value)
	  enum_bit=1LL << (enum_value-1);
      }
    }
  }
  return agg_arg_charsets_for_comparison(cmp_collation, args, 2);
}

static const char separator=',';

longlong Item_func_find_in_set::val_int()
{
  DBUG_ASSERT(fixed == 1);
  if (enum_value)
  {
    // enum_value is set iff args[0]->const_item() in fix_length_and_dec().
    DBUG_ASSERT(args[0]->const_item());

    ulonglong tmp= (ulonglong) args[1]->val_int();
    null_value= args[1]->null_value;
    /* 
      No need to check args[0]->null_value since enum_value is set iff
      args[0] is a non-null const item. Note: no DBUG_ASSERT on
      args[0]->null_value here because args[0] may have been replaced
      by an Item_cache on which val_int() has not been called. See
      BUG#11766317
    */
    if (!null_value)
    {
      if (tmp & enum_bit)
        return enum_value;
    }
    return 0L;
  }

  String *find=args[0]->val_str(&value);
  String *buffer=args[1]->val_str(&value2);
  if (!find || !buffer)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;

  if ((int) (buffer->length() - find->length()) >= 0)
  {
    my_wc_t wc= 0;
    CHARSET_INFO *cs= cmp_collation.collation;
    const char *str_begin= buffer->ptr();
    const char *str_end= buffer->ptr();
    const char *real_end= str_end+buffer->length();
    const uchar *find_str= (const uchar *) find->ptr();
    uint find_str_len= find->length();
    int position= 0;
    while (1)
    {
      int symbol_len;
      if ((symbol_len= cs->cset->mb_wc(cs, &wc, (uchar*) str_end, 
                                       (uchar*) real_end)) > 0)
      {
        const char *substr_end= str_end + symbol_len;
        bool is_last_item= (substr_end == real_end);
        bool is_separator= (wc == (my_wc_t) separator);
        if (is_separator || is_last_item)
        {
          position++;
          if (is_last_item && !is_separator)
            str_end= substr_end;
          if (!my_strnncoll(cs, (const uchar *) str_begin,
                            (uint) (str_end - str_begin),
                            find_str, find_str_len))
            return (longlong) position;
          else
            str_begin= substr_end;
        }
        str_end= substr_end;
      }
      else if (str_end - str_begin == 0 &&
               find_str_len == 0 &&
               wc == (my_wc_t) separator)
        return (longlong) ++position;
      else
        return 0;
    }
  }
  return 0;
}

longlong Item_func_bit_count::val_int()
{
  DBUG_ASSERT(fixed == 1);
  ulonglong value= (ulonglong) args[0]->val_int();
  if ((null_value= args[0]->null_value))
    return 0; /* purecov: inspected */
  return (longlong) my_count_bits(value);
}


/****************************************************************************
** Functions to handle dynamic loadable functions
** Original source by: Alexis Mikhailov <root@medinf.chuvashia.su>
** Rewritten by monty.
****************************************************************************/

#ifdef HAVE_DLOPEN

void udf_handler::cleanup()
{
  if (!not_original)
  {
    if (initialized)
    {
      if (u_d->func_deinit != NULL)
      {
        Udf_func_deinit deinit= u_d->func_deinit;
        (*deinit)(&initid);
      }
      free_udf(u_d);
      initialized= FALSE;
    }
    if (buffers)				// Because of bug in ecc
      delete [] buffers;
    buffers= 0;
  }
}


bool
udf_handler::fix_fields(THD *thd, Item_func_or_sum *func,
			uint arg_count, Item **arguments)
{
  uchar buff[STACK_BUFF_ALLOC];			// Max argument in function
  DBUG_ENTER("Item_udf_func::fix_fields");

  if (check_stack_overrun(thd, STACK_MIN_SIZE, buff))
    DBUG_RETURN(TRUE);				// Fatal error flag is set!

  udf_func *tmp_udf=find_udf(u_d->name.str,(uint) u_d->name.length,1);

  if (!tmp_udf)
  {
    my_error(ER_CANT_FIND_UDF, MYF(0), u_d->name.str);
    DBUG_RETURN(TRUE);
  }
  u_d=tmp_udf;
  args=arguments;

  /* Fix all arguments */
  func->maybe_null=0;
  func->used_tables_and_const_cache_init();

  if ((f_args.arg_count=arg_count))
  {
    if (!(f_args.arg_type= (Item_result*)
	  thd->alloc(f_args.arg_count*sizeof(Item_result))))

    {
      free_udf(u_d);
      DBUG_RETURN(TRUE);
    }
    uint i;
    Item **arg,**arg_end;
    for (i=0, arg=arguments, arg_end=arguments+arg_count;
	 arg != arg_end ;
	 arg++,i++)
    {
      if (!(*arg)->fixed &&
          (*arg)->fix_fields(thd, arg))
	DBUG_RETURN(1);
      // we can't assign 'item' before, because fix_fields() can change arg
      Item *item= *arg;
      if (item->check_cols(1))
	DBUG_RETURN(TRUE);
      /*
	TODO: We should think about this. It is not always
	right way just to set an UDF result to return my_charset_bin
	if one argument has binary sorting order.
	The result collation should be calculated according to arguments
	derivations in some cases and should not in other cases.
	Moreover, some arguments can represent a numeric input
	which doesn't effect the result character set and collation.
	There is no a general rule for UDF. Everything depends on
        the particular user defined function.
      */
      if (item->collation.collation->state & MY_CS_BINSORT)
	func->collation.set(&my_charset_bin);
      if (item->maybe_null)
	func->maybe_null=1;
      func->with_sum_func= func->with_sum_func || item->with_sum_func;
      func->with_window_func= func->with_window_func ||
                              item->with_window_func;
      func->with_field= func->with_field || item->with_field;
      func->with_param= func->with_param || item->with_param;
      func->with_subselect|= item->with_subselect;
      func->used_tables_and_const_cache_join(item);
      f_args.arg_type[i]=item->result_type();
    }
    //TODO: why all following memory is not allocated with 1 thd->alloc() call?
    if (!(buffers=new String[arg_count]) ||
	!(f_args.args= (char**) thd->alloc(arg_count * sizeof(char *))) ||
	!(f_args.lengths= (ulong*) thd->alloc(arg_count * sizeof(long))) ||
	!(f_args.maybe_null= (char*) thd->alloc(arg_count * sizeof(char))) ||
	!(num_buffer= (char*) thd->alloc(arg_count *
					ALIGN_SIZE(sizeof(double)))) ||
	!(f_args.attributes= (char**) thd->alloc(arg_count *
                                                 sizeof(char *))) ||
	!(f_args.attribute_lengths= (ulong*) thd->alloc(arg_count *
						       sizeof(long))))
    {
      free_udf(u_d);
      DBUG_RETURN(TRUE);
    }
  }
  if (func->fix_length_and_dec())
    DBUG_RETURN(TRUE);
  initid.max_length=func->max_length;
  initid.maybe_null=func->maybe_null;
  initid.const_item=func->const_item_cache;
  initid.decimals=func->decimals;
  initid.ptr=0;

  if (u_d->func_init)
  {
    char init_msg_buff[MYSQL_ERRMSG_SIZE];
    char *to=num_buffer;
    for (uint i=0; i < arg_count; i++)
    {
      /*
       For a constant argument i, args->args[i] points to the argument value. 
       For non-constant, args->args[i] is NULL.
      */
      f_args.args[i]= NULL;         /* Non-const unless updated below. */

      f_args.lengths[i]= arguments[i]->max_length;
      f_args.maybe_null[i]= (char) arguments[i]->maybe_null;
      f_args.attributes[i]= arguments[i]->name;
      f_args.attribute_lengths[i]= arguments[i]->name_length;

      if (arguments[i]->const_item())
      {
        switch (arguments[i]->result_type()) {
        case STRING_RESULT:
        case DECIMAL_RESULT:
        {
          String *res= arguments[i]->val_str(&buffers[i]);
          if (arguments[i]->null_value)
            continue;
          f_args.args[i]= (char*) res->c_ptr_safe();
          f_args.lengths[i]= res->length();
          break;
        }
        case INT_RESULT:
          *((longlong*) to)= arguments[i]->val_int();
          if (arguments[i]->null_value)
            continue;
          f_args.args[i]= to;
          to+= ALIGN_SIZE(sizeof(longlong));
          break;
        case REAL_RESULT:
          *((double*) to)= arguments[i]->val_real();
          if (arguments[i]->null_value)
            continue;
          f_args.args[i]= to;
          to+= ALIGN_SIZE(sizeof(double));
          break;
        case ROW_RESULT:
        case TIME_RESULT:
          DBUG_ASSERT(0);          // This case should never be chosen
          break;
        }
      }
    }
    Udf_func_init init= u_d->func_init;
    if ((error=(uchar) init(&initid, &f_args, init_msg_buff)))
    {
      my_error(ER_CANT_INITIALIZE_UDF, MYF(0),
               u_d->name.str, init_msg_buff);
      free_udf(u_d);
      DBUG_RETURN(TRUE);
    }
    func->max_length=MY_MIN(initid.max_length,MAX_BLOB_WIDTH);
    func->maybe_null=initid.maybe_null;
    /*
      The above call for init() can reset initid.const_item to "false",
      e.g. when the UDF function wants to be non-deterministic.
      See sequence_init() in udf_example.cc.
    */
    func->const_item_cache= initid.const_item;
    func->decimals=MY_MIN(initid.decimals,NOT_FIXED_DEC);
  }
  initialized=1;
  if (error)
  {
    my_error(ER_CANT_INITIALIZE_UDF, MYF(0),
             u_d->name.str, ER_THD(thd, ER_UNKNOWN_ERROR));
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


bool udf_handler::get_arguments()
{
  if (error)
    return 1;					// Got an error earlier
  char *to= num_buffer;
  uint str_count=0;
  for (uint i=0; i < f_args.arg_count; i++)
  {
    f_args.args[i]=0;
    switch (f_args.arg_type[i]) {
    case STRING_RESULT:
    case DECIMAL_RESULT:
      {
	String *res=args[i]->val_str(&buffers[str_count++]);
	if (!(args[i]->null_value))
	{
	  f_args.args[i]=    (char*) res->ptr();
	  f_args.lengths[i]= res->length();
	}
	else
	{
	  f_args.lengths[i]= 0;
	}
	break;
      }
    case INT_RESULT:
      *((longlong*) to) = args[i]->val_int();
      if (!args[i]->null_value)
      {
	f_args.args[i]=to;
	to+= ALIGN_SIZE(sizeof(longlong));
      }
      break;
    case REAL_RESULT:
      *((double*) to)= args[i]->val_real();
      if (!args[i]->null_value)
      {
	f_args.args[i]=to;
	to+= ALIGN_SIZE(sizeof(double));
      }
      break;
    case ROW_RESULT:
    case TIME_RESULT:
      DBUG_ASSERT(0);              // This case should never be chosen
      break;
    }
  }
  return 0;
}

/**
  @return
    (String*)NULL in case of NULL values
*/
String *udf_handler::val_str(String *str,String *save_str)
{
  uchar is_null_tmp=0;
  ulong res_length;
  DBUG_ENTER("udf_handler::val_str");

  if (get_arguments())
    DBUG_RETURN(0);
  char * (*func)(UDF_INIT *, UDF_ARGS *, char *, ulong *, uchar *, uchar *)=
    (char* (*)(UDF_INIT *, UDF_ARGS *, char *, ulong *, uchar *, uchar *))
    u_d->func;

  if ((res_length=str->alloced_length()) < MAX_FIELD_WIDTH)
  {						// This happens VERY seldom
    if (str->alloc(MAX_FIELD_WIDTH))
    {
      error=1;
      DBUG_RETURN(0);
    }
  }
  char *res=func(&initid, &f_args, (char*) str->ptr(), &res_length,
		 &is_null_tmp, &error);
  DBUG_PRINT("info", ("udf func returned, res_length: %lu", res_length));
  if (is_null_tmp || !res || error)		// The !res is for safety
  {
    DBUG_PRINT("info", ("Null or error"));
    DBUG_RETURN(0);
  }
  if (res == str->ptr())
  {
    str->length(res_length);
    DBUG_PRINT("exit", ("str: %*.s", (int) str->length(), str->ptr()));
    DBUG_RETURN(str);
  }
  save_str->set(res, res_length, str->charset());
  DBUG_PRINT("exit", ("save_str: %s", save_str->ptr()));
  DBUG_RETURN(save_str);
}


/*
  For the moment, UDF functions are returning DECIMAL values as strings
*/

my_decimal *udf_handler::val_decimal(my_bool *null_value, my_decimal *dec_buf)
{
  char buf[DECIMAL_MAX_STR_LENGTH+1], *end;
  ulong res_length= DECIMAL_MAX_STR_LENGTH;

  if (get_arguments())
  {
    *null_value=1;
    return 0;
  }
  char *(*func)(UDF_INIT *, UDF_ARGS *, char *, ulong *, uchar *, uchar *)=
    (char* (*)(UDF_INIT *, UDF_ARGS *, char *, ulong *, uchar *, uchar *))
    u_d->func;

  char *res= func(&initid, &f_args, buf, &res_length, &is_null, &error);
  if (is_null || error)
  {
    *null_value= 1;
    return 0;
  }
  end= res+ res_length;
  str2my_decimal(E_DEC_FATAL_ERROR, res, dec_buf, &end);
  return dec_buf;
}


void Item_udf_func::cleanup()
{
  udf.cleanup();
  Item_func::cleanup();
}


void Item_udf_func::print(String *str, enum_query_type query_type)
{
  str->append(func_name());
  str->append('(');
  for (uint i=0 ; i < arg_count ; i++)
  {
    if (i != 0)
      str->append(',');
    args[i]->print_item_w_name(str, query_type);
  }
  str->append(')');
}


double Item_func_udf_float::val_real()
{
  double res;
  my_bool tmp_null_value;
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_func_udf_float::val");
  DBUG_PRINT("info",("result_type: %d  arg_count: %d",
		     args[0]->result_type(), arg_count));
  res= udf.val(&tmp_null_value);
  null_value= tmp_null_value;
  DBUG_RETURN(res);
}


String *Item_func_udf_float::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  double nr= val_real();
  if (null_value)
    return 0;					/* purecov: inspected */
  str->set_real(nr,decimals,&my_charset_bin);
  return str;
}


longlong Item_func_udf_int::val_int()
{
  longlong res;
  my_bool tmp_null_value;
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_func_udf_int::val_int");
  res= udf.val_int(&tmp_null_value);
  null_value= tmp_null_value;
  DBUG_RETURN(res);
}


String *Item_func_udf_int::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  longlong nr=val_int();
  if (null_value)
    return 0;
  str->set_int(nr, unsigned_flag, &my_charset_bin);
  return str;
}


longlong Item_func_udf_decimal::val_int()
{
  my_bool tmp_null_value;
  longlong result;
  my_decimal dec_buf, *dec= udf.val_decimal(&tmp_null_value, &dec_buf);
  null_value= tmp_null_value;
  if (null_value)
    return 0;
  my_decimal2int(E_DEC_FATAL_ERROR, dec, unsigned_flag, &result);
  return result;
}


double Item_func_udf_decimal::val_real()
{
  my_bool tmp_null_value;
  double result;
  my_decimal dec_buf, *dec= udf.val_decimal(&tmp_null_value, &dec_buf);
  null_value= tmp_null_value;
  if (null_value)
    return 0.0;
  my_decimal2double(E_DEC_FATAL_ERROR, dec, &result);
  return result;
}


my_decimal *Item_func_udf_decimal::val_decimal(my_decimal *dec_buf)
{
  my_decimal *res;
  my_bool tmp_null_value;
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_func_udf_decimal::val_decimal");
  DBUG_PRINT("info",("result_type: %d  arg_count: %d",
                     args[0]->result_type(), arg_count));

  res= udf.val_decimal(&tmp_null_value, dec_buf);
  null_value= tmp_null_value;
  DBUG_RETURN(res);
}


String *Item_func_udf_decimal::val_str(String *str)
{
  my_bool tmp_null_value;
  my_decimal dec_buf, *dec= udf.val_decimal(&tmp_null_value, &dec_buf);
  null_value= tmp_null_value;
  if (null_value)
    return 0;
  if (str->length() < DECIMAL_MAX_STR_LENGTH)
    str->length(DECIMAL_MAX_STR_LENGTH);
  my_decimal_round(E_DEC_FATAL_ERROR, dec, decimals, FALSE, &dec_buf);
  my_decimal2string(E_DEC_FATAL_ERROR, &dec_buf, 0, 0, '0', str);
  return str;
}


/* Default max_length is max argument length */

bool Item_func_udf_str::fix_length_and_dec()
{
  DBUG_ENTER("Item_func_udf_str::fix_length_and_dec");
  max_length=0;
  for (uint i = 0; i < arg_count; i++)
    set_if_bigger(max_length,args[i]->max_length);
  DBUG_RETURN(FALSE);
}

String *Item_func_udf_str::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String *res=udf.val_str(str,&str_value);
  null_value = !res;
  return res;
}


/**
  @note
  This has to come last in the udf_handler methods, or C for AIX
  version 6.0.0.0 fails to compile with debugging enabled. (Yes, really.)
*/

udf_handler::~udf_handler()
{
  /* Everything should be properly cleaned up by this moment. */
  DBUG_ASSERT(not_original || !(initialized || buffers));
}

#else
bool udf_handler::get_arguments() { return 0; }
#endif /* HAVE_DLOPEN */


longlong Item_master_pos_wait::val_int()
{
  DBUG_ASSERT(fixed == 1);
  THD* thd = current_thd;
  String *log_name = args[0]->val_str(&value);
  int event_count= 0;

  null_value=0;
  if (thd->slave_thread || !log_name || !log_name->length())
  {
    null_value = 1;
    return 0;
  }
#ifdef HAVE_REPLICATION
  longlong pos = (ulong)args[1]->val_int();
  longlong timeout = (arg_count>=3) ? args[2]->val_int() : 0 ;
  String connection_name_buff;
  LEX_STRING connection_name;
  Master_info *mi= NULL;
  if (arg_count >= 4)
  {
    String *con;
    if (!(con= args[3]->val_str(&connection_name_buff)))
      goto err;

    connection_name.str= (char*) con->ptr();
    connection_name.length= con->length();
    if (check_master_connection_name(&connection_name))
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(ME_JUST_WARNING),
               "MASTER_CONNECTION_NAME");
      goto err;
    }
  }
  else
    connection_name= thd->variables.default_master_connection;

  if (!(mi= get_master_info(&connection_name, Sql_condition::WARN_LEVEL_WARN)))
    goto err;

  if ((event_count = mi->rli.wait_for_pos(thd, log_name, pos, timeout)) == -2)
  {
    null_value = 1;
    event_count=0;
  }
  mi->release();
#endif
  return event_count;

#ifdef HAVE_REPLICATION
err:
  {
    null_value = 1;
    return 0;
  }
#endif
}


longlong Item_master_gtid_wait::val_int()
{
  DBUG_ASSERT(fixed == 1);
  longlong result= 0;
  String *gtid_pos __attribute__((unused)) = args[0]->val_str(&value);

  if (args[0]->null_value)
  {
    null_value= 1;
    return 0;
  }

  null_value=0;
#ifdef HAVE_REPLICATION
  THD* thd= current_thd;
  longlong timeout_us;

  if (arg_count==2 && !args[1]->null_value)
    timeout_us= (longlong)(1e6*args[1]->val_real());
  else
    timeout_us= (longlong)-1;

  result= rpl_global_gtid_waiting.wait_for_pos(thd, gtid_pos, timeout_us);
#endif
  return result;
}


/**
  Enables a session to wait on a condition until a timeout or a network
  disconnect occurs.

  @remark The connection is polled every m_interrupt_interval nanoseconds.
*/

class Interruptible_wait
{
  THD *m_thd;
  struct timespec m_abs_timeout;
  static const ulonglong m_interrupt_interval;

  public:
    Interruptible_wait(THD *thd)
    : m_thd(thd) {}

    ~Interruptible_wait() {}

  public:
    /**
      Set the absolute timeout.

      @param timeout The amount of time in nanoseconds to wait
    */
    void set_timeout(ulonglong timeout)
    {
      /*
        Calculate the absolute system time at the start so it can
        be controlled in slices. It relies on the fact that once
        the absolute time passes, the timed wait call will fail
        automatically with a timeout error.
      */
      set_timespec_nsec(m_abs_timeout, timeout);
    }

    /** The timed wait. */
    int wait(mysql_cond_t *, mysql_mutex_t *);
};


/** Time to wait before polling the connection status. */
const ulonglong Interruptible_wait::m_interrupt_interval= 5 * 1000000000ULL;


/**
  Wait for a given condition to be signaled.

  @param cond   The condition variable to wait on.
  @param mutex  The associated mutex.

  @remark The absolute timeout is preserved across calls.

  @retval return value from mysql_cond_timedwait
*/

int Interruptible_wait::wait(mysql_cond_t *cond, mysql_mutex_t *mutex)
{
  int error;
  struct timespec timeout;

  while (1)
  {
    /* Wait for a fixed interval. */
    set_timespec_nsec(timeout, m_interrupt_interval);

    /* But only if not past the absolute timeout. */
    if (cmp_timespec(timeout, m_abs_timeout) > 0)
      timeout= m_abs_timeout;

    error= mysql_cond_timedwait(cond, mutex, &timeout);
    if (error == ETIMEDOUT || error == ETIME)
    {
      /* Return error if timed out or connection is broken. */
      if (!cmp_timespec(timeout, m_abs_timeout) || !m_thd->is_connected())
        break;
    }
    /* Otherwise, propagate status to the caller. */
    else
      break;
  }

  return error;
}


/**
  For locks with EXPLICIT duration, MDL returns a new ticket
  every time a lock is granted. This allows to implement recursive
  locks without extra allocation or additional data structures, such
  as below. However, if there are too many tickets in the same
  MDL_context, MDL_context::find_ticket() is getting too slow,
  since it's using a linear search.
  This is why a separate structure is allocated for a user
  level lock, and before requesting a new lock from MDL,
  GET_LOCK() checks thd->ull_hash if such lock is already granted,
  and if so, simply increments a reference counter.
*/

class User_level_lock
{
public:
  MDL_ticket *lock;
  int refs;
};


/** Extract a hash key from User_level_lock. */

uchar *ull_get_key(const uchar *ptr, size_t *length,
                   my_bool not_used __attribute__((unused)))
{
  User_level_lock *ull = (User_level_lock*) ptr;
  MDL_key *key = ull->lock->get_key();
  *length= key->length();
  return (uchar*) key->ptr();
}


/**
  Release all user level locks for this THD.
*/

void mysql_ull_cleanup(THD *thd)
{
  User_level_lock *ull;
  DBUG_ENTER("mysql_ull_cleanup");

  for (uint i= 0; i < thd->ull_hash.records; i++)
  {
    ull = (User_level_lock*) my_hash_element(&thd->ull_hash, i);
    thd->mdl_context.release_lock(ull->lock);
    my_free(ull);
  }

  my_hash_free(&thd->ull_hash);

  DBUG_VOID_RETURN;
}


/**
  Set explicit duration for metadata locks corresponding to
  user level locks to protect them from being released at the end
  of transaction.
*/

void mysql_ull_set_explicit_lock_duration(THD *thd)
{
  User_level_lock *ull;
  DBUG_ENTER("mysql_ull_set_explicit_lock_duration");

  for (uint i= 0; i < thd->ull_hash.records; i++)
  {
    ull= (User_level_lock*) my_hash_element(&thd->ull_hash, i);
    thd->mdl_context.set_lock_duration(ull->lock, MDL_EXPLICIT);
  }
  DBUG_VOID_RETURN;
}


/**
  When MDL detects a lock wait timeout, it pushes
  an error into the statement diagnostics area.
  For GET_LOCK(), lock wait timeout is not an error,
  but a special return value (0).
  Similarly, killing get_lock wait is not an error either,
  but a return value NULL.
  Capture and suppress lock wait timeouts and kills.
*/

class Lock_wait_timeout_handler: public Internal_error_handler
{
public:
  Lock_wait_timeout_handler() :m_lock_wait_timeout(false) {}

  bool m_lock_wait_timeout;

  bool handle_condition(THD * /* thd */, uint sql_errno,
                        const char * /* sqlstate */,
                        Sql_condition::enum_warning_level* /* level */,
                        const char *message,
                        Sql_condition ** /* cond_hdl */);
};

bool
Lock_wait_timeout_handler::
handle_condition(THD *thd, uint sql_errno,
                 const char * /* sqlstate */,
                 Sql_condition::enum_warning_level* /* level */,
                 const char *message,
                 Sql_condition ** /* cond_hdl */)
{
  if (sql_errno == ER_LOCK_WAIT_TIMEOUT)
  {
    m_lock_wait_timeout= true;
    return true;                                /* condition handled */
  }
  if (thd->is_killed())
    return true;

  return false;
}


static int ull_name_ok(String *name)
{
  if (!name || !name->length())
    return 0;

  if (name->length() > NAME_LEN)
  {
    my_error(ER_TOO_LONG_IDENT, MYF(0), name->c_ptr_safe());
    return 0;
  }
  return 1;
}


/**
  Get a user level lock.

  @retval
    1    : Got lock
  @retval
    0    : Timeout
  @retval
    NULL : Error
*/

longlong Item_func_get_lock::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res= args[0]->val_str(&value);
  double timeout= args[1]->val_real();
  THD *thd= current_thd;
  User_level_lock *ull;
  DBUG_ENTER("Item_func_get_lock::val_int");

  null_value= 1;
  /*
    In slave thread no need to get locks, everything is serialized. Anyway
    there is no way to make GET_LOCK() work on slave like it did on master
    (i.e. make it return exactly the same value) because we don't have the
    same other concurrent threads environment. No matter what we return here,
    it's not guaranteed to be same as on master.
  */
  if (thd->slave_thread)
  {
    null_value= 0;
    DBUG_RETURN(1);
  }

  if (args[1]->null_value ||
      (!args[1]->unsigned_flag && ((longlong) timeout < 0)))
  {
    char buf[22];
    if (args[1]->null_value)
      strmov(buf, "NULL");
    else
      llstr(((longlong) timeout), buf);
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_WRONG_VALUE_FOR_TYPE, ER(ER_WRONG_VALUE_FOR_TYPE),
                        "timeout", buf, "get_lock");
    null_value= 1;
    DBUG_RETURN(0);
  }

  if (!ull_name_ok(res))
    DBUG_RETURN(0);
  DBUG_PRINT("enter", ("lock: %.*s", res->length(), res->ptr()));
  /* HASH entries are of type User_level_lock. */
  if (! my_hash_inited(&thd->ull_hash) &&
        my_hash_init(&thd->ull_hash, &my_charset_bin,
                     16 /* small hash */, 0, 0, ull_get_key, NULL, 0))
  {
    DBUG_RETURN(0);
  }

  MDL_request ull_request;
  ull_request.init(MDL_key::USER_LOCK, res->c_ptr_safe(), "",
                   MDL_SHARED_NO_WRITE, MDL_EXPLICIT);
  MDL_key *ull_key = &ull_request.key;


  if ((ull= (User_level_lock*)
       my_hash_search(&thd->ull_hash, ull_key->ptr(), ull_key->length())))
  {
    /* Recursive lock */
    ull->refs++;
    null_value = 0;
    DBUG_PRINT("info", ("recursive lock, ref-count: %d", (int) ull->refs));
    DBUG_RETURN(1);
  }

  Lock_wait_timeout_handler lock_wait_timeout_handler;
  thd->push_internal_handler(&lock_wait_timeout_handler);
  bool error= thd->mdl_context.acquire_lock(&ull_request, timeout);
  (void) thd->pop_internal_handler();
  if (error)
  {
    if (lock_wait_timeout_handler.m_lock_wait_timeout)
      null_value= 0;
    DBUG_RETURN(0);
  }

  ull= (User_level_lock*) my_malloc(sizeof(User_level_lock),
                                    MYF(MY_WME|MY_THREAD_SPECIFIC));
  if (ull == NULL)
  {
    thd->mdl_context.release_lock(ull_request.ticket);
    DBUG_RETURN(0);
  }

  ull->lock= ull_request.ticket;
  ull->refs= 1;

  if (my_hash_insert(&thd->ull_hash, (uchar*) ull))
  {
    thd->mdl_context.release_lock(ull->lock);
    my_free(ull);
    DBUG_RETURN(0);
  }
  null_value= 0;

  DBUG_RETURN(1);
}


/**
  Release a user level lock.
  @return
    - 1 if lock released
    - 0 if lock wasn't held
    - (SQL) NULL if no such lock
*/

longlong Item_func_release_lock::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res= args[0]->val_str(&value);
  THD *thd= current_thd;
  DBUG_ENTER("Item_func_release_lock::val_int");
  null_value= 1;

  if (!ull_name_ok(res))
    DBUG_RETURN(0);

  DBUG_PRINT("enter", ("lock: %.*s", res->length(), res->ptr()));

  MDL_key ull_key;
  ull_key.mdl_key_init(MDL_key::USER_LOCK, res->c_ptr_safe(), "");

  User_level_lock *ull;

  if (!my_hash_inited(&thd->ull_hash) ||
      !(ull=
        (User_level_lock*) my_hash_search(&thd->ull_hash,
                                          ull_key.ptr(), ull_key.length())))
  {
    null_value= thd->mdl_context.get_lock_owner(&ull_key) == 0;
    DBUG_RETURN(0);
  }
  DBUG_PRINT("info", ("ref count: %d", (int) ull->refs));
  null_value= 0;
  if (--ull->refs == 0)
  {
    my_hash_delete(&thd->ull_hash, (uchar*) ull);
    thd->mdl_context.release_lock(ull->lock);
    my_free(ull);
  }
  DBUG_RETURN(1);
}


/**
  Check a user level lock.

  Sets null_value=TRUE on error.

  @retval
    1		Available
  @retval
    0		Already taken, or error
*/

longlong Item_func_is_free_lock::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res= args[0]->val_str(&value);
  THD *thd= current_thd;
  null_value= 1;

  if (!ull_name_ok(res))
    return 0;

  MDL_key ull_key;
  ull_key.mdl_key_init(MDL_key::USER_LOCK, res->c_ptr_safe(), "");

  null_value= 0;
  return thd->mdl_context.get_lock_owner(&ull_key) == 0;
}


longlong Item_func_is_used_lock::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res= args[0]->val_str(&value);
  THD *thd= current_thd;
  null_value= 1;

  if (!ull_name_ok(res))
    return 0;

  MDL_key ull_key;
  ull_key.mdl_key_init(MDL_key::USER_LOCK, res->c_ptr_safe(), "");
  ulong thread_id = thd->mdl_context.get_lock_owner(&ull_key);
  if (thread_id == 0)
    return 0;

  null_value= 0;
  return thread_id;
}


longlong Item_func_last_insert_id::val_int()
{
  THD *thd= current_thd;
  DBUG_ASSERT(fixed == 1);
  if (arg_count)
  {
    longlong value= args[0]->val_int();
    null_value= args[0]->null_value;
    /*
      LAST_INSERT_ID(X) must affect the client's mysql_insert_id() as
      documented in the manual. We don't want to touch
      first_successful_insert_id_in_cur_stmt because it would make
      LAST_INSERT_ID(X) take precedence over an generated auto_increment
      value for this row.
    */
    thd->arg_of_last_insert_id_function= TRUE;
    thd->first_successful_insert_id_in_prev_stmt= value;
    return value;
  }
  return
    static_cast<longlong>(thd->read_first_successful_insert_id_in_prev_stmt());
}


bool Item_func_last_insert_id::fix_fields(THD *thd, Item **ref)
{
  thd->lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
  return Item_int_func::fix_fields(thd, ref);
}


/* This function is just used to test speed of different functions */

longlong Item_func_benchmark::val_int()
{
  DBUG_ASSERT(fixed == 1);
  char buff[MAX_FIELD_WIDTH];
  String tmp(buff,sizeof(buff), &my_charset_bin);
  my_decimal tmp_decimal;
  THD *thd= current_thd;
  ulonglong loop_count;

  loop_count= (ulonglong) args[0]->val_int();

  if (args[0]->null_value ||
      (!args[0]->unsigned_flag && (((longlong) loop_count) < 0)))
  {
    if (!args[0]->null_value)
    {
      char buff[22];
      llstr(((longlong) loop_count), buff);
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WRONG_VALUE_FOR_TYPE,
                          ER_THD(thd, ER_WRONG_VALUE_FOR_TYPE),
                          "count", buff, "benchmark");
    }

    null_value= 1;
    return 0;
  }

  null_value=0;
  for (ulonglong loop=0 ; loop < loop_count && !thd->killed; loop++)
  {
    switch (args[1]->result_type()) {
    case REAL_RESULT:
      (void) args[1]->val_real();
      break;
    case INT_RESULT:
      (void) args[1]->val_int();
      break;
    case STRING_RESULT:
      (void) args[1]->val_str(&tmp);
      break;
    case DECIMAL_RESULT:
      (void) args[1]->val_decimal(&tmp_decimal);
      break;
    case ROW_RESULT:
    case TIME_RESULT:
      DBUG_ASSERT(0);              // This case should never be chosen
      return 0;
    }
  }
  return 0;
}


void Item_func_benchmark::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("benchmark("));
  args[0]->print(str, query_type);
  str->append(',');
  args[1]->print(str, query_type);
  str->append(')');
}


mysql_mutex_t LOCK_item_func_sleep;

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_LOCK_item_func_sleep;

static PSI_mutex_info item_func_sleep_mutexes[]=
{
  { &key_LOCK_item_func_sleep, "LOCK_user_locks", PSI_FLAG_GLOBAL}
};


static void init_item_func_sleep_psi_keys(void)
{
  const char* category= "sql";
  int count;

  if (PSI_server == NULL)
    return;

  count= array_elements(item_func_sleep_mutexes);
  PSI_server->register_mutex(category, item_func_sleep_mutexes, count);
}
#endif

static bool item_func_sleep_inited= 0;


void item_func_sleep_init(void)
{
#ifdef HAVE_PSI_INTERFACE
  init_item_func_sleep_psi_keys();
#endif

  mysql_mutex_init(key_LOCK_item_func_sleep, &LOCK_item_func_sleep, MY_MUTEX_INIT_SLOW);
  item_func_sleep_inited= 1;
}


void item_func_sleep_free(void)
{
  if (item_func_sleep_inited)
  {
    item_func_sleep_inited= 0;
    mysql_mutex_destroy(&LOCK_item_func_sleep);
  }
}


/** This function is just used to create tests with time gaps. */

longlong Item_func_sleep::val_int()
{
  THD *thd= current_thd;
  Interruptible_wait timed_cond(thd);
  mysql_cond_t cond;
  double timeout;
  int error;

  DBUG_ASSERT(fixed == 1);

  timeout= args[0]->val_real();
  /*
    On 64-bit OSX mysql_cond_timedwait() waits forever
    if passed abstime time has already been exceeded by 
    the system time.
    When given a very short timeout (< 10 mcs) just return 
    immediately.
    We assume that the lines between this test and the call 
    to mysql_cond_timedwait() will be executed in less than 0.00001 sec.
  */
  if (timeout < 0.00001)
    return 0;

  timed_cond.set_timeout((ulonglong) (timeout * 1000000000.0));

  mysql_cond_init(key_item_func_sleep_cond, &cond, NULL);
  mysql_mutex_lock(&LOCK_item_func_sleep);

  THD_STAGE_INFO(thd, stage_user_sleep);
  thd->mysys_var->current_mutex= &LOCK_item_func_sleep;
  thd->mysys_var->current_cond=  &cond;

  error= 0;
  thd_wait_begin(thd, THD_WAIT_SLEEP);
  while (!thd->killed)
  {
    error= timed_cond.wait(&cond, &LOCK_item_func_sleep);
    if (error == ETIMEDOUT || error == ETIME)
      break;
    error= 0;
  }
  thd_wait_end(thd);
  mysql_mutex_unlock(&LOCK_item_func_sleep);
  mysql_mutex_lock(&thd->mysys_var->mutex);
  thd->mysys_var->current_mutex= 0;
  thd->mysys_var->current_cond=  0;
  mysql_mutex_unlock(&thd->mysys_var->mutex);

  mysql_cond_destroy(&cond);

  DBUG_EXECUTE_IF("sleep_inject_query_done_debug_sync", {
      debug_sync_set_action
        (thd, STRING_WITH_LEN("dispatch_command_end SIGNAL query_done"));
    };);

  return MY_TEST(!error);                  // Return 1 killed
}


bool Item_func_user_var::check_vcol_func_processor(void *arg)
{
  return mark_unsupported_function("@", name.str, arg, VCOL_NON_DETERMINISTIC);
}

#define extra_size sizeof(double)

user_var_entry *get_variable(HASH *hash, LEX_STRING &name,
				    bool create_if_not_exists)
{
  user_var_entry *entry;

  if (!(entry = (user_var_entry*) my_hash_search(hash, (uchar*) name.str,
                                                 name.length)) &&
      create_if_not_exists)
  {
    uint size=ALIGN_SIZE(sizeof(user_var_entry))+name.length+1+extra_size;
    if (!my_hash_inited(hash))
      return 0;
    if (!(entry = (user_var_entry*) my_malloc(size,
                                              MYF(MY_WME | ME_FATALERROR |
                                                  MY_THREAD_SPECIFIC))))
      return 0;
    entry->name.str=(char*) entry+ ALIGN_SIZE(sizeof(user_var_entry))+
      extra_size;
    entry->name.length=name.length;
    entry->value=0;
    entry->length=0;
    entry->update_query_id=0;
    entry->set_charset(NULL);
    entry->unsigned_flag= 0;
    /*
      If we are here, we were called from a SET or a query which sets a
      variable. Imagine it is this:
      INSERT INTO t SELECT @a:=10, @a:=@a+1.
      Then when we have a Item_func_get_user_var (because of the @a+1) so we
      think we have to write the value of @a to the binlog. But before that,
      we have a Item_func_set_user_var to create @a (@a:=10), in this we mark
      the variable as "already logged" (line below) so that it won't be logged
      by Item_func_get_user_var (because that's not necessary).
    */
    entry->used_query_id=current_thd->query_id;
    entry->type=STRING_RESULT;
    memcpy(entry->name.str, name.str, name.length+1);
    if (my_hash_insert(hash,(uchar*) entry))
    {
      my_free(entry);
      return 0;
    }
  }
  return entry;
}


void Item_func_set_user_var::cleanup()
{
  Item_func::cleanup();
  m_var_entry= NULL;
}


bool Item_func_set_user_var::set_entry(THD *thd, bool create_if_not_exists)
{
  if (m_var_entry && thd->thread_id == entry_thread_id)
    goto end; // update entry->update_query_id for PS
  if (!(m_var_entry= get_variable(&thd->user_vars, name, create_if_not_exists)))
  {
    entry_thread_id= 0;
    return TRUE;
  }
  entry_thread_id= thd->thread_id;
  /* 
     Remember the last query which updated it, this way a query can later know
     if this variable is a constant item in the query (it is if update_query_id
     is different from query_id).
  */
end:
  m_var_entry->update_query_id= thd->query_id;
  return FALSE;
}


/*
  When a user variable is updated (in a SET command or a query like
  SELECT @a:= ).
*/

bool Item_func_set_user_var::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  /* fix_fields will call Item_func_set_user_var::fix_length_and_dec */
  if (Item_func::fix_fields(thd, ref) || set_entry(thd, TRUE))
    return TRUE;
  /*
    As it is wrong and confusing to associate any 
    character set with NULL, @a should be latin2
    after this query sequence:

      SET @a=_latin2'string';
      SET @a=NULL;

    I.e. the second query should not change the charset
    to the current default value, but should keep the 
    original value assigned during the first query.
    In order to do it, we don't copy charset
    from the argument if the argument is NULL
    and the variable has previously been initialized.
  */
  null_item= (args[0]->type() == NULL_ITEM);
  if (!m_var_entry->charset() || !null_item)
    m_var_entry->set_charset(args[0]->collation.derivation == DERIVATION_NUMERIC ?
                             default_charset() : args[0]->collation.collation);
  collation.set(m_var_entry->charset(), DERIVATION_IMPLICIT);
  set_handler_by_result_type(args[0]->result_type(),
                             max_length, collation.collation);
  if (thd->lex->current_select)
  {
    /*
      When this function is used in a derived table/view force the derived
      table to be materialized to preserve possible side-effect of setting a
      user variable.
    */
    SELECT_LEX_UNIT *unit= thd->lex->current_select->master_unit();
    TABLE_LIST *derived;
    for (derived= unit->derived;
         derived;
         derived= unit->derived)
    {
      derived->set_materialized_derived();
      derived->prohibit_cond_pushdown= true;
      if (unit->with_element && unit->with_element->is_recursive)
        break;
      unit= derived->select_lex->master_unit();
    }
  }

  return FALSE;
}


bool
Item_func_set_user_var::fix_length_and_dec()
{
  maybe_null=args[0]->maybe_null;
  decimals=args[0]->decimals;
  collation.set(DERIVATION_IMPLICIT);
  if (args[0]->collation.derivation == DERIVATION_NUMERIC)
    fix_length_and_charset(args[0]->max_char_length(), default_charset());
  else
  {
    fix_length_and_charset(args[0]->max_char_length(),
                           args[0]->collation.collation);
  }
  unsigned_flag= args[0]->unsigned_flag;
  return FALSE;
}


/*
  Mark field in read_map

  NOTES
    This is used by filesort to register used fields in a a temporary
    column read set or to register used fields in a view
*/

bool Item_func_set_user_var::register_field_in_read_map(void *arg)
{
  if (result_field)
  {
    TABLE *table= (TABLE *) arg;
    if (result_field->table == table || !table)
      bitmap_set_bit(result_field->table->read_set, result_field->field_index);
    if (result_field->vcol_info)
      return result_field->vcol_info->
               expr->walk(&Item::register_field_in_read_map, 1, arg);
  }
  return 0;
}

/*
  Mark field in bitmap supplied as *arg

*/

bool Item_func_set_user_var::register_field_in_bitmap(void *arg)
{
  MY_BITMAP *bitmap = (MY_BITMAP *) arg;
  DBUG_ASSERT(bitmap);
  if (result_field)
  {
    if (!bitmap)
      return 1;
    bitmap_set_bit(bitmap, result_field->field_index);
  }
  return 0;
}

/**
  Set value to user variable.

  @param entry          pointer to structure representing variable
  @param set_null       should we set NULL value ?
  @param ptr            pointer to buffer with new value
  @param length         length of new value
  @param type           type of new value
  @param cs             charset info for new value
  @param dv             derivation for new value
  @param unsigned_arg   indiates if a value of type INT_RESULT is unsigned

  @note Sets error and fatal error if allocation fails.

  @retval
    false   success
  @retval
    true    failure
*/

bool
update_hash(user_var_entry *entry, bool set_null, void *ptr, uint length,
            Item_result type, CHARSET_INFO *cs,
            bool unsigned_arg)
{
  if (set_null)
  {
    char *pos= (char*) entry+ ALIGN_SIZE(sizeof(user_var_entry));
    if (entry->value && entry->value != pos)
      my_free(entry->value);
    entry->value= 0;
    entry->length= 0;
  }
  else
  {
    if (type == STRING_RESULT)
      length++;					// Store strings with end \0
    if (length <= extra_size)
    {
      /* Save value in value struct */
      char *pos= (char*) entry+ ALIGN_SIZE(sizeof(user_var_entry));
      if (entry->value != pos)
      {
	if (entry->value)
	  my_free(entry->value);
	entry->value=pos;
      }
    }
    else
    {
      /* Allocate variable */
      if (entry->length != length)
      {
	char *pos= (char*) entry+ ALIGN_SIZE(sizeof(user_var_entry));
	if (entry->value == pos)
	  entry->value=0;
        entry->value= (char*) my_realloc(entry->value, length,
                                         MYF(MY_ALLOW_ZERO_PTR | MY_WME |
                                             ME_FATALERROR |
                                             MY_THREAD_SPECIFIC));
        if (!entry->value)
	  return 1;
      }
    }
    if (type == STRING_RESULT)
    {
      length--;					// Fix length change above
      entry->value[length]= 0;			// Store end \0
    }
    memmove(entry->value, ptr, length);
    if (type == DECIMAL_RESULT)
      ((my_decimal*)entry->value)->fix_buffer_pointer();
    entry->length= length;
    entry->set_charset(cs);
    entry->unsigned_flag= unsigned_arg;
  }
  entry->type=type;
  return 0;
}


bool
Item_func_set_user_var::update_hash(void *ptr, uint length,
                                    Item_result res_type,
                                    CHARSET_INFO *cs,
                                    bool unsigned_arg)
{
  /*
    If we set a variable explicitly to NULL then keep the old
    result type of the variable
  */
  if (args[0]->type() == Item::FIELD_ITEM)
  {
    /* args[0]->null_value may be outdated */
    null_value= ((Item_field*)args[0])->field->is_null();
  }
  else
    null_value= args[0]->null_value;
  if (null_value && null_item)
    res_type= m_var_entry->type;                 // Don't change type of item
  if (::update_hash(m_var_entry, null_value,
                    ptr, length, res_type, cs, unsigned_arg))
  {
    null_value= 1;
    return 1;
  }
  return 0;
}


/** Get the value of a variable as a double. */

double user_var_entry::val_real(bool *null_value)
{
  if ((*null_value= (value == 0)))
    return 0.0;

  switch (type) {
  case REAL_RESULT:
    return *(double*) value;
  case INT_RESULT:
    return (double) *(longlong*) value;
  case DECIMAL_RESULT:
  {
    double result;
    my_decimal2double(E_DEC_FATAL_ERROR, (my_decimal *)value, &result);
    return result;
  }
  case STRING_RESULT:
    return my_atof(value);                      // This is null terminated
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);				// Impossible
    break;
  }
  return 0.0;					// Impossible
}


/** Get the value of a variable as an integer. */

longlong user_var_entry::val_int(bool *null_value) const
{
  if ((*null_value= (value == 0)))
    return 0;

  switch (type) {
  case REAL_RESULT:
    return (longlong) *(double*) value;
  case INT_RESULT:
    return *(longlong*) value;
  case DECIMAL_RESULT:
  {
    longlong result;
    my_decimal2int(E_DEC_FATAL_ERROR, (my_decimal *)value, 0, &result);
    return result;
  }
  case STRING_RESULT:
  {
    int error;
    return my_strtoll10(value, (char**) 0, &error);// String is null terminated
  }
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);				// Impossible
    break;
  }
  return 0;					// Impossible
}


/** Get the value of a variable as a string. */

String *user_var_entry::val_str(bool *null_value, String *str,
				uint decimals)
{
  if ((*null_value= (value == 0)))
    return (String*) 0;

  switch (type) {
  case REAL_RESULT:
    str->set_real(*(double*) value, decimals, charset());
    break;
  case INT_RESULT:
    if (!unsigned_flag)
      str->set(*(longlong*) value, charset());
    else
      str->set(*(ulonglong*) value, charset());
    break;
  case DECIMAL_RESULT:
    str_set_decimal((my_decimal *) value, str, charset());
    break;
  case STRING_RESULT:
    if (str->copy(value, length, charset()))
      str= 0;					// EOM error
    break;
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);				// Impossible
    break;
  }
  return(str);
}

/** Get the value of a variable as a decimal. */

my_decimal *user_var_entry::val_decimal(bool *null_value, my_decimal *val)
{
  if ((*null_value= (value == 0)))
    return 0;

  switch (type) {
  case REAL_RESULT:
    double2my_decimal(E_DEC_FATAL_ERROR, *(double*) value, val);
    break;
  case INT_RESULT:
    int2my_decimal(E_DEC_FATAL_ERROR, *(longlong*) value, 0, val);
    break;
  case DECIMAL_RESULT:
    my_decimal2decimal((my_decimal *) value, val);
    break;
  case STRING_RESULT:
    str2my_decimal(E_DEC_FATAL_ERROR, value, length, charset(), val);
    break;
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);				// Impossible
    break;
  }
  return(val);
}

/**
  This functions is invoked on SET \@variable or
  \@variable:= expression.

  Evaluate (and check expression), store results.

  @note
    For now it always return OK. All problem with value evaluating
    will be caught by thd->is_error() check in sql_set_variables().

  @retval
    FALSE OK.
*/

bool
Item_func_set_user_var::check(bool use_result_field)
{
  DBUG_ENTER("Item_func_set_user_var::check");
  if (use_result_field && !result_field)
    use_result_field= FALSE;

  switch (Item_func_set_user_var::result_type()) {
  case REAL_RESULT:
  {
    save_result.vreal= use_result_field ? result_field->val_real() :
                        args[0]->val_real();
    break;
  }
  case INT_RESULT:
  {
    save_result.vint= use_result_field ? result_field->val_int() :
                       args[0]->val_int();
    unsigned_flag= (use_result_field ?
                    ((Field_num*)result_field)->unsigned_flag:
                    args[0]->unsigned_flag);
    break;
  }
  case STRING_RESULT:
  {
    save_result.vstr= use_result_field ? result_field->val_str(&value) :
                       args[0]->val_str(&value);
    break;
  }
  case DECIMAL_RESULT:
  {
    save_result.vdec= use_result_field ?
                       result_field->val_decimal(&decimal_buff) :
                       args[0]->val_decimal(&decimal_buff);
    break;
  }
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);                // This case should never be chosen
    break;
  }
  DBUG_RETURN(FALSE);
}


/**
  @brief Evaluate and store item's result.
  This function is invoked on "SELECT ... INTO @var ...".
  
  @param    item    An item to get value from.
*/

void Item_func_set_user_var::save_item_result(Item *item)
{
  DBUG_ENTER("Item_func_set_user_var::save_item_result");

  switch (args[0]->result_type()) {
  case REAL_RESULT:
    save_result.vreal= item->val_result();
    break;
  case INT_RESULT:
    save_result.vint= item->val_int_result();
    unsigned_flag= item->unsigned_flag;
    break;
  case STRING_RESULT:
    save_result.vstr= item->str_result(&value);
    break;
  case DECIMAL_RESULT:
    save_result.vdec= item->val_decimal_result(&decimal_buff);
    break;
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);                // This case should never be chosen
    break;
  }
  DBUG_VOID_RETURN;
}


/**
  This functions is invoked on
  SET \@variable or \@variable:= expression.

  @note
    We have to store the expression as such in the variable, independent of
    the value method used by the user

  @retval
    0	OK
  @retval
    1	EOM Error

*/

bool
Item_func_set_user_var::update()
{
  bool res= 0;
  DBUG_ENTER("Item_func_set_user_var::update");

  switch (Item_func_set_user_var::result_type()) {
  case REAL_RESULT:
  {
    res= update_hash((void*) &save_result.vreal,sizeof(save_result.vreal),
		     REAL_RESULT, default_charset(), 0);
    break;
  }
  case INT_RESULT:
  {
    res= update_hash((void*) &save_result.vint, sizeof(save_result.vint),
                     INT_RESULT, default_charset(), unsigned_flag);
    break;
  }
  case STRING_RESULT:
  {
    if (!save_result.vstr)					// Null value
      res= update_hash((void*) 0, 0, STRING_RESULT, &my_charset_bin, 0);
    else
      res= update_hash((void*) save_result.vstr->ptr(),
		       save_result.vstr->length(), STRING_RESULT,
		       save_result.vstr->charset(), 0);
    break;
  }
  case DECIMAL_RESULT:
  {
    if (!save_result.vdec)					// Null value
      res= update_hash((void*) 0, 0, DECIMAL_RESULT, &my_charset_bin, 0);
    else
      res= update_hash((void*) save_result.vdec,
                       sizeof(my_decimal), DECIMAL_RESULT,
                       default_charset(), 0);
    break;
  }
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);                // This case should never be chosen
    break;
  }
  DBUG_RETURN(res);
}


double Item_func_set_user_var::val_real()
{
  DBUG_ASSERT(fixed == 1);
  check(0);
  update();					// Store expression
  return m_var_entry->val_real(&null_value);
}

longlong Item_func_set_user_var::val_int()
{
  DBUG_ASSERT(fixed == 1);
  check(0);
  update();					// Store expression
  return m_var_entry->val_int(&null_value);
}

String *Item_func_set_user_var::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  check(0);
  update();					// Store expression
  return m_var_entry->val_str(&null_value, str, decimals);
}


my_decimal *Item_func_set_user_var::val_decimal(my_decimal *val)
{
  DBUG_ASSERT(fixed == 1);
  check(0);
  update();					// Store expression
  return m_var_entry->val_decimal(&null_value, val);
}


double Item_func_set_user_var::val_result()
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return m_var_entry->val_real(&null_value);
}

longlong Item_func_set_user_var::val_int_result()
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return m_var_entry->val_int(&null_value);
}

bool Item_func_set_user_var::val_bool_result()
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return m_var_entry->val_int(&null_value) != 0;
}

String *Item_func_set_user_var::str_result(String *str)
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return m_var_entry->val_str(&null_value, str, decimals);
}


my_decimal *Item_func_set_user_var::val_decimal_result(my_decimal *val)
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return m_var_entry->val_decimal(&null_value, val);
}


bool Item_func_set_user_var::is_null_result()
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return is_null();
}


void Item_func_set_user_var::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("@"));
  str->append(name.str, name.length);
  str->append(STRING_WITH_LEN(":="));
  args[0]->print_parenthesised(str, query_type, precedence());
}


void Item_func_set_user_var::print_as_stmt(String *str,
                                           enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("set @"));
  str->append(name.str, name.length);
  str->append(STRING_WITH_LEN(":="));
  args[0]->print_parenthesised(str, query_type, precedence());
}

bool Item_func_set_user_var::send(Protocol *protocol, String *str_arg)
{
  if (result_field)
  {
    check(1);
    update();
    return protocol->store(result_field);
  }
  return Item::send(protocol, str_arg);
}

void Item_func_set_user_var::make_field(THD *thd, Send_field *tmp_field)
{
  if (result_field)
  {
    result_field->make_field(tmp_field);
    DBUG_ASSERT(tmp_field->table_name != 0);
    if (Item::name)
      tmp_field->col_name=Item::name;               // Use user supplied name
  }
  else
    Item::make_field(thd, tmp_field);
}


/*
  Save the value of a user variable into a field

  SYNOPSIS
    save_in_field()
      field           target field to save the value to
      no_conversion   flag indicating whether conversions are allowed

  DESCRIPTION
    Save the function value into a field and update the user variable
    accordingly. If a result field is defined and the target field doesn't
    coincide with it then the value from the result field will be used as
    the new value of the user variable.

    The reason to have this method rather than simply using the result
    field in the val_xxx() methods is that the value from the result field
    not always can be used when the result field is defined.
    Let's consider the following cases:
    1) when filling a tmp table the result field is defined but the value of it
    is undefined because it has to be produced yet. Thus we can't use it.
    2) on execution of an INSERT ... SELECT statement the save_in_field()
    function will be called to fill the data in the new record. If the SELECT
    part uses a tmp table then the result field is defined and should be
    used in order to get the correct result.

    The difference between the SET_USER_VAR function and regular functions
    like CONCAT is that the Item_func objects for the regular functions are
    replaced by Item_field objects after the values of these functions have
    been stored in a tmp table. Yet an object of the Item_field class cannot
    be used to update a user variable.
    Due to this we have to handle the result field in a special way here and
    in the Item_func_set_user_var::send() function.

  RETURN VALUES
    FALSE       Ok
    TRUE        Error
*/

int Item_func_set_user_var::save_in_field(Field *field, bool no_conversions,
                                          bool can_use_result_field)
{
  bool use_result_field= (!can_use_result_field ? 0 :
                          (result_field && result_field != field));
  int error;

  /* Update the value of the user variable */
  check(use_result_field);
  update();

  if (result_type() == STRING_RESULT ||
      (result_type() == REAL_RESULT &&
       field->result_type() == STRING_RESULT))
  {
    String *result;
    CHARSET_INFO *cs= collation.collation;
    char buff[MAX_FIELD_WIDTH];		// Alloc buffer for small columns
    str_value.set_quick(buff, sizeof(buff), cs);
    result= m_var_entry->val_str(&null_value, &str_value, decimals);

    if (null_value)
    {
      str_value.set_quick(0, 0, cs);
      return set_field_to_null_with_conversions(field, no_conversions);
    }

    /* NOTE: If null_value == FALSE, "result" must be not NULL.  */

    field->set_notnull();
    error=field->store(result->ptr(),result->length(),cs);
    str_value.set_quick(0, 0, cs);
  }
  else if (result_type() == REAL_RESULT)
  {
    double nr= m_var_entry->val_real(&null_value);
    if (null_value)
      return set_field_to_null(field);
    field->set_notnull();
    error=field->store(nr);
  }
  else if (result_type() == DECIMAL_RESULT)
  {
    my_decimal decimal_value;
    my_decimal *val= m_var_entry->val_decimal(&null_value, &decimal_value);
    if (null_value)
      return set_field_to_null(field);
    field->set_notnull();
    error=field->store_decimal(val);
  }
  else
  {
    longlong nr= m_var_entry->val_int(&null_value);
    if (null_value)
      return set_field_to_null_with_conversions(field, no_conversions);
    field->set_notnull();
    error=field->store(nr, unsigned_flag);
  }
  return error;
}


String *
Item_func_get_user_var::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_func_get_user_var::val_str");
  if (!m_var_entry)
    DBUG_RETURN((String*) 0);			// No such variable
  DBUG_RETURN(m_var_entry->val_str(&null_value, str, decimals));
}


double Item_func_get_user_var::val_real()
{
  DBUG_ASSERT(fixed == 1);
  if (!m_var_entry)
    return 0.0;					// No such variable
  return (m_var_entry->val_real(&null_value));
}


my_decimal *Item_func_get_user_var::val_decimal(my_decimal *dec)
{
  DBUG_ASSERT(fixed == 1);
  if (!m_var_entry)
    return 0;
  return m_var_entry->val_decimal(&null_value, dec);
}


longlong Item_func_get_user_var::val_int()
{
  DBUG_ASSERT(fixed == 1);
  if (!m_var_entry)
    return 0;				// No such variable
  return (m_var_entry->val_int(&null_value));
}


/**
  Get variable by name and, if necessary, put the record of variable 
  use into the binary log.

  When a user variable is invoked from an update query (INSERT, UPDATE etc),
  stores this variable and its value in thd->user_var_events, so that it can be
  written to the binlog (will be written just before the query is written, see
  log.cc).

  @param      thd        Current thread
  @param      name       Variable name
  @param[out] out_entry  variable structure or NULL. The pointer is set
                         regardless of whether function succeeded or not.

  @retval
    0  OK
  @retval
    1  Failed to put appropriate record into binary log

*/

static int
get_var_with_binlog(THD *thd, enum_sql_command sql_command,
                    LEX_STRING &name, user_var_entry **out_entry)
{
  BINLOG_USER_VAR_EVENT *user_var_event;
  user_var_entry *var_entry;
  var_entry= get_variable(&thd->user_vars, name, 0);

  /*
    Any reference to user-defined variable which is done from stored
    function or trigger affects their execution and the execution of the
    calling statement. We must log all such variables even if they are 
    not involved in table-updating statements.
  */
  if (!(opt_bin_log && 
       (is_update_query(sql_command) || thd->in_sub_stmt)))
  {
    *out_entry= var_entry;
    return 0;
  }

  if (!var_entry)
  {
    /*
      If the variable does not exist, it's NULL, but we want to create it so
      that it gets into the binlog (if it didn't, the slave could be
      influenced by a variable of the same name previously set by another
      thread).
      We create it like if it had been explicitly set with SET before.
      The 'new' mimics what sql_yacc.yy does when 'SET @a=10;'.
      sql_set_variables() is what is called from 'case SQLCOM_SET_OPTION'
      in dispatch_command()). Instead of building a one-element list to pass to
      sql_set_variables(), we could instead manually call check() and update();
      this would save memory and time; but calling sql_set_variables() makes
      one unique place to maintain (sql_set_variables()). 

      Manipulation with lex is necessary since free_underlaid_joins
      is going to release memory belonging to the main query.
    */

    List<set_var_base> tmp_var_list;
    LEX *sav_lex= thd->lex, lex_tmp;
    thd->lex= &lex_tmp;
    lex_start(thd);
    tmp_var_list.push_back(new (thd->mem_root)
                           set_var_user(new (thd->mem_root)
                                        Item_func_set_user_var(thd, name,
                                                               new (thd->mem_root) Item_null(thd))),
                           thd->mem_root);
    /* Create the variable */
    if (sql_set_variables(thd, &tmp_var_list, false))
    {
      thd->lex= sav_lex;
      goto err;
    }
    thd->lex= sav_lex;
    if (!(var_entry= get_variable(&thd->user_vars, name, 0)))
      goto err;
  }
  else if (var_entry->used_query_id == thd->query_id ||
           mysql_bin_log.is_query_in_union(thd, var_entry->used_query_id))
  {
    /* 
       If this variable was already stored in user_var_events by this query
       (because it's used in more than one place in the query), don't store
       it.
    */
    *out_entry= var_entry;
    return 0;
  }

  uint size;
  /*
    First we need to store value of var_entry, when the next situation
    appears:
    > set @a:=1;
    > insert into t1 values (@a), (@a:=@a+1), (@a:=@a+1);
    We have to write to binlog value @a= 1.

    We allocate the user_var_event on user_var_events_alloc pool, not on
    the this-statement-execution pool because in SPs user_var_event objects 
    may need to be valid after current [SP] statement execution pool is
    destroyed.
  */
  size= ALIGN_SIZE(sizeof(BINLOG_USER_VAR_EVENT)) + var_entry->length;
  if (!(user_var_event= (BINLOG_USER_VAR_EVENT *)
        alloc_root(thd->user_var_events_alloc, size)))
    goto err;

  user_var_event->value= (char*) user_var_event +
    ALIGN_SIZE(sizeof(BINLOG_USER_VAR_EVENT));
  user_var_event->user_var_event= var_entry;
  user_var_event->type= var_entry->type;
  user_var_event->charset_number= var_entry->charset()->number;
  user_var_event->unsigned_flag= var_entry->unsigned_flag;
  if (!var_entry->value)
  {
    /* NULL value*/
    user_var_event->length= 0;
    user_var_event->value= 0;
  }
  else
  {
    user_var_event->length= var_entry->length;
    memcpy(user_var_event->value, var_entry->value,
           var_entry->length);
  }
  /* Mark that this variable has been used by this query */
  var_entry->used_query_id= thd->query_id;
  if (insert_dynamic(&thd->user_var_events, (uchar*) &user_var_event))
    goto err;

  *out_entry= var_entry;
  return 0;

err:
  *out_entry= var_entry;
  return 1;
}

bool Item_func_get_user_var::fix_length_and_dec()
{
  THD *thd=current_thd;
  int error;
  maybe_null=1;
  decimals=NOT_FIXED_DEC;
  max_length=MAX_BLOB_WIDTH;

  error= get_var_with_binlog(thd, thd->lex->sql_command, name, &m_var_entry);

  /*
    If the variable didn't exist it has been created as a STRING-type.
    'm_var_entry' is NULL only if there occurred an error during the call to
    get_var_with_binlog.
  */
  if (!error && m_var_entry)
  {
    unsigned_flag= m_var_entry->unsigned_flag;
    max_length= m_var_entry->length;
    collation.set(m_var_entry->charset(), DERIVATION_IMPLICIT);
    set_handler_by_result_type(m_var_entry->type);
    switch (Item_func_get_user_var::result_type()) {
    case REAL_RESULT:
      fix_char_length(DBL_DIG + 8);
      break;
    case INT_RESULT:
      fix_char_length(MAX_BIGINT_WIDTH);
      decimals=0;
      break;
    case STRING_RESULT:
      max_length= MAX_BLOB_WIDTH - 1;
      set_handler_by_field_type(MYSQL_TYPE_MEDIUM_BLOB);
      break;
    case DECIMAL_RESULT:
      fix_char_length(DECIMAL_MAX_STR_LENGTH);
      decimals= DECIMAL_MAX_SCALE;
      break;
    case ROW_RESULT:                            // Keep compiler happy
    case TIME_RESULT:
      DBUG_ASSERT(0);                // This case should never be chosen
      break;
    }
  }
  else
  {
    collation.set(&my_charset_bin, DERIVATION_IMPLICIT);
    null_value= 1;
    set_handler_by_field_type(MYSQL_TYPE_LONG_BLOB);
    max_length= MAX_BLOB_WIDTH;
  }
  return false;
}


bool Item_func_get_user_var::const_item() const
{
  return (!m_var_entry ||
          current_thd->query_id != m_var_entry->update_query_id);
}


void Item_func_get_user_var::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("@"));
  append_identifier(current_thd, str, name.str, name.length);
}


bool Item_func_get_user_var::eq(const Item *item, bool binary_cmp) const
{
  /* Assume we don't have rtti */
  if (this == item)
    return 1;					// Same item is same.
  /* Check if other type is also a get_user_var() object */
  if (item->type() != FUNC_ITEM ||
      ((Item_func*) item)->functype() != functype())
    return 0;
  Item_func_get_user_var *other=(Item_func_get_user_var*) item;
  return (name.length == other->name.length &&
	  !memcmp(name.str, other->name.str, name.length));
}


bool Item_func_get_user_var::set_value(THD *thd,
                                       sp_rcontext * /*ctx*/, Item **it)
{
  Item_func_set_user_var *suv= new (thd->mem_root) Item_func_set_user_var(thd, get_name(), *it);
  /*
    Item_func_set_user_var is not fixed after construction, call
    fix_fields().
  */
  return (!suv || suv->fix_fields(thd, it) || suv->check(0) || suv->update());
}


bool Item_user_var_as_out_param::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  DBUG_ASSERT(thd->lex->exchange);
  if (Item::fix_fields(thd, ref) ||
      !(entry= get_variable(&thd->user_vars, name, 1)))
    return TRUE;
  entry->type= STRING_RESULT;
  /*
    Let us set the same collation which is used for loading
    of fields in LOAD DATA INFILE.
    (Since Item_user_var_as_out_param is used only there).
  */
  entry->set_charset(thd->lex->exchange->cs ?
                     thd->lex->exchange->cs :
                     thd->variables.collation_database);
  entry->update_query_id= thd->query_id;
  return FALSE;
}


void Item_user_var_as_out_param::set_null_value(CHARSET_INFO* cs)
{
  ::update_hash(entry, TRUE, 0, 0, STRING_RESULT, cs, 0 /* unsigned_arg */);
}


void Item_user_var_as_out_param::set_value(const char *str, uint length,
                                           CHARSET_INFO* cs)
{
  ::update_hash(entry, FALSE, (void*)str, length, STRING_RESULT, cs,
                0 /* unsigned_arg */);
}


double Item_user_var_as_out_param::val_real()
{
  DBUG_ASSERT(0);
  return 0.0;
}


longlong Item_user_var_as_out_param::val_int()
{
  DBUG_ASSERT(0);
  return 0;
}


String* Item_user_var_as_out_param::val_str(String *str)
{
  DBUG_ASSERT(0);
  return 0;
}


my_decimal* Item_user_var_as_out_param::val_decimal(my_decimal *decimal_buffer)
{
  DBUG_ASSERT(0);
  return 0;
}


void Item_user_var_as_out_param::load_data_print_for_log_event(THD *thd,
                                                               String *str)
                                                               const
{
  str->append('@');
  append_identifier(thd, str, name.str, name.length);
}


Item_func_get_system_var::
Item_func_get_system_var(THD *thd, sys_var *var_arg, enum_var_type var_type_arg,
                       LEX_STRING *component_arg, const char *name_arg,
                       size_t name_len_arg):
  Item_func(thd), var(var_arg), var_type(var_type_arg),
  orig_var_type(var_type_arg), component(*component_arg), cache_present(0)
{
  /* set_name() will allocate the name */
  set_name(thd, name_arg, (uint) name_len_arg, system_charset_info);
}


bool Item_func_get_system_var::is_written_to_binlog()
{
  return var->is_written_to_binlog(var_type);
}


void Item_func_get_system_var::update_null_value()
{
  THD *thd= current_thd;
  int save_no_errors= thd->no_errors;
  thd->no_errors= TRUE;
  Item::update_null_value();
  thd->no_errors= save_no_errors;
}


bool Item_func_get_system_var::fix_length_and_dec()
{
  char *cptr;
  maybe_null= TRUE;
  max_length= 0;

  if (var->check_type(var_type))
  {
    if (var_type != OPT_DEFAULT)
    {
      my_error(ER_INCORRECT_GLOBAL_LOCAL_VAR, MYF(0),
               var->name.str, var_type == OPT_GLOBAL ? "SESSION" : "GLOBAL");
      return TRUE;
    }
    /* As there was no local variable, return the global value */
    var_type= OPT_GLOBAL;
  }

  switch (var->show_type())
  {
    case SHOW_HA_ROWS:
    case SHOW_UINT:
    case SHOW_ULONG:
    case SHOW_ULONGLONG:
      unsigned_flag= TRUE;
      /* fall through */
    case SHOW_SINT:
    case SHOW_SLONG:
    case SHOW_SLONGLONG:
      collation.set_numeric();
      fix_char_length(MY_INT64_NUM_DECIMAL_DIGITS);
      decimals=0;
      break;
    case SHOW_CHAR:
    case SHOW_CHAR_PTR:
      mysql_mutex_lock(&LOCK_global_system_variables);
      cptr= var->show_type() == SHOW_CHAR ? 
        (char*) var->value_ptr(current_thd, var_type, &component) :
        *(char**) var->value_ptr(current_thd, var_type, &component);
      if (cptr)
        max_length= system_charset_info->cset->numchars(system_charset_info,
                                                        cptr,
                                                        cptr + strlen(cptr));
      mysql_mutex_unlock(&LOCK_global_system_variables);
      collation.set(system_charset_info, DERIVATION_SYSCONST);
      max_length*= system_charset_info->mbmaxlen;
      decimals=NOT_FIXED_DEC;
      break;
    case SHOW_LEX_STRING:
      {
        mysql_mutex_lock(&LOCK_global_system_variables);
        LEX_STRING *ls= ((LEX_STRING*)var->value_ptr(current_thd, var_type, &component));
        max_length= system_charset_info->cset->numchars(system_charset_info,
                                                        ls->str,
                                                        ls->str + ls->length);
        mysql_mutex_unlock(&LOCK_global_system_variables);
        collation.set(system_charset_info, DERIVATION_SYSCONST);
        max_length*= system_charset_info->mbmaxlen;
        decimals=NOT_FIXED_DEC;
      }
      break;
    case SHOW_BOOL:
    case SHOW_MY_BOOL:
      collation.set_numeric();
      fix_char_length(1);
      decimals=0;
      break;
    case SHOW_DOUBLE:
      decimals= 6;
      collation.set_numeric();
      fix_char_length(DBL_DIG + 6);
      break;
    default:
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name.str);
      break;
  }
  return FALSE;
}


void Item_func_get_system_var::print(String *str, enum_query_type query_type)
{
  if (name_length)
    str->append(name, name_length);
  else
  {
    str->append(STRING_WITH_LEN("@@"));
    if (component.length)
    {
      str->append(&component);
      str->append('.');
    }
    else if (var_type == SHOW_OPT_GLOBAL && var->scope() != sys_var::GLOBAL)
    {
      str->append(STRING_WITH_LEN("global."));
    }
    str->append(&var->name);
  }
}

bool Item_func_get_system_var::check_vcol_func_processor(void *arg)
{
  return mark_unsupported_function("@@", var->name.str, arg, VCOL_SESSION_FUNC);
}

enum Item_result Item_func_get_system_var::result_type() const
{
  switch (var->show_type())
  {
    case SHOW_BOOL:
    case SHOW_MY_BOOL:
    case SHOW_SINT:
    case SHOW_SLONG:
    case SHOW_SLONGLONG:
    case SHOW_UINT:
    case SHOW_ULONG:
    case SHOW_ULONGLONG:
    case SHOW_HA_ROWS:
      return INT_RESULT;
    case SHOW_CHAR: 
    case SHOW_CHAR_PTR: 
    case SHOW_LEX_STRING:
      return STRING_RESULT;
    case SHOW_DOUBLE:
      return REAL_RESULT;
    default:
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name.str);
      return STRING_RESULT;                   // keep the compiler happy
  }
}


enum_field_types Item_func_get_system_var::field_type() const
{
  switch (var->show_type())
  {
    case SHOW_BOOL:
    case SHOW_MY_BOOL:
    case SHOW_SINT:
    case SHOW_SLONG:
    case SHOW_SLONGLONG:
    case SHOW_UINT:
    case SHOW_ULONG:
    case SHOW_ULONGLONG:
    case SHOW_HA_ROWS:
      return MYSQL_TYPE_LONGLONG;
    case SHOW_CHAR: 
    case SHOW_CHAR_PTR: 
    case SHOW_LEX_STRING:
      return MYSQL_TYPE_VARCHAR;
    case SHOW_DOUBLE:
      return MYSQL_TYPE_DOUBLE;
    default:
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name.str);
      return MYSQL_TYPE_VARCHAR;              // keep the compiler happy
  }
}


longlong Item_func_get_system_var::val_int()
{
  THD *thd= current_thd;

  DBUG_EXECUTE_IF("simulate_non_gtid_aware_master",
                  {
                    if (0 == strcmp("gtid_domain_id", var->name.str))
                    {
                      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name.str);
                      return 0;
                    }
                  });
  if (cache_present && thd->query_id == used_query_id)
  {
    if (cache_present & GET_SYS_VAR_CACHE_LONG)
    {
      null_value= cached_null_value;
      return cached_llval;
    } 
    else if (cache_present & GET_SYS_VAR_CACHE_DOUBLE)
    {
      null_value= cached_null_value;
      cached_llval= (longlong) cached_dval;
      cache_present|= GET_SYS_VAR_CACHE_LONG;
      return cached_llval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_STRING)
    {
      null_value= cached_null_value;
      if (!null_value)
        cached_llval= longlong_from_string_with_check(&cached_strval);
      else
        cached_llval= 0;
      cache_present|= GET_SYS_VAR_CACHE_LONG;
      return cached_llval;
    }
  }

  cached_llval= var->val_int(&null_value, thd, var_type, &component);
  cache_present |= GET_SYS_VAR_CACHE_LONG;
  used_query_id= thd->query_id;
  cached_null_value= null_value;
  return cached_llval;
}


String* Item_func_get_system_var::val_str(String* str)
{
  THD *thd= current_thd;

  if (cache_present && thd->query_id == used_query_id)
  {
    if (cache_present & GET_SYS_VAR_CACHE_STRING)
    {
      null_value= cached_null_value;
      return null_value ? NULL : &cached_strval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_LONG)
    {
      null_value= cached_null_value;
      if (!null_value)
        cached_strval.set (cached_llval, collation.collation);
      cache_present|= GET_SYS_VAR_CACHE_STRING;
      return null_value ? NULL : &cached_strval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_DOUBLE)
    {
      null_value= cached_null_value;
      if (!null_value)
        cached_strval.set_real (cached_dval, decimals, collation.collation);
      cache_present|= GET_SYS_VAR_CACHE_STRING;
      return null_value ? NULL : &cached_strval;
    }
  }

  str= var->val_str(&cached_strval, thd, var_type, &component);
  cache_present|= GET_SYS_VAR_CACHE_STRING;
  used_query_id= thd->query_id;
  cached_null_value= null_value= !str;
  return str;
}


double Item_func_get_system_var::val_real()
{
  THD *thd= current_thd;

  if (cache_present && thd->query_id == used_query_id)
  {
    if (cache_present & GET_SYS_VAR_CACHE_DOUBLE)
    {
      null_value= cached_null_value;
      return cached_dval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_LONG)
    {
      null_value= cached_null_value;
      cached_dval= (double)cached_llval;
      cache_present|= GET_SYS_VAR_CACHE_DOUBLE;
      return cached_dval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_STRING)
    {
      null_value= cached_null_value;
      if (!null_value)
        cached_dval= double_from_string_with_check(&cached_strval);
      else
        cached_dval= 0;
      cache_present|= GET_SYS_VAR_CACHE_DOUBLE;
      return cached_dval;
    }
  }

  cached_dval= var->val_real(&null_value, thd, var_type, &component);
  cache_present |= GET_SYS_VAR_CACHE_DOUBLE;
  used_query_id= thd->query_id;
  cached_null_value= null_value;
  return cached_dval;
}


bool Item_func_get_system_var::eq(const Item *item, bool binary_cmp) const
{
  /* Assume we don't have rtti */
  if (this == item)
    return 1;					// Same item is same.
  /* Check if other type is also a get_user_var() object */
  if (item->type() != FUNC_ITEM ||
      ((Item_func*) item)->functype() != functype())
    return 0;
  Item_func_get_system_var *other=(Item_func_get_system_var*) item;
  return (var == other->var && var_type == other->var_type);
}


void Item_func_get_system_var::cleanup()
{
  Item_func::cleanup();
  cache_present= 0;
  var_type= orig_var_type;
  cached_strval.free();
}


void Item_func_match::init_search(THD *thd, bool no_order)
{
  DBUG_ENTER("Item_func_match::init_search");

  if (!table->file->get_table()) // the handler isn't opened yet
    DBUG_VOID_RETURN;

  /* Check if init_search() has been called before */
  if (ft_handler)
  {
    if (join_key)
      table->file->ft_handler= ft_handler;
    DBUG_VOID_RETURN;
  }

  if (key == NO_SUCH_KEY)
  {
    List<Item> fields;
    fields.push_back(new (thd->mem_root)
                     Item_string(thd, " ", 1, cmp_collation.collation),
                     thd->mem_root);
    for (uint i= 1; i < arg_count; i++)
      fields.push_back(args[i]);
    concat_ws= new (thd->mem_root) Item_func_concat_ws(thd, fields);
    /*
      Above function used only to get value and do not need fix_fields for it:
      Item_string - basic constant
      fields - fix_fields() was already called for this arguments
      Item_func_concat_ws - do not need fix_fields() to produce value
    */
    concat_ws->quick_fix_field();
  }

  if (master)
  {
    join_key= master->join_key= join_key | master->join_key;
    master->init_search(thd, no_order);
    ft_handler= master->ft_handler;
    join_key= master->join_key;
    DBUG_VOID_RETURN;
  }

  String *ft_tmp= 0;

  // MATCH ... AGAINST (NULL) is meaningless, but possible
  if (!(ft_tmp=key_item()->val_str(&value)))
  {
    ft_tmp= &value;
    value.set("", 0, cmp_collation.collation);
  }

  if (ft_tmp->charset() != cmp_collation.collation)
  {
    uint dummy_errors;
    search_value.copy(ft_tmp->ptr(), ft_tmp->length(), ft_tmp->charset(),
                      cmp_collation.collation, &dummy_errors);
    ft_tmp= &search_value;
  }

  if (join_key && !no_order)
    flags|=FT_SORTED;

  if (key != NO_SUCH_KEY)
    THD_STAGE_INFO(table->in_use, stage_fulltext_initialization);

  ft_handler= table->file->ft_init_ext(flags, key, ft_tmp);

  if (join_key)
    table->file->ft_handler=ft_handler;

  DBUG_VOID_RETURN;
}


bool Item_func_match::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  Item *UNINIT_VAR(item);                        // Safe as arg_count is > 1

  status_var_increment(thd->status_var.feature_fulltext);

  maybe_null=1;
  join_key=0;

  /*
    const_item is assumed in quite a bit of places, so it would be difficult
    to remove;  If it would ever to be removed, this should include
    modifications to find_best and auto_close as complement to auto_init code
    above.
   */
  if (Item_func::fix_fields(thd, ref) ||
      !args[0]->const_during_execution())
  {
    my_error(ER_WRONG_ARGUMENTS,MYF(0),"AGAINST");
    return TRUE;
  }

  bool allows_multi_table_search= true;
  const_item_cache=0;
  table= 0;
  for (uint i=1 ; i < arg_count ; i++)
  {
    item= args[i]= args[i]->real_item();
    /*
      When running in PS mode, some Item_field's can already be replaced
      to Item_func_conv_charset during PREPARE time. This is possible
      in case of "MATCH (f1,..,fN) AGAINST (... IN BOOLEAN MODE)"
      when running without any fulltext indexes and when fields f1..fN
      have different character sets.
      So we check for FIELD_ITEM only during prepare time and in non-PS mode,
      and do not check in PS execute time.
    */
    if (!thd->stmt_arena->is_stmt_execute() &&
        item->type() != Item::FIELD_ITEM)
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), "MATCH");
      return TRUE;
    }
    /*
      During the prepare-time execution of fix_fields() of a PS query some
      Item_fields's could have been already replaced to Item_func_conv_charset
      (by the call for agg_arg_charsets_for_comparison below()).
      But agg_arg_charsets_for_comparison() is written in a way that
      at least *one* of the Item_field's is not replaced.
      This makes sure that "table" gets initialized during PS execution time.
    */
    if (item->type() == Item::FIELD_ITEM)
      table= ((Item_field *)item)->field->table;

    allows_multi_table_search &= allows_search_on_non_indexed_columns(table);
  }

  /*
    Check that all columns come from the same table.
    We've already checked that columns in MATCH are fields so
    PARAM_TABLE_BIT can only appear from AGAINST argument.
  */
  if ((used_tables_cache & ~PARAM_TABLE_BIT) != item->used_tables())
    key=NO_SUCH_KEY;

  if (key == NO_SUCH_KEY && !allows_multi_table_search)
  {
    my_error(ER_WRONG_ARGUMENTS,MYF(0),"MATCH");
    return TRUE;
  }
  if (!(table->file->ha_table_flags() & HA_CAN_FULLTEXT))
  {
    my_error(ER_TABLE_CANT_HANDLE_FT, MYF(0), table->file->table_type());
    return 1;
  }
  table->fulltext_searched=1;
  return agg_arg_charsets_for_comparison(cmp_collation, args+1, arg_count-1);
}

bool Item_func_match::fix_index()
{
  Item_field *item;
  uint ft_to_key[MAX_KEY], ft_cnt[MAX_KEY], fts=0, keynr;
  uint max_cnt=0, mkeys=0, i;

  /*
    We will skip execution if the item is not fixed
    with fix_field
  */
  if (!fixed)
    return false;

  if (key == NO_SUCH_KEY)
    return 0;
  
  if (!table) 
    goto err;

  for (keynr=0 ; keynr < table->s->keys ; keynr++)
  {
    if ((table->key_info[keynr].flags & HA_FULLTEXT) &&
        (flags & FT_BOOL ? table->keys_in_use_for_query.is_set(keynr) :
                           table->s->keys_in_use.is_set(keynr)))

    {
      ft_to_key[fts]=keynr;
      ft_cnt[fts]=0;
      fts++;
    }
  }

  if (!fts)
    goto err;

  for (i=1; i < arg_count; i++)
  {
    if (args[i]->type() != FIELD_ITEM)
      goto err;
    item=(Item_field*)args[i];
    for (keynr=0 ; keynr < fts ; keynr++)
    {
      KEY *ft_key=&table->key_info[ft_to_key[keynr]];
      uint key_parts=ft_key->user_defined_key_parts;

      for (uint part=0 ; part < key_parts ; part++)
      {
	if (item->field->eq(ft_key->key_part[part].field))
	  ft_cnt[keynr]++;
      }
    }
  }

  for (keynr=0 ; keynr < fts ; keynr++)
  {
    if (ft_cnt[keynr] > max_cnt)
    {
      mkeys=0;
      max_cnt=ft_cnt[mkeys]=ft_cnt[keynr];
      ft_to_key[mkeys]=ft_to_key[keynr];
      continue;
    }
    if (max_cnt && ft_cnt[keynr] == max_cnt)
    {
      mkeys++;
      ft_cnt[mkeys]=ft_cnt[keynr];
      ft_to_key[mkeys]=ft_to_key[keynr];
      continue;
    }
  }

  for (keynr=0 ; keynr <= mkeys ; keynr++)
  {
    // partial keys doesn't work
    if (max_cnt < arg_count-1 ||
        max_cnt < table->key_info[ft_to_key[keynr]].user_defined_key_parts)
      continue;

    key=ft_to_key[keynr];

    return 0;
  }

err:
  if (allows_search_on_non_indexed_columns(table))
  {
    key=NO_SUCH_KEY;
    return 0;
  }
  my_message(ER_FT_MATCHING_KEY_NOT_FOUND,
             ER(ER_FT_MATCHING_KEY_NOT_FOUND), MYF(0));
  return 1;
}


bool Item_func_match::eq(const Item *item, bool binary_cmp) const
{
  if (item->type() != FUNC_ITEM ||
      ((Item_func*)item)->functype() != FT_FUNC ||
      flags != ((Item_func_match*)item)->flags)
    return 0;

  Item_func_match *ifm=(Item_func_match*) item;

  if (key == ifm->key && table == ifm->table &&
      key_item()->eq(ifm->key_item(), binary_cmp))
    return 1;

  return 0;
}


double Item_func_match::val_real()
{
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_func_match::val");
  if (ft_handler == NULL)
    DBUG_RETURN(-1.0);

  if (key != NO_SUCH_KEY && table->null_row) /* NULL row from an outer join */
    DBUG_RETURN(0.0);

  if (join_key)
  {
    if (table->file->ft_handler)
      DBUG_RETURN(ft_handler->please->get_relevance(ft_handler));
    join_key=0;
  }

  if (key == NO_SUCH_KEY)
  {
    String *a= concat_ws->val_str(&value);
    if ((null_value= (a == 0)) || !a->length())
      DBUG_RETURN(0);
    DBUG_RETURN(ft_handler->please->find_relevance(ft_handler,
				      (uchar *)a->ptr(), a->length()));
  }
  DBUG_RETURN(ft_handler->please->find_relevance(ft_handler,
                                                 table->record[0], 0));
}

void Item_func_match::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("(match "));
  print_args(str, 1, query_type);
  str->append(STRING_WITH_LEN(" against ("));
  args[0]->print(str, query_type);
  if (flags & FT_BOOL)
    str->append(STRING_WITH_LEN(" in boolean mode"));
  else if (flags & FT_EXPAND)
    str->append(STRING_WITH_LEN(" with query expansion"));
  str->append(STRING_WITH_LEN("))"));
}

longlong Item_func_bit_xor::val_int()
{
  DBUG_ASSERT(fixed == 1);
  ulonglong arg1= (ulonglong) args[0]->val_int();
  ulonglong arg2= (ulonglong) args[1]->val_int();
  if ((null_value= (args[0]->null_value || args[1]->null_value)))
    return 0;
  return (longlong) (arg1 ^ arg2);
}


/***************************************************************************
  System variables
****************************************************************************/

/**
  Return value of an system variable base[.name] as a constant item.

  @param thd			Thread handler
  @param var_type		global / session
  @param name		        Name of base or system variable
  @param component		Component.

  @note
    If component.str = 0 then the variable name is in 'name'

  @return
    - 0  : error
    - #  : constant item
*/


Item *get_system_var(THD *thd, enum_var_type var_type, LEX_STRING name,
		     LEX_STRING component)
{
  sys_var *var;
  LEX_STRING *base_name, *component_name;

  if (component.str)
  {
    base_name= &component;
    component_name= &name;
  }
  else
  {
    base_name= &name;
    component_name= &component;			// Empty string
  }

  if (!(var= find_sys_var(thd, base_name->str, base_name->length)))
    return 0;
  if (component.str)
  {
    if (!var->is_struct())
    {
      my_error(ER_VARIABLE_IS_NOT_STRUCT, MYF(0), base_name->str);
      return 0;
    }
  }
  thd->lex->uncacheable(UNCACHEABLE_SIDEEFFECT);

  set_if_smaller(component_name->length, MAX_SYS_VAR_LENGTH);

  return new (thd->mem_root) Item_func_get_system_var(thd, var, var_type, component_name,
                                      NULL, 0);
}


longlong Item_func_row_count::val_int()
{
  DBUG_ASSERT(fixed == 1);
  THD *thd= current_thd;

  return thd->get_row_count_func();
}




Item_func_sp::Item_func_sp(THD *thd, Name_resolution_context *context_arg,
                           sp_name *name):
  Item_func(thd), context(context_arg), m_name(name), m_sp(NULL), sp_result_field(NULL)
{
  maybe_null= 1;
  m_name->init_qname(thd);
  dummy_table= (TABLE*) thd->calloc(sizeof(TABLE)+ sizeof(TABLE_SHARE));
  dummy_table->s= (TABLE_SHARE*) (dummy_table+1);
}


Item_func_sp::Item_func_sp(THD *thd, Name_resolution_context *context_arg,
                           sp_name *name_arg, List<Item> &list):
  Item_func(thd, list), context(context_arg), m_name(name_arg), m_sp(NULL),
  sp_result_field(NULL)
{
  maybe_null= 1;
  m_name->init_qname(thd);
  dummy_table= (TABLE*) thd->calloc(sizeof(TABLE)+ sizeof(TABLE_SHARE));
  dummy_table->s= (TABLE_SHARE*) (dummy_table+1);
}


void
Item_func_sp::cleanup()
{
  if (sp_result_field)
  {
    delete sp_result_field;
    sp_result_field= NULL;
  }
  m_sp= NULL;
  dummy_table->alias.free();
  Item_func::cleanup();
}

const char *
Item_func_sp::func_name() const
{
  THD *thd= current_thd;
  /* Calculate length to avoid reallocation of string for sure */
  uint len= (((m_name->m_explicit_name ? m_name->m_db.length : 0) +
              m_name->m_name.length)*2 + //characters*quoting
             2 +                         // ` and `
             (m_name->m_explicit_name ?
              3 : 0) +                   // '`', '`' and '.' for the db
             1 +                         // end of string
             ALIGN_SIZE(1));             // to avoid String reallocation
  String qname((char *)alloc_root(thd->mem_root, len), len,
               system_charset_info);

  qname.length(0);
  if (m_name->m_explicit_name)
  {
    append_identifier(thd, &qname, m_name->m_db.str, m_name->m_db.length);
    qname.append('.');
  }
  append_identifier(thd, &qname, m_name->m_name.str, m_name->m_name.length);
  return qname.c_ptr_safe();
}


void my_missing_function_error(const LEX_STRING &token, const char *func_name)
{
  if (token.length && is_lex_native_function (&token))
    my_error(ER_FUNC_INEXISTENT_NAME_COLLISION, MYF(0), func_name);
  else
    my_error(ER_SP_DOES_NOT_EXIST, MYF(0), "FUNCTION", func_name);
}


/**
  @brief Initialize the result field by creating a temporary dummy table
    and assign it to a newly created field object. Meta data used to
    create the field is fetched from the sp_head belonging to the stored
    proceedure found in the stored procedure functon cache.
  
  @note This function should be called from fix_fields to init the result
    field. It is some what related to Item_field.

  @see Item_field

  @param thd A pointer to the session and thread context.

  @return Function return error status.
  @retval TRUE is returned on an error
  @retval FALSE is returned on success.
*/

bool
Item_func_sp::init_result_field(THD *thd)
{
  LEX_STRING empty_name= { C_STRING_WITH_LEN("") };
  TABLE_SHARE *share;
  DBUG_ENTER("Item_func_sp::init_result_field");

  DBUG_ASSERT(m_sp == NULL);
  DBUG_ASSERT(sp_result_field == NULL);

  if (!(m_sp= sp_find_routine(thd, TYPE_ENUM_FUNCTION, m_name,
                               &thd->sp_func_cache, TRUE)))
  {
    my_missing_function_error (m_name->m_name, m_name->m_qname.str);
    context->process_error(thd);
    DBUG_RETURN(TRUE);
  }

  /*
     A Field need to be attached to a Table.
     Below we "create" a dummy table by initializing 
     the needed pointers.
   */
  
  share= dummy_table->s;
  dummy_table->alias.set("", 0, table_alias_charset);
  dummy_table->maybe_null = maybe_null;
  dummy_table->in_use= thd;
  dummy_table->copy_blobs= TRUE;
  share->table_cache_key = empty_name;
  share->table_name = empty_name;

  if (!(sp_result_field= m_sp->create_result_field(max_length, name,
                                                   dummy_table)))
  {
   DBUG_RETURN(TRUE);
  }
  
  if (sp_result_field->pack_length() > sizeof(result_buf))
  {
    void *tmp;
    if (!(tmp= thd->alloc(sp_result_field->pack_length())))
      DBUG_RETURN(TRUE);
    sp_result_field->move_field((uchar*) tmp);
  }
  else
    sp_result_field->move_field(result_buf);
  
  sp_result_field->null_ptr= (uchar *) &null_value;
  sp_result_field->null_bit= 1;
  DBUG_RETURN(FALSE);
}


/**
  @note
  Deterministic stored procedures are considered inexpensive.
  Consequently such procedures may be evaluated during optimization,
  if they are constant (checked by the optimizer).
*/

bool Item_func_sp::is_expensive()
{
  return !m_sp->m_chistics->detistic ||
          current_thd->locked_tables_mode < LTM_LOCK_TABLES;
}


/**
  @brief Initialize local members with values from the Field interface.

  @note called from Item::fix_fields.
*/

bool Item_func_sp::fix_length_and_dec()
{
  DBUG_ENTER("Item_func_sp::fix_length_and_dec");

  DBUG_ASSERT(sp_result_field);
  Type_std_attributes::set(sp_result_field);
  maybe_null= 1;

  DBUG_RETURN(FALSE);
}


/**
  @brief Execute function & store value in field.

  @return Function returns error status.
  @retval FALSE on success.
  @retval TRUE if an error occurred.
*/

bool
Item_func_sp::execute()
{
  THD *thd= current_thd;
  
  /* Execute function and store the return value in the field. */

  if (execute_impl(thd))
  {
    null_value= 1;
    context->process_error(thd);
    if (thd->killed)
      thd->send_kill_message();
    return TRUE;
  }

  /* Check that the field (the value) is not NULL. */

  null_value= sp_result_field->is_null();

  return null_value;
}


/**
   @brief Execute function and store the return value in the field.

   @note This function was intended to be the concrete implementation of
    the interface function execute. This was never realized.

   @return The error state.
   @retval FALSE on success
   @retval TRUE if an error occurred.
*/
bool
Item_func_sp::execute_impl(THD *thd)
{
  bool err_status= TRUE;
  Sub_statement_state statement_state;
  Security_context *save_security_ctx= thd->security_ctx;
  enum enum_sp_data_access access=
    (m_sp->m_chistics->daccess == SP_DEFAULT_ACCESS) ?
     SP_DEFAULT_ACCESS_MAPPING : m_sp->m_chistics->daccess;

  DBUG_ENTER("Item_func_sp::execute_impl");

  if (context->security_ctx)
  {
    /* Set view definer security context */
    thd->security_ctx= context->security_ctx;
  }
  if (sp_check_access(thd))
    goto error;

  /*
    Throw an error if a non-deterministic function is called while
    statement-based replication (SBR) is active.
  */

  if (!m_sp->m_chistics->detistic && !trust_function_creators &&
      (access == SP_CONTAINS_SQL || access == SP_MODIFIES_SQL_DATA) &&
      (mysql_bin_log.is_open() &&
       thd->variables.binlog_format == BINLOG_FORMAT_STMT))
  {
    my_error(ER_BINLOG_UNSAFE_ROUTINE, MYF(0));
    goto error;
  }

  /*
    Disable the binlogging if this is not a SELECT statement. If this is a
    SELECT, leave binlogging on, so execute_function() code writes the
    function call into binlog.
  */
  thd->reset_sub_statement_state(&statement_state, SUB_STMT_FUNCTION);
  err_status= m_sp->execute_function(thd, args, arg_count, sp_result_field); 
  thd->restore_sub_statement_state(&statement_state);

error:
  thd->security_ctx= save_security_ctx;

  DBUG_RETURN(err_status);
}


void
Item_func_sp::make_field(THD *thd, Send_field *tmp_field)
{
  DBUG_ENTER("Item_func_sp::make_field");
  DBUG_ASSERT(sp_result_field);
  sp_result_field->make_field(tmp_field);
  if (name)
    tmp_field->col_name= name;
  DBUG_VOID_RETURN;
}


enum enum_field_types
Item_func_sp::field_type() const
{
  DBUG_ENTER("Item_func_sp::field_type");
  DBUG_ASSERT(sp_result_field);
  DBUG_RETURN(sp_result_field->type());
}

Item_result
Item_func_sp::result_type() const
{
  DBUG_ENTER("Item_func_sp::result_type");
  DBUG_PRINT("info", ("m_sp = %p", (void *) m_sp));
  DBUG_ASSERT(sp_result_field);
  DBUG_RETURN(sp_result_field->result_type());
}

longlong Item_func_found_rows::val_int()
{
  DBUG_ASSERT(fixed == 1);
  return current_thd->found_rows();
}


/**
  @brief Checks if requested access to function can be granted to user.
    If function isn't found yet, it searches function first.
    If function can't be found or user don't have requested access
    error is raised.

  @param thd thread handler

  @return Indication if the access was granted or not.
  @retval FALSE Access is granted.
  @retval TRUE Requested access can't be granted or function doesn't exists.
    
*/

bool
Item_func_sp::sp_check_access(THD *thd)
{
  DBUG_ENTER("Item_func_sp::sp_check_access");
  DBUG_ASSERT(m_sp);
  if (check_routine_access(thd, EXECUTE_ACL,
			   m_sp->m_db.str, m_sp->m_name.str, 0, FALSE))
    DBUG_RETURN(TRUE);

  DBUG_RETURN(FALSE);
}


bool
Item_func_sp::fix_fields(THD *thd, Item **ref)
{
  bool res;
  DBUG_ENTER("Item_func_sp::fix_fields");
  DBUG_ASSERT(fixed == 0);

  /* 
    Checking privileges to execute the function while creating view and
    executing the function of select.
   */
  if (!(thd->lex->context_analysis_only & CONTEXT_ANALYSIS_ONLY_VIEW) ||
      (thd->lex->sql_command == SQLCOM_CREATE_VIEW))
  {
    Security_context *save_security_ctx= thd->security_ctx;
    if (context->security_ctx)
      thd->security_ctx= context->security_ctx;

    res= check_routine_access(thd, EXECUTE_ACL, m_name->m_db.str,
                              m_name->m_name.str, 0, FALSE);
    thd->security_ctx= save_security_ctx;

    if (res)
    {
      context->process_error(thd);
      DBUG_RETURN(res);
    }
  }

  /*
    We must call init_result_field before Item_func::fix_fields() 
    to make m_sp and result_field members available to fix_length_and_dec(),
    which is called from Item_func::fix_fields().
  */
  res= init_result_field(thd);

  if (res)
    DBUG_RETURN(res);

  res= Item_func::fix_fields(thd, ref);

  if (res)
    DBUG_RETURN(res);

  if (thd->lex->is_view_context_analysis())
  {
    /*
      Here we check privileges of the stored routine only during view
      creation, in order to validate the view.  A runtime check is
      perfomed in Item_func_sp::execute(), and this method is not
      called during context analysis.  Notice, that during view
      creation we do not infer into stored routine bodies and do not
      check privileges of its statements, which would probably be a
      good idea especially if the view has SQL SECURITY DEFINER and
      the used stored procedure has SQL SECURITY DEFINER.
    */
    res= sp_check_access(thd);
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    /*
      Try to set and restore the security context to see whether it's valid
    */
    Security_context *save_secutiry_ctx;
    res= set_routine_security_ctx(thd, m_sp, false, &save_secutiry_ctx);
    if (!res)
      m_sp->m_security_ctx.restore_security_context(thd, save_secutiry_ctx);
    
#endif /* ! NO_EMBEDDED_ACCESS_CHECKS */
  }

  if (!m_sp->m_chistics->detistic)
  {
    used_tables_cache |= RAND_TABLE_BIT;
    const_item_cache= FALSE;
  }

  DBUG_RETURN(res);
}


void Item_func_sp::update_used_tables()
{
  Item_func::update_used_tables();

  if (!m_sp->m_chistics->detistic)
  {
    used_tables_cache |= RAND_TABLE_BIT;
    const_item_cache= FALSE;
  }
}

bool Item_func_sp::check_vcol_func_processor(void *arg)
{
  return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
}

/*
  uuid_short handling.

  The short uuid is defined as a longlong that contains the following bytes:

  Bytes  Comment
  1      Server_id & 255
  4      Startup time of server in seconds
  3      Incrementor

  This means that an uuid is guaranteed to be unique
  even in a replication environment if the following holds:

  - The last byte of the server id is unique
  - If you between two shutdown of the server don't get more than
    an average of 2^24 = 16M calls to uuid_short() per second.
*/

ulonglong uuid_value;

void uuid_short_init()
{
  uuid_value= ((((ulonglong) global_system_variables.server_id) << 56) +
               (((ulonglong) server_start_time) << 24));
}


longlong Item_func_uuid_short::val_int()
{
  ulonglong val;
  mysql_mutex_lock(&LOCK_short_uuid_generator);
  val= uuid_value++;
  mysql_mutex_unlock(&LOCK_short_uuid_generator);
  return (longlong) val;
}


/**
  Last_value - return last argument.
*/

void Item_func_last_value::evaluate_sideeffects()
{
  DBUG_ASSERT(fixed == 1 && arg_count > 0);
  for (uint i= 0; i < arg_count-1 ; i++)
    args[i]->val_int();
}

String *Item_func_last_value::val_str(String *str)
{
  String *tmp;
  evaluate_sideeffects();
  tmp= last_value->val_str(str);
  null_value= last_value->null_value;
  return tmp;
}

longlong Item_func_last_value::val_int()
{
  longlong tmp;
  evaluate_sideeffects();
  tmp= last_value->val_int();
  null_value= last_value->null_value;
  return tmp;
}

double Item_func_last_value::val_real()
{
  double tmp;
  evaluate_sideeffects();
  tmp= last_value->val_real();
  null_value= last_value->null_value;
  return tmp;
}

my_decimal *Item_func_last_value::val_decimal(my_decimal *decimal_value)
{
  my_decimal *tmp;
  evaluate_sideeffects();
  tmp= last_value->val_decimal(decimal_value);
  null_value= last_value->null_value;
  return tmp;
}


bool Item_func_last_value::fix_length_and_dec()
{
  last_value=          args[arg_count -1];
  Type_std_attributes::set(last_value);
  maybe_null=          last_value->maybe_null;
  return FALSE;
}
