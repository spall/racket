/*
  Racket
  Copyright (c) 2004-2016 PLT Design Inc.
  Copyright (c) 1995-2001 Matthew Flatt

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301 USA.

  libscheme
  Copyright (c) 1994 Brent Benson
  All rights reserved.
*/

#include "schpriv.h"
#include "schrunst.h"

#ifdef MZ_PRECISE_GC
static void register_traversers(void);
#endif

/*========================================================================*/
/*                             initialization                             */
/*========================================================================*/

void
scheme_init_linklet(Scheme_Env *env)
{
#ifdef MZ_PRECISE_GC
  register_traversers();
#endif

  GLOBAL_PRIM_W_ARITY2("instantiate-linklet", instantiate_linklet, 1, 2, 0, -1, env);
}

/*========================================================================*/
/*                          instantiating linklets                        */
/*========================================================================*/

static Scheme_Object *body_one_expr(void *prefix_plus_expr, int argc, Scheme_Object **argv)
{
  Scheme_Object *v, **saved_runstack;

  saved_runstack = scheme_resume_prefix(SCHEME_CAR((Scheme_Object *)prefix_plus_expr));
  v = _scheme_eval_linked_expr_multi(SCHEME_CDR((Scheme_Object *)prefix_plus_expr));
  scheme_suspend_prefix(saved_runstack);

  return v;
}

static int needs_prompt(Scheme_Object *e)
{
  Scheme_Type t;
  
  while (1) {
    t = SCHEME_TYPE(e);
    if (t > _scheme_values_types_)
      return 0;
  
    switch (t) {
    case scheme_lambda_type:
    case scheme_toplevel_type:
    case scheme_local_type:
    case scheme_local_unbox_type:
      return 0;
    case scheme_case_lambda_sequence_type:
      return 0;
    case scheme_define_values_type:
      e = SCHEME_VEC_ELS(e)[0];
      break;
    case scheme_inline_variant_type:
      e = SCHEME_VEC_ELS(e)[0];
      break;
    default:
      return 1;
    }
  }
}

void *scheme_linklet_run_finish(Scheme_Linklet linklet)
{
  Scheme_Thread *p;
  Scheme_Module *m = menv->module;
  Scheme_Object *body, **save_runstack, *save_prefix, *v = scheme_void;
  int depth;
  int i, cnt;
  Scheme_Cont_Frame_Data cframe;
  Scheme_Config *config;
  int volatile save_phase_shift;
  mz_jmp_buf newbuf, * volatile savebuf;
  LOG_RUN_DECLS;

  p = scheme_current_thread;
  savebuf = p->error_buf;
  p->error_buf = &newbuf;

  if (scheme_setjmp(newbuf)) {
    Scheme_Thread *p2;
    p2 = scheme_current_thread;
    p2->error_buf = savebuf;
    scheme_longjmp(*savebuf, 1);
  } else {
    cnt = linklet->num_bodies;
    for (i = 0; i < cnt; i++) {
      body = m->bodies[i];
      if (needs_prompt(body)) {
        /* We need to push the prefix after the prompt is set, so
           restore the runstack and then add the prefix back. */
        save_prefix = suspend_prefix(save_runstack);
        v = _scheme_call_with_prompt_multi(body_one_expr, 
                                           scheme_make_raw_pair(save_prefix
                                                                scheme_make_raw_pair(body, instance)));
        resume_prefix(save_prefix);

        /* Double-check that the definition-installing part of the
           continuation was not skipped. Otherwise, the compiler would
           not be able to assume that a variable reference that is
           lexically later (incuding a reference to an imported
           variable) always references a defined variable. Putting the
           prompt around a definition's RHS might be a better
           approach, but that would change the language (so mabe next
           time). */
        if (SAME_TYPE(SCHEME_TYPE(body), scheme_define_values_type)) {
          int vcnt, j;
          
          vcnt = SCHEME_VEC_SIZE(body) - 1;
          for (j = 0; j < vcnt; j++) {
            Scheme_Object *var;
            Scheme_Prefix *toplevels;
            Scheme_Bucket *b;
            
            var = SCHEME_VEC_ELS(body)[j+1];
            toplevels = (Scheme_Prefix *)MZ_RUNSTACK[SCHEME_TOPLEVEL_DEPTH(var)];
            b = (Scheme_Bucket *)toplevels->a[SCHEME_TOPLEVEL_POS(var)];
            
            if (!b->val) {
              scheme_raise_exn(MZEXN_FAIL_CONTRACT_VARIABLE, 
                               b->key,
                               "define-values: skipped variable definition;\n"
                               " cannot continue without defining variable\n"
                               "  variable: %S\n"
                               "  in module: %D",
                               (Scheme_Object *)b->key,
                               menv->module->modsrc);
            }
          }
        }
      } else
        v = _eval_linked_expr_multi(body);

      if (i < cnt)
        scheme_ignore_result(v);
    }

    p = scheme_current_thread;
    p->error_buf = savebuf;
    p->current_phase_shift = save_phase_shift;
  }

  return v;
}

