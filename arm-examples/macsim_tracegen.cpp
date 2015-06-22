/*****************************************************************************\
* Qemu Simulation Framework (qsim)                                            *
* Qsim is a modified version of the Qemu emulator (www.qemu.org), couled     *
* a C++ API, for the use of computer architecture researchers.                *
*                                                                             *
* This work is licensed under the terms of the GNU GPL, version 2. See the    *
* COPYING file in the top-level directory.                                    *
\*****************************************************************************/
#include <iostream>
#include <fstream>
#include <iomanip>
#include <string>
#include <thread>

#include <qsim.h>
#include <stdio.h>
#include <capstone.h>

#include "gzstream.h"
#include "cs_disas.h"
#include "macsim_tracegen.h"

using Qsim::OSDomain;

using std::ostream;

class InstHandler {
public:
    InstHandler();
    InstHandler(ogzstream *outfile);
    void setOutFile(ogzstream *outfile);
    bool populateInstInfo(cs_insn *insn);
    bool populateMemInfo();
private:

    void init_uop_tables(void);

    trace_info_a64_s inst[2]; 
    bool inst_idx;
    ogzstream *outfile;
    int m_fp_uop_table[ARM64_INS_ENDING];
    int m_int_uop_table[ARM64_INS_ENDING];

    bool started;
};

InstHandler::InstHandler()
{
    inst_idx = 0;
    started = false;
}

void InstHandler::setOutFile(ogzstream *out)
{
    outfile = out;
}

bool InstHandler::populateInstInfo(cs_insn *insn)
{
    cs_arm64* arm64;
    trace_info_a64_s *op = &inst[inst_idx];
    trace_info_a64_s *prev_op = NULL;

    if (started)
        prev_op = &inst[!inst_idx];
    else
        started = true;

    if (insn->detail == NULL)
        return false;

    arm64 = &(insn->detail->arm64);

    op->m_num_read_regs = insn->detail->regs_read_count;
    op->m_num_dest_regs = insn->detail->regs_write_count;

    for (int i = 0; i < op->m_num_read_regs; i++)
        op->m_src[i] = insn->detail->regs_read[i];
    for (int i = 0; i < op->m_num_dest_regs; i++)
        op->m_dst[i] = insn->detail->regs_write[i];

    op->m_cf_type = 0;
    for (int grp_idx = 0; grp_idx < insn->detail->groups_count; grp_idx++) {
        if (insn->detail->groups[grp_idx] == ARM64_GRP_JUMP) {
            op->m_cf_type = 1;
            break;
        }
    }

    op->m_has_immediate = 0;
    for (int op_idx = 0; op_idx < arm64->op_count; op_idx++) {
        if (arm64->operands[op_idx].type == ARM64_OP_IMM ||
            arm64->operands[op_idx].type == ARM64_OP_CIMM) {
            op->m_has_immediate = 1;
            break;
        }
    }

    op->m_opcode = insn->id;
    op->m_has_st = 0;
    if (ARM64_INS_ST1 <= insn->id && insn->id <= ARM64_INS_STXR)
        op->m_has_st = 1;

    op->m_is_fp = 0;
    for (int op_idx = 0; op_idx < arm64->op_count; op_idx++) {
        if (arm64->operands[op_idx].type == ARM64_OP_FP) {
            op->m_is_fp = 1;
            break;
        }
    }

    op->m_write_flg  = arm64->writeback;

    // TODO: figure out based on opcode
    op->m_num_ld = 0;
    op->m_size = 4;

    // initialize current inst dynamic information
    op->m_ld_vaddr2 = 0;
    op->m_st_vaddr = 0;
    op->m_instruction_addr  = insn->address;
    
    op->m_branch_target = 0;
    int offset = 0;
    if (op->m_cf_type) {
        if (op->m_has_immediate)
            for (int op_idx = 0; op_idx < arm64->op_count; op_idx++) {
                if (arm64->operands[op_idx].type == ARM64_OP_IMM) {
                    offset = (int64_t) arm64->operands[op_idx].imm;
                    op->m_branch_target = op->m_instruction_addr + offset;
                    break;
                }
            }
    }

    op->m_mem_read_size = 0;
    op->m_mem_write_size = 0;
    op->m_rep_dir = 0;
    op->m_actually_taken = 0;

    // populate prev inst dynamic information
    if (prev_op) {
        if (op->m_instruction_addr == prev_op->m_branch_target)
            prev_op->m_actually_taken = 1;
        if (prev_op->m_cf_type)
            *outfile << " Taken " << prev_op->m_actually_taken << std::endl;
        else
            *outfile << std::endl;
    }

    *outfile << "IsBranch: " << (int)op->m_cf_type
             << " Offset:   " << std::setw(16) << std::hex << offset 
             << " Target:  " <<  std::setw(16) << std::hex << op->m_branch_target << " ";

    inst_idx = !inst_idx;
}

