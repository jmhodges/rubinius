#include "shotgun.h"
#include "cpu.h"
#include "tuple.h"
#include "module.h"
#include "class.h"
#include "hash.h"
#include "methctx.h"
#include "array.h"
#include "string.h"
#include "symbol.h"
#include "machine.h"
#include "flags.h"
#include "bytearray.h"
#include "fixnum.h"

#include <string.h>
#include <errno.h>

#define EXCESSIVE_TRACING state->excessive_tracing
#define USE_GLOBAL_CACHING 1
#define USE_INLINE_CACHING 1

#define RISA(obj,cls) (REFERENCE_P(obj) && ISA(obj,BASIC_CLASS(cls)))

#if USE_INTCODE
#define next_int _int = *c->ip_ptr++;
#else
#define set_int(i,s) ((i)=(((s)[0] << 24) | ((s)[1] << 16) | ((s)[2] << 8) | (s)[3]))
#define next_int set_int(_int,(c->ip_ptr)); c->ip_ptr += 4
#endif

/* #ifndef __BIG_ENDIAN__ */
/* #define next_int _int = swap32(*(int*)(c->data + c->ip)); c->ip += 4 */
/* #else */
/* #define next_int _int = *(int*)(c->data + c->ip); c->ip += 4 */
/* #endif */

#if DIRECT_THREADED
#include "instruction_funcs.gen"
DT_ADDRESSES;

#ifdef SHOW_OPS
#define NEXT_OP printf(" => %p\n", *c->ip_ptr); assert(*c->ip_ptr); goto **c->ip_ptr++
#else
#define NEXT_OP goto **c->ip_ptr++
#endif

#endif

#define next_literal next_int; _lit = tuple_at(state, c->literals, _int)

OBJECT cpu_open_class(STATE, cpu c, OBJECT under, OBJECT sup) {
  OBJECT sym, _lit, val, s1, s2, s3, s4, sup_itr;
  uint32_t _int;
    
  next_literal;
  sym = _lit;
    
  val = module_const_get(state, under, sym);
  if(!RTEST(val)) {    
    val = class_constitute(state, sup, under);
    if(NIL_P(val)) {
      cpu_raise_exception(state, c, 
        cpu_new_exception(state, c, state->global->exc_arg, "Invalid superclass"));
      return Qnil;
    }
    
    /*
    printf("Defining %s under %s.\n", rbs_symbol_to_cstring(state, sym), _inspect(c->enclosing_class));
    */
    if(c->enclosing_class != state->global->object) {
      s1 = symbol_to_string(state, module_get_name(c->enclosing_class));
      s2 = symbol_to_string(state, sym);
      s3 = string_dup(state, s1);
      string_append(state, s3, string_new(state, "::"));
      string_append(state, s3, s2);
      s4 = string_to_sym(state, s3);
      module_set_name(val, s4);
      // printf("Module %s name set to %s (%d)\n", _inspect(val), rbs_symbol_to_cstring(state, s4), FIXNUM_TO_INT(class_get_instance_fields(val)));
    } else {
      module_set_name(val, sym);
      // printf("Module %s name set to %s (%d)\n", _inspect(val), rbs_symbol_to_cstring(state, sym), FIXNUM_TO_INT(class_get_instance_fields(val)));
    }
    module_const_set(state, under, sym, val);
    sup_itr = sup;
    
    /* This code does not work. perform_hook will return before running */
    /*
    while(!NIL_P(sup_itr)) {
      cpu_perform_hook(state, c, sup_itr, state->global->sym_inherited, val);
      sup_itr = class_get_superclass(sup_itr);
      if(sup_itr == state->global->object) { break; }
    }
    */
  }
  return val;
}

OBJECT cpu_open_module(STATE, cpu c, OBJECT under) {
  OBJECT sym, _lit, val;
  int _int;
  next_literal;
  
  sym = _lit;
  
  val = module_const_get(state, under, sym);
  if(!RTEST(val)) {
    val = module_allocate_mature(state, 0);
    module_setup_fields(state, val);
    module_set_name(val, sym);
    module_const_set(state, under, sym, val);
    module_setup_fields(state, object_metaclass(state, val));
    module_set_parent(val, under);
  }
  
  return val;
}

static inline OBJECT _real_class(STATE, OBJECT obj) {
  if(REFERENCE_P(obj)) {
    return HEADER(obj)->klass;
  } else {
    return object_class(state, obj);
  }
}

