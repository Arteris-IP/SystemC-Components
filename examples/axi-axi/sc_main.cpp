/*
 * sc_main.cpp
 *
 *  Created on:
 *      Author: Rocco Jonack
 *  Description: Simple AXI example showing an initiator and a target
 *               communicating using an AXI socket based on TLM2 + AXI 
 *               specific extensions
 *               The communication shows the some basic transitions
 *               The dependencies are SystemC, TLM2, AXI extension libraries
 *               and the SystemC Components library. Optionally the SCV library
 *               to enable transaction tracing
 *               There are known limitations in the example
 *               - hard coded traffic
 *               - only one outstanding transaction
 */

#ifdef WITH_SCV
#include <axi/scv/recorder_modules.h>
#include <scv.h>
#include <scv/scv_tr.h>
#endif
#include "scc/report.h"
#include "tlm/tlm_id.h"
#include <array>
#include <axi/pe/simple_initiator.h>
#include <axi/pe/simple_target.h>
#include <tlm/tlm_mm.h>

using namespace axi;
using namespace axi::pe;
using namespace sc_core;

const unsigned SOCKET_WIDTH = 64;
const unsigned wData = SOCKET_WIDTH/8;
const unsigned AXI_SIZE = 5;
const unsigned MaxLength = 64;
const unsigned int NumberOfIterations = 10;
const unsigned int ClockPeriodNS = 10;
unsigned int addr = 0;
  
tlm::tlm_generic_payload* prepare_trans(unsigned int iteration) {
  auto trans = tlm::tlm_mm<>::get().allocate<axi::axi4_extension>(); 
  unsigned int len = ((iteration)*wData)%MaxLength;
  if (len==0) len=MaxLength;
  auto  ext = trans->get_extension<axi::axi4_extension>();
  trans->set_data_length(len);
  trans->set_streaming_width(len);
  if (iteration%2)
    trans->set_command(tlm::TLM_READ_COMMAND);
  else
    trans->set_command(tlm::TLM_WRITE_COMMAND);
  trans->set_address(addr);
  addr += len;
  trans->set_data_ptr(NULL);
  if (ext) {
    ext->set_size(AXI_SIZE);
    ext->set_id(axi::common::id_type::CTRL, 0);
    ext->set_length(len/wData);
    ext->set_burst(axi::burst_e::INCR);
  }
  else {
    SCCERR("testbench") << " no extension pointer?";
  };
  return trans;
}

class testbench : public sc_core::sc_module {
public:
    SC_HAS_PROCESS(testbench);
    sc_core::sc_time clk_period{ClockPeriodNS, sc_core::SC_NS};
    sc_core::sc_clock clk{"clk", clk_period, 0.5, sc_core::SC_ZERO_TIME, true};
    sc_core::sc_signal<bool> rst{"rst"};
    axi::axi_initiator_socket<SOCKET_WIDTH> intor{"intor"};
#ifdef WITH_SCV
    scv4axi::axi_recorder_module<SOCKET_WIDTH> intor_rec{"intor_rec"};
#endif
    axi::axi_target_socket<SOCKET_WIDTH> tgt{"tgt"};

    testbench(sc_core::sc_module_name nm)
    : sc_core::sc_module(nm)
    , intor_pe("intor_pe", intor)
    , tgt_pe("tgt_pe", tgt) {
        SC_THREAD(run);
        intor_pe.clk_i(clk);
        tgt_pe.clk_i(clk);
#ifdef WITH_SCV
        intor(intor_rec.tsckt);
        intor_rec.isckt(tgt);
#else
        intor(tgt);
#endif
    }

    void run() {
        std::array<uint8_t, MaxLength*8> data;
	rst.write(true);
	wait(10*ClockPeriodNS, SC_NS);
	rst.write(false);
	SCCDEBUG("testbench") << " out of reset";
	wait(ClockPeriodNS, SC_NS);
	for(auto i=1; i<NumberOfIterations; ++i) {
	  SCCDEBUG("testbench") << "executing transactions in iteration " << i;
	  auto trans = prepare_trans(i);
	  trans->acquire();
	  intor_pe.transport(*trans, false);
	  if(trans->get_response_status() != tlm::TLM_OK_RESPONSE)
	    SCCERR() << "Invalid response status" << trans->get_response_string();
	  trans->release();
	  SCCDEBUG("testbench") << " finished transaction for iteration " << i;
	};
	wait(10*ClockPeriodNS, SC_NS);
	sc_stop();
    }

private:
    axi::pe::simple_axi_initiator<SOCKET_WIDTH> intor_pe;
    axi::pe::simple_target<SOCKET_WIDTH> tgt_pe;
    unsigned id{0};
};

int sc_main(int argc, char* argv[]) {
    sc_report_handler::set_actions(SC_ID_MORE_THAN_ONE_SIGNAL_DRIVER_, SC_DO_NOTHING);
    scc::init_logging(
		      scc::LogConfig()
		      .logLevel(static_cast<scc::log>(7))
		      .logAsync(false)
		      .dontCreateBroker(true)
		      .coloredOutput(true));
    sc_report_handler::set_actions(SC_ERROR, SC_LOG | SC_CACHE_REPORT | SC_DISPLAY);
#ifdef WITH_SCV
    scv_startup();
    scv_tr_text_init();
    scv_tr_db* db = new scv_tr_db("axi_scc_test.txlog");
    scv_tr_db::set_default_db(db);
#endif
    testbench tb("tb");
    sc_core::sc_start(1_ms);
    SCCINFO() << "Finished";
#ifdef WITH_SCV
    delete db;
#endif
    return 0;
}
