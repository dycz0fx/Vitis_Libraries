/*
 * Copyright 2021 Xilinx, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "/proj/gw/Xilinx/Vitis_HLS/2020.2/include/gmp.h"
#include <ap_int.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <fstream>
#include <iostream>
#include <string>
#include "cmdlineparser.h"
#include "kernel_stream_utils.hpp"
#include "ap_axi_sdata.h"
#include "hls_stream.h"
#include "zlib_compress.hpp"
#include "compress_utils.hpp"

#include <string.h>

#define GMEM_DWIDTH 64 // bits
#define NUM_BLOCKS 8
typedef IntVectorStream_dt<8, GMEM_DWIDTH / 8> in_dT;
typedef IntVectorStream_dt<8, GMEM_DWIDTH / 8> out_dT;

const uint32_t c_size = (GMEM_DWIDTH / 8);

void gzipcMulticoreStreaming(hls::stream<in_dT>& inStream, hls::stream<out_dT>& outStream) {
    constexpr int c_defaultDepth = 4;

#pragma HLS STREAM variable = inStream depth = c_defaultDepth
#pragma HLS STREAM variable = outStream depth = c_defaultDepth

#pragma HLS BIND_STORAGE variable = inStream type = FIFO impl = SRL
#pragma HLS BIND_STORAGE variable = outStream type = FIFO impl = SRL

#pragma HLS DATAFLOW
    xf::compression::gzipMulticoreStaticCompressStream<32, 8, 0>(inStream, outStream);
}

// read_input(): Read Data from Global Memory and write into Stream inStream
static void read_input(uint8_t* input, hls::stream<in_dT>& inStream, unsigned long size) 
{
// Auto-pipeline is going to apply pipeline to this loop
mem_rd:
    in_dT val;
    val.strobe = c_size;
    for (int i = 0; i < size; i += c_size) {
        // Blocking write command to inStream
        bool last = false;
        uint32_t keep = -1;
        uint32_t rSize = c_size;
        if (i + c_size >= size) {
            rSize = size - i;
            last = true;
        }
        ap_uint<GMEM_DWIDTH> tmpVal;
        memcpy((void *)&tmpVal, input + i, rSize);
        if (last) {
            uint32_t num = 0;
            for (int b = 0; b < rSize; b++) {
                num |= 1UL << b;
            }
            keep = num;
        }

        if (!last)
        {
#pragma HLS PIPELINE
            for (int j = 0, k = 0; j < GMEM_DWIDTH; j += c_size)
            {
#pragma HLS UNROLL
                val.data[k++] = tmpVal.range(j + 7, j);
            }

            inStream << val;
        }
        else 
        {
            for (int j = 0, k = 0; j < GMEM_DWIDTH; j += c_size)
            {
#pragma HLS UNROLL
                val.data[k++] = tmpVal.range(j + 7, j);
            }
            ap_uint<c_size> cntr = 0;
            while (keep) {
                cntr += (keep & 0x1);
                keep >>= 1;
            }
            val.strobe = cntr;
            inStream << val;
            val.strobe = 0;
            inStream << val;
        }
    }
}

// write_output(): Read result from outStream and write the result to output array
static unsigned long write_output(uint8_t* output, hls::stream<out_dT>& outStream) 
{
// Auto-pipeline is going to apply pipeline to this loop
mem_wr:
    unsigned long size = 0;
    out_dT outVal = outStream.read();
    bool last = (outVal.strobe < c_size);
    ap_uint<GMEM_DWIDTH> tmpVal;
    int seg_id = 0;
    while (!last) {
#pragma HLS PIPELINE II = 1
        for (auto i = 0, j = 0; i < GMEM_DWIDTH; i += c_size) {
#pragma HLS UNROLL
            tmpVal.range(i + 7, i) = outVal.data[j++];
        }
        output[seg_id * c_size] = tmpVal;
        seg_id++;
        size += c_size;

        outVal = outStream.read();
        last = (outVal.strobe < c_size);
    }

    for (auto i = 0, j = 0; i < GMEM_DWIDTH; i += c_size) {
#pragma HLS UNROLL
        tmpVal.range(i + 7, i) = outVal.data[j++];
    }
    output[seg_id * GMEM_DWIDTH] = tmpVal;
    size += c_size;
    return size;
//     ap_uint<GMEM_DWIDTH / 8> cntr = 0;
//     auto strb = outVal.strobe;
//     for (auto i = 0; i < strb; i++) {
//         cntr.range(i, i) = 1;
//     }
//     tOut.keep = cntr;
//     outAxiStream << tOut;

//     for (int i = 0; i < size; i++) {
// #pragma HLS LOOP_TRIPCOUNT min = c_size max = c_size
//         // Blocking read command from OutStream
//         output[i] = outStream.read();
//     }
}


void gzipc(uint8_t *base, unsigned long input_offset, unsigned long output_offset, unsigned long size, unsigned long *new_size)
{
#pragma HLS INTERFACE m_axi port=base offset=off bundle=gmem depth=4194304

    hls::stream<in_dT> inStream("inStream");
    hls::stream<out_dT> outStream("outStream");
    // offset output
    uint8_t *input = base + input_offset;
	uint8_t *output = base + output_offset;
    
    // for testing: just copy the data
    *new_size = size;
    memcpy(output, input, size);
    return;

    // read data from input array to input stream inStream
    read_input(input, inStream, size);
    // Compression Call
    gzipcMulticoreStreaming(inStream, outStream);
    // write data from stream to output array
    *new_size = write_output(output, outStream);
}

int main(int argc, char* argv[]) {
    const char *text =
        "To evaluate our prefetcher we modelled the system using the gem5 simulator [4] in full system mode with the setup "
        "given in table 2 and the ARMv8 64-bit instruction set. Our applications are derived from existing benchmarks and "
        "libraries for graph traversal, using a range of graph sizes and characteristics. We simulate the core breadth-first search "
        "based kernels of each benchmark, skipping the graph construction phase. Our first benchmark is from the Graph 500 community [32]. "
        "We used their Kronecker graph generator for both the standard Graph 500 search benchmark and a connected components "
        "calculation. The Graph 500 benchmark is designed to represent data analytics workloads, such as 3D physics "
        "simulation. Standard inputs are too long to simulate, so we create smaller graphs with scales from 16 to 21 and edge "
        "factors from 5 to 15 (for comparison, the Graph 500 toy input has scale 26 and edge factor 16). "
        "Our prefetcher is most easily incorporated into libraries that implement graph traversal for CSR graphs. To this "
        "end, we use the Boost Graph Library (BGL) [41], a C++ templated library supporting many graph-based algorithms "
        "and graph data structures. To support our prefetcher, we added configuration instructions on constructors for CSR "
        "data structures, circular buffer queues (serving as the work list) and colour vectors (serving as the visited list). This "
        "means that any algorithm incorporating breadth-first searches on CSR graphs gains the benefits of our prefetcher without "
        "further modification. We evaluate breadth-first search, betweenness centrality and ST connectivity which all traverse "
        "graphs in this manner. To evaluate our extensions for sequential access prefetching (section 3.5) we use PageRank "
        "and sequential colouring. Inputs to the BGL algorithms are a set of real world "
        "graphs obtained from the SNAP dataset [25] chosen to represent a variety of sizes and disciplines, as shown in table 4. "
        "All are smaller than what we might expect to be processing in a real system, to enable complete simulation in a realistic "
        "time-frame, but as figure 2(a) shows, since stall rates go up for larger data structures, we expect the improvements we "
        "attain in simulation to be conservative when compared with real-world use cases.";

    size_t size = strlen(text);
    printf("size:%d\n", size);
    size += (8 - size % 8);
    printf("round size to %d\n", size);  

    char *buf = (char *)malloc(3 * 1024 * 1024); // 3MB buffer
    memcpy(buf, (const uint8_t *)text, size);    // copy to internal buff
    char *input = buf;
    char *compressed = buf + 1048576;
    int j = 0;
    cout << endl
         << "before deflate compression: " << to_string(size) << endl
         << endl;
    for (j = 0; j < size; j++)
    {
        cout << input[j];
    }
    cout << endl
         << endl;

    unsigned long new_size = 0;
    gzipc((uint8_t *)input, input - input, compressed - input, size, &new_size);

    cout << endl
         << "after deflate compression:" << to_string(new_size) << endl
         << endl;
    for (j = 0; j < new_size; j++)
    {
        cout << compressed[j];
    }
    cout << endl
         << endl;

}