/* Locate the method object for calling method +name+ on an instance of +klass+.
   +mod+ is updated to point to the Module that holds the method.
   
   
   * The method is then looked for in the hash tables up the superclass
     chain.
     
 */
 
#define TUPLE_P(obj) (CLASS_OBJECT(obj) == BASIC_CLASS(tuple))
 
static inline OBJECT cpu_check_for_method(STATE, cpu c, OBJECT hsh, OBJECT name) {
  OBJECT meth;
  
  meth = hash_find(state, hsh, name);
  
  if(NIL_P(meth)) { return Qnil; }
  
  if(c->call_flags == 1) { return meth; }
  
  /* Check that unless we can look for private methods that method isn't private. */
  if(TUPLE_P(meth) && (tuple_at(state, meth, 0) == state->global->sym_private)) {
    /* We skip private methods. */
    return Qnil;
  }
  
  return meth;
}

#define PUBLIC_P(meth) (tuple_at(state, meth, 0) == state->global->sym_public)
#define VISIBLE_P(cp, mo, tu) ((c->call_flags == 1) || (!tup) || PUBLIC_P(meth))
#define UNVIS_METHOD(var) if(TUPLE_P(var)) { var = tuple_at(state, var, 1); }
#define UNVIS_METHOD2(var) if(tup) { var = tuple_at(state, var, 1); }

static inline OBJECT cpu_find_method(STATE, cpu c, OBJECT klass, OBJECT name,  OBJECT *mod) {
  OBJECT ok, hsh, cache, orig_klass, meth, ser;
  int tup;
  struct method_cache *ent;
  
  cache = Qnil;

#if USE_INLINE_CACHING
  /* Skip if we aren't running a normal method. */
  if(c->type == FASTCTX_NORMAL) {
    
  cache = cmethod_get_cache(c->method);
  
  /* There is a cache index for this send, use it! */
  if(c->cache_index > 0) {
    if(tuple_at(state, cache, c->cache_index) == klass) {
      meth = tuple_at(state, cache, c->cache_index + 2);
      ser =  tuple_at(state, cache, c->cache_index + 3);
      /* We don't check the visibility here because the inline cache has
         fixed visibility, meaning it will always be the same after it's
         populated. */
      if(cmethod_get_serial(meth) == ser) {
#if TRACK_STATS
        state->cache_inline_hit++;
#endif
        *mod = tuple_at(state, cache, c->cache_index + 1);
        return meth;
      } else {
#if TRACK_STATS
        state->cache_inline_stale++;
#endif
      }
    }
  }

  }
#endif

#if USE_GLOBAL_CACHING
  ent = state->method_cache + CPU_CACHE_HASH(klass, name);
  if(ent->name == name && ent->klass == klass) {
    *mod = ent->module;
    meth = ent->method;
    tup = TUPLE_P(meth);
    
    if(VISIBLE_P(c, meth, tup)) {
      UNVIS_METHOD2(meth);

#if USE_INLINE_CACHING
      /* Update the inline cache. */
      if(c->type == FASTCTX_NORMAL && c->cache_index > 0) {
        tuple_put(state, cache, c->cache_index, klass);
        tuple_put(state, cache, c->cache_index + 1, ent->module);
        tuple_put(state, cache, c->cache_index + 2, meth);
        tuple_put(state, cache, c->cache_index + 3, cmethod_get_serial(meth));
      }
#endif
    
#if TRACK_STATS
      state->cache_hits++;
#endif
      return meth;
    }
  }
#if TRACK_STATS
  if(ent->name) {
    state->cache_collisions++;
  }
  state->cache_misses++;
#endif
#endif

  /* Validate klass is valid even. */
  if(NUM_FIELDS(klass) <= CLASS_f_SUPERCLASS) {
    printf("Warning: encountered invalid class (not big enough).\n");
    *mod = Qnil;
    return Qnil;
  }

  hsh = module_get_methods(klass);
  
  /* Ok, rather than assert, i'm going to just bail. Makes the error
     a little strange, but handle-able in ruby land. */
  
  if(!ISA(hsh, state->global->hash)) {
    printf("Warning: encountered invalid module (methods not a hash).\n");
    *mod = Qnil;
    return Qnil; 
  }
  
  meth = cpu_check_for_method(state, c, hsh, name);
  
  /*
  printf("Looking for method: %s in %p (%s)\n", 
    string_byte_address(state, symtbl_find_string(state, state->global->symbols, name)), obj,
    string_byte_address(state, symtbl_find_string(state, state->global->symbols, 
        class_get_name(object_class(state, obj))))
    );
  */
  
  orig_klass = klass;
  while(NIL_P(meth)) {
    ok = klass;
    
    /* Validate klass is still valid. */
    if(NUM_FIELDS(klass) <= CLASS_f_SUPERCLASS) {
      printf("Warning: encountered invalid class (not big enough).\n");
      *mod = Qnil;
      return Qnil;
    }
    
    klass = class_get_superclass(klass);
    if(NIL_P(klass)) { break; }
    /*
    printf("Looking for method (sup): %s in %ul (%s)\n", 
      string_byte_address(state, symtbl_find_string(state, state->global->symbols, name)), klass,
      string_byte_address(state, symtbl_find_string(state, state->global->symbols, 
        class_get_name(klass)))
    );
    */
    hsh = module_get_methods(klass);
    if(!ISA(hsh, state->global->hash)) {
      printf("Warning: encountered invalid module (methods not a hash).\n");
      *mod = Qnil;
      return Qnil; 
    }
        
    meth = cpu_check_for_method(state, c, hsh, name);
  }
  
  *mod = klass;

#if USE_GLOBAL_CACHING
  /* Update the cache. */
  if(RTEST(meth)) {
    ent->klass = orig_klass;
    ent->name = name;
    ent->module = klass;
    ent->method = meth;
    
    UNVIS_METHOD(meth);

#if USE_INLINE_CACHING
    /* Update the inline cache. */
    if(c->type == FASTCTX_NORMAL && c->cache_index > 0) {
      tuple_put(state, cache, c->cache_index, orig_klass);
      tuple_put(state, cache, c->cache_index + 1, ent->module);
      tuple_put(state, cache, c->cache_index + 2, meth);
      tuple_put(state, cache, c->cache_index + 3, cmethod_get_serial(meth));
    }
#endif    
  }
#else
  if(RTEST(meth)) {
    UNVIS_METHOD(meth);
  }
#endif
  
  return meth;
}

