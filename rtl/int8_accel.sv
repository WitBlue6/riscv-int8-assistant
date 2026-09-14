// Four signed INT8 MAC lanes, INT32 accumulator, memory-mapped local buffers.
module int8_accel (
    input wire clk, input wire resetn,
    input wire req, input wire [15:0] addr,
    input wire [31:0] wdata, input wire [3:0] wstrb,
    output reg [31:0] rdata,
    output reg busy, output reg done, output reg error
);
    reg signed [7:0] inputs [0:255];
    reg signed [7:0] weights [0:8191];
    reg signed [31:0] biases [0:31];
    reg signed [31:0] results [0:31];
    reg [31:0] n, m, relu_en, cycles, completed;
    reg [31:0] col, row;
    reg signed [31:0] accumulator;
    reg signed [31:0] lane_sum, next_acc;
    reg signed [15:0] product;
    integer lane, j, wi;

    always @* begin
        lane_sum = 0; product = 0;
        for (integer k=0; k<4; k=k+1) begin
            if (busy && col+k < n && row < m) begin
                product = $signed(inputs[col+k]) * $signed(weights[row*n+col+k]);
                lane_sum = lane_sum + {{16{product[15]}}, product};
            end
        end
        next_acc = accumulator + lane_sum;
        rdata = 0;
        case (addr)
            16'h0004: rdata = {29'b0,error,done,busy};
            16'h0008: rdata = n;
            16'h000c: rdata = m;
            16'h0010: rdata = relu_en;
            16'h0014: rdata = cycles;
            16'h0018: rdata = completed;
            default: begin
                if (addr >= 16'h5000 && addr < 16'h5080)
                    rdata = results[(addr-16'h5000)>>2];
            end
        endcase
    end

    always @(posedge clk) begin
        if (!resetn) begin
            busy <= 0; done <= 0; error <= 0;
            n <= 0; m <= 0; relu_en <= 0;
            row <= 0; col <= 0; accumulator <= 0;
            cycles <= 0; completed <= 0;
        end else begin
            if (busy) begin
                cycles <= cycles + 1;
                if (col+4 >= n) begin
                    if (relu_en[0])
                        results[row] <= next_acc < 0 ? 0 : (next_acc > 127 ? 127 : next_acc);
                    else results[row] <= next_acc;
                    col <= 0;
                    if (row+1 >= m) begin
                        busy <= 0; done <= 1; completed <= completed+1;
                    end else begin
                        row <= row+1; accumulator <= biases[row+1];
                    end
                end else begin
                    col <= col+4; accumulator <= next_acc;
                end
            end
            if (req && |wstrb) begin
                // Writes during execution never alter the active computation.
                if (busy) error <= 1;
                else if (addr < 16'h1000) begin
                    if (wstrb != 4'b1111) error <= 1;
                    else case (addr)
                        16'h0000: begin
                            if (wdata[1]) begin done<=0; error<=0; end
                            if (wdata[2]) completed<=0;
                            if (wdata[0]) begin
                                if (n==0 || n>256 || m==0 || m>32 || relu_en>1) error<=1;
                                else begin
                                    busy<=1; done<=0; cycles<=0;
                                    row<=0; col<=0; accumulator<=biases[0];
                                end
                            end
                        end
                        16'h0008: n<=wdata;
                        16'h000c: m<=wdata;
                        16'h0010: relu_en<=wdata;
                        default: error<=1;
                    endcase
                end else if (addr >= 16'h1000 && addr < 16'h1100) begin
                    for (j=0;j<4;j=j+1)
                        if (wstrb[j]) inputs[(addr-16'h1000 & 16'hfffc)+j] <= wdata[8*j+:8];
                end else if (addr >= 16'h2000 && addr < 16'h4000) begin
                    for (j=0;j<4;j=j+1)
                        if (wstrb[j]) weights[(addr-16'h2000 & 16'hfffc)+j] <= wdata[8*j+:8];
                end else if (addr >= 16'h4000 && addr < 16'h4080) begin
                    for (j=0;j<4;j=j+1)
                        if (wstrb[j]) biases[(addr-16'h4000)>>2][8*j+:8] <= wdata[8*j+:8];
                end else error<=1;
            end
        end
    end
endmodule
