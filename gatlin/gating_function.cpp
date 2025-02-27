/*
 * Gating Functions
 * 2018 Tong Zhang <t.zhang2@partner.samsung.com>
 */

#include "color.h"
#include "gating_function_base.h"

#include "llvm/IR/CallSite.h"
#include "llvm/Support/raw_ostream.h"

#include "utility.h"

#include <fstream>

void GatingFunctionBase::dump_interesting(InstructionSet *cis) {
  for (auto *ci : *cis) {
    CallInst *cs = dyn_cast<CallInst>(ci);
    Function *cf = get_callee_function_direct(cs);
    if (is_gating_function(cf)) {
      errs() << "    " << cf->getName() << " @ ";
      cs->getDebugLoc().print(errs());
      errs() << "\n";
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// GatingCap

void GatingCap::load_cap_func_list(std::string &file) {
  std::ifstream input(file);
  if (!input.is_open()) {
    // TODO:load builtin list into cap_func_name2cap_arg_pos
    for (int i = 0; i < BUILTIN_CAP_FUNC_LIST_SIZE; i++) {
      const struct str2int *p = &_builtin_cap_functions[i];
      cap_func_name2cap_arg_pos[p->k] = p->v;
    }
    return;
  }
  std::string line;
  while (std::getline(input, line)) {
    std::size_t found = line.find(" ");
    assert(found != std::string::npos);
    std::string name = line.substr(0, found);
    int pos = stoi(line.substr(found + 1));
    cap_func_name2cap_arg_pos[name] = pos;
  }
  input.close();
  errs() << "Load CAP FUNC list, total:" << cap_func_name2cap_arg_pos.size()
         << "\n";
}

bool GatingCap::is_builtin_gatlin_function(const std::string &str) {
  for (int i = 0; i < BUILTIN_CAP_FUNC_LIST_SIZE; i++) {
    const struct str2int *p = &_builtin_cap_functions[i];
    if (p->k == str)
      return true;
  }
  return false;
}

GatingCap::GatingCap(Module &module, std::string &capfile)
    : GatingFunctionBase(module) {
  errs() << "Gating Function Type: capability\n";
  load_cap_func_list(capfile);
  // add capable and ns_capable to chk_func_cap_position so that we can use them
  for (Module::iterator fi = module.begin(), f_end = module.end(); fi != f_end;
       ++fi) {
    Function *func = dyn_cast<Function>(fi);
    StringRef fname = func->getName();

    if (cap_func_name2cap_arg_pos.find(fname) !=
        cap_func_name2cap_arg_pos.end()) {
      chk_func_cap_position[func] = cap_func_name2cap_arg_pos[fname];
      if (chk_func_cap_position.size() == cap_func_name2cap_arg_pos.size())
        break; // we are done here
    }
  }

  // last time discovered functions
  FunctionData pass_data;
  // functions that will be used for discovery next time
  FunctionData pass_data_next;

  for (auto fpair : chk_func_cap_position)
    pass_data[fpair.first] = fpair.second;

  /*
   * First round:
   * discover inner permission check functions and add them all as basic
   * permission check functions
   */
  for (auto fpair : pass_data) {
    Function *f = fpair.first;
    int cap_pos = fpair.second;
    assert(cap_pos >= 0);
    // discover call instructions which use the parameter directly
    for (Function::iterator fi = f->begin(), fe = f->end(); fi != fe; ++fi) {
      BasicBlock *bb = dyn_cast<BasicBlock>(fi);
      for (BasicBlock::iterator ii = bb->begin(), ie = bb->end(); ii != ie;
           ++ii) {
        CallInst *ci = dyn_cast<CallInst>(ii);
        if (!ci)
          continue;
        // we are expecting a direct call
        Function *child = get_callee_function_direct(ci);
        if (!child)
          continue;
        StringRef fname = child->getName();
        // dont bother if this belongs to skip function
        if (skip_funcs->exists_ignore_dot_number(fname) ||
            kernel_api->exists_ignore_dot_number(fname))
          continue;

        // for each of the function argument
        for (unsigned int i = 0; i < ci->getNumArgOperands(); ++i) {
          Value *capv = ci->getArgOperand(i);
          int pos = use_parent_func_arg(capv, f);
          if (pos == cap_pos) {
            pass_data_next[child] = i;
            break;
          }
        }
      }
    }
  }
  // merge result of first round
  if (pass_data_next.size()) {
    errs() << ANSI_COLOR(BG_WHITE, FG_RED)
           << "Inner checking functions:" << ANSI_COLOR_RESET << "\n";
    for (auto fpair : pass_data_next) {
      Function *f = fpair.first;
      int cap_pos = fpair.second;
      errs() << "    - " << f->getName() << " @ " << cap_pos << "\n";
      pass_data[f] = cap_pos;
    }
    pass_data_next.clear();
  }
  // backward discovery and mark..
again:
  for (auto fpair : pass_data) {
    Function *func = fpair.first;
    int cap_pos = fpair.second;
    assert(cap_pos >= 0);
    // we got a capability check function or a wrapper function,
    // find all use without Constant Value and add them to wrapper
    for (auto u : func->users()) {
      InstructionSet uis;
      if (Instruction *i = dyn_cast<Instruction>(u))
        uis.insert(i);
      else
        uis = get_user_instruction(dyn_cast<Value>(u));
      if (uis.size() == 0) {
        u->print(errs());
        errs() << "\n";
        continue;
      }
      for (auto ui : uis) {
        CallInst *cs = dyn_cast<CallInst>(ui);
        if (cs == NULL)
          continue; // how come?
        Function *callee = get_callee_function_direct(cs);
        if (callee != func)
          continue;
        // assert(cs->getCalledFunction()==func);
        if (cap_pos >= (int)cs->getNumArgOperands()) {
          func->print(errs());
          llvm_unreachable(ANSI_COLOR_RED
                           "Check capability parameter" ANSI_COLOR_RESET);
        }
        Value *capv = cs->getArgOperand(cap_pos);
        if (isa<ConstantInt>(capv))
          continue;
        Function *parent_func = cs->getFunction();
        // we have a wrapper,
        int pos = use_parent_func_arg(capv, parent_func);
        if (pos >= 0) {
          // type 1 wrapper, cap is from parent function argument
          pass_data_next[parent_func] = pos;
        } else {
          // type 2 wrapper, cap is from inside this function
          // what to do with this?
          // llvm_unreachable("What??");
        }
      }
    }
  }
  // put pass_data_next in pass_data and chk_func_cap_position
  pass_data.clear();
  for (auto fpair : pass_data_next) {
    Function *f = fpair.first;
    int pos = fpair.second;
    if (chk_func_cap_position.count(f) == 0) {
      pass_data[f] = pos;
      chk_func_cap_position[f] = pos;
    }
  }
  // do this until we discovered everything
  if (pass_data.size())
    goto again;
}

bool GatingCap::is_gating_function(Function *f) {
  return chk_func_cap_position.find(f) != chk_func_cap_position.end();
}

bool GatingCap::is_gating_function(std::string &str) {
  for (auto &f2p : chk_func_cap_position) {
    if (f2p.first->getName() == str)
      return true;
  }
  return false;
}

void GatingCap::dump() {
  errs() << ANSI_COLOR(BG_BLUE, FG_WHITE)
         << "=chk functions and wrappers (total:"
         << chk_func_cap_position.size() << ")=" << ANSI_COLOR_RESET << "\n";
  for (auto &f2p : chk_func_cap_position) {
    errs() << ". " << f2p.first->getName() << "  @ " << f2p.second << "\n";
  }
  errs() << "=o=\n";
}

void GatingCap::dump_interesting(InstructionSet *cis) {
  int last_cap_no = -1;
  bool mismatched_chk_func = false;
  Function *last_cap_chk_func = NULL;
  for (auto *ci : *cis) {
    CallInst *cs = dyn_cast<CallInst>(ci);
    Function *cf = get_callee_function_direct(cs);
    int cap_no = -1;
    if (is_gating_function(cf)) {
      cap_no = chk_func_cap_position[cf];
      Value *capv = cs->getArgOperand(cap_no);
      if (!isa<ConstantInt>(capv)) {
        cs->getDebugLoc().print(errs());
        errs() << "\n";
        cs->print(errs());
        errs() << "\n";
        // llvm_unreachable("expect ConstantInt in capable");
        errs() << "Dynamic Load CAP\n";
        cs->getDebugLoc().print(errs());
        errs() << "\n";
        continue;
      }
      cap_no = dyn_cast<ConstantInt>(capv)->getSExtValue();
      if (last_cap_no == -1) {
        last_cap_no = cap_no;
        last_cap_chk_func = cf;
      }
      if (last_cap_no != cap_no)
        last_cap_no = -2;
      if (last_cap_chk_func != cf)
        mismatched_chk_func = true;
    }
    if (!((cap_no >= CAP_CHOWN) && (cap_no <= CAP_LAST_CAP))) {
      cs->print(errs());
      errs() << "\n";
      errs() << "cap_no=" << cap_no << "\n";
      cs->getDebugLoc().print(errs());
      errs() << "\n";
      // assert((cap_no>=CAP_CHOWN) && (cap_no<=CAP_LAST_CAP));
      continue;
    }
    errs() << "    " << cap2string[cap_no] << " @ " << cf->getName() << " ";
    cs->getDebugLoc().print(errs());
    errs() << "\n";
  }
  if ((last_cap_no == -2) || (mismatched_chk_func))
    errs() << ANSI_COLOR_RED << "inconsistent check" << ANSI_COLOR_RESET
           << "\n";
}

////////////////////////////////////////////////////////////////////////////////
// LSM

void GatingLSM::load_lsm_hook_list(std::string &file) {
  std::ifstream input(file);
  if (!input.is_open())
    return;
  std::string line;
  while (std::getline(input, line))
    lsm_hook_names.insert(line);
  input.close();
  errs() << "Load LSM hook list, total:" << lsm_hook_names.size() << "\n";
}

bool GatingLSM::is_lsm_hook(StringRef &str) {
  if (lsm_hook_names.size()) {
    return lsm_hook_names.find(str) != lsm_hook_names.end();
  }
  // use builtin name
  if (str.startswith("security_"))
    return true;
  return false;
}

void GatingLSM::init_reachable() {
  for (auto elem : lsm_hook_names)
    {
        std:string hook_name = elem;
        reachable_hooks[hook_name] = {0,0,0,0,0,0,0,0};
	      reachable_hooks_indiv[hook_name] = new Str2Int;
    }
}

// void GatingLSM::add_reachable(StringRef hook, Function *callee) {
//   std:string hook_name = hook;
//   errs() << "<!> REACHABLE: " << callee->getName() << " <-> " << hook << "\n";
//   // attribute hooks functions reachable by a wrapper to all wrapped functions
//   if (wrapper_mode) {
//     for (auto f : lsm_wrapper_functions) {
//       if (f->getName() == callee->getName()) {
//         add_reachable_from_wrapper(hook, callee);
//         return;
//       }
//     }
//     Str2Int *hook_mappings = ((Str2Int *)reachable_hooks_indiv[callee->getName()]);
//     (*hook_mappings)[hook_name] = 1;
//   } else {
//     reachable_hooks[hook_name] = 1;
//   }
// }

void GatingLSM::add_reachable(StringRef lsmhook, StringRef audhook) {
  std::string lsm_name = lsmhook;
  std::string aud_name = audhook;
  // errs() << "<!> REACHABLE: " << lsm_name << " <-> " << aud_name << "\n";
  // attribute hooks functions reachable by a wrapper to all wrapped functions

  reachable_hooks[lsm_name][0] = 1;

  if (aud_name == "audit_log_task_context") {
    reachable_hooks[lsm_name][1] = 1;
  }
  else if (aud_name == "audit_log_task_info") {
    reachable_hooks[lsm_name][2] = 1;
  }
  else if (aud_name == "audit_log_secctx") {
    reachable_hooks[lsm_name][3] = 1;
  }
  else if (aud_name == "audit_log_start") {
    reachable_hooks[lsm_name][4] = 1;
  }
  else if (aud_name == "audit_log_end") {
    reachable_hooks[lsm_name][5] = 1;
  }
  else if (aud_name == "audit_log_format") {
    reachable_hooks[lsm_name][6] = 1;
  }
  else if (aud_name == "audit_log") {
    reachable_hooks[lsm_name][7] = 1;
  } 
}

// void GatingLSM::add_reachable_from_wrapper(StringRef hook, Function *callee) {
//   std:string hook_name = hook;
//   if (lsm_wrapper_functions.find(callee) != lsm_wrapper_functions.end()) {
//     FunctionSet fset = *((FunctionSet*)(lsm_wrapper_to_hook[callee]));
//     for (auto f : fset) {
//       // extend reachability to each individual LSM hook that's wrapped
//       Str2Int *hook_mappings = ((Str2Int *)reachable_hooks_indiv[f->getName()]);
//       (*hook_mappings)[hook_name] = 1;
//     }
//   }
// }

FunctionSet GatingLSM::get_hook_from_wrapper(Function* f){
  FunctionSet fset = *((FunctionSet*)(lsm_wrapper_to_hook[f]));
  return fset;
}

void GatingLSM::dump_reachable(std::string& filename) {
  ofstream mapfile;
  mapfile.open (filename);
  if (wrapper_mode) {
    mapfile << "LSM Hook, reachable, task_context, task_info, secctx, start, end, format, log\n";
    for (auto x : reachable_hooks){
      mapfile << x.first << "," << x.second[0] << "," << x.second[1] << "," <<x.second[2] << "," <<x.second[3] << "," <<x.second[4] << "," <<x.second[5] << "," <<x.second[6] << "," <<x.second[7] <<  "\n";
    }
  }
  mapfile.close();
}

GatingLSM::GatingLSM(Module &module, std::string &lsmfile, int wrapper) 
    : GatingLSM(module, lsmfile) {
  wrapper_mode = 1;
}

GatingLSM::GatingLSM(Module &module, std::string &lsmfile)
    : GatingFunctionBase(module) {
  errs() << "Gating Function Type: LSM\n";
  load_lsm_hook_list(lsmfile);
  for (Module::iterator fi = module.begin(), f_end = module.end(); fi != f_end;
       ++fi) {
    Function *func = dyn_cast<Function>(fi);
    StringRef fname = func->getName();
    if (is_lsm_hook(fname)) {
      lsm_hook_functions.insert(func);
    }
  }

  init_reachable();
  // also try to discover wrapper function for LSM hooks
  FunctionSet wrappers;
  int loop_cnt = 0;
again:
  for (auto *lsmh : lsm_hook_functions) {
    errs() << " lsmh - " << lsmh->getName() << "\n";
    for (auto *u : lsmh->users()) {
      // should be call instruction and the callee is dacf
      InstructionSet uis;
      if (Instruction *i = dyn_cast<Instruction>(u))
        uis.insert(i);
      else
        uis = get_user_instruction(dyn_cast<Value>(u));
      if (uis.size() == 0) {
        u->print(errs());
        errs() << "\n";
        continue;
      }
      for (auto ui : uis) {
        CallInst *ci = dyn_cast<CallInst>(ui);
        if (!ci)
          continue;
        Function *callee = get_callee_function_direct(ci);
        if (callee != lsmh)
          continue;
        Function *userf = ci->getFunction();
	errs() << "WRAPPER --- \n";
        errs() << callee->getName() << " - used by - " << userf->getName() << "\n";
        // parameters comes from wrapper's parameter?
        for (unsigned int i = 0; i < ci->getNumOperands(); i++) {
          Value *a = ci->getOperand(i);
          if (use_parent_func_arg_deep(a, userf) >= 0) {
            errs() << " --- INSERTING WRAPPER --- " << "\n";
            wrappers.insert(userf);
            if(lsm_wrapper_to_hook[userf]){
              lsm_wrapper_to_hook[userf]->insert(lsmh);
            } else {
              lsm_wrapper_to_hook[userf] =  new FunctionSet;
              lsm_wrapper_to_hook[userf]->insert(lsmh);
            }     
            break;
          }
        }
      }
    }
  }

  if (wrappers.size()) {
    for (auto *wf : wrappers)
      lsm_wrapper_functions.insert(wf);
    wrappers.clear();
    loop_cnt++;
    if (loop_cnt < 1)
      goto again;
  }

  errs() << " ----- LSM WRAPPERS ------ " << "\n";
  for (auto fi : lsm_wrapper_functions){
    errs() << fi->getName() << "\n";
  }
}

bool GatingLSM::is_gating_function(Function *f) {
  return (lsm_hook_functions.find(f) != lsm_hook_functions.end());
}

bool GatingLSM::is_gating_function(std::string &str) {
  for (auto f : lsm_hook_functions) {
    if (f->getName() == str)
      return true;
  }
  return false;
}

bool GatingLSM::is_wrapper_function(std::string &str) {
  for (auto f : lsm_wrapper_functions) {
    if (f->getName() == str)
      return true;
  }
  return false;
}

bool GatingLSM::is_wrapper_function(Function *f) {
  // errs() << "IS WRAPPER CALLED " << f->getName() << "\n";
  // errs() << "return: " << (lsm_wrapper_functions.find(f) != lsm_wrapper_functions.end()) << "\n";
  return lsm_wrapper_functions.find(f) != lsm_wrapper_functions.end();
}

void GatingLSM::dump() {
  errs() << ANSI_COLOR(BG_BLUE, FG_WHITE)
         << "=LSM hook functions (total:" << lsm_hook_functions.size()
         << ")=" << ANSI_COLOR_RESET << "\n";
  for (auto f : lsm_hook_functions) {
    errs() << ". " << f->getName() << "\n";
  }
  errs() << "=o=\n";
}

////////////////////////////////////////////////////////////////////////////////
// DAC
GatingDAC::GatingDAC(Module &module) : GatingFunctionBase(module) {
  errs() << "Gating Function Type: DAC\n";
  for (Module::iterator fi = module.begin(), f_end = module.end(); fi != f_end;
       ++fi) {
    Function *func = dyn_cast<Function>(fi);
    StringRef fname = func->getName();
    if ((fname == "posix_acl_permission") || (fname == "check_acl") ||
        (fname == "acl_permission_check") || (fname == "generic_permission") ||
        (fname == "sb_permission") || (fname == "inode_permission")) {
      dac_functions.insert(func);
    }
    if (dac_functions.size() == 6)
      break; // we are done here
  }
#if 1
  // discover wrapper
  // for all user of dac function, find whether the parameter comes from
  // out layer wrapper parameter?
  // inline function may cause problem here...
  FunctionSet wrappers;
  int loop_cnt = 0;
again:
  for (auto *dacf : dac_functions) {
    errs() << " dacf - " << dacf->getName() << "\n";
    for (auto *u : dacf->users()) {
      // should be call instruction and the callee is dacf
      InstructionSet uis;
      if (Instruction *i = dyn_cast<Instruction>(u))
        uis.insert(i);
      else
        uis = get_user_instruction(dyn_cast<Value>(u));
      if (uis.size() == 0) {
        u->print(errs());
        errs() << "\n";
        continue;
      }
      for (auto ui : uis) {
        CallInst *ci = dyn_cast<CallInst>(ui);
        if (!ci)
          continue;
        Function *callee = get_callee_function_direct(ci);
        if (callee != dacf)
          continue;
        Function *userf = ci->getFunction();
        errs() << "    used by - " << userf->getName() << "\n";
        // parameters comes from wrapper's parameter?
        for (unsigned int i = 0; i < ci->getNumOperands(); i++) {
          Value *a = ci->getOperand(i);
          if (use_parent_func_arg_deep(a, userf) >= 0) {
            wrappers.insert(userf);
            break;
          }
        }
      }
    }
  }
  if (wrappers.size()) {
    for (auto *wf : wrappers)
      dac_functions.insert(wf);
    wrappers.clear();
    loop_cnt++;
    if (loop_cnt < 1)
      goto again;
  }
#endif
}

bool GatingDAC::is_gating_function(Function *f) {
  return dac_functions.find(f) != dac_functions.end();
}

bool GatingDAC::is_gating_function(std::string &str) {
  for (auto &f : dac_functions) {
    if (f->getName() == str)
      return true;
  }
  return false;
}

void GatingDAC::dump() {
  errs() << ANSI_COLOR(BG_BLUE, FG_WHITE)
         << "=chk functions and wrappers (total:" << dac_functions.size()
         << ")=" << ANSI_COLOR_RESET << "\n";
  for (auto &f : dac_functions) {
    errs() << ". " << f->getName() << "\n";
  }
  errs() << "=o=\n";
}

////////////////////////////////////////////////////////////////////////////////
// Audit

void GatingAudit::load_audit_hook_list(std::string &file) {
  std::ifstream input(file);
  if (!input.is_open())
    return;
  std::string line;
  while (std::getline(input, line))
    audit_hook_names.insert(line);
  input.close();
  errs() << "Load Audit hook list, total:" << audit_hook_names.size() << "\n";
}

bool GatingAudit::is_audit_hook(StringRef &str) {
  if (audit_hook_names.size()) {
    return audit_hook_names.find(str) != audit_hook_names.end();
  }
  // use builtin name
  // if (str.startswith("audit_"))
  //   return true;
  return false;
}

GatingAudit::GatingAudit(Module &module, std::string &auditfile)
    : GatingFunctionBase(module) {
  errs() << "Gating Function Type: Audit\n";
  load_audit_hook_list(auditfile);
  for (Module::iterator fi = module.begin(), f_end = module.end(); fi != f_end;
       ++fi) {
    Function *func = dyn_cast<Function>(fi);
    StringRef fname = func->getName();
    if (is_audit_hook(fname)) {
      audit_hook_functions.insert(func);
    }
  }

  // NOT ADDING AUDIT WRAPPERS AS OF NOW

  // also try to discover wrapper function for audit hooks
//   FunctionSet wrappers;
//   int loop_cnt = 0;
// again:
//   for (auto *audith : audit_hook_functions) {
//     errs() << " audith - " << audith->getName() << "\n";
//     for (auto *u : audith->users()) {
//       // should be call instruction and the callee is dacf
//       InstructionSet uis;
//       if (Instruction *i = dyn_cast<Instruction>(u))
//         uis.insert(i);
//       else
//         uis = get_user_instruction(dyn_cast<Value>(u));
//       if (uis.size() == 0) {
//         u->print(errs());
//         errs() << "\n";
//         continue;
//       }
//       for (auto ui : uis) {
//         CallInst *ci = dyn_cast<CallInst>(ui);
//         if (!ci)
//           continue;
//         Function *callee = get_callee_function_direct(ci);
//         if (callee != audith)
//           continue;
//         Function *userf = ci->getFunction();
//         errs() << "    used by - " << userf->getName() << "\n";
//         // parameters comes from wrapper's parameter?
//         for (unsigned int i = 0; i < ci->getNumOperands(); i++) {
//           Value *a = ci->getOperand(i);
//           if (use_parent_func_arg_deep(a, userf) >= 0) {
//             wrappers.insert(userf);
//             break;
//           }
//         }
//       }
//     }
//   }
//   if (wrappers.size()) {
//     for (auto *wf : wrappers)
//       audit_hook_functions.insert(wf);
//     wrappers.clear();
//     loop_cnt++;
//     if (loop_cnt < 1)
//       goto again;
//   }

}

bool GatingAudit::is_gating_function(Function *f) {
  return audit_hook_functions.find(f) != audit_hook_functions.end();
}

bool GatingAudit::is_gating_function(std::string &str) {
  for (auto f : audit_hook_functions) {
    if (f->getName() == str)
      return true;
  }
  return false;
}

void GatingAudit::dump() {
  errs() << ANSI_COLOR(BG_BLUE, FG_WHITE)
         << "=Audit hook functions (total:" << audit_hook_functions.size()
         << ")=" << ANSI_COLOR_RESET << "\n";
  for (auto f : audit_hook_functions) {
    errs() << ". " << f->getName() << "\n";
  }
  errs() << "=o=\n";
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// Sycalls 


bool GatingSyscall::is_syscall_func(StringRef &str) {
  if (str.startswith("sys_") || str.startswith("SyS_")){
    return true;
  }
  return false;
}

GatingSyscall::GatingSyscall(Module &module)
    : GatingFunctionBase(module) {
  errs() << "Gating Function Type: Sycall\n";
  for (Module::iterator fi = module.begin(), f_end = module.end(); fi != f_end;
       ++fi) {
    Function *func = dyn_cast<Function>(fi);
    StringRef fname = func->getName();
    if (is_syscall_func(fname)) {
      syscall_functions.insert(func);
    }
  }
}

bool GatingSyscall::is_gating_function(Function *f) {
  return syscall_functions.find(f) != syscall_functions.end();
}

bool GatingSyscall::is_gating_function(std::string &str) {
  for (auto f : syscall_functions) {
    if (f->getName() == str)
      return true;
  }
  return false;
}

void GatingSyscall::dump() {
  errs() << ANSI_COLOR(BG_BLUE, FG_WHITE)
         << "=Syscall functions (total:" << syscall_functions.size()
         << ")=" << ANSI_COLOR_RESET << "\n";
  for (auto f : syscall_functions) {
    errs() << ". " << f->getName() << "\n";
  }
  errs() << "=o=\n";
}