OBJECT exported_cpu_find_method(STATE, cpu c, OBJECT klass, OBJECT name, OBJECT *mod) {
    return cpu_find_method(state, c, klass, name, mod);
}

OBJECT cpu_locate_method_on(STATE, cpu c, OBJECT obj, OBJECT sym) {
  OBJECT mod, meth;
  
  meth = cpu_find_method(state, c, _real_class(state, obj), sym, &mod);
  if(RTEST(meth)) {
    return tuple_new2(state, 2, meth, mod);
  }
  return Qnil;
}

static inline OBJECT cpu_locate_method(STATE, cpu c, OBJECT obj, OBJECT sym, 
          OBJECT *mod, int *missing) {
  OBJECT mo;
  
  *missing = FALSE;
  
  mo = cpu_find_method(state, c, obj, sym, mod);
  if(!NIL_P(mo)) { return mo; }
    
  // printf("method missing: %p\n", state->global->method_missing);
  mo = cpu_find_method(state, c, obj, state->global->method_missing, mod);
  *missing = TRUE;
  
  // printf("Found method: %p\n", mo);
  
  return mo;
}

static inline OBJECT cpu_create_context(STATE, cpu c, OBJECT recv, OBJECT mo, 
      OBJECT name, OBJECT mod, unsigned long int args, OBJECT block) {
  OBJECT sender, ctx, ba;
  int num_lcls;
  struct fast_context *fc;
  
  sender = c->active_context;
  
  ba = cmethod_get_bytecodes(mo);

#if DIRECT_THREADED
  if(!FLAG2_SET_P(ba, IsFrozenFlag)) {
    if(FLAG2_SET_P(ba, IsLittleEndianFlag)) {
#if defined(__BIG_ENDIAN__)
      iseq_flip(state, ba);
#endif
    } else {
#if !defined(__BIG_ENDIAN__)
      iseq_flip(state, ba);
#endif
    }
    calculate_into_gotos(state, ba, _dt_addresses);
    FLAG2_SET(ba, IsFrozenFlag);
  }
#else
  if(FLAG2_SET_P(ba, IsLittleEndianFlag)) {
#if defined(__BIG_ENDIAN__)
    iseq_flip(state, ba);
#endif
  } else {
#if !defined(__BIG_ENDIAN__)
    iseq_flip(state, ba);
#endif
  }

#endif
  
  num_lcls = FIXNUM_TO_INT(cmethod_get_locals(mo));
  
  cpu_flush_sp(c);
    
  ctx = object_memory_new_context(state->om);
  if(ctx >= state->om->context_last) {
    state->om->collect_now |= OMCollectYoung;
  }
  
  HEADER(ctx)->klass = Qnil;
  HEADER(ctx)->fields = FASTCTX_FIELDS;
  /*
  
  ctx = _om_new_ultra(state->om, state->global->fastctx, (HEADER_SIZE + FASTCTX_FIELDS) * REFSIZE);
  SET_NUM_FIELDS(ctx, FASTCTX_FIELDS);
  
  FLAG_SET(ctx, CTXFastFlag);
  FLAG_SET(ctx, StoresBytesFlag);
    
  CHECK_PTR(mo);
  */
  
  fc = FASTCTX(ctx);
  // memset(fc, 0, sizeof(struct fast_context));
  fc->sender = sender;
  xassert(om_valid_sender_p(state->om, ctx, sender));
  fc->ip = 0;
  fc->sp = c->sp;
  /* fp_ptr points to the location on the stack as the context
     was being created. */
  fc->fp_ptr = c->sp_ptr;
  
  fc->block = block;
  fc->method = mo;
  fc->data = bytearray_byte_address(state, ba);
  fc->data_size = bytearray_bytes(state, ba);
  fc->literals = cmethod_get_literals(mo);
  fc->self = recv;
  if(num_lcls > 0) {
    fc->locals = tuple_new(state, num_lcls + 2);
    GC_MAKE_FOREVER_YOUNG(fc->locals);
  } else {
    fc->locals = Qnil;
  }
  fc->argcount = args;
  fc->name = name;
  fc->method_module = mod;
  fc->type = FASTCTX_NORMAL;

  return ctx;
}