static void eval_linklet_body(Scheme_Linklet *linklet)
{
#ifdef MZ_USE_JIT
  (void)scheme_linklet_run_start(linklet, scheme_make_pair(instance->name, scheme_true));
#else
  (void)scheme_linklet_run_finish(linklet);
#endif
}

static void *instantiate_linklet_k(void)
{
  Scheme_Thread *p = scheme_current_thread;
  Scheme_Linklet *linklet = (Scheme_Linklet *)p->ku.k.p1;
  Scheme_Env *instances = (Scheme_Env *)p->ku.k.p2;
  Scheme_Env **instances = (Scheme_Env **)p->ku.k.p3;
  int multi = p->ku.k.i1;
  int num_instances = p->ku.k.i2;
  Scheme_Object *b;
  Scheme_Object **save_runstack;
  Resolve_Prefix *rp;
  Scheme_Env *env;

  p->ku.k.p1 = NULL;
  p->ku.k.p2 = NULL;
  p->ku.k.p3 = NULL;

  depth = linklet->max_let_depth;  
  if (!scheme_check_runstack(depth)) {
    p->ku.k.p1 = top;
    p->ku.k.p2 = instance;
    p->ku.k.p3 = instances;
    p->ku.k.i1 = multi;
    p->ku.k.i2 = num_instances;
    return (Scheme_Object *)scheme_enlarge_runstack(depth, instantiate_linklet_k);
  }

  b = scheme_get_param(scheme_current_config(), MZCONFIG_USE_JIT);

  if (SCHEME_TRUEP(b))
    linklet = scheme_jit_linklet(linklet);

  for (i = linklet->num_exports; i--; ) {
    scheme_hash_set(instance->exports, linklet->exports[i], linklet->defns[i]);
  }

  save_runstack = push_prefix(linklet, instance, num_instances, instances);
  eval_linklet_body(linklet);  
  pop_prefix(save_runstack);

  if (!multi)
    v = scheme_check_one_value(v);
  
  return (void *)v;
}

static Scheme_Object *instantiate_linklet(Scheme_Linklet *linket, Scheme_Env *instance, Scheme_Env *env,
                                          int num_instances, Scheme_Env **instances,
                                          int multi, int top)
{
  Scheme_Thread *p = scheme_current_thread;
  
  p->ku.k.p1 = linklet;
  p->ku.k.p2 = instance;
  p->ku.k.p3 = instances;
  
  p->ku.k.i1 = multi;
  p->ku.k.i2 = num_instances;

  if (top)
    return (Scheme_Object *)scheme_top_level_do(instantiate_linklet_k, 1);
  else
    return (Scheme_Object *)instantiate_linklet_k();
}

Scheme_Object *scheme_instiantate_linklet(Scheme_Linklet *linklet, Scheme_Env *instance, int num_instances, Scheme_Env **instances)
{
  return _eval(linklet, instance, num_instances, instances, 0, 1);
}

