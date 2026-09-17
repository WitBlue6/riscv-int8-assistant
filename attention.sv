// Single-head causal attention datapath. CPU performs softmax/normalization.
// Append K/V once per token, then QK and AV commands reuse hardware-resident history.
module attention (
    input wire clk, resetn, req,
    input wire [15:0] addr,
    input wire [31:0] wdata,
    input wire [3:0] wstrb,
    output reg [31:0] rdata,
    output reg busy, done, error
);
    reg signed [7:0] query[0:63], new_k[0:63], new_v[0:63];
    reg signed [7:0] keys[0:2047], values[0:2047];
    reg [15:0] probability[0:31];
    reg signed [31:0] scores[0:31], sums[0:63];
    reg [31:0] dim, count, cycles, completed;
    reg [31:0] row, col;
    reg [1:0] op; // 1 append, 2 QK, 3 AV
    reg signed [31:0] acc, lane_sum, next_acc;
    reg signed [15:0] dot_product;
    reg signed [24:0] weighted_product;
    integer j;
    always @* begin
        lane_sum=0; dot_product=0; weighted_product=0;
        for(integer k=0;k<4;k=k+1) begin
            if(busy && op==2 && col+k<dim) begin
                dot_product=$signed(query[col+k])*$signed(keys[row*64+col+k]);
                lane_sum=lane_sum+{{16{dot_product[15]}},dot_product};
            end
            if(busy && op==3 && col+k<count) begin
                // Unsigned 16-bit probability is zero-extended before signed multiply.
                weighted_product=$signed({1'b0,probability[col+k]})*$signed(values[(col+k)*64+row]);
                lane_sum=lane_sum+{{7{weighted_product[24]}},weighted_product};
            end
        end
        next_acc=acc+lane_sum;
        rdata=0;
        case(addr)
            16'h0004:rdata={29'b0,error,done,busy};
            16'h0008:rdata=dim;
            16'h000c:rdata=count;
            16'h0010:rdata=cycles;
            16'h0014:rdata=completed;
            default:begin
                if(addr>=16'h2000 && addr<16'h2080)rdata=scores[(addr-16'h2000)>>2];
                if(addr>=16'h3000 && addr<16'h3100)rdata=sums[(addr-16'h3000)>>2];
            end
        endcase
    end
    always @(posedge clk) begin
        if(!resetn)begin
            busy<=0;done<=0;error<=0;dim<=0;count<=0;cycles<=0;completed<=0;
            row<=0;col<=0;op<=0;acc<=0;
        end else begin
            if(busy)begin
                cycles<=cycles+1;
                if(op==1)begin
                    for(integer k=0;k<4;k=k+1)if(col+k<dim)begin
                        keys[count*64+col+k]<=new_k[col+k];
                        values[count*64+col+k]<=new_v[col+k];
                    end
                    if(col+4>=dim)begin count<=count+1;busy<=0;done<=1;completed<=completed+1;end
                    else col<=col+4;
                end else if(col+4 >= (op==2 ? dim : count))begin
                    if(op==2)scores[row]<=next_acc;else sums[row]<=next_acc;
                    col<=0;acc<=0;
                    if(row+1 >= (op==2 ? count : dim))begin busy<=0;done<=1;completed<=completed+1;end
                    else row<=row+1;
                end else begin col<=col+4;acc<=next_acc;end
            end
            if(req && |wstrb)begin
                if(busy)error<=1;
                else if(addr<16'h1000)begin
                    if(wstrb!=15)error<=1;
                    else case(addr)
                        0:begin
                            // command 0 clear flags; 4 clear KV history; 1/2/3 start.
                            if(wdata==0)begin done<=0;error<=0;end
                            else if(wdata==4)begin count<=0;done<=0;error<=0;completed<=0;end
                            else if(wdata>3 || dim==0 || dim>64 ||
                                    (wdata==1 && count==32) || (wdata!=1 && count==0))error<=1;
                            else begin busy<=1;done<=0;error<=0;op<=wdata[1:0];cycles<=0;row<=0;col<=0;acc<=0;end
                        end
                        8:begin
                            // Changing dimension with cached vectors would reinterpret history.
                            if(count!=0)error<=1;else dim<=wdata;
                        end
                        default:error<=1;
                    endcase
                end else if(addr>=16'h1000 && addr<16'h1040)begin
                    for(j=0;j<4;j=j+1)if(wstrb[j])query[((addr-16'h1000)&16'hfffc)+j]<=wdata[8*j+:8];
                end else if(addr>=16'h1100 && addr<16'h1140)begin
                    for(j=0;j<4;j=j+1)if(wstrb[j])new_k[((addr-16'h1100)&16'hfffc)+j]<=wdata[8*j+:8];
                end else if(addr>=16'h1200 && addr<16'h1240)begin
                    for(j=0;j<4;j=j+1)if(wstrb[j])new_v[((addr-16'h1200)&16'hfffc)+j]<=wdata[8*j+:8];
                end else if(addr>=16'h1300 && addr<16'h1340)begin
                    for(j=0;j<4;j=j+1)if(wstrb[j])
                        probability[(((addr-16'h1300)&16'hfffc)+j)>>1][8*(j%2)+:8]<=wdata[8*j+:8];
                end else error<=1;
            end
        end
    end
endmodule