void cpu_raise_from_errno(STATE, cpu c, const char *msg) {
  OBJECT cls;
  char buf[32];
  
  cls = hash_get(state, state->global->errno_mapping, I2N(errno));
  if(NIL_P(cls)) {
    cls = state->global->exc_arg;
    snprintf(buf, sizeof(buf), "Unknown errno %d", errno);
    msg = buf;
  }
    
  cpu_raise_exception(state, c, cpu_new_exception(state, c, cls, msg));
}

void cpu_raise_arg_error_generic(STATE, cpu c, const char *msg) {
  cpu_raise_exception(state, c, cpu_new_exception(state, c, state->global->exc_arg, msg));
}

void cpu_raise_arg_error(STATE, cpu c, int args, int req) {
  char msg[1024];
  snprintf(msg, 1024, "wrong number of arguments (got %d, required %d)", args, req);
  cpu_raise_exception(state, c, cpu_new_exception(state, c, state->global->exc_arg, msg));
}

void cpu_raise_primitive_failure(STATE, cpu c, int primitive_idx) {
  char msg[1024];
  OBJECT primitive_failure;
  snprintf(msg, 1024, "Primitive with index (%d) failed", primitive_idx);

  primitive_failure = cpu_new_exception(state, c, state->global->exc_primitive_failure, msg);
  cpu_raise_exception(state, c, primitive_failure);
}

static inline int cpu_try_primitive(STATE, cpu c, OBJECT mo, OBJECT recv, int args, OBJECT sym, OBJECT mod) {
  int prim, req, ret;
  
  prim = FIXNUM_TO_INT(cmethod_get_primitive(mo));
  req = FIXNUM_TO_INT(cmethod_get_required(mo));
  
  ret = FALSE;
  if(prim > -1) {
    if(req < 0 || args == req) {
      stack_push(recv);
      // printf("Running primitive: %d\n", prim);
      if(cpu_perform_primitive(state, c, prim, mo, args, sym, mod)) {
        /* Worked! */
        return TRUE;
      }
      /* Didn't work, need to remove the recv we put on before. */
      stack_pop();
      if(EXCESSIVE_TRACING) {
        printf("[[ Primitive failed! -- %d ]]\n", prim);
      }
    } else if(req >= 0) { // not sure why this was here... } && object_kind_of_p(state, mo, state->global->cmethod)) {
      /* raise an exception about them not doing it right. */
      cpu_raise_arg_error(state, c, args, req);
      ret = TRUE;
    }
  }
  
  return ret;
}

/* Raw most functions for moving in a method. Adjusts register. */
/* Stack offset is used to adjust sp when it's saved so when
   this context is swapped back in,  any arguments are automatically
   removed from the stack */