Scheme_Object *scheme_instiantate_linklet_multi(Scheme_Linklet *linklet, Scheme_Env *instance, int num_instances, Scheme_Env **instances)
{
  return _eval(linklet, instance, num_instances, instances, 1, 1);
}

Scheme_Object *_scheme_instiantate_linklet(Scheme_Linklet *linklet, Scheme_Env *instance, int num_instances, Scheme_Env **instances)
{
  return _eval(linklet, instance, num_instances, instances, 0, 0);
}

Scheme_Object *_scheme_instiantate_linklet_multi(Scheme_Linklet *linklet, Scheme_Env *instance, int num_instances, Scheme_Env **instances)
{
  return _eval(linklet, instance, num_instances, instances, 1, 0);
}

/*========================================================================*/
/*        creating/pushing prefix for top-levels and syntax objects       */
/*========================================================================*/

static Scheme_Object **push_prefix(Scheme_Linklet *linklet, Scheme_Object *instance,
                                   int num_instances, Scheme_Object **instances)
{
  Scheme_Object **rs_save, **rs, *v;
  Scheme_Prefix *pf;
  int i, j, pos, tl_map_len;

  rs_save = rs = MZ_RUNSTACK;

  i = 0;
  for (j = linklet->num_importss; j--; ) {
    i += linklet->num_imports[j];
  }
  i += linklet->num_exports;
  tl_map_len = (i + 31) / 32;

  pf = scheme_malloc_tagged(sizeof(Scheme_Prefix) 
                            + ((i-mzFLEX_DELTA) * sizeof(Scheme_Object *))
                            + (tl_map_len * sizeof(int)));
  pf->iso.so.type = scheme_prefix_type;
  pf->num_slots = i;
  --rs;
  MZ_RUNSTACK = rs;
  rs[0] = (Scheme_Object *)pf;

  pos = 0;
  for (j = 0; j < linklet->num_importss; j++) {
    for (i = 0; i < linklet->num_imports[j]; i++) {
      v = scheme_hash_ref(instances[j], linklet->importss[j][i]);
      if (!v) {
        scheme_signal_error("instantiate-linklet: mismatch;\n"
                            " possibly, bytecode file needs re-compile because dependencies changed\n"
                            "  name: %V\n"
                            "  exporting instance: %V\n"
                            "  importing instance: %V",
                            linklet->importss[j][i],
                            instances[j]->name,
                            instance->name);
      }
      v = scheme_global_bucket(v, (Scheme_Env *)instances[j]);
      pf->a[pos++] = v;
    }
  }

  for (i = 0; i < linklet->num_exports; i++) {
    v = get_instance_variable_bucket(instance, linklet->exports[i], 1);
    pf->a[pos++] = v;
  }

  return rs_save;
}

static void pop_prefix(Scheme_Object **rs)
{
  /* This function must not allocate, since a relevant multiple-values
     result may be in the thread record (and we don't want it zerod) */
  MZ_RUNSTACK = rs;
}

static Scheme_Object *suspend_prefix(Scheme_Object **rs)
{
  if (rs != MZ_RUNSTACK) {
    Scheme_Object *v;
    v = MZ_RUNSTACK[0];
    MZ_RUNSTACK++;
    return v;
  } else
    return NULL;
}

static Scheme_Object **resume_prefix(Scheme_Object *v)
{
  if (v) {
    --MZ_RUNSTACK;
    MZ_RUNSTACK[0] = v;
    return MZ_RUNSTACK + 1;
  } else
    return MZ_RUNSTACK;
}

