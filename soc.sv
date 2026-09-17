module soc (
    input wire clk, input wire resetn,
    input wire host_we, input wire [15:0] host_word, input wire [31:0] host_data,
    output reg console_valid, output reg [7:0] console_data,
    output reg finished, output reg [31:0] exit_code,
    output wire trap,
    output wire accel_busy, output wire accel_done, output wire accel_error
);
    reg [31:0] ram[0:65535];
    // External read-only weight memory model: 1 MiB, configurable request latency.
    // This models transactions, not a physical DDR controller.
    reg [31:0] external_mem[0:262143];
    string external_path;
    integer ext_latency;
    reg ext_pending;
    reg [31:0] ext_wait, external_reads;
    string firmware_path;
    initial begin
        for (integer i=0;i<65536;i=i+1) ram[i]=0;
        if (!$value$plusargs("firmware=%s",firmware_path)) firmware_path="build/firmware.hex";
        $readmemh(firmware_path,ram);
        for(integer i=0;i<262144;i=i+1)external_mem[i]=0;
        if(!$value$plusargs("weights=%s",external_path))external_path="external.hex";
        $readmemh(external_path,external_mem);
        if(!$value$plusargs("ext_latency=%d",ext_latency))ext_latency=3;
        if(ext_latency<0 || ext_latency>100)$fatal(1,"bad external latency");
    end
    wire valid, instr;
    wire [31:0] address, write_data;
    wire [3:0] strobe;
    wire [31:0] accel_rdata;
    wire is_ram = address < 32'h00040000;
    wire is_accel = address[31:16] == 16'h2000;
    wire is_attention = address[31:16] == 16'h2100;
    wire is_external = address>=32'h01000000 && address<32'h01100000;
    wire [31:0] attention_rdata;
    wire ready = valid && (!is_external || (ext_pending && ext_wait==0));
    wire [31:0] read_data = is_ram ? ram[address[17:2]] :
        is_accel ? accel_rdata : is_attention ? attention_rdata :
        is_external ? external_mem[address[19:2]] : address==32'h10000008 ? external_reads : 0;
    picorv32 #(.PROGADDR_RESET(0),.STACKADDR(32'h0002fff0),
        .ENABLE_MUL(1),.ENABLE_FAST_MUL(1),.ENABLE_DIV(1),
        .ENABLE_COUNTERS(1),.ENABLE_COUNTERS64(1),.ENABLE_IRQ(0)) cpu (
        .clk(clk),.resetn(resetn),.trap(trap),
        .mem_valid(valid),.mem_instr(instr),.mem_ready(ready),
        .mem_addr(address),.mem_wdata(write_data),.mem_wstrb(strobe),.mem_rdata(read_data),
        .pcpi_wr(1'b0),.pcpi_rd(32'b0),.pcpi_wait(1'b0),.pcpi_ready(1'b0),.irq(32'b0)
    );
    tiled_fc accelerator (
        .clk(clk),.resetn(resetn),.req(valid && ready && is_accel),.addr(address[15:0]),
        .wdata(write_data),.wstrb(strobe),.rdata(accel_rdata),
        .busy(accel_busy),.done(accel_done),.error(accel_error)
    );
    attention attention_unit (
        .clk(clk),.resetn(resetn),.req(valid && ready && is_attention),.addr(address[15:0]),
        .wdata(write_data),.wstrb(strobe),.rdata(attention_rdata),
        .busy(),.done(),.error()
    );
    always @(posedge clk) begin
        console_valid<=0;
        if (!resetn) begin
            finished<=0;exit_code<=0;console_data<=0;
            ext_pending<=0;ext_wait<=0;external_reads<=0;
        end else begin
            if(!ext_pending && valid && is_external)begin ext_pending<=1;ext_wait<=ext_latency;end
            else if(ext_pending)begin
                if(ext_wait!=0)ext_wait<=ext_wait-1;
                else if(valid)begin ext_pending<=0;external_reads<=external_reads+1;end
            end
        end
        if (host_we && !resetn) ram[host_word]<=host_data;
        if (resetn && valid && ready && |strobe) begin
            if (is_ram)
                for (integer j=0;j<4;j=j+1) if(strobe[j]) ram[address[17:2]][8*j+:8]<=write_data[8*j+:8];
            if (address==32'h10000000) begin console_valid<=1;console_data<=write_data[7:0];end
            if (address==32'h10000004) begin finished<=1;exit_code<=write_data;end
        end
    end
endmodule