inline void cpu_save_registers(STATE, cpu c, int offset) {
  if(!RTEST(c->active_context)) return;
  cpu_flush_ip(c);
  cpu_flush_sp(c);
  if(HEADER(c->active_context)->klass == Qnil || methctx_is_fast_p(state, c->active_context)) {
    struct fast_context *fc;
    fc = (struct fast_context*)BYTES_OF(c->active_context);
    fc->sp = c->sp - offset;
    fc->ip = c->ip;
  } else {
    methctx_set_sp(c->active_context, I2N(c->sp - offset));
    methctx_set_ip(c->active_context, I2N(c->ip));
  }
}

#include <string.h>
void nmc_activate(STATE, cpu c, OBJECT nmc, int reraise);

inline void cpu_restore_context_with_home(STATE, cpu c, OBJECT ctx, OBJECT home, int ret, int is_block) {
  struct fast_context *fc;
  
  c->depth--;
  
  /* Home is actually the main context here because it's the method
     context that holds all the data. So if it's a fast, we restore
     it's data, then if ctx != home, we restore a little more */
  
  fc = (struct fast_context*)BYTES_OF(home);
  CHECK_PTR(fc->self);
  CHECK_PTR(fc->method);
  memcpy((void*)c, (void*)fc, sizeof(struct fast_context));
  /* Only happens if we're restoring a block. */
  if(ctx != home) {
    // assert(!ISA(ctx, state->global->fastctx));
    c->sp = FIXNUM_TO_INT(methctx_get_sp(ctx));
    c->ip = FIXNUM_TO_INT(methctx_get_ip(ctx));
          
    c->sender = methctx_get_sender(ctx);
    /* FIXME: seems like we should set c->block too.. but that
       seems to break things.. */
  }
  
  /* Ok, reason we'd be restoring a native context:
     1) the native context used rb_funcall and we need to return
        it the result of the call.
  */
  if(fc->type == FASTCTX_NMC) {
    nmc_activate(state, c, home, FALSE);
    /* We return because nmc_activate will setup the cpu to do whatever
       it needs to next. */
    return;
  }
  
  cpu_cache_ip(c);
  cpu_cache_sp(c);
  
  c->home_context = home;
  c->active_context = ctx;
}

/* Layer 2 method movement: use lower level only. */

inline void cpu_activate_context(STATE, cpu c, OBJECT ctx, OBJECT home, int so) {
  c->depth += 2;
  cpu_save_registers(state, c, so);
  cpu_restore_context_with_home(state, c, ctx, home, FALSE, FALSE);
}

static inline void cpu_restore_context(STATE, cpu c, OBJECT x) {
    cpu_restore_context_with_home(state, c, x, x, FALSE, FALSE);
}

/* Layer 2.5: Uses lower layers to return to the calling context.
   Returning ends here. */

inline int cpu_return_to_sender(STATE, cpu c, int consider_block, int exception) {
  OBJECT sender, home, home_sender;
  int is_block;
  
  is_block = blokctx_s_block_context_p(state, c->active_context);
  sender = c->sender;
    
  if(sender == Qnil) {
    c->active_context = Qnil;
    
    /* Thread exitting, reschedule.. */
    if(c->current_thread != c->main_thread) {
      cpu_thread_run_best(state, c);
      return FALSE;
    /* Switch back to the main task... */
    } else if(c->current_task != c->main_task) {
      cpu_task_select(state, c, c->main_task);
      return FALSE;
    }    
  } else {
    
    /* Implements a block causing the the context it was created in
       to return (IE, a non local return) */
    
    if(consider_block && is_block) {
      home = blokctx_home(state, c->active_context);
      if(methctx_is_fast_p(state, home)) {
        home_sender = FASTCTX(home)->sender;
      } else {
        home_sender = methctx_get_sender(home);
      }
      
      sender = home_sender;
    }
        
    if(blokctx_s_block_context_p(state, sender)) {
      home = blokctx_home(state, sender);
    } else {
      home = sender;
    }
    
    if(!is_block) {
      /* Break the chain. Lets us detect invalid non-local returns, as well
         is much nicer on the GC. exception is set if we're returning
         due to raising an exception. We keep the sender in this case so that
         the context chain can be walked to generate a backtrace. 
         
         NOTE: This might break Kernel#caller when called inside a block, which
         was created inside a method that has already returned. Thats an edge
         case I'm wiling to live with (for now). */
      if(!exception) { 
        // FASTCTX(c->active_context)->sender = Qnil;
      }
      object_memory_retire_context(state->om, c->active_context);
    }
    
    if(EXCESSIVE_TRACING) {
      if(HEADER(sender)->klass == Qnil) {
        printf("Returning to a stack context.\n");
      } else {
        printf("Returning to %s.\n", _inspect(sender));
      }
    }
    xassert(om_valid_context_p(state, sender));
    xassert(om_valid_context_p(state, home));
    
    cpu_restore_context_with_home(state, c, sender, home, TRUE, is_block);
  }
  
  return TRUE;
}

