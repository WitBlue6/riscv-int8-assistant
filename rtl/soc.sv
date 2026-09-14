module soc (
    input wire clk, input wire resetn,
    input wire host_we, input wire [15:0] host_word, input wire [31:0] host_data,
    output reg console_valid, output reg [7:0] console_data,
    output reg finished, output reg [31:0] exit_code,
    output wire trap,
    output wire accel_busy, output wire accel_done, output wire accel_error
);
    reg [31:0] ram[0:65535];
    string firmware_path;
    initial begin
        for (integer i=0;i<65536;i=i+1) ram[i]=0;
        if (!$value$plusargs("firmware=%s",firmware_path)) firmware_path="build/firmware.hex";
        $readmemh(firmware_path,ram);
    end
    wire valid, instr;
    wire [31:0] address, write_data;
    wire [3:0] strobe;
    wire [31:0] accel_rdata;
    wire is_ram = address < 32'h00040000;
    wire is_accel = address[31:16] == 16'h2000;
    wire [31:0] read_data = is_ram ? ram[address[17:2]] : (is_accel ? accel_rdata : 32'b0);
    picorv32 #(.PROGADDR_RESET(0),.STACKADDR(32'h0002fff0),
        .ENABLE_MUL(1),.ENABLE_FAST_MUL(1),.ENABLE_DIV(1),
        .ENABLE_COUNTERS(1),.ENABLE_COUNTERS64(1),.ENABLE_IRQ(0)) cpu (
        .clk(clk),.resetn(resetn),.trap(trap),
        .mem_valid(valid),.mem_instr(instr),.mem_ready(valid),
        .mem_addr(address),.mem_wdata(write_data),.mem_wstrb(strobe),.mem_rdata(read_data),
        .pcpi_wr(1'b0),.pcpi_rd(32'b0),.pcpi_wait(1'b0),.pcpi_ready(1'b0),.irq(32'b0)
    );
    int8_accel accelerator (
        .clk(clk),.resetn(resetn),.req(valid && is_accel),.addr(address[15:0]),
        .wdata(write_data),.wstrb(strobe),.rdata(accel_rdata),
        .busy(accel_busy),.done(accel_done),.error(accel_error)
    );
    always @(posedge clk) begin
        console_valid<=0;
        if (!resetn) begin finished<=0;exit_code<=0;console_data<=0;end
        if (host_we && !resetn) ram[host_word]<=host_data;
        if (resetn && valid && |strobe) begin
            if (is_ram)
                for (integer j=0;j<4;j=j+1) if(strobe[j]) ram[address[17:2]][8*j+:8]<=write_data[8*j+:8];
            if (address==32'h10000000) begin console_valid<=1;console_data<=write_data[7:0];end
            if (address==32'h10000004) begin finished<=1;exit_code<=write_data;end
        end
    end
endmodule