class TraceWriter {
public:
  TraceWriter(OSDomain &osd) :
    osd(osd), finished(false), dis(CS_ARCH_ARM64, CS_MODE_ARM)
  { 
    osd.set_app_start_cb(this, &TraceWriter::app_start_cb);
    trace_file_count = 0;
    finished = false;
  }

  bool hasFinished() { return finished; }

  int app_start_cb(int c) {
    static bool ran = false;
    if (!ran) {
      ran = true;
      osd.set_inst_cb(this, &TraceWriter::inst_cb);
      //osd.set_mem_cb(this, &TraceWriter::mem_cb);
      osd.set_app_end_cb(this, &TraceWriter::app_end_cb);
    }
    tracefile = new ogzstream(("trace_" + std::to_string(trace_file_count) + ".log.gz").c_str());
    inst_handle.setOutFile(tracefile);
    trace_file_count++;
    finished = false;

    return 0;
  }

  int app_end_cb(int c)
  {
      std::cout << "App end cb called" << std::endl;
      finished = true;

      if (tracefile) {
          tracefile->close();
          delete tracefile;
          tracefile = NULL;
          inst_handle.setOutFile(tracefile);
      }

      return 0;
  }

  void inst_cb(int c, uint64_t v, uint64_t p, uint8_t l, const uint8_t *b, 
               enum inst_type t)
  {
      cs_insn *insn;
      int count = dis.decode((unsigned char *)b, l, insn);
      insn[0].address = v;
      inst_handle.populateInstInfo(insn);
      if (tracefile) {
          for (int j = 0; j < count; j++) {
              *tracefile << std::hex << v <<
                  ": " << std::hex << insn[j].address <<
                  ": " << insn[j].mnemonic <<
                  ": " << insn[j].op_str;
                  //<< ": " << get_inst_string(t);
          }
      } else {
          std::cout << "Writing to a null tracefile" << std::endl;
      }
      dis.free_insn(insn, count);
      return;
  }

  int mem_cb(int c, uint64_t v, uint64_t p, uint8_t s, int w)
  {
      if (tracefile) {
          *tracefile << (w ? "Write: " : "Read: ")
                     << "v: 0x" << std::hex << v
                     << " p: 0x" << std::hex << p 
                     << " s: " << std::dec << (int)s
                     << " val: " << *(uint32_t *)p
                     << std::endl;
      }
  }

private:
  cs_disas dis;
  OSDomain &osd;
  ogzstream* tracefile;
  bool finished;
  int  trace_file_count;
  InstHandler inst_handle;

  static const char * itype_str[];
};

const char *TraceWriter::itype_str[] = {
  "QSIM_INST_NULL",
  "QSIM_INST_INTBASIC",
  "QSIM_INST_INTMUL",
  "QSIM_INST_INTDIV",
  "QSIM_INST_STACK",
  "QSIM_INST_BR",
  "QSIM_INST_CALL",
  "QSIM_INST_RET",
  "QSIM_INST_TRAP",
  "QSIM_INST_FPBASIC",
  "QSIM_INST_FPMUL",
  "QSIM_INST_FPDIV"
};

int main(int argc, char** argv) {
  using std::istringstream;
  using std::ofstream;

  unsigned n_cpus = 1;

  std::string qsim_prefix(getenv("QSIM_PREFIX"));

  // Read number of CPUs as a parameter. 
  if (argc >= 2) {
    istringstream s(argv[1]);
    s >> n_cpus;
  }

  OSDomain *osd_p(NULL);

  if (argc >= 4) {
    // Create new OSDomain from saved state.
    osd_p = new OSDomain(argv[3]);
    n_cpus = osd_p->get_n();
  } else {
    osd_p = new OSDomain(n_cpus, qsim_prefix + "/../arm64_images/vmlinuz");
  }
  OSDomain &osd(*osd_p);

  // Attach a TraceWriter if a trace file is given.
  TraceWriter tw(osd);

  // If this OSDomain was created from a saved state, the app start callback was
  // received prior to the state being saved.
  //if (argc >= 4) tw.app_start_cb(0);

  osd.connect_console(std::cout);

  //tw.app_start_cb(0);
  // The main loop: run until 'finished' is true.
  uint64_t inst_per_iter = 1000000000;
  int inst_run = inst_per_iter;
  while (!(inst_per_iter - inst_run)) {
    for (unsigned long j = 0; j < n_cpus; j++) {
        inst_run = osd.run(j, inst_per_iter);
    }
    osd.timer_interrupt();
  }

  delete osd_p;

  return 0;
}