/* Layer 2.6: Uses lower layers to return to the created context of the
   current block (ie, break in a block) */
static inline void cpu_return_to_block_creator(STATE, cpu c) {
  OBJECT caller, home, top;
  
  top = cpu_stack_top(state, c);
  caller = blokctx_home(state, c->active_context);
  
  if(blokctx_s_block_context_p(state, caller)) {
    home = blokctx_home(state, caller);
  } else {
    home = caller;
  }
  
  cpu_restore_context_with_home(state, c, caller, home, TRUE, TRUE);
  cpu_stack_push(state, c, top, FALSE);
}

/* Layer 3: goto. Basically jumps directly into the specificed method. 
   no lookup required. */

inline void cpu_goto_method(STATE, cpu c, OBJECT recv, OBJECT meth,
                                     int count, OBJECT name, OBJECT block) {
  OBJECT ctx;
  
  if(cpu_try_primitive(state, c, meth, recv, count, name, Qnil)) { return; }
  ctx = cpu_create_context(state, c, recv, meth, name, 
        _real_class(state, recv), (unsigned long int)count, block);
  cpu_activate_context(state, c, ctx, ctx, 0);
}

/* Layer 3: hook. Shortcut for running hook methods. */

inline void cpu_perform_hook(STATE, cpu c, OBJECT recv, OBJECT meth, OBJECT arg) {
  OBJECT ctx, mo, mod;
  mo = cpu_find_method(state, c, _real_class(state, recv), meth, &mod);
  if(NIL_P(mo)) return;
  stack_push(arg);
  
  ctx = cpu_create_context(state, c, recv, mo, meth, 
        _real_class(state, recv), 1, Qnil);
  cpu_activate_context(state, c, ctx, ctx, 1);
}

/* Layer 4: High level method calling. */

/* Callings +mo+ either as a primitive or by allocating a context and
   activating it. */

static inline void _cpu_build_and_activate(STATE, cpu c, OBJECT mo, 
        OBJECT recv, OBJECT sym, int args, OBJECT block, int missing, OBJECT mod) {
  OBJECT ctx;
  int prim = 0;
  c->call_flags = 0;
  if(missing) {
    cpu_flush_ip(c);
    DEBUG("%05d: Missed Calling %s => %s on %s (%p/%d) (%d).\n", c->depth,
     rbs_symbol_to_cstring(state, cmethod_get_name(c->method)),
     rbs_symbol_to_cstring(state, sym), _inspect(recv), c->method, c->ip, missing);
  }

  if(missing) {
    args += 1;
    stack_push(sym);
    // printf("EEK! method_missing!\n");
    // abort();
  } else {
    if(cpu_try_primitive(state, c, mo, recv, args, sym, mod)) {
      if(EXCESSIVE_TRACING) {
        printf("%05d: Called prim %s => %s on %s.\n", c->depth,
          rbs_symbol_to_cstring(state, cmethod_get_name(c->method)),  
          rbs_symbol_to_cstring(state, sym), _inspect(recv));
      }
      return;
    }
    prim = 1;
  }

  if(EXCESSIVE_TRACING) {
    cpu_flush_ip(c);
    printf("%05d: Calling %s => %s#%s on %s (%p/%lu) (%s).\n", c->depth,
      rbs_symbol_to_cstring(state, cmethod_get_name(c->method)),  
      rbs_symbol_to_cstring(state, module_get_name(mod)),
      rbs_symbol_to_cstring(state, sym), 
      _inspect(recv), (void*)c->method, c->ip,
      prim ? "" : "PRIM FAILED"
      );
  }
  ctx = cpu_create_context(state, c, recv, mo, sym, mod, (unsigned long int)args, block);
  /*
  if(RTEST(block)) {
    printf("in send to '%s', block %p\n", rbs_symbol_to_cstring(state, sym), block);
  }
  */
  cpu_activate_context(state, c, ctx, ctx, args);
  //printf("Setting method module to: %s\n", _inspect(mod));
  c->method_module = mod;
}


/* Layer 4: direct activation. Used for calling a method thats already
   been looked up. */
static inline void cpu_activate_method(STATE, cpu c, OBJECT recv, OBJECT mo,
                                       int args, OBJECT name, OBJECT block) {
  
  _cpu_build_and_activate(state, c, mo, recv, name, args, block, 0, Qnil);  
}