#ifdef MZ_PRECISE_GC
static void mark_pruned_prefixes(struct NewGC *gc) XFORM_SKIP_PROC
{
  if (!GC_is_partial(gc)) {
    if (scheme_inc_prefix_finalize != (Scheme_Prefix *)0x1) {
      Scheme_Prefix *pf = scheme_inc_prefix_finalize;
      while (pf->next_final != (Scheme_Prefix *)0x1) {
        pf = pf->next_final;
      }
      pf->next_final = scheme_prefix_finalize;
      scheme_prefix_finalize = scheme_inc_prefix_finalize;
      scheme_inc_prefix_finalize = (Scheme_Prefix *)0x1;
    }
  }
  
  if (scheme_prefix_finalize != (Scheme_Prefix *)0x1) {
    Scheme_Prefix *pf = scheme_prefix_finalize, *next;
    Scheme_Object *clo;
    int i, *use_bits, maxpos;
    
    scheme_prefix_finalize = (Scheme_Prefix *)0x1;
    while (pf != (Scheme_Prefix *)0x1) {
      /* If not marked, only references are through closures: */
      if (!GC_is_marked2(pf, gc)) {
        /* Clear slots that are not use in map */
        maxpos = pf->num_slots;
        use_bits = PREFIX_TO_USE_BITS(pf);
        for (i = (maxpos + 31) / 32; i--; ) {
          int j;
          for (j = 0; j < 32; j++) {
            if (!(use_bits[i] & ((unsigned)1 << j))) {
              int pos;
              pos = (i * 32) + j;
              pf->a[pos] = NULL;
            }
          }
          use_bits[i] = 0;
        }
        /* Should mark/copy pf, but not trigger or require mark propagation: */
#ifdef MZ_GC_BACKTRACE
        GC_set_backpointer_object(pf->backpointer);
#endif
        GC_mark_no_recur(gc, 1);
        gcMARK2(pf, gc);
        pf = (Scheme_Prefix *)GC_resolve2(pf, gc);
        GC_retract_only_mark_stack_entry(pf, gc);
        GC_mark_no_recur(gc, 0);
      } else
        pf = (Scheme_Prefix *)GC_resolve2(pf, gc);

      /* Clear use map */
      use_bits = PREFIX_TO_USE_BITS(pf);
      maxpos = pf->num_slots;
      for (i = (maxpos + 31) / 32; i--; )
        use_bits[i] = 0;

      /* Fix up closures that reference this prefix: */
      clo = (Scheme_Object *)GC_resolve2(pf->fixup_chain, gc);
      pf->fixup_chain = NULL;
      while (clo) {
        Scheme_Object *next;
        if (SCHEME_TYPE(clo) == scheme_closure_type) {
          Scheme_Closure *cl = (Scheme_Closure *)clo;
          int closure_size = ((Scheme_Lambda *)GC_resolve2(cl->code, gc))->closure_size;
          next = cl->vals[closure_size - 1];
          cl->vals[closure_size-1] = (Scheme_Object *)pf;
        } else if (SCHEME_TYPE(clo) == scheme_native_closure_type) {
          Scheme_Native_Closure *cl = (Scheme_Native_Closure *)clo;
          int closure_size = ((Scheme_Native_Lambda *)GC_resolve2(cl->code, gc))->closure_size;
          next = cl->vals[closure_size - 1];
          cl->vals[closure_size-1] = (Scheme_Object *)pf;
        } else {
          MZ_ASSERT(0);
          next = NULL;
        }
        clo = (Scheme_Object *)GC_resolve2(next, gc);
      }
      if (SCHEME_PREFIX_FLAGS(pf) & 0x1)
        SCHEME_PREFIX_FLAGS(pf) -= 0x1;

      /* Next */
      next = pf->next_final;
      pf->next_final = NULL;

      pf = next;
    }
  }
}

int check_pruned_prefix(void *p) XFORM_SKIP_PROC
{
  Scheme_Prefix *pf = (Scheme_Prefix *)p;
  return SCHEME_PREFIX_FLAGS(pf) & 0x1;
}
#endif

/*========================================================================*/
/*                         precise GC traversers                          */
/*========================================================================*/

#ifdef MZ_PRECISE_GC

START_XFORM_SKIP;

#include "mzmark_linklet.inc"

static void register_traversers(void)
{
  GC_REG_TRAV(scheme_rt_saved_stack, mark_saved_stack);
}

END_XFORM_SKIP;

#endif
