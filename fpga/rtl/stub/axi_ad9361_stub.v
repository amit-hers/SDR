// Datapath-only stand-in for ADI's axi_ad9361, used ONLY by fpga/bd/validate_bd.tcl.
//
// The real core lives in the ADI HDL tree and is not in this repository, so a
// block design that instantiates it cannot be validated here. This stub carries
// the exact datapath port names, widths and directions taken from
// analogdevicesinc/hdl library/axi_ad9361/axi_ad9361.v, which is enough to
// check everything this project actually authors: the parallel-IQ adapters,
// the AXI-Lite and SmartConnect wiring, the clock and reset topology, the DMA
// attachment and the address map.
//
// It deliberately does NOT model the radio. It proves the design connects, not
// that it receives.
module axi_ad9361_stub (
    // The real core declares FREQ_HZ on this interface; without it Vivado
    // raises "FREQ_HZ bus parameter is missing for output clock interface"
    // and every downstream clock domain is left undefined. 8 MS/s is this
    // link's operating point and, importantly, is under the demodulator's
    // 44.41 MHz closure -- see the note in validate_bd.tcl.
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 l_clk CLK" *)
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 8000000" *)
    output wire        l_clk,
    input  wire        clk,
    output wire        rst,

    output wire        adc_enable_i0,
    output wire        adc_valid_i0,
    output wire [15:0] adc_data_i0,
    output wire        adc_enable_q0,
    output wire        adc_valid_q0,
    output wire [15:0] adc_data_q0,

    output wire        dac_enable_i0,
    output wire        dac_valid_i0,
    input  wire [15:0] dac_data_i0,
    output wire        dac_enable_q0,
    output wire        dac_valid_q0,
    input  wire [15:0] dac_data_q0
);
    assign l_clk         = clk;
    assign rst           = 1'b0;
    assign adc_enable_i0 = 1'b1;
    assign adc_enable_q0 = 1'b1;
    assign adc_valid_i0  = 1'b1;
    assign adc_valid_q0  = 1'b1;
    assign adc_data_i0   = 16'd0;
    assign adc_data_q0   = 16'd0;
    assign dac_enable_i0 = 1'b1;
    assign dac_enable_q0 = 1'b1;
    assign dac_valid_i0  = 1'b1;
    assign dac_valid_q0  = 1'b1;
endmodule