/* Layer 4: send. Primary method calling function. */

static inline void cpu_unified_send(STATE, cpu c, OBJECT recv, int idx, int args, OBJECT block) {
  OBJECT sym, mo, mod;
  int missing;

  sym = tuple_at(state, c->literals, idx);
  
  if(0 && c->depth == 1000) {
    printf("Runaway depth...\n");
    abort();
  }
  
  missing = 0;
  
  mo = cpu_locate_method(state, c, _real_class(state, recv), sym, &mod, &missing);
  if(NIL_P(mo)) {
    cpu_flush_ip(c);
    printf("%05d: Calling %s on %s (%p/%lu) (%d).\n", c->depth, rbs_symbol_to_cstring(state, sym), _inspect(recv), (void *)c->method, c->ip, missing);
    printf("Fuck. no method found at all, was trying %s on %s.\n", rbs_symbol_to_cstring(state, sym), rbs_inspect(state, recv));
    assert(RTEST(mo));
  }
  
  if(TUPLE_P(mo)) {
    mo = cpu_locate_method(state, c, _real_class(state, recv), sym, &mod, &missing);
  }
  
  /* Make sure no one else sees the a recently set cache_index, it was
     only for us! */
  c->cache_index = -1;
  
  _cpu_build_and_activate(state, c, mo, recv, sym, args, block, missing, mod);
}

/* This is duplicated from above rather than adding another parameter
   because unified_send is used SO often that I didn't want to slow it down
   any with checking a flag. */
static inline void cpu_unified_send_super(STATE, cpu c, OBJECT recv, int idx, int args, OBJECT block) {
  OBJECT sym, mo, klass, mod;
  int missing;
  xassert(RTEST(c->literals));
  sym = tuple_at(state, c->literals, idx);
    
  missing = 0;
  
  // printf("Looking up from: %s\n", _inspect(c->method_module));
  
  klass = class_get_superclass(c->method_module);
    
  mo = cpu_locate_method(state, c, klass, sym, &mod, &missing);
  if(NIL_P(mo)) {
    printf("Fuck. no method found at all, was trying %s on %s.\n", rbs_symbol_to_cstring(state, sym), rbs_inspect(state, recv));
    assert(RTEST(mo));
  }
  
  /* Make sure no one else sees the a recently set cache_index, it was
     only for us! */
  c->cache_index = -1;
  
  _cpu_build_and_activate(state, c, mo, recv, sym, args, block, missing, mod);
}

void cpu_send_method(STATE, cpu c, OBJECT recv, OBJECT sym, int args) {
  OBJECT mo, mod;
  int missing;
  
  missing = 0;
  
  /* No cache location, sorry. */
  c->cache_index = -1;
  
  mo = cpu_locate_method(state, c, _real_class(state, recv), sym, &mod, &missing);
  if(NIL_P(mo)) {
    cpu_flush_ip(c);
    printf("%05d: Calling %s on %s (%p/%lu) (%d).\n", c->depth, rbs_symbol_to_cstring(state, sym), _inspect(recv), (void *)c->method, c->ip, missing);
    printf("Fuck. no method found at all, was trying %s on %s.\n", rbs_symbol_to_cstring(state, sym), rbs_inspect(state, recv));
    assert(RTEST(mo));
  }
    
  _cpu_build_and_activate(state, c, mo, recv, sym, args, Qnil, missing, mod);
}

void cpu_send_method2(STATE, cpu c, OBJECT recv, OBJECT sym, int args, OBJECT block) {
  OBJECT mo, mod;
  int missing;
  
  missing = 0;
  
  /* No cache location, sorry. */
  c->cache_index = -1;
  
  mo = cpu_locate_method(state, c, _real_class(state, recv), sym, &mod, &missing);
  if(NIL_P(mo)) {
    cpu_flush_ip(c);
    printf("%05d: Calling %s on %s (%p/%lu) (%d).\n", c->depth, rbs_symbol_to_cstring(state, sym), _inspect(recv), (void *)c->method, c->ip, missing);
    printf("Fuck. no method found at all, was trying %s on %s.\n", rbs_symbol_to_cstring(state, sym), rbs_inspect(state, recv));
    assert(RTEST(mo));
  }
    
  _cpu_build_and_activate(state, c, mo, recv, sym, args, block, missing, mod);
}

const char *cpu_op_to_name(STATE, char op) {
#include "instruction_names.h"
  return get_instruction_name(op);
}

int cpu_dispatch(STATE, cpu c) {
  unsigned char op;  

  op = *c->ip_ptr++;
  // printf("IP: %d, SP: %d, OP: %s (%d)\n", c->ip, c->sp, cpu_op_to_name(state, op), op);
  // #include "instructions.gen"
  return TRUE;
}

void state_collect(STATE, cpu c);
void state_major_collect(STATE, cpu c);

void cpu_run(STATE, cpu c, int setup) {
  IP_TYPE op;

  if(setup) {
    (void)op;
#if DIRECT_THREADED
    SETUP_DT_ADDRESSES;
    return;
#else
    return;
#endif
  }
  
  g_use_firesuit = 1;
  g_access_violation = 0;
  getcontext(&g_firesuit);
  
  /* Ok, we jumped back here because something went south. */
  if(g_access_violation) {
    if(g_access_violation == 1) {
      cpu_raise_exception(state, c, 
        cpu_new_exception(state, c, state->global->exc_arg, 
            "Accessed outside bounds of object"));
    } else if(g_access_violation == 2) {
      cpu_raise_exception(state, c, 
        cpu_new_exception(state, c, state->global->exc_arg, 
            "Attempted to access field of non-reference (null pointer)")); 
    }
  }

#if DIRECT_THREADED
insn_start:
#endif

  while(c->active_context != Qnil) {
    
    /* This check I've commented out is a safety blanket. I've tested
       running my benchmark (compiling sirb.rb) and it's not hit. It's
       therefore just excess overhead per VM instruction, so it's not
       going to be run normally. */
       
    #if 0
    if(!c->data || c->ip >= c->data_size) {
      cpu_return_to_sender(state, c, FALSE);
      continue;
    }
    #endif

#if DIRECT_THREADED
    if(EXCESSIVE_TRACING) {
      printf("%-15s: => %p\n",
        rbs_symbol_to_cstring(state, cmethod_get_name(c->method)),
        (void*)*c->ip_ptr);
    }
    NEXT_OP;
    #include "instruction_dt.gen"
#else
    op = *c->ip_ptr++;
    // #undef stack_push
    // #define stack_push(obj) if(!cpu_stack_push(state, c, obj, TRUE)) { goto stack_error; }
    
    if(EXCESSIVE_TRACING) {
    cpu_flush_ip(c);
    cpu_flush_sp(c);
    printf("%-15s: OP: %s (%d/%lu/%lu)\n", 
      rbs_symbol_to_cstring(state, cmethod_get_name(c->method)),
      cpu_op_to_name(state, op), op, c->ip, c->sp);
    }
    
    #include "instructions.gen"
    
#endif

    goto check_interupts;
//stack_error:
    /* enlarge the stack to handle the exception */
    c->stack_top = realloc(c->stack_top, c->stack_size += 128);
    
    // printf("HEAP INFO: %d used, %d total.\n", state->om->gc->current->current - state->om->gc->current->address, state->om->gc->current->size);
    cpu_raise_exception(state, c, 
          cpu_new_exception(state, c, state->global->exc_stack_explosion,
          "Stack has exploded"));
check_interupts:
    // if (0 && !state->om->collect_now) object_memory_check_memory(state->om);
    if(state->om->collect_now) {
      int cm = state->om->collect_now;
      
      /* Collect the first generation. */
      if(cm & OMCollectYoung) {
        if(EXCESSIVE_TRACING) {
          printf("[[ Collecting young objects. ]]\n");
          printf("[[ method=%p, data=%p, ip_ptr=%p, ip=%lu, op=%d ]]\n", (void*)c->method, c->data, c->ip_ptr, c->ip, *c->ip_ptr);
        }
        state_collect(state, c);
        if(EXCESSIVE_TRACING) {
          printf("[[ method=%p, data=%p, ip_ptr=%p, ip=%lu, op=%d ]]\n", (void*)c->method, c->data, c->ip_ptr, c->ip, *c->ip_ptr);
          printf("[[ Finished collect. ]]\n");
        }
      }
      
      /* Collect the old generation. */
      if(cm & OMCollectMature) {
        if(EXCESSIVE_TRACING) {
          printf("[[ Collecting old objects. ]\n");
        }
        state_major_collect(state, c);
        // printf("Done with major collection.\n");
      }
      
      state->om->collect_now = 0;
    }
  }   
}

void cpu_run_script(STATE, cpu c, OBJECT meth) {
  OBJECT name;
  name = string_to_sym(state, string_new(state, "__script__"));
  cpu_goto_method(state, c, c->main, meth, 0, name, Qnil);
}
